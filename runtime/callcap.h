// Slow-frame call capture (callcap.c): the inputs, the result and the time of chosen host calls, kept for the frames
// over a threshold and written to files that tests/callbench replays offline, so a change to one of those functions is
// timed and checked against the game's own data without running the game.
//
// XC2_CALLCAP=<dir> turns it on. Every call of a captured kind between two presents is recorded in memory; when the
// present interval exceeds XC2_CALLCAP_MS (default 10), the frame's calls are written to <dir>/f<frame>.ccap by a
// writer thread, otherwise they are dropped. XC2_CALLCAP_MB (default 256) bounds one frame's records (later calls of
// that frame are counted as dropped), XC2_CALLCAP_FILES (default 300) the files of a run. Recording copies every input,
// so the captured run is slower than a plain one: its own frame times are not a measurement.
//
// This header is also the file format, included by tests/callbench/callbench.c (CALLBENCH defined).
#ifndef XC2_CALLCAP_H
#define XC2_CALLCAP_H

#include <stdint.h>
#include <string.h>

// A file: CcFileHdr, then nrec records. A record: CcRec, then nblob blobs, each a uint64_t length and that many bytes
// padded with zeros to a multiple of 8.
#define CC_MAGIC "XC2CCAP1"
typedef struct { char magic[8]; uint64_t frame, interval_ns, nrec, dropped; } CcFileHdr;
typedef struct { uint32_t kind, nblob; uint64_t ns; uint32_t tid, pad; } CcRec;

enum {
    CC_INFLATE = 1,    // ov_inflate.c inflate_fast: CcInflateIn, lcode, dcode, window, output before next_out, input; CcInflateOut
    CC_DESWIZZLE = 2,  // tex_deswizzle.c nvn_deswizzle: CcDeswizzle, nvn_texdesc, the texture's storage
    CC_COPY = 3,       // memory.c mem_copy_par: CcCopy (sizes only: a copy's time does not depend on the bytes)
    CC_WWQUERY = 4,    // vk_memory.c ww_query: CcWwQuery, the written pages' indices (uint32_t, 4 KiB pages from addr)
    CC_KINDS
};

typedef struct {
    uint32_t start, avail_in, avail_out, bits, wsize, whave, wnext, lenbits, distbits, sane, back, window_n;
    uint64_t hold;
} CcInflateIn;
#define CC_LCODES 852   // zlib ENOUGH_LENS: the length/literal table's entries
#define CC_DCODES 592   // zlib ENOUGH_DISTS
typedef struct { uint32_t avail_in, avail_out, bits, mode; uint64_t hold, out_hash; } CcInflateOut;

typedef struct { uint32_t level, layer, ret, pad; uint64_t dst_pitch, dst_size, dst_hash; } CcDeswizzle;
typedef struct { uint64_t n, dst_off, src_off; } CcCopy;   // offsets: the pointers' low 12 bits
typedef struct { uint64_t size, n, cap; } CcWwQuery;       // cap: the page buffer's entries

// A 64-bit hash of n bytes, eight at a time (the result of a call, compared by callbench).
static inline uint64_t cc_hash(const void *p, uint64_t n) {
    const uint8_t *b = p;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, w;
    for (; n >= 8; n -= 8, b += 8) memcpy(&w, b, 8), h = (h ^ w) * 0x100000001B3ull, h ^= h >> 29;
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001B3ull;
    return h ^ h >> 32;
}

#ifndef CALLBENCH
typedef struct { const void *p; uint64_t n; } CcBlob;
extern int callcap_level;   // 1 while capturing; read without a lock by the hooks
static inline int callcap_on(void) { return __atomic_load_n(&callcap_level, __ATOMIC_RELAXED); }
void callcap_init(void);                                                  // perf_init: reads XC2_CALLCAP
void callcap_add(uint32_t kind, uint64_t ns, const CcBlob *b, int nb);   // any thread
void callcap_frame(uint64_t frame, uint64_t interval_ns);                // perf_present: keep or drop the frame's calls
uint64_t callcap_now(void);                                              // the clock the hooks time calls with
#endif

#endif
