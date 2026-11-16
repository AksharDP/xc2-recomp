// The game's zlib inflate_fast in native code. XC2 3.0.1 links zlib 1.2.8 ("1.2.8.f-NINTENDO-SDK-v1" at main+0x9a96d7)
// and its file loader (main+0x6b1608, main+0x6b29a0) inflates the xbc1 archives of streamed assets through it. Lifted,
// inflate_fast (main+0x8acf00) is the guest function with the most samples on a streaming route: each byte goes through
// LD8 and ST8 and the bit buffer through Cpu registers. The patch set "xc2zlib" (recomp/patches-v301.txt) sends its one
// call, in inflate (main+0x8aa910), to inflate_fast_host on the patch page, which runs zlib 1.2.8's inffast.c on the
// guest's own z_stream and inflate_state: the same bytes out, the same fields written.
//
// - The structs are zlib 1.2.8's as an LP64 build lays them out (unsigned long is 64 bits on the guest, 32 on Windows,
//   so the fields have explicit widths here). The lifted function reads and writes the same offsets: state at
//   strm+0x38, lenbits and distbits at +0x70, sane at +0x1be0.
// - strm->msg gets the guest's own strings, so inflate's callers see the pointers they would.
// XC2_ZLIB_NATIVE=0 keeps the guest function (the patch set switch); XC2_ZLIB_CHECK=1 runs both on each call, keeps the
// guest's result and logs every call where the two differ.
//
// inflate_fast below is adapted from zlib 1.2.8 inffast.c (zlib license):
//   inffast.c -- fast decoding
//   Copyright (C) 1995-2008, 2010, 2013 Mark Adler
//   For conditions of distribution and use, see copyright notice in zlib.h
// zlib.h: This software is provided 'as-is', without any express or implied warranty. In no event will the authors be
// held liable for any damages arising from the use of this software. Permission is granted to anyone to use this
// software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to
// the following restrictions: 1. The origin of this software must not be misrepresented; you must not claim that you
// wrote the original software. If you use this software in a product, an acknowledgment in the product documentation
// would be appreciated but is not required. 2. Altered source versions must be plainly marked as such, and must not be
// misrepresented as being the original software. 3. This notice may not be removed or altered from any source
// distribution. (Jean-loup Gailly, Mark Adler.) Altered: guest struct layouts, guest message pointers, the copy of a
// match from the output (fills and words instead of bytes, the same bytes written), and the host call wrapper, stats
// and check mode around it.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// tests/callbench compiles this file with CALLBENCH defined: inflate_fast alone, without the host call around it.
#ifdef CALLBENCH
#define OV_INFLATE_KERNEL 1
#else
#include "callcap.h"
#include "cfg.h"
#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#endif
#ifdef RT_PATCH_PAGE
#define OV_INFLATE_KERNEL 1
#endif
#endif

#ifdef OV_INFLATE_KERNEL

#define GUEST_INFLATE_FAST 0x70008acf00ull   // 3.0.1 main: inflate_fast (only 3.0.1 and 3.0.0 JP have the patch site)
#define HOST_INFLATE_FAST (RT_PATCH_PAGE + 0xfd0)
#define MSG_BAD_LITLEN 0x70009b0eedull       // "invalid literal/length code"
#define MSG_BAD_DIST 0x70009b0f09ull         // "invalid distance code"
#define MSG_TOO_FAR 0x70009b0f1full          // "invalid distance too far back"
#define MODE_TYPE 11                         // zlib 1.2.8 inflate_mode TYPE
#define MODE_BAD 29                          // and BAD

typedef struct {   // zlib 1.2.8 z_stream, LP64
    uint64_t next_in;
    uint32_t avail_in, pad0;
    uint64_t total_in, next_out;
    uint32_t avail_out, pad1;
    uint64_t total_out, msg, state;
} GStream;
_Static_assert(__builtin_offsetof(GStream, state) == 0x38, "z_stream");

typedef struct {   // zlib 1.2.8 code
    uint8_t op, bits;
    uint16_t val;
} Code;

// The inflate_state fields inflate_fast uses, at their 1.2.8 LP64 offsets.
#define ST_MODE 0x00
#define ST_WSIZE 0x34
#define ST_WHAVE 0x38
#define ST_WNEXT 0x3c
#define ST_WINDOW 0x40
#define ST_HOLD 0x48
#define ST_BITS 0x50
#define ST_LENCODE 0x60
#define ST_DISTCODE 0x68
#define ST_LENBITS 0x70
#define ST_DISTBITS 0x74
#define ST_SANE 0x1be0
#define ST_SIZE 0x1bec

static inline uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static inline void wr32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline void wr64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }

