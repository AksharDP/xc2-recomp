// Crash reports (host.h, "crash reports"): what a crash or a fatal error leaves for the developers to debug it with.
//
// A report goes to the log as it is written, line by line, so a second fault inside the report still leaves what came
// before it, and into a buffer, which ends up in logs/crash-<time>.txt with the log's last 256 KiB and first 64 KiB
// (main.c rt_log_copy): one file a player attaches, complete even when the log file hit its cap or the run logged to
// stderr only. Sections, in order:
//   - the fault: what, where (host and guest), the faulting access and what that address is (module, guest region,
//     page state, frozen region, NVN pool and how the GPU sees it), the run's version and age, the last frame;
//   - host: thread, registers, code bytes around the instruction, the call stack as module+offset (the lifted function
//     nearest each frame in xc2.exe);
//   - guest: the lifted function, registers, the frame-pointer chain, and a scan of the stack for return addresses
//     (the words that point just past a BL or BLR), which still names the callers when the chain is broken;
//   - memory: around the fault address, around every register that points at readable memory, and the guest stack;
//   - every guest thread: state, wait, last SVC, registers, host and guest call chains (kernel.c kern_threads_dump);
//   - the GPU's fence values and memory mode, then the machine and the settings with a value (sysinfo.c).
// A minidump (Windows) goes beside it, logs/crash-<time>.dmp, unless XC2_CRASH_DUMP=0: the thread stacks and the
// memory they point at, for a debugger with this build's xc2.exe. The ten newest reports and dumps are kept.
//
// Only one thread reports: another thread that crashes meanwhile waits for the process to end, and a fault inside the
// report itself writes the file with what it has and ends the process.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"
#include "sysinfo.h"
#include "vk.h"

#define MAX_MODULES 16
#define REPORT_CAP (2u << 20)
#define HEAD_CAP (64u << 10)
#define TAIL_CAP (256u << 10)
#define KEEP_REPORTS 10

static char rep[REPORT_CAP];
static size_t rep_n;
static char rep_path[PLAT_PATH_MAX * 3], dump_path[PLAT_PATH_MAX * 3];
static int rep_written;
static volatile uint32_t rep_owner;   // host thread id of the reporting thread, 0 while none
static volatile uint64_t last_frame, last_frame_ns;

// ---- output: the log as it goes, and the buffer for the file
static void out_raw(const char *p, size_t n) {
    rt_log_raw(p, n);
    if (rep_n + n > REPORT_CAP) n = REPORT_CAP - rep_n;
    memcpy(rep + rep_n, p, n);
    rep_n += n;
}

static void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char *fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) out_raw(b, n < (int)sizeof b ? (size_t)n : sizeof b - 1);
}

static void out_line(void *ctx, const char *line) {
    outf("%s%s\n", (const char *)ctx, line);
}

// ---- lifted functions by host address
typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;
static uintptr_t fn_lo, fn_hi;   // host code range of the lifted functions

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

// The lifted function that contains a host code address, by the nearest function start below it.
uint64_t crash_lifted_function(uintptr_t rip) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
        // Runtime code follows the last lifted function: past it, a guess at most 16 KiB long.
        if (fn_rev_n) fn_lo = fn_rev[0].host, fn_hi = fn_rev[fn_rev_n - 1].host + (16u << 10);
    }
    if (rip < fn_lo || rip >= fn_hi) return 0;
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? fn_rev[lo - 1].guest : 0;
}

// ---- host symbols from the linker map (xc2.map beside xc2.exe, which build_game.py writes): each function is its own
// input section, so the map names runtime code and lifted code (f_<guest address>) alike. Offsets are from the image
// base. One pass over the file for all the wanted offsets; names[i] stays "" for an offset no section holds.
#define SYM_MAX 64
typedef struct { uint64_t off, at; char name[96]; } SymWant;

