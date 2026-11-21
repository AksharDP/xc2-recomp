// The keyboard button prompts drawn for the keys bound (prompts_keys.h). Every function mirrors the one of the same
// name in tools/xc2_prompts.py, arithmetic included (doubles where Python has floats, nearbyint for its round, which
// rounds half to even as nearbyint does in the default rounding mode), so a key draws the same pixels and blocks here
// as there.
#include "prompts_keys.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "input_map.h"
#include "platform/platform.h"

typedef struct { uint8_t r, g, b, a; } Rgba;
typedef struct { int w, h; Rgba *p; } Pix;   // rows of RGBA, p[y * w + x]

static const Rgba FILL = {247, 251, 255, 255};   // the atlas's button faces
static const Rgba INK = {49, 138, 189, 255};     // its letters, rims and glow
static const Rgba WHITE = {255, 255, 255, 255};
static const Rgba TEAL = {34, 168, 190, 255};    // the battle palette's buttons

static int rnd(double v) { return (int)nearbyint(v); }

static Pix blank(int w, int h) {
    Pix p = {w, h, calloc((size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1), sizeof(Rgba))};
    if (!p.p) rt_fatal("prompts_keys: out of memory");
    return p;
}

static void pix_free(Pix *p) {
    free(p->p);
    p->p = NULL;
}

static Rgba darker(Rgba c, double k) {
    return (Rgba){(uint8_t)rnd(c.r * k), (uint8_t)rnd(c.g * k), (uint8_t)rnd(c.b * k), c.a};
}

// src to w x h, averaging premultiplied colour over each target pixel's area (xc2_prompts.py resample).
static Pix resample(const Pix *src, int w, int h) {
    Pix out = blank(w, h);
    int sh = src->h, sw = src->w;
    double fx = (double)sw / w, fy = (double)sh / h;
    for (int y = 0; y < h; y++) {
        double y0 = y * fy, y1 = (y + 1) * fy;
        for (int x = 0; x < w; x++) {
            double x0 = x * fx, x1 = (x + 1) * fx, acc[4] = {0, 0, 0, 0}, wsum = 0;
            int sy_end = (int)(y1 + 0.999), sx_end = (int)(x1 + 0.999);
            if (sy_end > sh) sy_end = sh;
            if (sx_end > sw) sx_end = sw;
            for (int sy = (int)y0; sy < sy_end; sy++) {
                double wy = fmin(y1, sy + 1) - fmax(y0, sy);
                for (int sx = (int)x0; sx < sx_end; sx++) {
                    double wt = wy * (fmin(x1, sx + 1) - fmax(x0, sx));
                    Rgba q = src->p[sy * sw + sx];
                    acc[0] += (double)(q.r * q.a) * wt;
                    acc[1] += (double)(q.g * q.a) * wt;
                    acc[2] += (double)(q.b * q.a) * wt;
                    acc[3] += q.a * wt;
                    wsum += wt;
                }
            }
            if (acc[3] != 0)
                out.p[y * w + x] = (Rgba){(uint8_t)rnd(acc[0] / acc[3]), (uint8_t)rnd(acc[1] / acc[3]),
                                          (uint8_t)rnd(acc[2] / acc[3]), (uint8_t)rnd(acc[3] / wsum)};
        }
    }
    return out;
}

// src composited over dst at x0, y0, in place (xc2_prompts.py over).
static void over(Pix *dst, const Pix *src, int x0, int y0) {
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++) {
            Rgba p = src->p[y * src->w + x];
            int dx = x0 + x, dy = y0 + y;
            if (!p.a || dx < 0 || dy < 0 || dx >= dst->w || dy >= dst->h) continue;
            Rgba *q = &dst->p[dy * dst->w + dx];
            double a = p.a + (double)(q->a * (255 - p.a)) / 255;
            int pc[3] = {p.r, p.g, p.b}, qc[3] = {q->r, q->g, q->b}, oc[3];
            for (int c = 0; c < 3; c++)
                oc[c] = rnd(((double)(pc[c] * p.a) + (double)(qc[c] * q->a * (255 - p.a)) / 255) / a);
            *q =(Rgba){(uint8_t)oc[0], (uint8_t)oc[1], (uint8_t)oc[2], (uint8_t)rnd(a)};
        }
}