// zlib 1.2.8 inflate_fast with OFF 1 (pre-increment), no INFLATE_STRICT, no INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR.
static void inflate_fast(uint64_t strm_a, unsigned start) {
    GStream *strm = (GStream *)(uintptr_t)strm_a;
    uint64_t st = strm->state;
    const uint8_t *in = (const uint8_t *)(uintptr_t)strm->next_in - 1;
    const uint8_t *last = in + (strm->avail_in - 5);
    uint8_t *out = (uint8_t *)(uintptr_t)strm->next_out - 1;
    uint8_t *beg = out - (start - strm->avail_out);
    uint8_t *end = out + (strm->avail_out - 257);
    unsigned wsize = rd32(st + ST_WSIZE), whave = rd32(st + ST_WHAVE), wnext = rd32(st + ST_WNEXT);
    const uint8_t *window = (const uint8_t *)(uintptr_t)rd64(st + ST_WINDOW);
    uint64_t hold = rd64(st + ST_HOLD);
    unsigned bits = rd32(st + ST_BITS);
    const Code *lcode = (const Code *)(uintptr_t)rd64(st + ST_LENCODE);
    const Code *dcode = (const Code *)(uintptr_t)rd64(st + ST_DISTCODE);
    unsigned lmask = (1U << rd32(st + ST_LENBITS)) - 1;
    unsigned dmask = (1U << rd32(st + ST_DISTBITS)) - 1;
    Code here;
    unsigned op, len, dist;
    const uint8_t *from;

    do {
        if (bits < 15) {
            hold += (uint64_t)(*++in) << bits;
            bits += 8;
            hold += (uint64_t)(*++in) << bits;
            bits += 8;
        }
        here = lcode[hold & lmask];
    dolen:
        op = here.bits;
        hold >>= op;
        bits -= op;
        op = here.op;
        if (op == 0) {   // literal
            *++out = (uint8_t)here.val;
        } else if (op & 16) {   // length base
            len = here.val;
            op &= 15;
            if (op) {
                if (bits < op) {
                    hold += (uint64_t)(*++in) << bits;
                    bits += 8;
                }
                len += (unsigned)hold & ((1U << op) - 1);
                hold >>= op;
                bits -= op;
            }
            if (bits < 15) {
                hold += (uint64_t)(*++in) << bits;
                bits += 8;
                hold += (uint64_t)(*++in) << bits;
                bits += 8;
            }
            here = dcode[hold & dmask];
        dodist:
            op = here.bits;
            hold >>= op;
            bits -= op;
            op = here.op;
            if (op & 16) {   // distance base
                dist = here.val;
                op &= 15;
                if (bits < op) {
                    hold += (uint64_t)(*++in) << bits;
                    bits += 8;
                    if (bits < op) {
                        hold += (uint64_t)(*++in) << bits;
                        bits += 8;
                    }
                }
                dist += (unsigned)hold & ((1U << op) - 1);
                hold >>= op;
                bits -= op;
                op = (unsigned)(out - beg);   // max distance in output
                if (dist > op) {   // copy from the window
                    op = dist - op;
                    if (op > whave && rd32(st + ST_SANE)) {
                        strm->msg = MSG_TOO_FAR;
                        wr32(st + ST_MODE, MODE_BAD);
                        break;
                    }
                    from = window - 1;
                    if (wnext == 0) {
                        from += wsize - op;
                        if (op < len) {
                            len -= op;
                            do *++out = *++from;
                            while (--op);
                            from = out - dist;
                        }
                    } else if (wnext < op) {   // wrap around the window
                        from += wsize + wnext - op;
                        op -= wnext;
                        if (op < len) {
                            len -= op;
                            do *++out = *++from;
                            while (--op);
                            from = window - 1;
                            if (wnext < len) {
                                op = wnext;
                                len -= op;
                                do *++out = *++from;
                                while (--op);
                                from = out - dist;
                            }
                        }
                    } else {   // contiguous in the window
                        from += wnext - op;
                        if (op < len) {
                            len -= op;
                            do *++out = *++from;
                            while (--op);
                            from = out - dist;
                        }
                    }
                    while (len > 2) {
                        *++out = *++from;
                        *++out = *++from;
                        *++out = *++from;
                        len -= 3;
                    }
                    if (len) {
                        *++out = *++from;
                        if (len > 1) *++out = *++from;
                    }
                } else {
                    // Copy from the output; the minimum length is three. zlib copies a byte at a time, and with a
                    // distance of 1 (a quarter of the match bytes in XC2's archives) each byte waits for the store
                    // before it. Altered: a fill for distance 1 and whole words from 8 on; the bytes written are the
                    // same and none past out + len.
                    uint8_t *o = out + 1;
                    const uint8_t *s = o - dist;
                    if (dist >= 8) {
                        if (len >= 8) {   // words, the last one ending exactly at o + len
                            uint8_t *stop = o + len;
                            do { memcpy(o, s, 8); o += 8; s += 8; } while (o + 8 <= stop);
                            memcpy(stop - 8, stop - 8 - dist, 8);
                        } else if (len >= 4) {   // two words of 4 that overlap
                            memcpy(o, s, 4);
                            memcpy(o + len - 4, s + len - 4, 4);
                        } else {
                            o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
                        }
                    } else if (dist == 1) {
                        memset(o, s[0], len);
                    } else {   // 2 to 7: each byte may be one this copy wrote
                        unsigned k = 0;
                        do o[k] = s[k]; while (++k < len);
                    }
                    out += len;
                }
            } else if ((op & 64) == 0) {   // second-level distance code
                here = dcode[here.val + (hold & ((1U << op) - 1))];
                goto dodist;
            } else {
                strm->msg = MSG_BAD_DIST;
                wr32(st + ST_MODE, MODE_BAD);
                break;
            }
        } else if ((op & 64) == 0) {   // second-level length code
            here = lcode[here.val + (hold & ((1U << op) - 1))];
            goto dolen;
        } else if (op & 32) {   // end of block
            wr32(st + ST_MODE, MODE_TYPE);
            break;
        } else {
            strm->msg = MSG_BAD_LITLEN;
            wr32(st + ST_MODE, MODE_BAD);
            break;
        }
    } while (in < last && out < end);

    // Return the unused bytes (on entry bits < 8, so in does not go too far back).
    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= (1U << bits) - 1;

    strm->next_in = (uint64_t)(uintptr_t)(in + 1);
    strm->next_out = (uint64_t)(uintptr_t)(out + 1);
    strm->avail_in = (unsigned)(in < last ? 5 + (last - in) : 5 - (in - last));
    strm->avail_out = (unsigned)(out < end ? 257 + (end - out) : 257 - (out - end));
    wr64(st + ST_HOLD, hold);
    wr32(st + ST_BITS, bits);
}