static int map_names(SymWant *w, int n) {
    char path[PLAT_PATH_MAX * 3];
    if (!plat_exe_path(path, sizeof path)) return 0;
    char *dot = strrchr(path, '.');
    if (!dot || strlen(dot) != 4) return 0;
    strcpy(dot, ".map");
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    static char line[1024];
    uint64_t lo = 0, hi = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long addr, size;
        unsigned align;
        int used = 0;
        if (sscanf(line, "%llx %llx %u %n", &addr, &size, &align, &used) < 3 || !used) continue;
        char *rest = line + used;
        rest[strcspn(rest, "\r\n")] = 0;
        if (strstr(rest, ":(")) {   // an input section: the bounds its symbols name
            lo = addr, hi = addr + size;
            continue;
        }
        if (size || !*rest || addr < lo || addr >= hi) continue;
        for (int i = 0; i < n; i++)
            if (w[i].off >= addr && w[i].off < hi && (!w[i].name[0] || addr >= w[i].at)) {
                w[i].at = addr;
                snprintf(w[i].name, sizeof w[i].name, "%s", rest);
            }
    }
    fclose(f);
    return 1;
}

// ---- what an address is
static const char *page_state(uint64_t a) {
    uint64_t end;
    int s = plat_mem_state(a & ~4095ull, (a & ~4095ull) + 4096, &end);
    return s == PLAT_PROT_RW ? "read-write" : s == PLAT_PROT_READ ? "read-only" : s == PLAT_PROT_NONE ? "no access"
         : s == PLAT_MEM_RESERVED ? "reserved, not committed" : "not reserved by the runtime";
}

static void describe(uint64_t a, char *out, size_t n) {
    char d[192] = "";
    const RtGuestLayout *g = mem_layout();
    const struct { const char *name; RtRange r; } regions[] = {
        {"guest stack region", g->stack}, {"guest heap", g->heap},     {"guest alias region", g->alias},
        {"kernel region", g->kernel},     {"host function window", g->hostfn}, {"guest address space", g->aslr}};
    for (int i = 0; i < rt_layout_count; i++)
        if (a - rt_layout[i].base < rt_layout[i].size) {
            ldr_describe(a, d, sizeof d);
            break;
        }
    if (!d[0])
        for (unsigned i = 0; i < sizeof regions / sizeof *regions; i++)
            if (regions[i].r.size && a - regions[i].r.base < regions[i].r.size) {
                const char *h = i == 4 ? hostfn_name(a) : NULL;
                snprintf(d, sizeof d, "%s+%#llx%s%s", regions[i].name, (unsigned long long)(a - regions[i].r.base),
                         h ? " " : "", h ? h : "");
                break;
            }
    if (!d[0]) {
        char m[256];
        uintptr_t base;
        if (a < 0x10000) snprintf(d, sizeof d, "the null page");
        else if (plat_module_at((uintptr_t)a, m, sizeof m, &base))
            snprintf(d, sizeof d, "host %s+%#llx", m, (unsigned long long)(a - base));
        else snprintf(d, sizeof d, "host memory");
    }
    char pool[256] = "";
    if (vk_mem_describe(a, pool, sizeof pool)) {
        size_t k = strlen(pool);
        memmove(pool + 2, pool, k + 1);
        pool[0] = ';', pool[1] = ' ';
    }
    snprintf(out, n, "%s; page %s%s%s", d, page_state(a), a < (1ull << 39) && mem_frozen(a) ? ", frozen region" : "",
             pool);
}

// ---- memory
typedef struct { uint64_t lo, hi; } Span;
static Span dumped[128];
static int ndumped;

// 16 bytes a line, hex and ASCII; a line that cannot be read says so. Ranges dumped before are skipped.
static void hexdump(const char *label, uint64_t a, uint64_t before, uint64_t len) {
    uint64_t lo = (a - (a < before ? a : before)) & ~15ull, hi = (a + len + 15) & ~15ull;
    for (int i = 0; i < ndumped; i++)
        if (lo >= dumped[i].lo && hi <= dumped[i].hi) {
            outf("  %s: %#llx, dumped above\n", label, (unsigned long long)a);
            return;
        }
    if (ndumped < (int)(sizeof dumped / sizeof *dumped)) dumped[ndumped++] = (Span){lo, hi};
    char d[512];
    describe(a, d, sizeof d);
    outf("  %s: %#llx (%s)\n", label, (unsigned long long)a, d);
    for (uint64_t p = lo; p < hi; p += 16) {
        uint8_t b[16];
        if (!plat_mem_read(p, b, 16)) {
            outf("    %016llx  unreadable\n", (unsigned long long)p);
            p = ((p | 4095) + 1) - 16;   // the rest of the page is unreadable too
            if (p + 16 >= hi) break;
            continue;
        }
        char hex[16 * 3 + 2], asc[17];
        int k = 0;
        for (int i = 0; i < 16; i++) {
            k += snprintf(hex + k, sizeof hex - (size_t)k, i == 8 ? " %02x " : "%02x ", b[i]);
            asc[i] = b[i] >= 0x20 && b[i] < 0x7f ? (char)b[i] : '.';
        }
        asc[16] = 0;
        outf("    %016llx  %s |%s|%s\n", (unsigned long long)p, hex, asc, p <= a && a < p + 16 ? " <-" : "");
    }
}