// A Kenney icon (white shapes with the letters cut out) in the atlas's style (xc2_prompts.py styled): inside the
// shape's outline the white becomes face, the letters letters, any other colour rim; around it a rim of two pixels at
// scale 1 and a glow of six, both in rim.
static Pix styled(const Pix *src, Rgba face, Rgba letters, Rgba rim, double scale, int glow) {
    int w = src->w, h = src->h, n = w * h;
    uint8_t *outside = calloc((size_t)n, 1);
    int *queue = malloc(sizeof(int) * (size_t)n);
    double *dist = malloc(sizeof(double) * (size_t)n);
    if (!outside || !queue || !dist) rt_fatal("prompts_keys: out of memory");
    int head = 0, tail = 0;   // the transparent pixels joined to the border
#define SEED(x, y)                                                                                                     \
    do {                                                                                                               \
        int i_ = (y) * w + (x);                                                                                        \
        if (!outside[i_] && src->p[i_].a < 128) outside[i_] = 1, queue[tail++] = i_;                                  \
    } while (0)
    for (int x = 0; x < w; x++) { SEED(x, 0); SEED(x, h - 1); }
    for (int y = 0; y < h; y++) { SEED(0, y); SEED(w - 1, y); }
    while (head < tail) {
        int i = queue[head++], x = i % w, y = i / w;
        if (x + 1 < w) SEED(x + 1, y);
        if (x > 0) SEED(x - 1, y);
        if (y + 1 < h) SEED(x, y + 1);
        if (y > 0) SEED(x, y - 1);
    }
#undef SEED
    for (int i = 0; i < n; i++) dist[i] = outside[i] ? 99.0 : 0.0;
    static const struct { int dx, dy; double c; } fwd[4] = {{-1, 0, 1}, {0, -1, 1}, {-1, -1, 1.414}, {1, -1, 1.414}},
                                                  back[4] = {{1, 0, 1}, {0, 1, 1}, {1, 1, 1.414}, {-1, 1, 1.414}};
    for (int pass = 0; pass < 2; pass++)   // chamfer distance to the shape, forwards and backwards
        for (int dir = 0; dir < 2; dir++) {
            const __typeof__(fwd[0]) *steps = dir ? back : fwd;
            for (int yi = 0; yi < h; yi++) {
                int y = dir ? h - 1 - yi : yi;
                for (int xi = 0; xi < w; xi++) {
                    int x = dir ? w - 1 - xi : xi;
                    for (int s = 0; s < 4; s++) {
                        int nx = x + steps[s].dx, ny = y + steps[s].dy;
                        if (nx >= 0 && nx < w && ny >= 0 && ny < h && dist[ny * w + nx] + steps[s].c < dist[y * w + x])
                            dist[y * w + x] = dist[ny * w + nx] + steps[s].c;
                    }
                }
            }
        }
    double r_in = 2.0 * scale, r_out = (glow ? 8.0 : 2.6) * scale;
    Pix out = blank(w, h);
    for (int i = 0; i < n; i++) {
        Rgba p = src->p[i];
        int lo = p.r < p.g ? p.r : p.g;
        if (p.b < lo) lo = p.b;
        Rgba col = lo > 180 ? face : rim;
        double t = p.a / 255.0;
        int cc[3] = {col.r, col.g, col.b}, rc[3] = {rim.r, rim.g, rim.b}, lc[3] = {letters.r, letters.g, letters.b};
        if (!outside[i]) {   // the face, and the letters cut out of it
            out.p[i] = (Rgba){(uint8_t)rnd(lc[0] + (cc[0] - lc[0]) * t), (uint8_t)rnd(lc[1] + (cc[1] - lc[1]) * t),
                              (uint8_t)rnd(lc[2] + (cc[2] - lc[2]) * t), 255};
            continue;
        }
        double d = dist[i];
        double halo = d <= r_in ? 1.0 : pow(fmax(0.0, 1 - (d - r_in) / (r_out - r_in)), 1.6) * (glow ? 0.9 : 1.0);
        double ga = halo * (1 - t), al = t + ga;   // the rim and glow under the shape's anti-aliased edge
        if (al > 0)
            out.p[i] = (Rgba){(uint8_t)rnd((cc[0] * t + rc[0] * ga) / al), (uint8_t)rnd((cc[1] * t + rc[1] * ga) / al),
                              (uint8_t)rnd((cc[2] * t + rc[2] * ga) / al), (uint8_t)rnd(al * 255)};
        else
            out.p[i] = (Rgba){rim.r, rim.g, rim.b, 0};
    }
    free(outside);
    free(queue);
    free(dist);
    return out;
}

// ---- the icon bank (xc2_prompts.py key_bank)

static struct { int nicons, nkeys; Pix *icons; char (*names)[32]; int *index; } bank;

