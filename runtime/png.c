// PNG files of 8-bit RGB or RGBA pictures (png.h). The deflate encoder (RFC 1950 zlib stream, RFC 1951 blocks) is the
// runtime's own, so no compression library is linked or shipped:
//   - each row gets the PNG filter (none, sub, up, average, Paeth) whose residuals, read as signed bytes, have the
//     smallest sum of magnitudes (libpng's default choice); on a rendered frame they cluster around zero
//   - LZ77 over the 32 KiB window: hash chains on 3-byte keys, at most CHAIN candidates per position, and a match held
//     back one byte in case the next position starts a longer one (zlib's deflate_slow)
//   - every BLOCK_SYMS symbols a block with Huffman codes built from its own symbol counts, or stored blocks when those
//     are smaller (noise), which also bounds the stream at the filtered bytes plus a few bytes per block
// compress 0 skips all of that and writes stored blocks of unfiltered rows, as the runtime's first PNG writer did.
// The match compare reads 8 bytes at a time and assumes a little-endian host (x86-64, ARM64).
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "png.h"
#include "platform/platform.h"

#define WSIZE 32768u          // the deflate window: the largest distance
#define HBITS 15              // hash table of 2^HBITS chain heads
#define CHAIN 32              // candidates tried per position
#define NICE 128              // a match this long is taken without trying more candidates
#define GOOD 4                // a held match this long: a quarter of CHAIN at the next position
#define LAZY 8                // a held match this long goes out without a search at the next position
#define BLOCK_SYMS 32768      // symbols per block

// ---- bit output, least significant bit first (RFC 1951 3.1.1)
typedef struct { uint8_t *p; size_t n; uint64_t acc; int nb; } Bits;

static inline void put(Bits *b, uint32_t v, int n) {
    b->acc |= (uint64_t)v << b->nb;
    b->nb += n;
    while (b->nb >= 8) b->p[b->n++] = (uint8_t)b->acc, b->acc >>= 8, b->nb -= 8;
}

static void align(Bits *b) {
    if (b->nb) put(b, 0, 8 - b->nb);
}

// n bytes of src as stored blocks of at most 65535 bytes; the last one is BFINAL when final.
static void put_stored(Bits *b, const uint8_t *src, size_t n, int final) {
    do {
        uint32_t k = n > 65535 ? 65535 : (uint32_t)n;
        put(b, final && k == n, 1);
        put(b, 0, 2);
        align(b);
        b->p[b->n++] = (uint8_t)k, b->p[b->n++] = (uint8_t)(k >> 8);
        b->p[b->n++] = (uint8_t)~k, b->p[b->n++] = (uint8_t)(~k >> 8);
        memcpy(b->p + b->n, src, k);
        b->n += k, src += k, n -= k;
    } while (n);
}

// ---- Huffman codes (RFC 1951 3.2.5 - 3.2.7)
static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                   131, 163, 195, 227, 258};
static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                   1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
static const uint8_t cl_extra[19] = {[16] = 2, [17] = 3, [18] = 7};   // extra bits of the code length symbols

// The length code (0..28, symbol 257 + code) of a match length 3..258: four codes per power of two above 10.
static inline int len_code(uint32_t len) {
    if (len == 258) return 28;
    uint32_t l = len - 3;
    if (l < 8) return (int)l;
    int b = 31 - __builtin_clz(l);
    return 4 * (b - 1) + (int)(l >> (b - 2) & 3);
}

// The distance code (0..29) of a distance 1..32768: two codes per power of two above 4.
static inline int dist_code(uint32_t d) {
    d--;
    if (d < 4) return (int)d;
    int b = 31 - __builtin_clz(d);
    return 2 * b + (int)(d >> (b - 1) & 1);
}

