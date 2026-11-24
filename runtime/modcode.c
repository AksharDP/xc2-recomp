// The code of the player's mods (modcode.h). The lifted game cannot be patched while it runs, and lifting and linking it
// again for a mod takes minutes. So the functions a mod changes go into a library of their own (modcode_abi.h), lifted
// with the patches in, which takes a second or three; the hooks at the start of every lifted function (fnhook.c) send
// their calls there. A patch is still a switch inside that code (rt_patch_on), so cheats switch while the game runs.
//
// build-cache next to xc2.exe holds what is kept between starts (tools/mod_code.py writes it):
//
//   modcode.txt         the command of tools/mod_code.py, one argument per line; it runs in the folder of xc2.exe
//                       (installer/steps.py write_mod_command; tools/build_game.py for a source build)
//   mod-base/           the patch table the exe was lifted with, and the bytes its code folded
//   cheats.status, exefs.status, exefs.data    what the last run made of each file (cheats.c, mods.c)
//   mods/current.txt    the key of the build it was made for, and the library's file name or "-"
//
// The tool runs when an enabled mod has a patch file or cheat without a status line for its present content, or when
// what is there was made for another build. It handles every mod, so switching one on or off needs no run, nor does a
// start with nothing new. XC2_MODS_BUILD=off never runs it; =always runs it at each start (a developer who edits the
// patch table). When it fails, the game starts without the code it could not make: the cheats stay off and a mod with
// unbuilt ExeFS patches stays off as a whole (mods.c); the next start tries again.
//
// The tool runs before the game has a window, so its progress is a bar in the console the game was started in, or that
// Windows opened for it; its other lines go to the log as they come. A log file gets the lines only.
#include "modcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cheats.h"
#include "dispatch.h"
#include "fnhook.h"
#include "host.h"
#include "modcode_abi.h"
#include "mods.h"
#include "platform/platform.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

enum { TOOL_TIMEOUT_MS = 300000 };   // a first run over many mods; a usual one takes 1-3 s

static char g_dir[PLAT_PATH_MAX];   // the folder of xc2.exe, with a trailing separator
static const ModLib *g_lib;
static uint8_t *g_used;             // per set of the library: an enabled mod has it
static char g_error[256];
static int g_discarded;

static int find_dir(void) {
    if (g_dir[0]) return 1;
    if (!plat_exe_path(g_dir, sizeof g_dir)) return 0;
    char *cut = plat_last_sep(g_dir);
    if (cut) cut[1] = 0;
    else g_dir[0] = 0;
    return g_dir[0] != 0;
}

static char *cache_text(const char *name) {
    char path[PLAT_PATH_MAX + 64];
    uint64_t n;
    snprintf(path, sizeof path, "%sbuild-cache" SEP "%s", g_dir, name);
    return (char *)plat_file_read(path, &n);
}

// The lines of text, cut in place, without their line ends and without empty ones.
static int lines_of(char *text, const char **out, int max) {
    int n = 0;
    for (char *line = text; line && *line && n < max;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        if (*line) out[n++] = line;
        line = next;
    }
    return n;
}

// 1 when what build-cache holds was made for this build: path gets the library's, "" when there is none.
static int current(char *path, size_t cap) {
    char *text = cache_text("mods" SEP "current.txt");
    const char *l[2];
    int ok = text && lines_of(text, l, 2) == 2 && !strcmp(l[0], rt_mod_abi);
    if (ok) {
        path[0] = 0;
        if (strcmp(l[1], "-"))
            ok = !strpbrk(l[1], "/\\") && snprintf(path, cap, "%sbuild-cache" SEP "mods" SEP "%s", g_dir, l[1]) < (int)cap &&
                 plat_file_exists(path);
    }
    free(text);
    return ok;
}

int modcode_current(void) {
    char path[PLAT_PATH_MAX + 128];
    return !g_discarded && find_dir() && rt_mod_abi[0] && current(path, sizeof path);
}

void modcode_discard(const char *why) {
    snprintf(g_error, sizeof g_error, "%s", why);
    g_discarded = 1;
    char path[PLAT_PATH_MAX + 128];
    if (!find_dir()) return;
    if (current(path, sizeof path) && path[0]) plat_file_delete(path);   // else the tool takes the file as made
    snprintf(path, sizeof path, "%sbuild-cache" SEP "mods" SEP "current.txt", g_dir);
    plat_file_delete(path);
}

enum { BAR_WIDTH = 76 };   // under the 80 columns of the narrowest usual console: a wrapped line cannot be drawn over