static int bank_load(const uint8_t *d, uint64_t size) {
    if (size < 16 || memcmp(d, "XC2KEYS1", 8)) return 0;
    uint32_t ni, nk;
    memcpy(&ni, d + 8, 4);
    memcpy(&nk, d + 12, 4);
    if (ni > 4096 || nk > 4096) return 0;
    bank.icons = calloc(ni ? ni : 1, sizeof(Pix));
    bank.names = calloc(nk ? nk : 1, sizeof *bank.names);
    bank.index = calloc(nk ? nk : 1, sizeof(int));
    if (!bank.icons || !bank.names || !bank.index) rt_fatal("prompts_keys: out of memory");
    uint64_t at = 16;
    for (uint32_t i = 0; i < ni; i++) {
        if (at + 4 > size) return 0;
        uint16_t w, h;
        memcpy(&w, d + at, 2);
        memcpy(&h, d + at + 2, 2);
        at += 4;
        if (!w || !h || at + (uint64_t)w * h * 4 > size) return 0;
        bank.icons[i] = blank(w, h);
        memcpy(bank.icons[i].p, d + at, (size_t)w * h * 4);
        at += (uint64_t)w * h * 4;
        bank.nicons = (int)i + 1;
    }
    for (uint32_t k = 0; k < nk; k++) {
        if (at + 1 > size) return 0;
        unsigned len = d[at++];
        if (len >= sizeof bank.names[0] || at + len + 2 > size) return 0;
        memcpy(bank.names[k], d + at, len);
        bank.names[k][len] = 0;
        uint16_t idx;
        memcpy(&idx, d + at + len, 2);
        at += len + 2;
        if (idx >= ni) return 0;
        bank.index[k] = idx;
        bank.nkeys = (int)k + 1;
    }
    return 1;
}

static void bank_free(void) {
    for (int i = 0; i < bank.nicons; i++) pix_free(&bank.icons[i]);
    free(bank.icons);
    free(bank.names);
    free(bank.index);
    memset(&bank, 0, sizeof bank);
}

enum { KEY_MOUSE_MOVE = 255 };   // the right stick: the mouse moves the camera

static const char *key_icon_name(int key) { return key == KEY_MOUSE_MOVE ? "mouse_move" : input_key_name(key); }

static const Pix *bank_icon(int key) {
    const char *name = key_icon_name(key);
    for (int k = 0; name && k < bank.nkeys; k++)
        if (!strcmp(bank.names[k], name)) return &bank.icons[bank.index[k]];
    return NULL;
}

// ---- the buttons: what each prompt shows for a table of bindings

// The key a prompt shows for action: its first keyboard key in the table, else its first mouse button (the defaults
// give ZL both the right mouse button and Z: the prompt shows Z, as xc2_prompts.py KEYS does); KEY_NONE for none.
static int shown_key(const InputMap *m, int action) {
    uint8_t keys[INPUT_BINDING_MAX];
    int n = input_map_keys_of(m, action, keys, INPUT_BINDING_MAX), mouse = KEY_NONE;
    for (int i = 0; i < n; i++) {
        int is_mouse = keys[i] >= KEY_MOUSE_L && keys[i] <= KEY_MOUSE_X2;
        if (!is_mouse) return keys[i];
        if (mouse == KEY_NONE) mouse = keys[i];
    }
    return mouse;
}

// One prompt: a key, or a group of small keys side by side ((key, x, y) on a 64-pixel cell, xc2_prompts.py
// KEY_GROUPS). A key of KEY_NONE draws nothing; a prompt with none keeps the game's art.
typedef struct { int group, n, key[4], x[4], y[4]; } Shown;

enum { BT_A, BT_B, BT_X, BT_Y, BT_L, BT_R, BT_ZL, BT_ZR, BT_PLUS, BT_MINUS, BT_L3, BT_R3, BT_UP, BT_DOWN, BT_LEFT,
       BT_RIGHT, BT_LSTICK, BT_RSTICK, BT_DPAD, BT_VERTICAL, BT_HORIZONTAL, BT_COUNT };
static const char *const button_names[BT_COUNT] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "plus", "minus", "L3",
                                                   "R3", "up", "down", "left", "right", "lstick", "rstick", "dpad",
                                                   "vertical", "horizontal"};
static const int single_actions[BT_RIGHT + 1] = {B_A, B_B, B_X, B_Y, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS,
                                                 B_STICK_L, B_STICK_R, B_UP, B_DOWN, B_LEFT, B_RIGHT};
static const struct { int button, n, action[4], x[4], y[4]; } groups[] = {
    {BT_LSTICK, 4, {B_SL_UP, B_SL_LEFT, B_SL_DOWN, B_SL_RIGHT}, {19, 0, 19, 38}, {4, 30, 30, 30}},
    {BT_DPAD, 4, {B_UP, B_LEFT, B_RIGHT, B_DOWN}, {19, 0, 38, 19}, {0, 19, 19, 38}},
    {BT_VERTICAL, 2, {B_UP, B_DOWN}, {18, 18}, {3, 33}},
    {BT_HORIZONTAL, 2, {B_LEFT, B_RIGHT}, {3, 33}, {18, 18}},
};

