// XC2_FLIGHT=1 flight recorder (flight.c): the last frames' timing and event counts plus a thumbnail each, written to
// disk on F8 or after a slow frame, so a glitch or stutter seen in play can be read after the fact.
#pragma once
#include <stdint.h>

#include "host.h"   // PerfFrame

#define FLIGHT_THUMB_W 320
#define FLIGHT_THUMB_H 180

extern int flight_on;   // XC2_FLIGHT=1 or XC2_FLIGHT_SLOW_MS, set once by flight_init before the first present

// The threads whose CPU time the slow-frame log (XC2_FLIGHT_SLOW_MS) splits per frame: each calls this once on itself
// as it starts, before or after flight_init. guest: the guest thread handle, 0 for a host thread; name: a label for a
// host thread or the main thread, NULL for the other guest threads (they are labelled by handle).
void flight_thread_self(uint32_t guest, const char *name);

void flight_init(void);   // prof_start calls it
// perf_present, once per present on the render thread: the interval since the previous present, this frame's timing
// (PerfFrame, host.h) and the running PERF_CNT_* totals.
void flight_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, const PerfFrame *pf,
                  const volatile int64_t counts[]);
// prof_write (exit): dumps the ring once more, unless XC2_FLIGHT_EXIT=0, so a run ends with its last frames on disk
// whatever the triggers did, and writes the slow-frame log.
void flight_exit(void);

// gfx_present.c: the present thread blits every presented frame into a host-visible ring of n thumbnails.
void vk_thumbs_enable(int n);            // before the first present; n slots of FLIGHT_THUMB_W x FLIGHT_THUMB_H
void vk_thumbs_frame(uint64_t frame);    // the present thread names the frame of the vk_present_image call that follows
// Copies frame's thumbnail as RGBA when the ring still holds it complete; 0 when it was never written, is still in
// flight or was overwritten. Takes the present lock for one slot copy (about 20 us); any thread.
int vk_thumbs_read(uint64_t frame, uint8_t *rgba);