typedef struct {
    char line[1024], last[sizeof g_error];   // the line so far; the last whole one
    size_t n;
    int bar;   // a bar is on the console's current line
} ToolOut;

static void bar_clear(ToolOut *o) {
    char b[BAR_WIDTH + 3];
    if (!o->bar) return;
    int n = snprintf(b, sizeof b, "\r%*s\r", BAR_WIDTH, "");
    fwrite(b, 1, (size_t)n, stderr);
    o->bar = 0;
}

static void bar_draw(ToolOut *o, int percent, const char *text) {
    enum { CELLS = 24 };
    if (!plat_stderr_console()) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    char cells[CELLS + 1], s[BAR_WIDTH + 1], b[BAR_WIDTH + 2];
    for (int i = 0; i < CELLS; i++) cells[i] = i < percent * CELLS / 100 ? '#' : '-';
    cells[CELLS] = 0;
    snprintf(s, sizeof s, "Preparing mods [%s] %3d %%  %s", cells, percent, text);
    int n = snprintf(b, sizeof b, "\r%-*s", BAR_WIDTH, s);
    fwrite(b, 1, (size_t)n, stderr);
    o->bar = 1;
}

static void tool_line(ToolOut *o) {
    o->line[o->n] = 0;
    o->n = 0;
    if (!o->line[0]) return;
    int percent, at = 0;
    if (sscanf(o->line, "progress %d %n", &percent, &at) == 1 && at) {
        bar_draw(o, percent, o->line + at);
        return;
    }
    bar_clear(o);
    rt_log("  %s", o->line);
    snprintf(o->last, sizeof o->last, "%s", o->line);
}

static void tool_output(void *ctx, const char *p, size_t n) {
    ToolOut *o = (ToolOut *)ctx;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n') tool_line(o);
        else if (p[i] != '\r' && o->n + 1 < sizeof o->line) o->line[o->n++] = p[i];
    }
}

// Runs the command in modcode.txt; 1 when it worked.
static int run_tool(void) {
    char *cmd = cache_text("modcode.txt"), program[PLAT_PATH_MAX + 64];
    static char exefs[PLAT_PATH_MAX * 3], mods[PLAT_PATH_MAX * 3];   // the tool runs in the folder of xc2.exe
    const char *argv[64];
    int argc = cmd ? lines_of(cmd, argv, 56) : 0;
    if (!argc) {
        snprintf(g_error, sizeof g_error, "no build-cache/modcode.txt");
        free(cmd);
        return 0;
    }
    argv[argc++] = "--exefs";
    argv[argc++] = plat_full_path(rt_config()->exefs, exefs, sizeof exefs) ? exefs : rt_config()->exefs;
    argv[argc++] = "--mods";
    argv[argc++] = plat_full_path(mods_dir(), mods, sizeof mods) ? mods : mods_dir();
    argv[argc++] = "--abi";
    argv[argc++] = rt_mod_abi;
    argv[argc++] = "--progress";
    argv[argc] = NULL;
    // the platform starts a relative program from this process's working folder, which a shortcut may set
    if (!plat_path_absolute(argv[0]) && snprintf(program, sizeof program, "%s%s", g_dir, argv[0]) < (int)sizeof program)
        argv[0] = program;
    ToolOut o = {0};
    bar_draw(&o, 0, "starting");
    uint64_t t0 = plat_now_ms();
    int code = plat_run(argv, g_dir, tool_output, &o, TOOL_TIMEOUT_MS);
    tool_line(&o);   // a last line without its end
    bar_clear(&o);
    rt_log("modcode: %s: exit code %d, %llu ms%s", argv[0], code, (unsigned long long)(plat_now_ms() - t0),
           code ? "; the game starts without the code it could not make" : "");
    if (!code) g_error[0] = 0;
    else if (code == -1 || o.last[0])
        snprintf(g_error, sizeof g_error, "%s", code == -1 ? "tools/mod_code.py did not start" : o.last);
    else snprintf(g_error, sizeof g_error, "tools/mod_code.py: exit code %d", code);
    free(cmd);
    return code == 0;
}