static int pointer_like(uint64_t v) { return v >= 0x10000 && v < (1ull << 47) && plat_mem_readable(v, 1); }

// A guest return address: inside a module, 4-aligned, just past a BL or a BLR.
static int return_address(uint64_t v) {
    if (v & 3) return 0;
    int in = 0;
    for (int i = 0; i < rt_layout_count && !in; i++) in = v - 4 - rt_layout[i].base < rt_layout[i].size;
    uint32_t insn;
    if (!in || !plat_mem_read(v - 4, &insn, 4)) return 0;
    return (insn & 0xfc000000u) == 0x94000000u || (insn & 0xfffffc1fu) == 0xd63f0000u;
}

// ---- files
static int crash_folder(char *out, size_t n) {
    if (!sysinfo_log_folder(out, n)) return 0;
    if (!rt_log_path()) {   // logging to stderr: beside that file; else xc2.exe's folder gets a logs folder
        char exe[PLAT_PATH_MAX * 3];
        if (plat_exe_path(exe, sizeof exe)) {
            char *s = plat_last_sep(exe), *f = strrchr(exe, '/');
            if (f > s) s = f;
            if (s) *s = 0;
            if (!strcmp(exe, out) && strlen(out) + sizeof "/logs" <= n) strcat(out, "/logs");
        }
    }
    plat_file_mkdir_p(out);
    return 1;
}

typedef struct { char names[64][64]; int n; char prefix[16]; } Old;

static int old_name(const char *name, void *ctx) {
    Old *o = ctx;
    size_t k = strlen(o->prefix);
    if (!strncmp(name, o->prefix, k) && strlen(name) < 64 && o->n < 64) snprintf(o->names[o->n++], 64, "%s", name);
    return 1;
}

static int by_name(const void *a, const void *b) { return strcmp(a, b); }

// Leaves room for one more: keeps the newest KEEP_REPORTS - 1 files <prefix>-*<ext> (their names sort by time).
static void prune(const char *dir, const char *prefix, const char *ext) {
    static Old o;
    o.n = 0;
    snprintf(o.prefix, sizeof o.prefix, "%s-", prefix);
    if (!plat_file_list(dir, ext, old_name, &o)) return;
    qsort(o.names, (size_t)o.n, sizeof o.names[0], by_name);
    for (int i = 0; i + KEEP_REPORTS - 1 < o.n; i++) {
        char p[PLAT_PATH_MAX * 3 + 64];
        snprintf(p, sizeof p, "%s/%s", dir, o.names[i]);
        plat_file_delete(p);
    }
}

