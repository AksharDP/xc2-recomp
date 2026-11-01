// xcx.exe --probe-nvn-tex: texture storage size, block height, alignment and mip offsets of nvn_tex.c.
// Ported from MacNativeGames games/totk/tests/texture_auto_layout/probe.c @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou).
// The 40 size and block-height goldens are original-SDK CPU queries for uncompressed and BC3-sRGB boundary cases; no game bytes.
// The old rounded-up heuristic mismatches 16 of them, including the 160x160 shrine pad. MNG's nvn_bl_offset address checks
// belong to the deswizzler and are not part of this probe.
#include <stdint.h>
#include <stdio.h>
#include "host.h"
#include "nvn_tex.h"

typedef struct { unsigned format, width, height, levels, h; uint64_t size; } LayoutCase;
static const LayoutCase layout_cases[] = {
    {0x25, 40, 1, 6, 0, 4608},
    {0x25, 40, 8, 6, 0, 4608},
    {0x25, 40, 9, 6, 0, 6144},
    {0x25, 40, 10, 6, 0, 6144},
    {0x25, 40, 11, 6, 1, 6144},
    {0x25, 40, 16, 6, 1, 6144},
    {0x25, 40, 17, 6, 1, 9216},
    {0x25, 40, 21, 6, 1, 10240},
    {0x25, 40, 22, 6, 2, 10240},
    {0x25, 40, 32, 6, 2, 10240},
    {0x25, 40, 33, 6, 2, 16384},
    {0x25, 40, 40, 6, 2, 18944},
    {0x25, 40, 42, 6, 2, 18944},
    {0x25, 40, 43, 6, 3, 18944},
    {0x25, 40, 64, 7, 3, 19456},
    {0x25, 40, 65, 7, 3, 31744},
    {0x25, 40, 85, 7, 3, 37376},
    {0x25, 40, 86, 7, 4, 37376},
    {0x25, 40, 128, 8, 4, 37888},
    {0x25, 40, 129, 8, 4, 62464},
    {0x48, 160, 4, 8, 0, 12288},
    {0x48, 160, 32, 8, 0, 12288},
    {0x48, 160, 36, 8, 0, 17408},
    {0x48, 160, 40, 8, 0, 17408},
    {0x48, 160, 44, 8, 1, 17408},
    {0x48, 160, 64, 8, 1, 17408},
    {0x48, 160, 68, 8, 1, 30208},
    {0x48, 160, 84, 8, 1, 30208},
    {0x48, 160, 88, 8, 2, 30208},
    {0x48, 160, 128, 8, 2, 30208},
    {0x48, 160, 132, 8, 2, 57344},
    {0x48, 160, 160, 8, 2, 57344},
    {0x48, 160, 168, 8, 2, 57344},
    {0x48, 160, 172, 8, 3, 57344},
    {0x48, 160, 256, 9, 3, 57856},
    {0x48, 160, 260, 9, 3, 112128},
    {0x48, 160, 340, 9, 3, 113152},
    {0x48, 160, 344, 9, 4, 113152},
    {0x48, 160, 512, 10, 4, 113664},
    {0x48, 160, 516, 10, 4, 222208},
};

// Hand-derived from the layout rules: RGBA8 40x128, 8 levels, block height 16 GOBs shrinking per level.
static const uint64_t mip_offsets_rgba8_40x128[9] = {0, 24576, 32768, 34816, 35840, 36352, 36864, 37376, 37888};

static int fail(const char *what, unsigned i, unsigned format, unsigned w, unsigned h, unsigned long long got, unsigned long long want) {
    printf("probe-nvn-tex: FAIL %s case %u format=%#x %ux%u: got %llu expected %llu\n", what, i, format, w, h, got, want);
    return 1;
}

int probe_nvn_tex(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof layout_cases / sizeof *layout_cases; i++) {
        const LayoutCase *c = &layout_cases[i];
        nvn_texdesc d = {.target = 1, .format = (int)c->format, .samples = 1, .width = c->width, .height = c->height, .depth = 1, .levels = c->levels};
        nvn_texlayout l;
        nvn_tex_layout(&d, &l);
        if (l.tileH != c->h) return fail("block height", i, c->format, c->width, c->height, l.tileH, c->h);
        if (l.size != c->size) return fail("size", i, c->format, c->width, c->height, l.size, c->size);
        if (l.align != 512) return fail("alignment", i, c->format, c->width, c->height, l.align, 512);
        n += 3;
    }

    nvn_texdesc pad = {.target = 1, .format = 0x48, .samples = 1, .width = 160, .height = 160, .depth = 1, .levels = 8};
    unsigned h, dd;
    nvn_tex_level_tiles(&pad, 0, &h, &dd);
    if (h != 2 || dd != 0) return fail("160x160 BC3 base mip block height", 0, 0x48, 160, 160, h, 2);
    pad.pkg_block_h = 4;
    nvn_tex_level_tiles(&pad, 0, &h, &dd);
    if (h != 3) return fail("explicit packaged H3 over automatic H2", 0, 0x48, 160, 160, h, 3);
    n += 2;

    nvn_texdesc m = {.target = 1, .format = 0x25, .samples = 1, .width = 40, .height = 128, .depth = 1, .levels = 8};
    for (unsigned lv = 0; lv <= 8; lv++) {
        uint64_t off = nvn_tex_level_offset(&m, lv);
        if (off != mip_offsets_rgba8_40x128[lv]) return fail("mip offset", lv, 0x25, 40, 128, off, mip_offsets_rgba8_40x128[lv]);
        n++;
    }

    // Pitch storage: LINEAR_RENDER_TARGET (0x40) aligns the stride to 128 bytes, LINEAR (0x10) to 32.
    nvn_texdesc lin = {.target = 1, .format = 0x25, .flags = 0x40, .samples = 1, .width = 40, .height = 4, .depth = 1, .levels = 1};
    nvn_texlayout l;
    nvn_tex_layout(&lin, &l);
    if (l.size != 256ull * 4 || l.align != 128) return fail("linear render target", 0, 0x25, 40, 4, l.size, 1024);
    lin.flags = 0x10;
    nvn_tex_layout(&lin, &l);
    if (l.size != 160ull * 4 || l.align != 32) return fail("linear", 0, 0x25, 40, 4, l.size, 640);
    n += 2;

    printf("probe-nvn-tex: passed (%u cases)\n", n);
    return 0;
}