void modcode_prepare(void) {
    const char *e = cfg_get("XC2_MODS_BUILD");
    if (!find_dir() || modcode_off()) return;
    if (!rt_mod_abi[0]) return;   // lifted before gen.py wrote patches.txt: no mod code
    mods_scan();
    char path[PLAT_PATH_MAX + 128];
    int files = mods_exefs_files(), cheats = cheats_files(), pending = mods_exefs_pending() + cheats_pending();
    int always = e && !strcmp(e, "always"), ok = current(path, sizeof path);
    if (!always && !pending && (ok || !(files + cheats))) return;
    rt_log("modcode: %d patch files and cheats of mods are new or changed%s", pending,
           ok ? "" : "; the mods' code was made for another build");
    int sized = mods_status_replaces("xc2size");
    // a run that failed before it replaced anything changed nothing; one that failed while it did left nothing usable
    if (!run_tool() && modcode_current()) return;
    mods_probe_cache(NULL, NULL);   // the status files are new
    mods_rescan();
    mods_scan();
    // memory.c sized the application pool before this, by the old status file
    if (sized != mods_status_replaces("xc2size")) {
        if (getenv("XC2_RESTARTED")) return;
        rt_log("modcode: a mod changes the size of the game's memory: starting again");
        if (plat_restart("XC2_RESTARTED", "1")) plat_exit(0);
    }
}

void modcode_load(void) {
    char path[PLAT_PATH_MAX + 128];
    if (!find_dir() || !rt_mod_abi[0] || !current(path, sizeof path) || !path[0]) return;
    void *h = plat_lib_open(path);
    ModEntry entry = (ModEntry)plat_lib_sym(h, MOD_ENTRY);
    const ModLib *lib = entry ? entry() : NULL;
    if (!lib || lib->version != MOD_ABI_VERSION || strcmp(lib->key, rt_mod_abi) ||
        !rt_patches_add(lib->first_set, lib->sets, lib->nsets)) {
        plat_lib_close(h);   // a loaded DLL cannot be deleted
        modcode_discard(!lib ? "the library did not load" : "the library is of another build");
        rt_log("modcode: %s: %s", path, g_error);
        mods_rescan();   // the mods whose code is in it are not usable now
        mods_scan();
        return;
    }
    g_used = (uint8_t *)calloc((size_t)lib->nsets + 1, 1);
    if (!g_used) rt_fatal("modcode: out of memory");
    g_lib = lib;
}

void modcode_use(int set) {
    if (g_lib && set >= g_lib->first_set && set < g_lib->first_set + g_lib->nsets) g_used[set - g_lib->first_set] = 1;
}

// The lifted function at guest address addr: the library's own, else the exe's (as a direct call in the exe reaches it).
static GuestFn target(uint64_t addr) {
    for (int p = 0; p < g_lib->nparts; p++) {
        const ModPart *part = g_lib->parts[p];
        int lo = 0, hi = part->nfns;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (part->fns[mid].addr < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < part->nfns && part->fns[lo].addr == addr) return part->fns[lo].fn;
    }
    RtModule mods[RT_MODULES_MAX];
    int n = rt_linked_modules(mods, RT_MODULES_MAX);
    for (int i = 0; i < n; i++) {
        uint64_t lo = 0, hi = mods[i].count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (mods[i].addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < mods[i].count && mods[i].addrs[lo] == addr) return mods[i].fns[lo];
    }
    return NULL;
}

void modcode_hook(void) {
    if (!g_lib) return;
    for (int i = 0; i < g_lib->nsets; i++)
        if (!g_used[i]) rt_patch_disable(g_lib->first_set + i);
    *g_lib->rt = (ModRt){rt_dispatch, rt_spin_gate, rt_svc, rt_brk, rt_udf, rt_ticks, rt_patch_on};
    int fns = 0, hooked = 0;
    for (int p = 0; p < g_lib->nparts; p++) {
        const ModPart *part = g_lib->parts[p];
        for (int i = 0; i < part->nimports; i++) {
            part->imports[i] = target(part->import_addrs[i]);
            if (!part->imports[i]) rt_fatal("modcode: no function at %#llx", (unsigned long long)part->import_addrs[i]);
        }
        for (int i = 0; i < part->nfns; i++, fns++) {
            const ModFn *f = &part->fns[i];
            int use = !f->nsets;
            for (int k = 0; k < f->nsets; k++)
                use |= f->sets[k] < g_lib->first_set || g_used[f->sets[k] - g_lib->first_set];
            if (!use) continue;
            fnhook_replace(f->addr, f->fn);
            hooked++;
        }
    }
    rt_log("modcode: %d functions and %d patch sets of mods loaded, %d functions in use", fns, g_lib->nsets, hooked);
}

int modcode_unbuilt(void) { return mods_exefs_pending() + cheats_unbuilt(); }

const char *modcode_error(void) {
    return g_error[0] ? g_error : rt_mod_abi[0] ? "new since the game started" : "a build without mod code";
}

int modcode_off(void) {
    const char *e = cfg_get("XC2_MODS_BUILD");
    return e && !strcmp(e, "off");
}