// The file: the report, the log's last bytes, its first bytes. The log copies end with the report itself (it went to
// the log as it was written), which the file holds already: those bytes are cut.
static void write_file(void) {
    if (rep_written || !rep_path[0]) return;
    rep_written = 1;
    static char head[HEAD_CAP], tail[TAIL_CAP], file[REPORT_CAP + HEAD_CAP + TAIL_CAP + 512];
    size_t hn = 0, tn = 0, at = 0;
    uint64_t total = rt_log_copy(head, sizeof head, &hn, tail, sizeof tail, &tn);
    memcpy(file, rep, rep_n);
    at = rep_n;
    size_t own = rep_n < tn ? rep_n : tn;
    if (total <= tn) {   // the tail holds the whole log
        at += (size_t)snprintf(file + at, 256, "\n==== the whole log before the report ====\n");
        memcpy(file + at, tail, tn - own);
        at += tn - own;
    } else {             // the lines before the report, then the start-up lines (settings, GPU, mods)
        at += (size_t)snprintf(file + at, 256, "\n==== the log's last %zu KiB before the report (of %llu KiB logged) "
                               "====\n", (tn - own) >> 10, (unsigned long long)(total >> 10));
        memcpy(file + at, tail, tn - own);
        at += tn - own;
        at += (size_t)snprintf(file + at, 256, "\n==== the log's first %zu KiB ====\n", hn >> 10);
        memcpy(file + at, head, hn);
        at += hn;
    }
    if (!plat_file_write(rep_path, file, at)) {
        rt_logf("crash report: cannot write %s\n", rep_path);
        rep_path[0] = 0;
    }
}

const char *crash_report_path(void) { return rep_written && rep_path[0] ? rep_path : NULL; }

// ---- the report
static const char *const host_reg[17] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8",
                                         "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rflags"};

static void section(const char *name) { outf("---- %s\n", name); }

// prefix: "crash" or "hang", the files' names.
static void header(const char *kind, const char *prefix) {
    struct tm t;
    plat_local_time(&t);
    char dir[PLAT_PATH_MAX * 3], stamp[32];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec);
    if (crash_folder(dir, sizeof dir)) {
        prune(dir, prefix, ".txt");
        prune(dir, prefix, ".dmp");
        snprintf(rep_path, sizeof rep_path, "%s/%s-%s.txt", dir, prefix, stamp);
        snprintf(dump_path, sizeof dump_path, "%s/%s-%s.dmp", dir, prefix, stamp);
    }
    outf("\n==== XC2 Recomp %s report ====\n", kind);
    char os[256];
    const char *bid = mods_main_build_id();
    outf("version %s, title %016llx, main build id %s, %s\n", rt_port_version, (unsigned long long)rt_title_id,
         bid && *bid ? bid : "?", plat_os_version(os, sizeof os) ? os : "unknown OS");
    uint64_t now = plat_now_ns(), lf = last_frame, lns = last_frame_ns;
    int64_t world = frame_step_world_us();
    outf("time %04d-%02d-%02d %02d:%02d:%02d, process up %.1f s, guest up %.1f s, ", t.tm_year + 1900, t.tm_mon + 1,
         t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (double)plat_process_uptime_ns() / 1e9,
         rt_guest_start_ns ? (double)(kern_now_ns() - rt_guest_start_ns) / 1e9 : 0.0);
    if (lns) outf("last frame %llu presented %.2f s ago", (unsigned long long)lf, (double)(now - lns) / 1e9);
    else outf("no frame presented yet");
    if (world >= 0) outf(", world clock %.1f s", (double)world / 1e6);
    outf("\n");
}

static char rip_sym[96];   // host_section: xc2.map's name for the faulting instruction, when rip_mapped
static int rip_mapped;

