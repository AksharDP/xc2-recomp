// Game resolution: the size the game composes its frame in, above or beside the shipped 1920x1080 (2560x1440,
// 3840x2160, 21:9 3440x1440). The game does the rendering itself, as with the community resolution mods for 1.0.1 by
// Fl4sh_ (github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods), which this follows.
//
// The game reads its frame size from a table in its read-only data, main+0x17fe914 in 2.0.0 (1.0.0 +0x180d840):
// {docked width, height, handheld width, height}, shipped {1920, 1080, 1280, 720}. Its users (main+0x971a30,
// +0x9ca190, +0xa478d8) take the frame size and the aspect ratio from it, so a wider table widens the 3D view as well.
// The mods rewrite the table; res_init writes it once the modules are loaded, before the game runs: the docked size is
// XCX_OUTPUT_RES = <w>x<h> (default 1920x1080), the handheld size half of it.
//
// One more site reads the table: main+0x9ccce0 sizes two buffers from a width it loads (main+0x9cccec) and the table's
// docked height (main+0x9ccd00). The mods replace both loads with sizes of their own that differ per mod (1280x720 at
// 3840x2160, 1920x1080 at 2560x1440, none at 5160x2160). The patch set "resbuf" (recomp/patches-upd.txt) loads them
// from the patch page's +0x50 and +0x54 instead: XCX_RES_BUFFER = <w>x<h> switches it on, unset keeps the game's.
//
// XCX_RENDER_SCALE sets the share of the game resolution the 3D scene draws in. The game picks that share itself by
// dynamic resolution (at 100 % in every run so far, even at 19 fps), only inside its full-size render targets, so a scale
// above 100 would draw outside them: the patch sets "scene" and "scenefull" (recomp/patches-upd.txt) replace the game's
// share with a fixed smaller or a whole one, and a higher value counts as 100. "game" keeps the dynamic one. Supersampling
// is a game resolution above the window's.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define BASE_W 1920
#define BASE_H 1080
#define TABLE (MOD_MAIN_BASE + 0x17fe914)

static struct {
    int decided;
    unsigned ow, oh;   // output (docked)
    int on;            // res_init wrote the table
    int scale;         // XCX_RENDER_SCALE in percent, 0 for the game's own
} R;

static int parse(const char *s, unsigned *w, unsigned *h) {
    char tail;
    return s && sscanf(s, "%ux%u%c", w, h, &tail) == 2 && *w >= 640 && *h >= 360 && *w <= 7680 && *h <= 4320;
}

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    R.ow = BASE_W;
    R.oh = BASE_H;
    const char *o = cfg_value("XCX_OUTPUT_RES");
    unsigned w, h;
    if (parse(o, &w, &h)) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: XCX_OUTPUT_RES=%s is not <width>x<height> in 640x360..7680x4320; 1920x1080", o);
    }
    const char *s = cfg_value("XCX_RENDER_SCALE");
    int pct = s ? atoi(s) : 0;
    if (pct >= 25) {
        R.scale = pct > 100 ? 100 : pct;
    } else if (s && strcmp(s, "game")) {
        rt_log("resolution: XCX_RENDER_SCALE=%s is not game or a percentage from 25; the game's own", s);
    }
}

// Switches the game's scene scale for XCX_RENDER_SCALE; unset keeps the game's.
static void scene_scale(void) {
    int part = rt_patch_set("scene"), full = rt_patch_set("scenefull");
    uint8_t *page = rt_patch_page();
    if (part < 0 || full < 0 || !page) {
        if (R.scale) rt_log("resolution: this build has no 3D render scale; the game's own");
        R.scale = 0;
    } else {
        float f = R.scale / 100.0f;
        memcpy(page + 0x58, &f, sizeof f);
        if (R.scale) rt_log("resolution: 3D render scale %d %%", R.scale);
    }
    rt_patch_switch(part, R.scale && R.scale < 100);
    rt_patch_switch(full, R.scale == 100);
}

// TotK Recomp grows the texture heap through its HeapDef table; XCX has no such table, and the 1.0.1 mods run at
// 3840x2160 without a bigger heap.
uint64_t res_heap_extra_kib(void) { return 0; }
uint64_t res_heap_extra_bytes(void) { return 0; }
int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) {
    (void)in, (void)n, (void)out_n;
    *out = NULL;
    return 0;
}
void res_heapdef_served(void) {}

void res_init(void) {
    decide();
    scene_scale();
    uint32_t table[4];
    if (!plat_mem_read(TABLE, table, sizeof table) || table[0] != BASE_W || table[1] != BASE_H || table[2] != 1280 ||
        table[3] != 720) {
        rt_log("resolution: no {1920, 1080, 1280, 720} at main+0x17fe914; the game keeps its own resolution");
        return;
    }
    unsigned bw, bh;
    int resbuf = rt_patch_set("resbuf");
    uint8_t *page = rt_patch_page();
    // patches.c switched the set on for any value but 0; only a size keeps it on.
    if (resbuf >= 0 && page && parse(cfg_get("XCX_RES_BUFFER"), &bw, &bh)) {
        uint32_t v[2] = {bw, bh};
        memcpy(page + 0x50, v, sizeof v);
        rt_patch_switch(resbuf, 1);
        rt_log("resolution: the buffers of main+0x9ccce0 at %ux%u", bw, bh);
    } else if (resbuf >= 0) {
        rt_patch_switch(resbuf, 0);
    }
    if (R.ow == BASE_W && R.oh == BASE_H) {
        rt_log("resolution: the game's own (1920x1080)");
        return;
    }
    uint32_t want[4] = {R.ow, R.oh, (R.ow / 2) & ~1u, (R.oh / 2) & ~1u};
    uint64_t page_at = TABLE & ~0xfffull;
    if (!plat_mem_protect(page_at, 0x1000, PLAT_PROT_RW)) {
        rt_log("resolution: main+0x17fe914 cannot be made writable; the game keeps its own resolution");
        return;
    }
    memcpy((void *)(uintptr_t)TABLE, want, sizeof want);
    plat_mem_protect(page_at, 0x1000, PLAT_PROT_READ);
    R.on = 1;
    rt_log("resolution: %ux%u (handheld %ux%u)", want[0], want[1], want[2], want[3]);
}

// One side of the scene at this scale, as main+0x9dc9f4 sizes it: up to a multiple of 8. The patch page holds 0 until
// the first frame.
static unsigned scene_side(unsigned side, float scale) {
    if (!(scale > 0) || scale >= 1) return side;
    unsigned s = ((unsigned)((float)side * scale) + 7) & ~7u;
    return s < side ? s : side;
}

void res_sizes(unsigned out[2], unsigned scene[2]) {
    decide();
    out[0] = R.on ? R.ow : BASE_W, out[1] = R.on ? R.oh : BASE_H;
    uint8_t *page = rt_patch_page();
    uint32_t used[2] = {0, 0};   // the patch set sceneread
    if (page) {
        used[0] = __atomic_load_n((uint32_t *)(page + 0x60), __ATOMIC_RELAXED);
        used[1] = __atomic_load_n((uint32_t *)(page + 0x64), __ATOMIC_RELAXED);
    }
    float scale[2];
    memcpy(scale, used, sizeof scale);
    scene[0] = scene_side(out[0], scale[0]), scene[1] = scene_side(out[1], scale[1]);
}