#ifndef CALLBENCH
static struct {
    uint64_t calls, out, ns, guest_ns, checked, mismatches;
} S;

static int check_mode(void) {
    static int v = -1;
    if (v < 0) v = cfg_bool("XC2_ZLIB_CHECK");
    return v;
}

static void count(uint64_t out, uint64_t ns) {
    uint64_t n = __atomic_add_fetch(&S.calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.out, out, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.ns, ns, __ATOMIC_RELAXED);
    if (!(n & (n - 1)) || !(n & 0x3fff))   // powers of two, then every 16384th call
        rt_log("zlib: %llu inflate_fast calls on the host, %.1f MiB out, %.0f ms (check mode: %llu compared, %llu differ, the guest function %.0f ms)",
               (unsigned long long)n, S.out / 1048576.0, S.ns / 1e6, (unsigned long long)S.checked,
               (unsigned long long)S.mismatches, S.guest_ns / 1e6);
}

// Check mode: the host's run on the call's inputs, then the guest's on the same inputs, which it keeps. inflate_fast
// writes the output window [next_out, next_out + avail_out), the stream's four in and out fields, msg, and the state's
// mode, hold and bits; those are compared.
static void check_call(Cpu *c, uint64_t strm_a, unsigned start) {
    GStream *strm = (GStream *)(uintptr_t)strm_a;
    uint64_t st = strm->state;
    GStream s0 = *strm;
    uint8_t st0[ST_SIZE];
    memcpy(st0, (void *)(uintptr_t)st, ST_SIZE);
    uint64_t outn = s0.avail_out;
    uint8_t *o0 = malloc(outn ? outn : 1), *mine = malloc(outn ? outn : 1);
    if (!o0 || !mine) {
        free(o0);
        free(mine);
        rt_call(c, GUEST_INFLATE_FAST, 2, strm_a, (uint64_t)start);
        return;
    }
    memcpy(o0, (void *)(uintptr_t)s0.next_out, outn);
    uint64_t t0 = kern_now_ns();
    inflate_fast(strm_a, start);
    uint64_t ns = kern_now_ns() - t0;
    GStream s1 = *strm;
    uint8_t st1[ST_SIZE];
    memcpy(st1, (void *)(uintptr_t)st, ST_SIZE);
    memcpy(mine, (void *)(uintptr_t)s0.next_out, outn);
    *strm = s0;   // the guest runs on the same inputs
    memcpy((void *)(uintptr_t)st, st0, ST_SIZE);
    memcpy((void *)(uintptr_t)s0.next_out, o0, outn);
    uint64_t g0 = kern_now_ns();
    rt_call(c, GUEST_INFLATE_FAST, 2, strm_a, (uint64_t)start);
    __atomic_add_fetch(&S.guest_ns, kern_now_ns() - g0, __ATOMIC_RELAXED);
    uint64_t made = s0.avail_out - strm->avail_out;
    int same = !memcmp(&s1, strm, sizeof s1) && !memcmp(st1, (void *)(uintptr_t)st, ST_SIZE) &&
               !memcmp(mine, (void *)(uintptr_t)s0.next_out, outn);
    __atomic_add_fetch(&S.checked, 1, __ATOMIC_RELAXED);
    if (!same) {
        uint64_t m = __atomic_add_fetch(&S.mismatches, 1, __ATOMIC_RELAXED);
        if (m <= 8)
            rt_log("zlib check: MISMATCH strm %#llx: host in %u out %u mode %u bits %u, guest in %u out %u mode %u bits %u",
                   (unsigned long long)strm_a, s1.avail_in, s1.avail_out, rd32((uint64_t)(uintptr_t)st1 + ST_MODE),
                   rd32((uint64_t)(uintptr_t)st1 + ST_BITS), strm->avail_in, strm->avail_out, rd32(st + ST_MODE),
                   rd32(st + ST_BITS));
    }
    free(o0);
    free(mine);
    count(made, ns);
}