static void host_section(const PlatCrash *x) {
    char name[64], d[192];
    plat_thread_name(name, sizeof name);
    section("host");
    outf("thread %u%s%s%s\n", plat_thread_id(), name[0] ? " \"" : "", name, name[0] ? "\"" : "");
    if (x && x->has_regs) {
        for (int i = 0; i < 17; i++)
            outf("  %-6s %016llx%s", host_reg[i], (unsigned long long)x->regs[i], i % 4 == 3 || i == 16 ? "\n" : "");
        uint8_t code[48];
        if (x->rip > 16 && plat_mem_read(x->rip - 16, code, sizeof code)) {   // for a disassembler with this xc2.exe
            outf("  code at rip-16:");
            for (int i = 0; i < 48; i++) outf(i == 16 ? " [%02x" : i == 17 ? "] %02x" : " %02x", code[i]);
            outf("\n");
        }
    }
    const uintptr_t *fr = x ? x->frames : NULL;
    uintptr_t own[PLAT_CRASH_FRAMES];
    int n = x ? x->nframes : 0;
    if (!x || !n) fr = own, n = plat_backtrace(own, PLAT_CRASH_FRAMES);
    // Names from xc2.map for the frames in xc2.exe (and the faulting instruction's, frame 0 or not).
    static SymWant w[SYM_MAX];
    int nw = 0;
    uintptr_t self = 0;
    plat_module_at((uintptr_t)crash_init, NULL, 0, &self);
    for (int i = 0; i < n && nw < SYM_MAX; i++) w[nw++] = (SymWant){fr[i] - self, 0, ""};
    int have_map = self && map_names(w, nw);
    rip_mapped = have_map && x && x->rip && nw && fr[0] == x->rip;
    if (rip_mapped) snprintf(rip_sym, sizeof rip_sym, "%s", w[0].name);
    for (int i = 0; i < n; i++) {
        char m[256];
        uintptr_t base = 0;
        if (!plat_module_at(fr[i], m, sizeof m, &base)) snprintf(m, sizeof m, "?"), base = 0;
        outf("  host frame %2d: %s+%#llx", i, m, (unsigned long long)(fr[i] - base));
        if (base == self && i < nw && w[i].name[0]) {
            outf("  %s+%#llx", w[i].name, (unsigned long long)(w[i].off - w[i].at));
            uint64_t g = !strncmp(w[i].name, "f_", 2) ? strtoull(w[i].name + 2, NULL, 16) : 0;
            if (g) outf(" = %s", ldr_describe(g, d, sizeof d));
        } else if (base == self && !have_map) {   // no map: the lifted function by the table, runtime code unnamed
            uint64_t f = crash_lifted_function(fr[i]);
            if (f) outf("  near lifted f_%llx = %s", (unsigned long long)f, ldr_describe(f, d, sizeof d));
        }
        outf("\n");
    }
    if (!have_map) outf("  (no xc2.map beside xc2.exe: runtime frames are offsets only)\n");
}