// Code lengths of at most limit bits for the counts of n symbols (n <= 286), by Huffman's construction with two queues
// over the counts sorted ascending. A tree deeper than limit halves every count (keeping it nonzero) and is built again,
// which at the latest ends with equal counts and a balanced tree. At least two symbols get a code, as zlib does: a
// decoder may reject a one-symbol code, and the distance code needs one even in a block without matches.
static void huff_lengths(const uint32_t *count, int n, int limit, uint8_t *len) {
    uint32_t f[286];
    memcpy(f, count, sizeof *f * (size_t)n);
    int used = 0;
    for (int i = 0; i < n; i++) used += f[i] != 0;
    for (int i = 0; i < n && used < 2; i++)
        if (!f[i]) f[i] = 1, used++;
    for (;;) {
        int sym[286], m = 0;
        for (int i = 0; i < n; i++)
            if (f[i]) sym[m++] = i;
        for (int i = 1; i < m; i++) {   // insertion sort by count; n is small
            int s = sym[i], j = i;
            for (; j > 0 && f[sym[j - 1]] > f[s]; j--) sym[j] = sym[j - 1];
            sym[j] = s;
        }
        // Nodes 0..m-1 are the leaves in that order, m.. the inner nodes in the order they are made, which is also by
        // weight, so each step takes the two lightest heads of the two queues.
        uint32_t w[2 * 286];
        int parent[2 * 286], depth[2 * 286], leaf = 0, inner = m, nodes = m;
        for (int i = 0; i < m; i++) w[i] = f[sym[i]];
        while (nodes < 2 * m - 1) {
            int a = leaf < m && (inner >= nodes || w[leaf] <= w[inner]) ? leaf++ : inner++;
            int b = leaf < m && (inner >= nodes || w[leaf] <= w[inner]) ? leaf++ : inner++;
            w[nodes] = w[a] + w[b];
            parent[a] = parent[b] = nodes++;
        }
        depth[nodes - 1] = 0;
        for (int i = nodes - 2; i >= 0; i--) depth[i] = depth[parent[i]] + 1;   // a parent comes after its children
        int max = 0;
        memset(len, 0, (size_t)n);
        for (int i = 0; i < m; i++) {
            len[sym[i]] = (uint8_t)(depth[i] < 255 ? depth[i] : 255);
            if (depth[i] > max) max = depth[i];
        }
        if (max <= limit) return;
        for (int i = 0; i < n; i++)
            if (f[i]) f[i] = f[i] >> 1 | 1;
    }
}

// The canonical codes of the lengths (RFC 1951 3.2.2), bit-reversed for the output's bit order.
static void huff_codes(const uint8_t *len, int n, uint16_t *code) {
    uint16_t count[16] = {0}, next[16] = {0};
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    uint32_t c = 0;
    for (int b = 1; b < 16; b++) next[b] = (uint16_t)(c = (c + count[b - 1]) << 1);
    for (int i = 0; i < n; i++) {
        code[i] = 0;
        if (!len[i]) continue;
        uint32_t v = next[len[i]]++, r = 0;
        for (int k = 0; k < len[i]; k++) r = r << 1 | (v & 1), v >>= 1;
        code[i] = (uint16_t)r;
    }
}

// ---- LZ77 and blocks
typedef struct {
    const uint8_t *src;
    size_t n;
    Bits out;
    uint32_t *head, *prev;           // hash chains: position + 1 (0 none); prev indexed by position % WSIZE
    uint16_t *sym_len, *sym_dist;    // the block's symbols: a literal byte (dist 0) or a match's length and distance
    int nsym;
    size_t block_start, pos;         // the block holds src[block_start, pos)
} Z;