static void shown_all(const InputMap *m, Shown out[BT_COUNT]) {
    memset(out, 0, sizeof(Shown) * BT_COUNT);
    for (int b = 0; b <= BT_RIGHT; b++) out[b].n = 1, out[b].key[0] = shown_key(m, single_actions[b]);
    out[BT_RSTICK].n = 1;
    out[BT_RSTICK].key[0] = KEY_MOUSE_MOVE;
    for (size_t g = 0; g < sizeof groups / sizeof groups[0]; g++) {
        Shown *s = &out[groups[g].button];
        s->group = 1;
        s->n = groups[g].n;
        for (int i = 0; i < s->n; i++)
            s->key[i] = shown_key(m, groups[g].action[i]), s->x[i] = groups[g].x[i], s->y[i] = groups[g].y[i];
    }
}

static Shown shown[BT_COUNT];

static int button_id(const char *name) {
    for (int b = 0; b < BT_COUNT; b++)
        if (!strcmp(button_names[b], name)) return b;
    return -1;
}

// Whether the prompt draws: a key it shows has an icon (xc2_prompts.py Painter.has).
static int has(int b) {
    if (b < 0) return 0;
    for (int i = 0; i < shown[b].n; i++)
        if (shown[b].key[i] != KEY_NONE && bank_icon(shown[b].key[i])) return 1;
    return 0;
}

static Pix icon(int key, int size) { return resample(bank_icon(key), size, size); }

// The icon (or group of keys) of button b at size x size (xc2_prompts.py source).
static Pix source(int b, int size) {
    const Shown *s = &shown[b];
    if (!s->group) return icon(s->key[0], size);
    double k = size / 64.0;
    Pix out = blank(size, size);
    for (int i = 0; i < s->n; i++) {
        if (s->key[i] == KEY_NONE || !bank_icon(s->key[i])) continue;
        Pix key = icon(s->key[i], rnd(28 * k));
        over(&out, &key, rnd(s->x[i] * k), rnd(s->y[i] * k));
        pix_free(&key);
    }
    return out;
}

static Pix atlas_cell(int b, int size) {
    Pix src = source(b, size), out = styled(&src, FILL, INK, INK, size / 64.0, 1);
    pix_free(&src);
    return out;
}

// The battle palette's style: a solid teal button with white letters (xc2_prompts.py Painter.palette).
static Pix palette_icon(int b, int size, int w, int h) {
    Pix src = source(b, size);
    if (w) {
        Pix r = resample(&src, w, h);
        pix_free(&src);
        src = r;
    }
    Pix out = styled(&src, TEAL, WHITE, darker(TEAL, 0.45), size / 64.0, 0);
    pix_free(&src);
    return out;
}

// ---- MIBL textures and BC1 / BC3 blocks (xc2_prompts.py Texture, Image)

enum { K_RGBA8 = 1, K_BC1, K_BC3 };

typedef struct {
    uint32_t size, w, h, fmt;
    int kind, bpe, blk;
    int64_t start;
    int wb, hb, bh;
    Pix px;
    uint8_t *dirty;   // one per element
} Tex;

static void tex_layout(Tex *t, uint64_t *used) {
    t->wb = (int)((t->w + t->blk - 1) / t->blk);
    t->hb = (int)((t->h + t->blk - 1) / t->blk);
    int half = t->hb + t->hb / 2;
    t->bh = half >= 128 ? 16 : half >= 64 ? 8 : half >= 32 ? 4 : half >= 16 ? 2 : 1;
    uint64_t gobs_x = ((uint64_t)t->wb * t->bpe + 63) / 64;
    uint64_t gobs_y = ((uint64_t)(t->hb + 7) / 8 + t->bh - 1) / t->bh * t->bh;
    *used = gobs_x * gobs_y * 512;
}

// The byte offset in the image of element (xi, y): block-linear, 64x8-byte GOBs.
static uint64_t tex_offset(const Tex *t, int xi, int y) {
    uint64_t row = (uint64_t)t->wb * t->bpe, gobs_x = (row + 63) / 64, xb = (uint64_t)xi * t->bpe, bh = (uint64_t)t->bh;
    uint64_t gob = (y / (8 * bh)) * bh * gobs_x * 512 + (xb / 64) * 512 * bh + ((y % (8 * bh)) / 8) * 512;
    return gob + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + xb % 16;
}

typedef struct { int r, g, b; } Rgb;

static int ch(Rgb p, int c) { return c == 0 ? p.r : c == 1 ? p.g : p.b; }

static Rgb rgb565(unsigned v) {
    unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;
    return (Rgb){(int)(r << 3 | r >> 2), (int)(g << 2 | g >> 4), (int)(b << 3 | b >> 2)};
}

