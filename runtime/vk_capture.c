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
    nranges = 0, ncalls = 0;
    plat_mutex_unlock(&cap_lock);
}

// Called at each present with its frame count: arms the measurement when frame N-1 has presented (so frame N's work
// runs measured) and reports it at present N.
void vk_capture_frame(uint64_t presented) {
    uint64_t t = capture_frame();
    if (!t) return;
    if (presented + 1 == t) {
        plat_mutex_lock(&cap_lock);
        nranges = 0, ncalls = 0;
        plat_mutex_unlock(&cap_lock);
        vk_capture_on = 1;
    } else if (presented == t && vk_capture_on) {
        vk_capture_on = 0;
        capture_report(t);
    }
}
