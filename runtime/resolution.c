// Game resolution: the size the game composes its frame in, above or beside the shipped 1920x1080 (2560x1440,
// 3840x2160, 21:9 3440x1440). The game does the rendering itself, as with the community resolution mods for 1.0.1 by
// Fl4sh_ (github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods), which this follows.
//
// The game reads its frame size from a table in its read-only data, main+0x17fe914 in 2.0.0 (1.0.0 +0x180d840):
// {docked width, height, handheld width, height}, shipped {1920, 1080, 1280, 720}. Its users (main+0x971a30,
// +0x9ca190, +0xa478d8) take the frame size and the aspect ratio from it, so a wider table widens the 3D view as well.
// The mods rewrite the table; res_init writes it once the modules are loaded, before the game runs: the docked size is
// XC2_OUTPUT_RES = <w>x<h> (default 1920x1080), the handheld size half of it.
//
// One more site reads the table: main+0x9ccce0 sizes two buffers from a width it loads (main+0x9cccec) and the table's
// docked height (main+0x9ccd00). The mods replace both loads with sizes of their own that differ per mod (1280x720 at
// 3840x2160, 1920x1080 at 2560x1440, none at 5160x2160). The patch set "resbuf" (recomp/patches-upd.txt) loads them
// from the patch page's +0x50 and +0x54 instead: XC2_RES_BUFFER = <w>x<h> switches it on, unset keeps the game's.
//
// XC2_RENDER_SCALE sets the share of the game resolution the 3D scene draws in. The game picks that share itself by
// dynamic resolution (at 100 % in every run so far, even at 19 fps), only inside its full-size render targets, so a scale
// above 100 would draw outside them: the patch sets "scene" and "scenefull" (recomp/patches-upd.txt) replace the game's
// share with a fixed smaller or a whole one, and a higher value counts as 100. "game" keeps the dynamic one. Supersampling
// is a game resolution above the window's.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cpu.h"
#include "dispatch.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"

#define BASE_W 1920
#define BASE_H 1080
#define XC2_W 1280
#define XC2_H 720
#define TABLE (RT_MAIN_BASE + 0x17fe914)

static struct {
    int decided;
    unsigned ow, oh;   // output (docked)
    int on;            // res_init wrote the table
    int scale;         // XC2_RENDER_SCALE in percent, 0 for the game's own
    int xc2;           // XC2's patch sets (xc2_res_init): ow x oh is the size it composes, on or not
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
    const char *o = cfg_value("XC2_OUTPUT_RES");
    unsigned w, h;
    if (parse(o, &w, &h)) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: XC2_OUTPUT_RES=%s is not <width>x<height> in 640x360..7680x4320; 1920x1080", o);
    }
    const char *s = cfg_value("XC2_RENDER_SCALE");
    int pct = s ? atoi(s) : 0;
    if (pct >= 25) {
        R.scale = pct > 100 ? 100 : pct;
    } else if (s && strcmp(s, "game")) {
        rt_log("resolution: XC2_RENDER_SCALE=%s is not game or a percentage from 25; the game's own", s);
    }
}

// Switches the game's scene scale for XC2_RENDER_SCALE; unset keeps the game's.
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

// XC2 (recomp/patches-v301.txt, sets xc2size, xc2mem and xc2res): the game composes 1280x720 in both modes. Its graphics
// device's size getters and the window texture setup read the patch page's +0x70 (width) and +0x74 (height) instead
// (xc2size); xc2res fits the interface and the markers to that frame. The render
// targets come from the graphics heap GLIBPXL (+0x78, 362 MiB at 1280x720), inside the game's 512 MiB graphics block
// (+0x7c), inside the application pool (memory.c): all three grow by the same amount. 2560x1440 runs through the first
// battle and Goldmouth with 650 MiB more and aborts at boot with 578 (its first 1 MiB from GLIBPXL, main+0x701388), so
// 232 MiB per 1280x720 of extra area, in 2 MiB steps. The game sums its memory layout (main+0x6421b4, 0xbbc00000
// bytes) in 32 bits, so the growth stops at 1080 MiB: a larger size is scaled down to the largest that fits, at the
// same shape (about 2920x1642 at 16:9).
#define XC2_PIXEL_HEAP 0x16a00000ull
#define XC2_GFX_BLOCK 0x20000000ull
#define XC2_MIB_PER_AREA 232.0
#define XC2_EXTRA_MAX_MIB 1080.0