static void bc1_palette(unsigned c0, unsigned c1, Rgb pal[4]) {
    Rgb a = rgb565(c0), b = rgb565(c1);
    pal[0] = a, pal[1] = b;
    if (c0 > c1) {
        pal[2] = (Rgb){(2 * a.r + b.r + 1) / 3, (2 * a.g + b.g + 1) / 3, (2 * a.b + b.b + 1) / 3};
        pal[3] = (Rgb){(a.r + 2 * b.r + 1) / 3, (a.g + 2 * b.g + 1) / 3, (a.b + 2 * b.b + 1) / 3};
    } else {
        pal[2] = (Rgb){(a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2};
        pal[3] = (Rgb){0, 0, 0};
    }
}

static void alpha_palette(int a0, int a1, int pal[8]) {
    pal[0] = a0, pal[1] = a1;
    if (a0 > a1)
        for (int i = 1; i < 7; i++) pal[1 + i] = ((7 - i) * a0 + i * a1 + 3) / 7;
    else {
        for (int i = 1; i < 5; i++) pal[1 + i] = ((5 - i) * a0 + i * a1 + 2) / 5;
        pal[6] = 0, pal[7] = 255;
    }
}

static void bc1_decode(const uint8_t *el, Rgba out[16]) {
    unsigned c0 = el[0] | el[1] << 8, c1 = el[2] | el[3] << 8;
    uint32_t bits = (uint32_t)el[4] | (uint32_t)el[5] << 8 | (uint32_t)el[6] << 16 | (uint32_t)el[7] << 24;
    Rgb pal[4];
    bc1_palette(c0, c1, pal);
    for (int i = 0; i < 16; i++) {
        int k = (bits >> 2 * i) & 3;
        out[i] = (Rgba){(uint8_t)pal[k].r, (uint8_t)pal[k].g, (uint8_t)pal[k].b, c0 <= c1 && k == 3 ? 0 : 255};
    }
}

static void bc3_alpha_decode(const uint8_t *el, uint8_t out[16]) {
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)el[2 + i] << 8 * i;
    int pal[8];
    alpha_palette(el[0], el[1], pal);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)pal[(bits >> 3 * i) & 7];
}

static void bc3_alpha_encode(const uint8_t alpha[16], uint8_t out[8]) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) lo = alpha[i] < lo ? alpha[i] : lo, hi = alpha[i] > hi ? alpha[i] : hi;
    memset(out, 0, 8);
    out[0] = (uint8_t)hi, out[1] = (uint8_t)lo;
    if (lo == hi) return;
    int pal[8];
    alpha_palette(hi, lo, pal);   // a0 > a1: eight steps between them
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        for (int k = 1; k < 8; k++)
            if (abs(alpha[i] - pal[k]) < abs(alpha[i] - pal[best])) best = k;
        bits |= (uint64_t)best << 3 * i;
    }
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> 8 * i);
}

// 16 RGB pixels to a 4-colour BC1 block: the endpoints are the extremes of the pixels along their principal axis,
// weighted by alpha; every pixel takes the nearest of the four colours (xc2_prompts.py bc1_encode).
static void bc1_encode(const Rgb px[16], const double wts[16], uint8_t out[8]) {
    double total = 0;
    for (int i = 0; i < 16; i++) total += wts[i];
    if (total == 0) total = 1.0;
    double mean[3], cov[3][3];
    for (int c = 0; c < 3; c++) {
        double s = 0;
        for (int i = 0; i < 16; i++) s += ch(px[i], c) * wts[i];
        mean[c] = s / total;
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 16; k++) s += (ch(px[k], i) - mean[i]) * (ch(px[k], j) - mean[j]) * wts[k];
            cov[i][j] = s;
        }
    double axis[3] = {1.0, 1.0, 1.0};
    for (int it = 0; it < 8; it++) {   // power iteration
        double v[3], n = 0;
        for (int i = 0; i < 3; i++) {
            double s = 0;
            for (int j = 0; j < 3; j++) s += cov[i][j] * axis[j];
            v[i] = s;
        }
        for (int i = 0; i < 3; i++) n = fabs(v[i]) > n ? fabs(v[i]) : n;
        if (n < 1e-9) break;
        for (int i = 0; i < 3; i++) axis[i] = v[i] / n;
    }
    int used[16], nused = 0;
    for (int i = 0; i < 16; i++)
        if (wts[i] > 0) used[nused++] = i;
    if (!nused)
        for (int i = 0; i < 16; i++) used[nused++] = i;
    int e0 = -1, e1 = -1;
    double pmax = 0, pmin = 0;
    for (int u = 0; u < nused; u++) {
        double s = 0;
        for (int c = 0; c < 3; c++) s += (ch(px[used[u]], c) - mean[c]) * axis[c];
        if (e0 < 0 || s > pmax) pmax = s, e0 = used[u];
        if (e1 < 0 || s < pmin) pmin = s, e1 = used[u];
    }
