// Offline replay of XC2_CALLCAP files (runtime/callcap.h): every captured call is run again on its own inputs by the
// runtime's own code, compiled into this program (tools/callbench.py builds it from a runtime folder, so a changed
// function is timed and checked against the game's data without running the game). Per call the best of --reps runs
// is kept; per kind it prints the calls, the time the game took (captured), the time here, and how many results differ
// from the captured ones.
//
//   CC_INFLATE    ov_inflate.c inflate_fast: output bytes, stream fields, hold, bits and mode compared
//   CC_DESWIZZLE  tex_deswizzle.c nvn_deswizzle: return value and output rows compared
//   CC_WWQUERY    the write-watch query (Windows GetWriteWatch with reset) on a region of this program with the same
//                 pages written: the page count compared. The OS call is the code under test; its cost in the game also
//                 depends on everything else in the process's address space, so compare runs of this program with each
//                 other, not with the captured time.
//   CC_COPY       sizes only: a plain memcpy of each, as a model of mem_copy_par's copies
//
// usage: callbench <dir or .ccap>... [--reps N] [--kind NAME] [--csv FILE]
#define CALLBENCH 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <time.h>
#endif

#include "callcap.h"
#include "tex.h"

#include "ov_inflate.c"

const char *cfg_get(const char *name) { (void)name; return NULL; }   // nvn_tex.c reads its knobs: defaults here

static const char *const kind_name[CC_KINDS] = {"?", "inflate", "deswizzle", "copy", "wwquery"};

static uint64_t now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

typedef struct { const uint8_t *p; uint64_t n; } Blob;

typedef struct {
    uint64_t calls, cap_ns, run_ns, bytes, differ, skipped;
} Stat;
static Stat S[CC_KINDS];
static int reps = 5, only = 0;
static FILE *csv;

// ---- inflate_fast
static int run_inflate(const Blob *b, int nb, uint64_t *best) {
    if (nb < 7 || b[0].n < sizeof(CcInflateIn) || b[6].n < sizeof(CcInflateOut)) return -1;
    CcInflateIn in;
    CcInflateOut want;
    memcpy(&in, b[0].p, sizeof in);
    memcpy(&want, b[6].p, sizeof want);
    uint8_t *st = calloc(1, ST_SIZE + 64), *obuf = malloc((size_t)in.back + in.avail_out + 1),
            *ibuf = malloc((size_t)in.avail_in + 1), *win = malloc(b[3].n + 1), *lc = malloc(b[1].n), *dc = malloc(b[2].n);
    if (!st || !obuf || !ibuf || !win || !lc || !dc) return -1;
    memcpy(lc, b[1].p, b[1].n), memcpy(dc, b[2].p, b[2].n), memcpy(ibuf, b[5].p, in.avail_in);
    GStream s;
    int same = 1;
    *best = UINT64_MAX;
    for (int r = 0; r < reps; r++) {
        memset(st, 0, ST_SIZE);
        uint64_t sa = (uint64_t)(uintptr_t)st;
        wr64(sa + ST_HOLD, in.hold), wr32(sa + ST_BITS, in.bits), wr32(sa + ST_WSIZE, in.wsize);
        wr32(sa + ST_WHAVE, in.whave), wr32(sa + ST_WNEXT, in.wnext);
        if (in.window_n) memcpy(win, b[3].p, in.window_n);
        wr64(sa + ST_WINDOW, in.window_n ? (uint64_t)(uintptr_t)win : 0);
        wr64(sa + ST_LENCODE, (uint64_t)(uintptr_t)lc), wr64(sa + ST_DISTCODE, (uint64_t)(uintptr_t)dc);
        wr32(sa + ST_LENBITS, in.lenbits), wr32(sa + ST_DISTBITS, in.distbits), wr32(sa + ST_SANE, in.sane);
        wr32(sa + ST_MODE, 0x3f3f);
        memcpy(obuf, b[4].p, in.back);
        memset(&s, 0, sizeof s);
        s.next_in = (uint64_t)(uintptr_t)ibuf, s.avail_in = in.avail_in;
        s.next_out = (uint64_t)(uintptr_t)(obuf + in.back), s.avail_out = in.avail_out, s.state = sa;
        uint64_t t0 = now_ns();
        inflate_fast((uint64_t)(uintptr_t)&s, in.start);
        uint64_t d = now_ns() - t0;
        if (d < *best) *best = d;
        uint32_t mode = rd32(sa + ST_MODE) == 0x3f3f ? want.mode : rd32(sa + ST_MODE);   // mode is written on exits only
        same &= s.avail_in == want.avail_in && s.avail_out == want.avail_out && rd32(sa + ST_BITS) == want.bits &&
                rd64(sa + ST_HOLD) == want.hold && mode == want.mode &&
                cc_hash(obuf + in.back, in.avail_out - s.avail_out) == want.out_hash;
    }
    S[CC_INFLATE].bytes += in.avail_out - s.avail_out;
    free(st), free(obuf), free(ibuf), free(win), free(lc), free(dc);
    return same;
}