// XC2_CALLCAP (callcap.h CC_INFLATE): the call's inputs, inflate_fast run on them and timed, and its result.
static void capture_call(uint64_t strm_a, unsigned start) {
    GStream *strm = (GStream *)(uintptr_t)strm_a;
    uint64_t st = strm->state;
    uint32_t made = start - strm->avail_out;
    CcInflateIn in = {start, strm->avail_in, strm->avail_out, rd32(st + ST_BITS), rd32(st + ST_WSIZE), rd32(st + ST_WHAVE),
                      rd32(st + ST_WNEXT), rd32(st + ST_LENBITS), rd32(st + ST_DISTBITS), rd32(st + ST_SANE),
                      made < 32768 ? made : 32768, 0, rd64(st + ST_HOLD)};
    in.window_n = rd64(st + ST_WINDOW) ? in.wsize : 0;
    uint64_t out0 = strm->next_out;
    // The inputs before the call: it consumes the input and may write over the window's bytes it reads.
    CcBlob b[7] = {{&in, sizeof in},
                   {(const void *)(uintptr_t)rd64(st + ST_LENCODE), 4 * CC_LCODES},
                   {(const void *)(uintptr_t)rd64(st + ST_DISTCODE), 4 * CC_DCODES},
                   {(const void *)(uintptr_t)rd64(st + ST_WINDOW), in.window_n},
                   {(const void *)(uintptr_t)(out0 - in.back), in.back},
                   {(const void *)(uintptr_t)strm->next_in, in.avail_in}};
    uint64_t total = 0;
    for (int i = 0; i < 6; i++) total += b[i].n;
    uint8_t *copy = malloc(total ? total : 1);
    if (!copy) {
        uint64_t t0 = kern_now_ns();
        inflate_fast(strm_a, start);
        count(in.avail_out - strm->avail_out, kern_now_ns() - t0);
        return;
    }
    for (uint64_t i = 0, o = 0; i < 6; o += b[i].n, i++) {
        if (b[i].n) memcpy(copy + o, b[i].p, b[i].n);
        b[i].p = copy + o;
    }
    uint64_t t0 = kern_now_ns();
    inflate_fast(strm_a, start);
    uint64_t ns = kern_now_ns() - t0;
    CcInflateOut out = {strm->avail_in, strm->avail_out, rd32(st + ST_BITS), rd32(st + ST_MODE), rd64(st + ST_HOLD),
                        cc_hash((const void *)(uintptr_t)out0, in.avail_out - strm->avail_out)};
    b[6] = (CcBlob){&out, sizeof out};
    callcap_add(CC_INFLATE, ns, b, 7);
    free(copy);
    count(in.avail_out - strm->avail_out, ns);
}

// x0 strm, w1 start (inflate's avail_out on entry).
static void inflate_fast_host(Cpu *c) {
    uint64_t strm_a = c->x[0];
    unsigned start = (uint32_t)c->x[1];
    int *d = kern_host_track && c->thread ? kern_host_depth(c->thread) : NULL;
    if (d) __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    if (check_mode()) {
        check_call(c, strm_a, start);
    } else if (callcap_on()) {
        capture_call(strm_a, start);
    } else {
        uint32_t avail = ((GStream *)(uintptr_t)strm_a)->avail_out;
        uint64_t t0 = kern_now_ns();
        inflate_fast(strm_a, start);
        count(avail - ((GStream *)(uintptr_t)strm_a)->avail_out, kern_now_ns() - t0);
    }
    if (d) {
        int n = *d - 1;
        __atomic_store_n(d, n, __ATOMIC_RELAXED);
        if (!n && kern_preempt_pending(c->thread)) kern_host_left(c->thread);
    }
}

__attribute__((constructor)) static void inflate_register(void) { rt_register_host(HOST_INFLATE_FAST, inflate_fast_host); }
#endif   // CALLBENCH

#endif