#define Q(p)                                                                                                           \
    ((unsigned)rnd((p).r * 31 / 255.0) << 11 | (unsigned)rnd((p).g * 63 / 255.0) << 5 |                               \
     (unsigned)rnd((p).b * 31 / 255.0))
    unsigned c0 = Q(px[e0]), c1 = Q(px[e1]);
#undef Q
    if (c0 < c1) {
        unsigned t = c0;
        c0 = c1, c1 = t;
    }
    uint32_t bits = 0;
    if (c0 != c1) {
        Rgb pal[4];
        bc1_palette(c0, c1, pal);
        for (int i = 0; i < 16; i++) {
            int best = 0, bd = -1;
            for (int k = 0; k < 4; k++) {
                int dr = px[i].r - pal[k].r, dg = px[i].g - pal[k].g, db = px[i].b - pal[k].b;
                int d = dr * dr + dg * dg + db * db;
                if (bd < 0 || d < bd) bd = d, best = k;
            }
            bits |= (uint32_t)best << 2 * i;
        }
    }
    out[0] = (uint8_t)c0, out[1] = (uint8_t)(c0 >> 8), out[2] = (uint8_t)c1, out[3] = (uint8_t)(c1 >> 8);
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(bits >> 8 * i);
}

// The MIBL textures of a layout, found by their footers (xc2_prompts.py textures). A footer is 40 bytes (size,
// alignment, width, height, depth, dimension, format, mip count, version, 'LBIM'); the footer ends the image's pages,
// or a page of its own when the swizzled image fills them. Returns the count, -1 for a format the tool does not write.
static int textures(const uint8_t *blob, size_t len, Tex *out, int max) {
    int n = 0;
    for (size_t at = 0; at + 4 <= len && n < max; at++) {
        if (memcmp(blob + at, "LBIM", 4)) continue;
        int64_t end = (int64_t)at + 4;
        if (end < 40) continue;
        uint32_t f[9];
        memcpy(f, blob + end - 40, sizeof f);
        Tex t = {.size = f[0], .w = f[2], .h = f[3], .fmt = f[6]};
        if (t.fmt == 37) t.kind = K_RGBA8, t.bpe = 4, t.blk = 1;
        else if (t.fmt == 66) t.kind = K_BC1, t.bpe = 8, t.blk = 4;
        else if (t.fmt == 68) t.kind = K_BC3, t.bpe = 16, t.blk = 4;
        uint64_t used = t.size;
        if (t.kind) tex_layout(&t, &used);
        t.start = used + 40 <= t.size ? end - (int64_t)t.size : end - ((int64_t)t.size + 40 + 4095) / 4096 * 4096;
        if (t.start < 0 || !t.size || !t.w || !t.h) continue;
        if (!t.kind) return -1;
        out[n++] = t;
    }
    return n;
}

static int tex_decode(Tex *t, const uint8_t *blob, size_t len) {
    t->px = blank((int)t->w, (int)t->h);
    t->dirty = calloc((size_t)t->wb * t->hb, 1);
    if (!t->dirty) rt_fatal("prompts_keys: out of memory");
    const uint8_t *data = blob + t->start;
    for (int by = 0; by < t->hb; by++)
        for (int bx = 0; bx < t->wb; bx++) {
            uint64_t o = tex_offset(t, bx, by);
            if (o + t->bpe > t->size || (uint64_t)t->start + o + t->bpe > len) return 0;
            const uint8_t *el = data + o;
            if (t->kind == K_RGBA8) {
                t->px.p[by * t->px.w + bx] = (Rgba){el[0], el[1], el[2], el[3]};
                continue;
            }
            Rgba cols[16];
            uint8_t alpha[16];
            bc1_decode(t->kind == K_BC3 ? el + 8 : el, cols);
            if (t->kind == K_BC3) bc3_alpha_decode(el, alpha);
            else
                for (int i = 0; i < 16; i++) alpha[i] = cols[i].a;
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < (int)t->w && y < (int)t->h) cols[i].a = alpha[i], t->px.p[y * t->px.w + x] = cols[i];
            }
        }
    return 1;
}

// Replace the pixels at x0, y0 with icon (xc2_prompts.py Image.paste).
static void paste(Tex *t, int x0, int y0, const Pix *icon) {
    for (int y = 0; y < icon->h; y++)
        for (int x = 0; x < icon->w; x++) {
            int px = x0 + x, py = y0 + y;
            if (px < 0 || py < 0 || px >= t->px.w || py >= t->px.h) continue;
            t->px.p[py * t->px.w + px] = icon->p[y * icon->w + x];
            t->dirty[(py / t->blk) * t->wb + px / t->blk] = 1;
        }
}