// The block's symbols with Huffman codes of their own, or as stored blocks when that is smaller.
static void flush_block(Z *z, int final) {
    uint32_t lf[286] = {0}, df[30] = {0}, cf[19] = {0};
    for (int i = 0; i < z->nsym; i++)
        if (!z->sym_dist[i]) lf[z->sym_len[i]]++;
        else lf[257 + len_code(z->sym_len[i])]++, df[dist_code(z->sym_dist[i])]++;
    lf[256] = 1;   // end of block
    uint8_t ll[286], dl[30], cl[19];
    huff_lengths(lf, 286, 15, ll);
    huff_lengths(df, 30, 15, dl);
    int hlit = 286, hdist = 30;
    while (hlit > 257 && !ll[hlit - 1]) hlit--;
    while (hdist > 1 && !dl[hdist - 1]) hdist--;
    // Both codes' lengths as one sequence, run-length coded: 16 repeats the previous length 3-6 times, 17 writes 3-10
    // zeros, 18 writes 11-138 zeros (op the symbol, opx its extra bits' value).
    uint8_t seq[286 + 30], op[286 + 30], opx[286 + 30];
    int nseq = hlit + hdist, nop = 0;
    memcpy(seq, ll, (size_t)hlit);
    memcpy(seq + hlit, dl, (size_t)hdist);
    for (int i = 0; i < nseq;) {
        int v = seq[i], run = 1;
        while (i + run < nseq && seq[i + run] == v) run++;
        i += run;
        if (!v) {
            while (run >= 11) {
                int k = run > 138 ? 138 : run;
                op[nop] = 18, opx[nop++] = (uint8_t)(k - 11), run -= k;
            }
            if (run >= 3) op[nop] = 17, opx[nop++] = (uint8_t)(run - 3), run = 0;
        } else {
            op[nop] = (uint8_t)v, opx[nop++] = 0, run--;
            while (run >= 3) {
                int k = run > 6 ? 6 : run;
                op[nop] = 16, opx[nop++] = (uint8_t)(k - 3), run -= k;
            }
        }
        for (; run > 0; run--) op[nop] = (uint8_t)v, opx[nop++] = 0;
    }
    for (int i = 0; i < nop; i++) cf[op[i]]++;
    huff_lengths(cf, 19, 7, cl);
    int hclen = 19;
    while (hclen > 4 && !cl[cl_order[hclen - 1]]) hclen--;

    // The block's size in bits with these codes, and as stored blocks (counting the most padding a header can need)
    uint64_t dyn = 3 + 5 + 5 + 4 + 3 * (uint64_t)hclen;
    for (int i = 0; i < nop; i++) dyn += cl[op[i]] + cl_extra[op[i]];
    for (int s = 0; s < 286; s++) dyn += (uint64_t)lf[s] * (ll[s] + (s > 256 ? lext[s - 257] : 0));
    for (int s = 0; s < 30; s++) dyn += (uint64_t)df[s] * (dl[s] + dext[s]);
    size_t bytes = z->pos - z->block_start;
    uint64_t stored = ((uint64_t)bytes / 65535 + 1) * (3 + 7 + 32) + 8 * (uint64_t)bytes;
    Bits *b = &z->out;
    if (stored <= dyn) {
        put_stored(b, z->src + z->block_start, bytes, final);
    } else {
        uint16_t lc[286], dc[30], cc[19];
        huff_codes(ll, 286, lc);
        huff_codes(dl, 30, dc);
        huff_codes(cl, 19, cc);
        put(b, final, 1);
        put(b, 2, 2);   // dynamic Huffman codes
        put(b, (uint32_t)(hlit - 257), 5);
        put(b, (uint32_t)(hdist - 1), 5);
        put(b, (uint32_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; i++) put(b, cl[cl_order[i]], 3);
        for (int i = 0; i < nop; i++) {
            put(b, cc[op[i]], cl[op[i]]);
            if (cl_extra[op[i]]) put(b, opx[i], cl_extra[op[i]]);
        }
        for (int i = 0; i < z->nsym; i++) {
            uint32_t v = z->sym_len[i], d = z->sym_dist[i];
            if (!d) {
                put(b, lc[v], ll[v]);
                continue;
            }
            int k = len_code(v), j = dist_code(d);
            put(b, lc[257 + k], ll[257 + k]);
            if (lext[k]) put(b, v - lbase[k], lext[k]);
            put(b, dc[j], dl[j]);
            if (dext[j]) put(b, d - dbase[j], dext[j]);
        }
        put(b, lc[256], ll[256]);
    }
    z->nsym = 0;
    z->block_start = z->pos;
}

static inline void emit(Z *z, uint32_t len_or_byte, uint32_t dist) {
    z->sym_len[z->nsym] = (uint16_t)len_or_byte;
    z->sym_dist[z->nsym++] = (uint16_t)dist;
    z->pos += dist ? len_or_byte : 1;
    if (z->nsym == BLOCK_SYMS) flush_block(z, 0);
}

static inline uint32_t hash3(const uint8_t *p) {
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 2654435761u >> (32 - HBITS);
}

static inline void insert(const uint8_t *src, uint32_t *head, uint32_t *prev, size_t i) {
    uint32_t h = hash3(src + i);
    prev[i % WSIZE] = head[h];
    head[h] = (uint32_t)i + 1;
}

// How many of the first lim bytes of a and b are equal.
static inline uint32_t match_len(const uint8_t *a, const uint8_t *b, uint32_t lim) {
    uint32_t m = 0;
    for (; m + 8 <= lim; m += 8) {
        uint64_t x, y;
        memcpy(&x, a + m, 8);
        memcpy(&y, b + m, 8);
        if (x != y) return m + (uint32_t)(__builtin_ctzll(x ^ y) >> 3);
    }
    while (m < lim && a[m] == b[m]) m++;
    return m;
}

// The longest match for position i of src[0, n) (searched before i is inserted) longer than min, trying at most chain
// candidates, with its distance in *dist; 0 when there is none. A chain entry more than WSIZE back ends the search: its
// slot may already hold a newer position.
static uint32_t longest(const uint8_t *src, size_t n, const uint32_t *head, const uint32_t *prev, size_t i, uint32_t min,
                        int chain, uint32_t *dist) {
    const uint8_t *s = src + i;
    uint32_t lim = n - i < 258 ? (uint32_t)(n - i) : 258, best = min > 2 ? min : 2, d = 0;
    if (best >= lim) return 0;
    for (uint32_t cand = head[hash3(s)]; cand && chain--;) {
        size_t c = cand - 1;
        if (i - c > WSIZE) break;
        const uint8_t *t = src + c;
        if (t[best] == s[best]) {   // a candidate that cannot beat best fails here, mostly
            uint32_t m = match_len(t, s, lim);
            if (m > best) {
                best = m, d = (uint32_t)(i - c);
                if (m >= NICE || m == lim) break;
            }
        }
        cand = prev[c % WSIZE];
    }
    *dist = d;
    return d ? best : 0;
}

// The symbols of src: at each position the longest match is held back until the next position is searched, and goes out
// as a literal when the next position starts a longer one. As in zlib, a held match of GOOD bytes or more makes that
// search shorter, and one of LAZY or more skips it.
static void lz77(Z *z) {
    const uint8_t *src = z->src;
    uint32_t *head = z->head, *prev = z->prev;
    size_t n = z->n, i = 0;
    int held = 0;   // position i - 1 waits: a literal (hlen 0) or a match
    uint32_t hlen = 0, hdist = 0;
    while (i < n) {
        uint32_t len = 0, dist = 0;
        if (n - i >= 3) {
            if (!held || hlen < LAZY)
                len = longest(src, n, head, prev, i, held ? hlen : 0, hlen >= GOOD ? CHAIN / 4 : CHAIN, &dist);
            insert(src, head, prev, i);
        }
        if (held) {
            if (hlen && !len) {   // nothing longer starts at i: the held match goes out
                emit(z, hlen, hdist);
                size_t end = i - 1 + hlen;
                for (size_t s = i + 1; s < end; s++)
                    if (n - s >= 3) insert(src, head, prev, s);
                i = end;
                held = 0;
                continue;
            }
            emit(z, src[i - 1], 0);
        }
        held = 1, hlen = len, hdist = dist;
        i++;
    }
    if (held) {
        if (hlen) emit(z, hlen, hdist);
        else emit(z, src[n - 1], 0);
    }
}

// ---- filters (PNG 9.2): each row's bytes from its pixels and the pixels of the row above (zeros above the first)
static inline int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static inline uint8_t residual(int f, int x, int a, int b, int c) {
    switch (f) {
    case 1: return (uint8_t)(x - a);
    case 2: return (uint8_t)(x - b);
    case 3: return (uint8_t)(x - ((a + b) >> 1));
    case 4: return (uint8_t)(x - paeth(a, b, c));
    default: return (uint8_t)x;
    }
}

// raw gets each row as its filter type and its filtered bytes; zero is a row of zeros as long as one row.
static void filter_rows(const uint8_t *px, uint32_t w, uint32_t h, int bpp, const uint8_t *zero, uint8_t *raw) {
    size_t stride = (size_t)w * (size_t)bpp;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *cur = px + stride * y, *up = y ? cur - stride : zero;
        uint8_t *o = raw + (stride + 1) * y;
        uint64_t sum[5] = {0};
        for (size_t i = 0; i < stride; i++) {
            int x = cur[i], a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = up[i], c = i >= (size_t)bpp ? up[i - bpp] : 0;
            sum[0] += (uint64_t)abs((int8_t)x);
            sum[1] += (uint64_t)abs((int8_t)(x - a));
            sum[2] += (uint64_t)abs((int8_t)(x - b));
            sum[3] += (uint64_t)abs((int8_t)(x - ((a + b) >> 1)));
            sum[4] += (uint64_t)abs((int8_t)(x - paeth(a, b, c)));
        }
        int f = 0;
        for (int k = 1; k < 5; k++)
            if (sum[k] < sum[f]) f = k;
        o[0] = (uint8_t)f;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0, c = i >= (size_t)bpp ? up[i - bpp] : 0;
            o[1 + i] = residual(f, cur[i], a, up[i], c);
        }
    }
}