static void guest_section(const PlatCrash *x, Cpu *c) {
    char d[192];
    section("guest");
    if (x && !x->module && x->rip) {   // the map knows runtime code from lifted code; the table only guesses
        uint64_t f = rip_mapped ? (!strncmp(rip_sym, "f_", 2) ? strtoull(rip_sym + 2, NULL, 16) : 0)
                                : crash_lifted_function(x->rip);
        if (f) outf("  in lifted function f_%llx = %s\n", (unsigned long long)f, ldr_describe(f, d, sizeof d));
        else if (rip_mapped && rip_sym[0]) outf("  in runtime code %s\n", rip_sym);
    }
    if (!c) {
        outf("  not on a guest thread\n");
        return;
    }
    for (int i = 0; i < 31; i++)
        outf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
    const char *hf = hostfn_name(c->pc);   // a crash inside a host function: c->pc is its synthetic address
    outf("  sp  %016llx\n  pc  %016llx%s%s  lr %s\n", (unsigned long long)c->sp, (unsigned long long)c->pc,
         hf ? " host function " : "", hf ? hf : "", ldr_describe(c->x[30], d, sizeof d));
    outf("  nzcv %08llx tpidr %016llx tpidrro %016llx excl %016llx\n", (unsigned long long)c->nzcv,
         (unsigned long long)c->tpidr, (unsigned long long)c->tpidrro, (unsigned long long)c->excl_addr);
    uint64_t fp = c->x[29];
    for (int n = 0; n < 48 && fp && !(fp & 7) && plat_mem_readable(fp, 16); n++) {
        uint64_t next = LD64(fp), lr = LD64(fp + 8);
        outf("  guest frame %2d: %s (fp %#llx)\n", n, ldr_describe(lr, d, sizeof d), (unsigned long long)fp);
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
    // Return addresses on the stack: the callers even where a function keeps no frame record.
    int hits = 0;
    for (uint64_t a = c->sp & ~7ull; a < c->sp + (32u << 10) && hits < 48; a += 8) {
        uint64_t v;
        if (!plat_mem_read(a, &v, 8)) break;
        if (!return_address(v)) continue;
        if (!hits++) outf("  return addresses on the guest stack:\n");
        outf("    sp+%#06llx: %s\n", (unsigned long long)(a - c->sp), ldr_describe(v, d, sizeof d));
    }
}

static void memory_section(const PlatCrash *x, const Cpu *c) {
    section("memory");
    ndumped = 0;
    if (x && x->access >= 0 && pointer_like(x->addr)) hexdump("fault address", x->addr, 128, 128);
    else if (x && x->access >= 0) {
        char d[512];
        describe(x->addr, d, sizeof d);
        outf("  fault address: %#llx (%s), unreadable\n", (unsigned long long)x->addr, d);
    }
    char label[32];
    if (c) {
        for (int i = 0; i < 31; i++)
            if (pointer_like(c->x[i])) {
                snprintf(label, sizeof label, "x%d", i);
                hexdump(label, c->x[i], 32, 64);
            }
        if (pointer_like(c->sp)) hexdump("guest stack (sp)", c->sp, 0, 512);
    }
    if (x && x->has_regs) {   // the lifted code's own registers: the guest pointers among them
        const RtGuestLayout *g = mem_layout();
        for (int i = 0; i < 16; i++) {
            uint64_t v = x->regs[i];
            if (i == 4 || v < g->aslr.base || v - g->aslr.base >= g->aslr.size || !pointer_like(v)) continue;
            snprintf(label, sizeof label, "host %s", host_reg[i]);
            hexdump(label, v, 32, 64);
        }
    }
}

static void gpu_section(void) {
    section("gpu");
    if (!vk_ctx()) {
        outf("  no Vulkan device\n");
        return;
    }
    outf("  fence values: issued %llu, done %llu, reserved %llu; VRAM mirrors %s\n",
         (unsigned long long)vk_sync_issued(), (unsigned long long)vk_sync_fence_done(),
         (unsigned long long)vk_sync_reserved(), vk_mirror_on ? "on" : "off");
}

static void system_section(void) {
    static char s[32768];
    section("system");
    int n = sysinfo_text(s, sizeof s);
    if (n > 0) out_raw(s, (size_t)n);
}

static void report(const PlatCrash *x, const char *fatal) {
    Cpu *c = kern_cpu();
    rep_n = 0;   // a hang report may have used the buffer
    rep_written = 0;
    rep_path[0] = dump_path[0] = 0;
    header(x ? "crash" : "fatal error", "crash");
    char d[512];
    if (x) {
        if (x->module)
            outf("FATAL: %s (%#x) in %s+%#llx (host %p)", x->what, x->code, x->module, (unsigned long long)x->offset,
                 (void *)x->rip);
        else if (x->rip)
            outf("FATAL: %s (%#x) at host %p (xc2+%#llx)", x->what, x->code, (void *)x->rip,
                 (unsigned long long)x->offset);
        else
            outf("FATAL: %s", x->what);
        if (x->access >= 0)
            outf(", %s address %#llx", x->access == 1 ? "writing" : x->access == 8 ? "executing" : "reading",
                 (unsigned long long)x->addr);
        outf("\n");
        if (x->access >= 0) {
            describe(x->addr, d, sizeof d);
            outf("  that address: %s\n", d);
        }
    } else {
        outf("FATAL: %s\n", fatal);
    }
    host_section(x);
    guest_section(x, c);
    memory_section(x, c);
    section("guest threads");
    kern_threads_dump(out_line, "  ", 1);
    gpu_section();
    system_section();
    const char *dump = cfg_get("XC2_CRASH_DUMP");
    int want_dump = dump_path[0] && !(dump && !strcmp(dump, "0"));
    if (want_dump) {   // the guest stack and the bytes around the fault, besides what the dump takes by itself
        uint64_t ranges[3][2] = {{0, 0}, {0, 0}, {0, 0}};
        if (c) ranges[0][0] = c->sp & ~4095ull, ranges[0][1] = (c->sp & ~4095ull) + (64u << 10);
        if (x && x->access >= 0) ranges[1][0] = (x->addr & ~4095ull) - 4096, ranges[1][1] = (x->addr & ~4095ull) + 8192;
        if (c && c->x[29]) ranges[2][0] = c->x[29] & ~4095ull, ranges[2][1] = (c->x[29] & ~4095ull) + 8192;
        outf("minidump: %s\n", plat_crash_dump(dump_path, x, (const uint64_t (*)[2])ranges, 3) ? dump_path
                                                                                                : "none written");
    }
    outf("crash report: %s\n", rep_path[0] ? rep_path : "no folder to write it in");
    outf("==== end of the report ====\n");
    write_file();
}

// 1 when this thread may report; never returns on another thread while one reports, nor on a fault in the report.
static int report_enter(void) {
    uint32_t me = plat_thread_id(), none = 0;
    if (__atomic_compare_exchange_n(&rep_owner, &none, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    if (none == me) {   // a fault inside the report: keep what it has
        rt_logf("\nFATAL: a second fault inside the crash report; the report so far is kept\n");
        write_file();
        plat_exit(71);
    }
    for (;;) plat_sleep_ms(1000);   // the reporting thread ends the process
}

static void crash_handler(const PlatCrash *x) {
    report_enter();
    report(x, NULL);
    fflush(stderr);
    char text[PLAT_PATH_MAX * 3 + 512];
    snprintf(text, sizeof text,
             "XC2 Recomp crashed (%s%s%s).\n\nPlease attach this crash report to a bug report:\n%s\n\nThe .dmp file "
             "beside it may hold game data: share it only when a developer asks for it.",
             x->what, x->module ? " in " : "", x->module ? x->module : "",
             rep_path[0] ? rep_path : "the logs folder next to the game");
    plat_fatal_notice(text);
    plat_exit(71);
}

void crash_init(void) { plat_crash_hook(crash_handler); }

void crash_fatal(const char *msg) {
    if (!rt_guest_start_ns) return;   // start-up errors before the game runs: the message says it all
    report_enter();
    report(NULL, msg);
    fflush(stderr);
}

// XC2_CRASH_TEST=fault|fatal|abort|hang[:frame]: at that present (default 300), on the presenting guest thread, a
// write to an unmapped address, an rt_fatal, an abort() or a 3.5 s stall (for XC2_HANG_DUMP=1), to check the reports
// end to end.
static void crash_test(uint64_t frame) {
    static int kind = -1;
    static uint64_t at = 300;
    if (kind < 0) {
        const char *e = cfg_get("XC2_CRASH_TEST");
        kind = !e || !*e ? 0 : !strncmp(e, "fault", 5) ? 1 : !strncmp(e, "fatal", 5) ? 2 : !strncmp(e, "abort", 5) ? 3
             : !strncmp(e, "hang", 4) ? 4 : 0;
        const char *c = e ? strchr(e, ':') : NULL;
        if (c) at = strtoull(c + 1, NULL, 10);
    }
    if (!kind || frame != at) return;
    static const char *const names[] = {"", "fault", "fatal", "abort", "hang"};
    rt_log("crash test: %s at frame %llu", names[kind], (unsigned long long)frame);
    if (kind == 4) {   // with XC2_HANG_DUMP=1: a stall long enough for a hang report, then the game goes on
        plat_sleep_ms(3500);
        return;
    }
    if (kind == 1) *(volatile uint64_t *)(uintptr_t)0x18 = frame;
    if (kind == 2) rt_fatal("crash test at frame %llu", (unsigned long long)frame);
    abort();
}

void crash_note_frame(uint64_t frame) {
    last_frame = frame;
    __atomic_store_n(&last_frame_ns, plat_now_ns(), __ATOMIC_RELEASE);
    crash_test(frame);
}

// A stall: the threads into the log and into a hang-<time> file like a crash report's (no fault, no minidump).
void crash_hang_report(int64_t seconds) {
    uint32_t none = 0;
    if (!__atomic_compare_exchange_n(&rep_owner, &none, plat_thread_id(), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;   // a crash is being reported
    rep_n = 0;
    rep_written = 0;
    rep_path[0] = dump_path[0] = 0;
    header("hang", "hang");
    outf("no frame presented for %lld s\n", (long long)seconds);
    section("guest threads");
    kern_threads_dump(out_line, "  ", 0);
    gpu_section();
    outf("hang report: %s\n==== end of the report ====\n", rep_path[0] ? rep_path : "no folder to write it in");
    write_file();
    __atomic_store_n(&rep_owner, 0, __ATOMIC_RELEASE);
}