static void tex_encode(const Tex *t, uint8_t *blob) {
    int w = t->px.w, h = t->px.h;
    for (int by = 0; by < t->hb; by++)
        for (int bx = 0; bx < t->wb; bx++) {
            if (!t->dirty[by * t->wb + bx]) continue;
            uint8_t *el = blob + t->start + tex_offset(t, bx, by);
            if (t->kind == K_RGBA8) {
                Rgba p = t->px.p[by * w + bx];
                el[0] = p.r, el[1] = p.g, el[2] = p.b, el[3] = p.a;
                continue;
            }
            Rgb rgb[16];
            double wts[16];
            uint8_t alpha[16];
            for (int i = 0; i < 16; i++) {
                int y = by * 4 + i / 4, x = bx * 4 + i % 4;
                Rgba p = t->px.p[(y < h - 1 ? y : h - 1) * w + (x < w - 1 ? x : w - 1)];
                rgb[i] = (Rgb){p.r, p.g, p.b}, wts[i] = p.a / 255.0, alpha[i] = p.a;
            }
            if (t->kind == K_BC3) {
                bc3_alpha_encode(alpha, el);
                bc1_encode(rgb, wts, el + 8);
            } else
                bc1_encode(rgb, wts, el);
        }
}

// ---- the layouts (xc2_prompts.py LAYOUTS and their redraw functions); imgs are the layout's textures in file order

// button.wilay and font_icon.wilay: an atlas of 8x4 cells; (column, row) of each button. ZL2 and ZR2 are ZL and ZR
// again; SL, SR, home and capture have no key and keep the game's art.
static const struct { const char *button; int col, row; } cells[] = {
    {"dpad", 0, 0}, {"capture", 1, 0}, {"home", 2, 0}, {"minus", 3, 0}, {"plus", 4, 0}, {"X", 5, 0}, {"Y", 6, 0},
    {"A", 7, 0}, {"B", 0, 1}, {"L3", 1, 1}, {"R3", 2, 1}, {"lstick", 3, 1}, {"rstick", 4, 1}, {"L", 5, 1},
    {"R", 6, 1}, {"ZL", 7, 1}, {"ZR", 0, 2}, {"ZL", 3, 2}, {"ZR", 4, 2}, {"up", 0, 3}, {"down", 1, 3},
    {"left", 2, 3}, {"right", 3, 3}, {"vertical", 4, 3}, {"horizontal", 5, 3},
};

static int redraw_atlas(Tex *imgs, int n) {
    Tex *img = &imgs[n - 1];
    int size = img->px.w / 8;
    for (size_t c = 0; c < sizeof cells / sizeof cells[0]; c++) {
        int b = button_id(cells[c].button);
        if (!has(b)) continue;
        Pix icon = atlas_cell(b, size);
        paste(img, cells[c].col * size, cells[c].row * size, &icon);
        pix_free(&icon);
    }
    return 1;
}

static const int face_buttons[4] = {BT_A, BT_B, BT_X, BT_Y};

// mnu032_button_icon(_new).wilay: the battle palette's A B X Y, 72-pixel cells in a row.
static int redraw_palette(Tex *imgs, int n) {
    for (int k = 0; k < 4; k++) {
        if (!has(face_buttons[k])) continue;
        Pix icon = palette_icon(face_buttons[k], 72, 0, 0);
        paste(&imgs[n - 1], k * 72, 0, &icon);
        pix_free(&icon);
    }
    return 1;
}

// mnu032_button_push_main.wilay: textures 2 to 9 are A A B B X X Y Y as discs tilted away, each over its rim; the
// button lies flat at 56x34 over a darker copy 7 pixels lower.
static int redraw_push(Tex *imgs, int n) {
    if (n < 10) return 0;
    for (int k = 0; k < 8; k++) {
        int b = face_buttons[k / 2];
        if (!has(b)) continue;
        Tex *img = &imgs[2 + k];
        Pix top = palette_icon(b, 56, 56, 34), side = blank(top.w, top.h);
        for (int i = 0; i < top.w * top.h; i++) side.p[i] = darker(top.p[i], 0.6), side.p[i].a = top.p[i].a;
        Pix canvas = blank(img->px.w, img->px.h);
        over(&canvas, &side, (img->px.w - 56) / 2, 26);
        over(&canvas, &top, (img->px.w - 56) / 2, 19);
        paste(img, 0, 0, &canvas);
        pix_free(&top);
        pix_free(&side);
        pix_free(&canvas);
    }
    return 1;
}

