// XC2_CAPTURE=<frame> (diagnostic, changes nothing in the replay): measures the guest memory that one frame reads. While
// frame N is replayed, every vk_mem_resolve call notes its range; at the end of the frame the ranges are merged and the
// total is logged. A single-frame capture has to hold these bytes, so the total says how big that capture is.
// The frame is the one built between present N-1 and present N, the same numbering as the dump's XC2_DUMP.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"

volatile int vk_capture_on;   // 1 while the measured frame is replayed; vk_mem_resolve checks it

typedef struct { uint64_t lo, hi; } CapRange;
static CapRange *ranges;
static size_t nranges, capranges;
static uint64_t ncalls, target;
static int target_read;
static PlatMutex cap_lock = PLAT_MUTEX_INIT;

// The textures the measured frame samples: the image's bytes (levels taken as 4/3 of the base level), noted once per
// lookup and counted once per image at the report. GPU-written images are render targets or copies, not guest data.
typedef struct { const void *img; uint64_t bytes; int gpu; } CapTex;
static CapTex *texs;
static size_t ntexs, captexs;

// Bytes per texel for the formats the dump logs show (the VkFormat numbers are those in the dump's rgba8(37) style).
static double texel_bytes(VkFormat f) {
    switch ((int)f) {
    case 131: case 132: case 133: case 134: case 139: case 140: return 0.5;   // BC1, BC4 (8 bytes per 4x4)
    case 135: case 136: case 137: case 138: case 141: case 142: case 143: case 144: case 145: case 146: return 1.0;   // BC2/3/5/6/7
    case 124: return 2.0;                                                // D16
    case 97: case 130: return 8.0;                                       // R16G16B16A16_SFLOAT, D32S8 (padded)
    case 109: return 16.0;                                               // R32G32B32A32_SFLOAT
    default: return 4.0;                                                 // RGBA8, RGB10A2, R11G11B10F, R32F, D32
    }
}

static uint64_t capture_frame(void) {
    if (!target_read) {
        const char *e = cfg_get("XC2_CAPTURE");
        target = e && *e ? strtoull(e, NULL, 10) : 0;
        target_read = 1;
    }
    return target;
}

void vk_capture_note(uint64_t addr, uint64_t size) {
    if (!size) return;
    plat_mutex_lock(&cap_lock);
    ncalls++;
    if (nranges == capranges) {   // grow; a failed grow drops the range, and the total then reads low
        size_t n = capranges ? capranges * 2 : 65536;
        CapRange *p = realloc(ranges, n * sizeof *ranges);
        if (p) ranges = p, capranges = n;
    }
    if (nranges < capranges) ranges[nranges].lo = addr, ranges[nranges].hi = addr + size, nranges++;
    plat_mutex_unlock(&cap_lock);
}

void vk_capture_texture(const VkTexImage *img) {
    if (!img) return;
    uint64_t texels = (uint64_t)img->width * img->height * (img->depth ? img->depth : 1) * (img->layers ? img->layers : 1);
    double base = (double)texels * texel_bytes(img->format);
    uint64_t bytes = (uint64_t)(img->levels > 1 ? base * 4.0 / 3.0 : base);
    plat_mutex_lock(&cap_lock);
    if (ntexs == captexs) {
        size_t n = captexs ? captexs * 2 : 4096;
        CapTex *p = realloc(texs, n * sizeof *texs);
        if (p) texs = p, captexs = n;
    }
    if (ntexs < captexs) texs[ntexs].img = img, texs[ntexs].bytes = bytes, texs[ntexs].gpu = img->gpu_owned, ntexs++;
    plat_mutex_unlock(&cap_lock);
}

static int by_tex(const void *a, const void *b) {
    const CapTex *x = a, *y = b;
    return x->img < y->img ? -1 : x->img > y->img;
}

static int by_lo(const void *a, const void *b) {
    const CapRange *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

// Ends the measurement: sorts and merges the noted ranges and logs the unique bytes they cover.
static void capture_report(uint64_t frame) {
    plat_mutex_lock(&cap_lock);
    qsort(ranges, nranges, sizeof *ranges, by_lo);
    uint64_t bytes = 0, merged = 0, lo = 0, hi = 0, largest = 0;
    for (size_t i = 0; i < nranges; i++) {
        if (merged == 0 || ranges[i].lo > hi) {
            if (merged) bytes += hi - lo;
            if (merged && hi - lo > largest) largest = hi - lo;
            lo = ranges[i].lo, hi = ranges[i].hi;
            merged++;
        } else if (ranges[i].hi > hi) {
            hi = ranges[i].hi;
        }
    }
    if (merged) {
        bytes += hi - lo;
        if (hi - lo > largest) largest = hi - lo;
    }
    rt_log("capture: frame %llu read %llu guest ranges in %llu resolve calls: %llu unique ranges, %.1f MiB unique, "
           "largest %.1f MiB",
           (unsigned long long)frame, (unsigned long long)nranges, (unsigned long long)ncalls,
           (unsigned long long)merged, bytes / (1024.0 * 1024.0), largest / (1024.0 * 1024.0));
    // Textures: each image once; the bytes of GPU-written images and of images uploaded from guest memory apart.
    qsort(texs, ntexs, sizeof *texs, by_tex);
    uint64_t gpu_bytes = 0, guest_bytes = 0;
    size_t gpu_n = 0, guest_n = 0;
    for (size_t i = 0; i < ntexs; i++) {
        if (i && texs[i].img == texs[i - 1].img) continue;
        if (texs[i].gpu) gpu_bytes += texs[i].bytes, gpu_n++;
        else guest_bytes += texs[i].bytes, guest_n++;
    }
    rt_log("capture: frame %llu samples %llu GPU-written textures (%.1f MiB) and %llu guest textures (%.1f MiB)",
           (unsigned long long)frame, (unsigned long long)gpu_n, gpu_bytes / (1024.0 * 1024.0),
           (unsigned long long)guest_n, guest_bytes / (1024.0 * 1024.0));
    nranges = 0, ncalls = 0, ntexs = 0;
    plat_mutex_unlock(&cap_lock);
}

// Called at each present with its frame count: arms the measurement when frame N-1 has presented (so frame N's work
// runs measured) and reports it at present N.
void vk_capture_frame(uint64_t presented) {
    uint64_t t = capture_frame();
    if (!t) return;
    if (presented + 1 == t) {
        plat_mutex_lock(&cap_lock);
        nranges = 0, ncalls = 0, ntexs = 0;
        plat_mutex_unlock(&cap_lock);
        vk_capture_on = 1;
    } else if (presented == t && vk_capture_on) {
        vk_capture_on = 0;
        capture_report(t);
    }
}