static struct {
    int decided;       // 1: an XC2 build (the set xc2res exists), -1: not
    unsigned w, h;     // XC2_OUTPUT_RES, or the size a mod's patch gives (xc2_res_init)
    uint64_t extra;    // bytes added to each of the three
    uint64_t mod_extra;   // the pool's growth when a mod's ExeFS patch may replace xc2size: the largest
} X;

// The growth of the graphics heaps for a frame of area times 1280x720, in 2 MiB steps; 0 at or below 1280x720.
static uint64_t extra_for(double area) {
    double mib = (area - 1) * XC2_MIB_PER_AREA;
    return mib > 0 ? (((uint64_t)mib + 2) & ~1ull) << 20 : 0;
}

static int xc2_decide(void) {
    if (X.decided) return X.decided > 0;
    X.decided = rt_patch_set("xc2res") >= 0 ? 1 : -1;
    if (X.decided < 0) return 0;
    const char *o = cfg_value("XC2_OUTPUT_RES");
    unsigned w = XC2_W, h = XC2_H;
    if (!parse(o, &w, &h)) rt_log("resolution: XC2_OUTPUT_RES=%s is not <width>x<height> in 640x360..7680x4320; %ux%u", o, XC2_W, XC2_H);
    double area = (double)w * h / (XC2_W * XC2_H), most = 1 + XC2_EXTRA_MAX_MIB / XC2_MIB_PER_AREA;
    if (area > most) {
        double k = sqrt(most / area);
        unsigned fw = (unsigned)(w * k), fh = (unsigned)(h * k);
        rt_log("resolution: %ux%u needs more graphics memory than XC2 can address; %ux%u", w, h, fw & ~1u, fh & ~1u);
        w = fw, h = fh, area = (double)(w & ~1u) * (h & ~1u) / (XC2_W * XC2_H);
    }
    X.w = w & ~1u, X.h = h & ~1u;
    X.extra = extra_for(area);
    // A mod's ExeFS patch that replaces xc2size (mods.c) sets a frame size of its own, as the 21:9 mods do (2580x1080);
    // on a Switch emulator they get by with the larger memory layout the emulator offers. xc2_res_init reads that size
    // once the sets are switched, but the application pool is sized now, before the mods are scanned and the patches
    // switched: any built patch file that replaces xc2size makes room for the most XC2 can address.
    if (mods_status_replaces("xc2size")) X.mod_extra = ((uint64_t)XC2_EXTRA_MAX_MIB & ~1ull) << 20;
    return 1;
}

// TotK Recomp grows the texture heap through its HeapDef table; XCX has no such table, and the 1.0.1 mods run at
// 3840x2160 without a bigger heap. XC2 grows its own heaps (xc2_decide).
uint64_t res_heap_extra_kib(void) { return 0; }
uint64_t res_heap_extra_bytes(void) { return xc2_decide() ? (X.extra > X.mod_extra ? X.extra : X.mod_extra) : 0; }
int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) {
    (void)in, (void)n, (void)out_n;
    *out = NULL;
    return 0;
}
void res_heapdef_served(void) {}

// A game resolution wider than 16:9 (patches-v301.txt, set xc2wide): the layout library fits the 1280x720 design by
// height and holds its left edge on the screen's, so the interface filled a 16:9 region at the left. +0x8c is the left
// edge in clip space for the design centred instead, -1280 * fit / width (-1 at 16:9); the values after it place the
// markers, stretch full-screen layers, keep captured panes (the minimap) in place and move the field HUD to the frame's
// edges (patches-v301.txt). XC2_HUD_CENTER is on by default. At 16:9 and narrower shapes the set stays off: the fit is
// by width there and the left edge is already the screen's.
static uint8_t *hud_page;   // set while xc2wide is on