// mnu032_btn_circle.wilay: a white B button (radius 37 about the centre) inside a ring; the button is drawn again over
// the ring's inside.
static int redraw_circle(Tex *imgs, int n) {
    if (!has(BT_B)) return 1;
    Tex *img = &imgs[n - 1];
    int w = img->px.w, h = img->px.h, cx = w / 2, cy = h / 2;
    if (cy < 48 || cx < 48 || cx + 48 > w || cy + 48 > h) return 0;
    Rgba ring = img->px.p[(cy - 46) * w + cx];   // the ring's inside, above the button
    Pix base = blank(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double dx = x - cx + 0.5, dy = y - cy + 0.5;
            base.p[y * w + x] = dx * dx + dy * dy <= 40 * 40 ? ring : img->px.p[y * w + x];
        }
    Pix b = atlas_cell(BT_B, 96), crop = blank(96, 96);
    over(&base, &b, cx - 48, cy - 48);
    for (int y = 0; y < 96; y++) memcpy(&crop.p[y * 96], &base.p[(cy - 48 + y) * w + cx - 48], 96 * sizeof(Rgba));
    paste(img, cx - 48, cy - 48, &crop);
    pix_free(&base);
    pix_free(&b);
    pix_free(&crop);
    return 1;
}

// mnu001_btn.wilay: the plus button, 32x32.
static int redraw_plus(Tex *imgs, int n) {
    if (!has(BT_PLUS)) return 1;
    Pix icon = atlas_cell(BT_PLUS, 32);
    paste(&imgs[n - 1], 0, 0, &icon);
    pix_free(&icon);
    return 1;
}

static const struct { const char *path; int (*redraw)(Tex *imgs, int n); } layouts[] = {
    {"menu/image/button.wilay", redraw_atlas},
    {"menu/image/font_icon.wilay", redraw_atlas},
    {"menu/image/mnu032_button_icon.wilay", redraw_palette},
    {"menu/image/mnu032_button_icon_new.wilay", redraw_palette},
    {"menu/image/mnu032_button_push_main.wilay", redraw_push},
    {"menu/image/mnu032_btn_circle.wilay", redraw_circle},
    {"menu/image/mnu001_btn.wilay", redraw_plus},
};

enum { MAX_TEX = 32 };

// The layout at path redrawn into a malloc'd copy (*size its length); NULL when it has not the expected shape.
static uint8_t *redraw_file(const char *path, int (*redraw)(Tex *, int), uint64_t *size) {
    uint8_t *blob = plat_file_read(path, size);
    if (!blob) return NULL;
    Tex texs[MAX_TEX];
    int n = textures(blob, (size_t)*size, texs, MAX_TEX), ok = n > 0;
    int decoded = 0;
    for (; ok && decoded < n; decoded++) ok = tex_decode(&texs[decoded], blob, (size_t)*size);
    if (ok) ok = redraw(texs, n);
    for (int i = 0; ok && i < n; i++) tex_encode(&texs[i], blob);
    for (int i = 0; i < decoded; i++) pix_free(&texs[i].px), free(texs[i].dirty);
    if (!ok) {
        free(blob);
        return NULL;
    }
    return blob;
}

int prompts_keys_draw(const char *prompts_dir, const char *out_dir) {
    InputMap *m = malloc(sizeof *m);
    if (!m) rt_fatal("prompts_keys: out of memory");
    input_map_load(m);
    Shown defaults[BT_COUNT];
    shown_all(input_map_defaults(), defaults);
    shown_all(m, shown);
    free(m);
    if (!memcmp(defaults, shown, sizeof shown)) return 0;
    char path[1400];
    snprintf(path, sizeof path, "%s/keys/keys.bin", prompts_dir);
    uint64_t size = 0;
    uint8_t *d = plat_file_read(path, &size);
    if (!d) {
        rt_log("prompts: no %s (a setup before remapped keyboard prompts: run it again); the default keys' prompts",
               path);
        return 0;
    }
    int ok = bank_load(d, size);
    free(d);
    if (!ok) rt_log("prompts: %s is not a key bank; the default keys' prompts", path);
    for (int b = 0; ok && b < BT_COUNT; b++)
        for (int i = 0; i < shown[b].n; i++)
            if (shown[b].key[i] != KEY_NONE && !bank_icon(shown[b].key[i]))
                rt_log("prompts: no icon for key %s; the %s prompt is drawn without it", key_icon_name(shown[b].key[i]),
                       button_names[b]);
    for (size_t l = 0; ok && l < sizeof layouts / sizeof layouts[0]; l++) {
        snprintf(path, sizeof path, "%s/keys/%s", prompts_dir, layouts[l].path);
        uint8_t *blob = redraw_file(path, layouts[l].redraw, &size);
        if (!blob) {
            rt_log("prompts: %s missing or not the expected layout; the default keys' prompts", path);
            ok = 0;
            break;
        }
        snprintf(path, sizeof path, "%s/%s", out_dir, layouts[l].path);
        char dir[1400];
        snprintf(dir, sizeof dir, "%s", path);
        char *sep = plat_last_sep(dir);
        if (sep) *sep = 0;
        plat_file_mkdir_p(dir);
        ok = plat_file_write(path, blob, (size_t)size);
        free(blob);
        if (!ok) rt_log("prompts: cannot write %s; the default keys' prompts", path);
    }
    bank_free();
    return ok;
}