// ---- the file
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static uint32_t crc32(const uint32_t *table, const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;   // the most bytes before b can overflow 32 bits (zlib's NMAX)
        n -= k;
        while (k--) a += *p++, b += a;
        a %= 65521, b %= 65521;
    }
    return b << 16 | a;
}

uint8_t *png_encode(const uint8_t *px, uint32_t w, uint32_t h, int channels, int compress, size_t *size) {
    if (!w || !h || (channels != 3 && channels != 4)) return NULL;
    uint64_t stride = (uint64_t)w * (uint32_t)channels;
    if (stride + 1 > 0x7fffffffu / h) return NULL;   // larger than one IDAT, checked before the product can wrap
    uint64_t raw_n = (stride + 1) * h;
    // No block is written larger than its bytes as stored blocks: 42 bits per 65535 bytes and per block, and a block of
    // BLOCK_SYMS symbols covers at least that many bytes. With the zlib header, the checksum and the final padding the
    // stream stays well within this; one IDAT chunk holds at most 2^31 - 1 bytes.
    uint64_t zcap = raw_n + raw_n / 1024 + 64;
    if (zcap > 0x7fffffffu) return NULL;
    uint8_t *raw = malloc((size_t)raw_n), *file = malloc((size_t)zcap + 57);
    uint8_t *zero = compress ? calloc((size_t)stride, 1) : NULL;
    Z z = {.src = raw, .n = (size_t)raw_n};
    if (compress) {
        z.head = calloc((size_t)1 << HBITS, sizeof *z.head);
        z.prev = malloc(WSIZE * sizeof *z.prev);
        z.sym_len = malloc(BLOCK_SYMS * sizeof *z.sym_len);
        z.sym_dist = malloc(BLOCK_SYMS * sizeof *z.sym_dist);
    }
    uint8_t *ret = NULL;
    if (!raw || !file || (compress && (!zero || !z.head || !z.prev || !z.sym_len || !z.sym_dist))) goto out;

    if (compress) {
        filter_rows(px, w, h, channels, zero, raw);
    } else {
        for (uint32_t y = 0; y < h; y++) {
            raw[(stride + 1) * y] = 0;   // filter: none
            memcpy(raw + (stride + 1) * y + 1, px + stride * y, (size_t)stride);
        }
    }
    // signature 0-7, IHDR 8-32, IDAT from 33 with the stream from 41, IEND
    z.out = (Bits){file + 41, 0, 0, 0};
    z.out.p[z.out.n++] = 0x78;                      // deflate, 32 KiB window
    z.out.p[z.out.n++] = compress ? 0x5e : 0x01;    // compression level hint; (0x78 << 8 | this) % 31 == 0
    if (compress) {
        lz77(&z);
        flush_block(&z, 1);
    } else {
        put_stored(&z.out, raw, (size_t)raw_n, 1);
    }
    align(&z.out);
    be32(z.out.p + z.out.n, adler32(raw, (size_t)raw_n));
    size_t zn = z.out.n + 4;

    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    memcpy(file, sig, 8);
    be32(file + 8, 13);
    memcpy(file + 12, "IHDR", 4);
    be32(file + 16, w);
    be32(file + 20, h);
    file[24] = 8;                          // bits per channel
    file[25] = channels == 4 ? 6 : 2;      // colour type: RGBA or RGB
    file[26] = file[27] = file[28] = 0;    // deflate, adaptive filters, no interlace
    be32(file + 29, crc32(table, file + 12, 17));
    be32(file + 33, (uint32_t)zn);
    memcpy(file + 37, "IDAT", 4);
    be32(file + 41 + zn, crc32(table, file + 37, zn + 4));
    uint8_t *end = file + 45 + zn;
    be32(end, 0);
    memcpy(end + 4, "IEND", 4);
    be32(end + 8, crc32(table, end + 4, 4));
    *size = 57 + zn;
    ret = file;
    file = NULL;
out:
    free(raw), free(file), free(zero), free(z.head), free(z.prev), free(z.sym_len), free(z.sym_dist);
    return ret;
}

int png_save(const char *path, const uint8_t *px, uint32_t w, uint32_t h, int channels, int compress) {
    size_t n = 0;
    uint8_t *f = png_encode(px, w, h, channels, compress, &n);
    if (!f) {
        rt_log("png: cannot encode %s (%ux%u): out of memory", path, w, h);
        return 0;
    }
    int ok = plat_file_write_atomic(path, f, n);   // a temporary file, renamed over path once complete
    free(f);
    if (!ok) rt_log("png: cannot write %s", path);
    return ok;
}