static void wide_hud(uint8_t *page) {
    int set = rt_patch_set("xc2wide");
    if (set < 0) return;
    int wide = (uint64_t)X.w * XC2_H > (uint64_t)X.h * XC2_W;   // X: the port's size or a mod's (xc2_res_init)
    float left = wide ? (float)(-(double)XC2_W * X.h / XC2_H / X.w) : -1.0f;
    memcpy(page + 0x8c, &left, sizeof left);
    // +0x90: how far the centred interface moved right, in interface units, (viewport width - 1280) / 2; the
    // world-to-screen conversions of the markers subtract it so they stay on their objects
    float shift = wide ? (float)(((double)XC2_H * X.w / X.h - XC2_W) / 2) : 0.0f;
    memcpy(page + 0x90, &shift, sizeof shift);
    // +0x98, +0x9c: pictures at least as wide as the design (fades, loading and menu backgrounds) are stretched to the
    // frame: the width in clip units from which a picture counts (98 % of the design's, 2 * -left), and the stretch
    // minus one, 1 / -left - 1, which their x row is scaled by about the frame's centre
    float fill[2] = {wide ? (float)(-2.0 * 0.98 * left) : 4.0f, wide ? (float)(-1.0 / left - 1.0) : 0.0f};
    memcpy(page + 0x98, fill, sizeof fill);
    // +0xa0: the same move in the units of the panes' screen bounds (main+0x82aa7c, half the clip-space distance from
    // the left edge): (1 + left) / 2, for the capture region of the minimap and other captured panes
    float bounds = wide ? (float)((1.0 + left) / 2) : 0.0f;
    memcpy(page + 0xa0, &bounds, sizeof bounds);
    // +0xa4..+0xd8: the field HUD's move to the frame's edges (patches-v301.txt, the group sites). A group moves as a
    // whole: the topmost layout container other than a layout's root decides for everything under it, from its
    // position, which is its origin rather than its middle; a pane outside such a container decides alone, from its
    // centre, and only when it is narrower than 98 % of the design (+0xc8, tested on the matrix's a). A pane's global
    // matrix (tx, ty, a, ...) is in units of the frame's width in pixels (+0xd4) per interface unit, a counting from the
    // rectangle's 0..1, so a pane's centre lies (tx + a / 2) * width interface units from the design's left edge and a
    // container (a = 1, no size) at tx * width; a container within 4 interface units of the left edge (+0xd8)
    // does not move. The move is shift(u) = sign(u) * min(s_max, max(0, |u| - a) * m) with u measured from 588.25
    // (+0xb8) and a = 202.25: a group starts to move 254 interface units left of 640 and 150.5 right of it, and moves
    // by the full distance from the design's edge to the frame's (s_max) 28 units further out. The thresholds come from
    // the positions measured in the field and in battle: on the right the compass's objective marker, which stops at
    // the compass's end, reaches 148 and stays; on the left the enemy gauge's level (237 and 204 left of 640) and a
    // field group at 252 stay, and the active blade's name (283), the party list and Blade Switch move whole. The
    // group sites work in interface units (+0xb8 the middle, +0xbc the start, +0xa8 the slope, +0xc4 the limit); +0xa4
    // and +0xac are the start and the limit in clip units. +0xb0 is 1 in the field and 0 in menus (res_hud_frame),
    // +0xb4 counts the frame's full-width pictures. +0xc0 (m / (1 + m), the shift's inverse slope) is not read at
    // present. +0xcc is the current group's shift in clip units, written by the pane draws for their builders, and
    // +0xd0 the clip units per interface unit (k), to convert it.
    float h = -left, s_max = 1.0f - h, k = h / (XC2_W / 2.0f);
    float start = 202.25f, mid = 588.25f, m = s_max / k / 28.0f;
    float anchor[14] = {start * k, m, s_max, 1.0f, 0.0f, mid, start, m / (1.0f + m), s_max / k,
                        0.98f * XC2_W / X.w, 0.0f, k, (float)X.w, 4.0f / X.w};
    if (!wide) memset(anchor, 0, sizeof anchor);
    memcpy(page + 0xa4, anchor, sizeof anchor);
    // +0xdc: the centring in the frame's pixels, (1 + left) / 2 * width, for the viewport and scissor of panes that
    // clip their children (main+0x8399c0, the Skip Travel map's panel; their projection takes +0x90)
    float clip = wide ? (float)((1.0 + left) / 2 * X.w) : 0.0f;
    memcpy(page + 0xdc, &clip, sizeof clip);
    // +0xe0: written by the minimap capture's region site (main+0x8365fc), the capture pane's group shift kept for its
    // draw-back (main+0x8367a0), since the children's draws rewrite +0xcc in between
    memset(page + 0xe0, 0, 8);   // and +0xe4, the thin-picture threshold res_hud_frame sets each frame
    // +0xec: a 12-bit frame counter (res_hud_frame), from which the container sites (main+0x833ca4) tell a pane drawn on the
    // frame before from one that was hidden
    // +0x100: 4096 * 1080 / height, which turns a group's contents rectangle, kept in the frame's pixels, into 1080p
    // pixels for the same sites' test of a centred group
    uint32_t to_1080 = wide ? (4096u * 1080 + X.h / 2) / X.h : 4096;
    memcpy(page + 0x100, &to_1080, sizeof to_1080);
    int on = wide && rt_patch_enabled(set);   // rt_patches_init read XC2_HUD_CENTER
    rt_patch_switch(set, on);
    hud_page = on ? page : NULL;
    if (on) rt_log("resolution: interface centred in the %ux%u frame, field HUD at its edges (XC2_HUD_CENTER=1)", X.w, X.h);
    else if (wide) rt_log("resolution: interface at the left of the %ux%u frame, as the game places it", X.w, X.h);
}