// ---- nvn_deswizzle
static int run_deswizzle(const Blob *b, int nb, uint64_t *best) {
    if (nb < 3 || b[0].n < sizeof(CcDeswizzle) || b[1].n < sizeof(nvn_texdesc)) return -1;
    CcDeswizzle r;
    nvn_texdesc d;
    memcpy(&r, b[0].p, sizeof r);
    memcpy(&d, b[1].p, sizeof d);
    uint8_t *dst = malloc((size_t)r.dst_size + 1);
    if (!dst) return -1;
    int same = 1;
    *best = UINT64_MAX;
    for (int k = 0; k < reps; k++) {
        uint64_t t0 = now_ns();
        int ret = nvn_deswizzle(&d, r.level, r.layer, b[2].p, dst, (size_t)r.dst_pitch);
        uint64_t t = now_ns() - t0;
        if (t < *best) *best = t;
        same &= (uint32_t)ret == r.ret && (ret || cc_hash_rows(dst, r.dst_size, r.row_bytes, r.dst_pitch) == r.dst_hash);
    }
    S[CC_DESWIZZLE].bytes += r.dst_size;
    free(dst);
    return same;
}

// ---- memcpy model of mem_copy_par
static int run_copy(const Blob *b, int nb, uint64_t *best) {
    if (nb < 1 || b[0].n < sizeof(CcCopy)) return -1;
    CcCopy c;
    memcpy(&c, b[0].p, sizeof c);
    // The game's copies read and write guest memory that is mostly not in the cache, so each one here goes to the next
    // part of two 256 MiB arenas (committed and written once, as the game's pages are by then): copying the same small
    // buffer again would time the cache instead.
    enum { ARENA = 256 << 20 };
    static uint8_t *src, *dst;
    static uint64_t at;
    if (!src) {
        src = malloc(ARENA), dst = malloc(ARENA);
        if (!src || !dst) return free(src), free(dst), src = dst = NULL, -1;
        memset(src, 1, ARENA);
        memset(dst, 0, ARENA);
    }
    uint64_t span = (c.n + 8191) & ~4095ull;
    if (span > ARENA) return -1;
    *best = UINT64_MAX;
    for (int k = 0; k < reps; k++) {
        if (at + span > ARENA) at = 0;
        uint64_t t0 = now_ns();
        memcpy(dst + at + (c.dst_off & 0xfff), src + at + (c.src_off & 0xfff), (size_t)c.n);
        uint64_t t = now_ns() - t0;
        if (t < *best) *best = t;
        at += span;
    }
    S[CC_COPY].bytes += c.n;
    return 1;
}

// ---- write-watch query
#ifdef _WIN32
static int run_wwquery(const Blob *b, int nb, uint64_t *best) {
    if (nb < 2 || b[0].n < sizeof(CcWwQuery)) return -1;
    CcWwQuery q;
    memcpy(&q, b[0].p, sizeof q);
    static uint8_t *mem;
    static uint64_t mem_size;
    if (q.size > mem_size) {
        if (mem) VirtualFree(mem, 0, MEM_RELEASE);
        mem = VirtualAlloc(NULL, (SIZE_T)q.size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (!mem) return mem_size = 0, -1;
        mem_size = q.size;
        memset(mem, 0, (size_t)q.size);
    }
    static void **pages;
    static uint64_t pages_cap;
    if (q.cap > pages_cap) {
        free(pages);
        pages = malloc(sizeof *pages * (size_t)q.cap);
        pages_cap = pages ? q.cap : 0;
        if (!pages) return -1;
    }
    const uint32_t *idx = (const uint32_t *)b[1].p;
    uint64_t n = b[1].n / 4;
    int same = 1;
    *best = UINT64_MAX;
    for (int k = 0; k < reps; k++) {
        ULONG_PTR cnt = (ULONG_PTR)q.cap;
        DWORD gran;
        ResetWriteWatch(mem, (SIZE_T)mem_size);
        for (uint64_t i = 0; i < n; i++) mem[(uint64_t)idx[i] << 12] ^= 1;   // the same pages written
        uint64_t t0 = now_ns();
        UINT rc = GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem, (SIZE_T)q.size, pages, &cnt, &gran);
        uint64_t t = now_ns() - t0;
        if (t < *best) *best = t;
        same &= rc == 0 && cnt == q.n;
    }
    return same;
}
#else
static int run_wwquery(const Blob *b, int nb, uint64_t *best) { (void)b, (void)nb, (void)best; return -1; }
#endif

static int (*const runner[CC_KINDS])(const Blob *, int, uint64_t *) = {NULL, run_inflate, run_deswizzle, run_copy,
                                                                       run_wwquery};