void res_hud_frame(void) {
    if (!hud_page) return;
    // +0xb4 sums the stretch minus one (+0x9c) of every full-width picture drawn, so it counts them. The field draws 1
    // to 4 (fades and overlays), a menu 16 to 32 (its background, header and bars; the Skip Travel menu, measured):
    // above 8 the interface stays a whole 16:9 block
    float seen, unit, field;
    memcpy(&seen, hud_page + 0xb4, sizeof seen);
    memcpy(&unit, hud_page + 0x9c, sizeof unit);
    field = seen > 8.0f * unit ? 0.0f : 1.0f;
    // The title screen draws few full-width pictures but is laid out as a menu: its list, cursor glow and dialogs are
    // centred under containers that start left of the middle, which the edge shift moved toward the frame's left edge, and
    // its dialog bands are as thin as the title card. The title's object (title_main.cpp, made once at boot; the
    // pointer at main+0xc232e8, set by its constructor main+0x14e74c) holds a pointer at +0x1a0 from the time its
    // screen loads until a game starts (New Game or a loaded save, measured), so while it is set the frame is a menu.
    uint64_t title = 0, screen = 0;
    if (plat_mem_read(RT_MAIN_BASE + 0xc232e8, &title, sizeof title) && title &&
        plat_mem_read(title + 0x1a0, &screen, sizeof screen) && screen)
        field = 0.0f;
    memcpy(hud_page + 0xb0, &field, sizeof field);
    memset(hud_page + 0xb4, 0, sizeof seen);
    // +0xe4: the least height (the picture matrix's y scale, in units of 1 / frame width) of a full-width picture that
    // is stretched across the frame. In menus every one is (their header bars are 0.06 at 3840 wide); in the field and
    // in cutscenes only the full-height layers (fades, 0.33 and up at 3840 wide) are, so a thin picture as wide as the
    // design, such as the opening's "Xenoblade Chronicles 2" title card (0.067), keeps its shape
    float thin = field ? 384.0f / X.w : 0.0f;
    memcpy(hud_page + 0xe4, &thin, sizeof thin);
    static uint16_t count;
    count = (count + 1) & 0xfff;
    memcpy(hud_page + 0xec, &count, sizeof count);
}

// A mod's ExeFS patch that holds xc2size off writes the graphics device's size getters (main+0x6e3fc8 width,
// main+0x6e3fd0 height) with a size of its own; the 21:9 mods load constants there. The getters are leaves that only
// set w0, so calling their lifted code with the mod's set switched on gives that size before the game runs; 0 when a
// value is out of 640x360..7680x4320.
// The lifted function at guest address a, from the linked modules' function tables: res_init runs before the first
// guest call builds the dispatch table (hostfn_seal), so rt_dispatch_lookup has nothing yet.
static GuestFn lifted_at(uint64_t a) {
    RtModule mods[RT_MODULES_MAX];
    int n = rt_linked_modules(mods, RT_MODULES_MAX);
    for (int i = 0; i < n; i++) {
        if (a < mods[i].base || a - mods[i].base >= mods[i].size) continue;
        uint64_t lo = 0, hi = mods[i].count;
        while (lo < hi) {
            uint64_t m = (lo + hi) / 2;
            if (mods[i].addrs[m] < a) lo = m + 1;
            else hi = m;
        }
        return lo < mods[i].count && mods[i].addrs[lo] == a ? mods[i].fns[lo] : NULL;
    }
    return NULL;
}

static int mod_frame_size(unsigned *w, unsigned *h) {
    unsigned v[2];
    for (int i = 0; i < 2; i++) {
        GuestFn f = lifted_at(RT_MAIN_BASE + (i ? 0x6e3fd0 : 0x6e3fc8));
        if (!f) return 0;
        Cpu c;
        memset(&c, 0, sizeof c);
        c.x[30] = RT_RET_HOST;
        f(&c);
        v[i] = (uint32_t)c.x[0];
    }
    if (v[0] < 640 || v[1] < 360 || v[0] > 7680 || v[1] > 4320) return 0;
    *w = v[0], *h = v[1];
    return 1;
}

static int xc2_res_init(void) {
    uint8_t *page = rt_patch_page();
    if (!xc2_decide() || !page) return 0;
    int set = rt_patch_set("xc2res"), size = rt_patch_set("xc2size"), mem = rt_patch_set("xc2mem");
    // mods.c held xc2size: a mod's patch sets the frame size. The interface and marker sets then follow that size.
    int by_mod = rt_patch_held(size);
    uint64_t grow = X.extra;
    if (by_mod) {
        unsigned mw, mh;
        if (mod_frame_size(&mw, &mh)) {
            double area = (double)mw * mh / (XC2_W * XC2_H);
            if (X.w != XC2_W || X.h != XC2_H)
                rt_log("resolution: XC2_OUTPUT_RES %ux%u gives way to the %ux%u a mod's ExeFS patch sets", X.w, X.h, mw, mh);
            X.w = mw, X.h = mh;
            grow = extra_for(area);
            if (grow > X.mod_extra) {
                rt_log("resolution: the mod's %ux%u needs %llu MiB more graphics memory than XC2 can address; the game "
                       "may stop at boot", mw, mh, (unsigned long long)((grow - X.mod_extra) >> 20));
                grow = X.mod_extra;
            }
        } else {
            rt_log("resolution: a mod's ExeFS patch sets the frame size, which the port cannot read; the interface keeps "
                   "the game's 1280x720 placement");
            X.w = XC2_W, X.h = XC2_H, grow = X.mod_extra;
        }
    }
    R.ow = X.w, R.oh = X.h;
    // +0x80: the camera viewport the markers are placed with (patches-v301.txt), 1280x720 at 16:9; the camera takes its
    // aspect ratio from it, so another shape keeps the height and widens or narrows
    uint32_t ui_w = (uint32_t)((double)XC2_H * X.w / X.h + 0.5);
    uint32_t v[6] = {X.w, X.h, (uint32_t)(XC2_PIXEL_HEAP + grow), (uint32_t)(XC2_GFX_BLOCK + grow), ui_w, XC2_H};
    memcpy(page + 0x70, v, sizeof v);
    float pivot = (float)XC2_H / X.h;   // +0x88: rotated interface panes' pivot back in interface units
    memcpy(page + 0x88, &pivot, sizeof pivot);
    wide_hud(page);
    R.xc2 = 1;
    R.on = X.w != XC2_W || X.h != XC2_H;
    rt_patch_switch(set, R.on);
    rt_patch_switch(size, R.on);   // held off while a mod sets the size
    rt_patch_switch(mem, grow != 0);
    if (by_mod)
        rt_log("resolution: %ux%u from a mod's ExeFS patch (it replaces xc2size); graphics heaps %llu MiB larger", X.w,
               X.h, (unsigned long long)(grow >> 20));
    else if (R.on) rt_log("resolution: %ux%u, graphics heaps %llu MiB larger", X.w, X.h, (unsigned long long)(X.extra >> 20));
    else rt_log("resolution: the game's own (%ux%u)", X.w, X.h);
    // XC2_RENDER_SCALE: the set xc2scale fixes lib_nx.ini's scene shares (dynamic 70 to 100 %) at the page's +0x58
    int scale = rt_patch_set("xc2scale");
    if (scale < 0) {
        if (R.scale) rt_log("resolution: this build has no 3D render scale; the game's own");
        R.scale = 0;
    } else {
        float f = R.scale / 100.0f;
        memcpy(page + 0x58, &f, sizeof f);
        rt_log(R.scale ? "resolution: 3D render scale %d %%" : "resolution: 3D render scale the game's (dynamic, 70 to 100 %%)", R.scale);
    }
    rt_patch_switch(scale, R.scale != 0);
    return 1;
}
void res_init(void) {
    decide();
    if (xc2_res_init()) return;
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
    if (resbuf >= 0 && page && parse(cfg_get("XC2_RES_BUFFER"), &bw, &bh)) {
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
    out[0] = R.on || R.xc2 ? R.ow : BASE_W, out[1] = R.on || R.xc2 ? R.oh : BASE_H;
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