static uint8_t *slurp(const char *path, uint64_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    fseeko(f, 0, SEEK_END);
    long long sz = ftello(f);
    fseeko(f, 0, SEEK_SET);
#endif
    uint8_t *b = malloc((size_t)sz + 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) free(b), b = NULL;
    fclose(f);
    *n = (uint64_t)sz;
    return b;
}

static uint64_t frames, frame_ns;

static void bench_file(const char *path) {
    uint64_t n;
    uint8_t *buf = slurp(path, &n);
    if (!buf || n < sizeof(CcFileHdr) || memcmp(buf, CC_MAGIC, 8)) {
        fprintf(stderr, "callbench: %s: not a capture file\n", path);
        free(buf);
        return;
    }
    CcFileHdr h;
    memcpy(&h, buf, sizeof h);
    frames++, frame_ns += h.interval_ns;
    const uint8_t *p = buf + sizeof h, *end = buf + n;
    for (uint64_t i = 0; i < h.nrec && p + sizeof(CcRec) <= end; i++) {
        CcRec r;
        memcpy(&r, p, sizeof r);
        p += sizeof r;
        Blob b[16];
        int nb = 0;
        int whole = 1;
        for (uint32_t k = 0; k < r.nblob; k++) {
            uint64_t len;
            if ((uint64_t)(end - p) < 8) { whole = 0; break; }
            memcpy(&len, p, 8);
            p += 8;
            if (len > (uint64_t)(end - p) || ((len + 7) & ~7ull) > (uint64_t)(end - p)) { whole = 0; break; }
            if (nb < 16) b[nb++] = (Blob){p, len};
            p += (len + 7) & ~7ull;
        }
        if (!whole) {   // a cut-off file: its last record is not replayed
            fprintf(stderr, "callbench: %s: record %llu runs past the end of the file\n", path, (unsigned long long)i);
            break;
        }
        if (r.kind >= CC_KINDS || (only && (int)r.kind != only)) continue;
        uint64_t best = 0;
        int ok = runner[r.kind](b, nb, &best);
        Stat *s = &S[r.kind];
        if (ok < 0) {
            s->skipped++;
            continue;
        }
        s->calls++, s->cap_ns += r.ns, s->run_ns += best, s->differ += !ok;
        if (csv)
            fprintf(csv, "%llu,%s,%u,%llu,%llu,%d\n", (unsigned long long)h.frame, kind_name[r.kind], r.tid,
                    (unsigned long long)r.ns, (unsigned long long)best, ok);
    }
    free(buf);
}

static void bench_path(const char *path) {
    size_t l = strlen(path);
    if (l > 5 && !strcmp(path + l - 5, ".ccap")) {
        bench_file(path);
        return;
    }
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*.ccap", path);
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        char p[1024];
        snprintf(p, sizeof p, "%s\\%s", path, fd.cFileName);
        bench_file(p);
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
#else
    DIR *d = opendir(path);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n > 5 && !strcmp(e->d_name + n - 5, ".ccap")) {
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            bench_file(p);
        }
    }
    closedir(d);
#endif
}

int main(int argc, char **argv) {
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), 4);   // one core: timings on a moving thread swing twofold
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    int npaths = 0;
    const char *paths[256];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--kind") && i + 1 < argc) {
            const char *k = argv[++i];
            for (int j = 1; j < CC_KINDS; j++)
                if (!strcmp(k, kind_name[j])) only = j;
        } else if (npaths < 256) paths[npaths++] = argv[i];
    }
    if (!npaths || reps < 1) {
        fprintf(stderr, "usage: callbench <dir or .ccap>... [--reps N] [--kind NAME] [--csv FILE]\n");
        return 2;
    }
    if (csv) fprintf(csv, "frame,kind,tid,captured_ns,run_ns,same\n");
    for (int i = 0; i < npaths; i++) bench_path(paths[i]);
    printf("%llu frames, mean interval %.2f ms; best of %d runs per call\n", (unsigned long long)frames,
           frames ? frame_ns / 1e6 / frames : 0.0, reps);
    printf("%-10s %8s %14s %14s %12s %8s %8s\n", "kind", "calls", "captured ms/f", "here ms/f", "MiB/s here", "differ",
           "skipped");
    int bad = 0;
    for (int k = 1; k < CC_KINDS; k++) {
        Stat *s = &S[k];
        if (!s->calls && !s->skipped) continue;
        double f = frames ? (double)frames : 1;
        printf("%-10s %8llu %14.3f %14.3f %12.0f %8llu %8llu\n", kind_name[k], (unsigned long long)s->calls,
               s->cap_ns / 1e6 / f, s->run_ns / 1e6 / f, s->run_ns ? s->bytes / 1048576.0 / (s->run_ns / 1e9) : 0.0,
               (unsigned long long)s->differ, (unsigned long long)s->skipped);
        bad |= s->differ != 0;
    }
    if (csv) fclose(csv);
    return bad;
}
