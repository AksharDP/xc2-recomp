// xc2.exe --probe-vkexec: the Vulkan replay, through gfx_vulkan_backend. Sync part: empty
// submissions get consecutive fence values; fence_wait and fence_done agree; completion callbacks run in submit order on
// the completion thread; a guest-memory word a callback writes is visible once fence_wait returns; finish drains every
// submission; a present of a cleared vk_texture.c image (layout GENERAL) reads back exactly through the swapchain
// capture. Clear part: a hand-built op stream (SET_RT to an RGBA8 and a D32 texture, whole-target clears,
// then scissored clears) through submit reads back exactly with read_texels.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#define SUBMITS 100
#define TAIL 10
#define TEX_W 256
#define TEX_H 144
#define CHECK(cond, ...)                                                                                                \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("probe-vkexec: FAILED: ");                                                                          \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static uint32_t main_tid;
static uint64_t ran[SUBMITS + TAIL + 8];
static long nran, on_main;

static void note(void *arg) {   // completion thread
    if (nran < (long)(sizeof ran / sizeof ran[0])) ran[nran] = (uint64_t)(uintptr_t)arg;
    nran++;
    if (plat_thread_id() == main_tid) on_main++;
}

typedef struct { volatile uint32_t *word; uint32_t value; } WordWrite;
static void write_word(void *arg) {
    const WordWrite *w = arg;
    *w->word = w->value;
}

static long ran_now(void) { return __atomic_load_n(&nran, __ATOMIC_ACQUIRE); }

static int test_sync(const GfxBackend *b, uint8_t *guest, uint64_t *first, uint64_t *last) {
    // An empty recording as the front end hands it over: {OP_BEGIN, OP_END}.
    static op_hdr empty[2] = {{OP_BEGIN, sizeof(op_hdr)}, {OP_END, sizeof(op_hdr)}};
    uint64_t handles[2] = {(uint64_t)(uintptr_t)empty, (uint64_t)(uintptr_t)empty};
    uint64_t prev = b->fence_done();
    *first = prev + 1;
    for (int i = 0; i < SUBMITS; i++) {
        vk_completion_add(prev + 1, note, (void *)(uintptr_t)(prev + 1));   // queued before the value exists, as vk_exec.c does
        uint64_t f = b->submit(i % 3 ? handles : NULL, i % 3);
        CHECK(f == prev + 1, "submission %d got fence %llu after %llu", i, (unsigned long long)f, (unsigned long long)prev);
        CHECK(b->fence_done() <= f, "fence_done %llu ahead of the newest submission %llu",
              (unsigned long long)b->fence_done(), (unsigned long long)f);
        CHECK(!b->fence_wait(f + 1, 0), "fence_wait of a value never submitted returned 1");
        if (i % 10 == 9) {
            CHECK(b->fence_wait(f, 5000000000ull), "fence_wait(%llu) timed out", (unsigned long long)f);
            CHECK(b->fence_done() >= f, "fence_done %llu after fence_wait(%llu)", (unsigned long long)b->fence_done(),
                  (unsigned long long)f);
            CHECK(ran_now() == i + 1, "%ld callbacks ran after fence_wait of submission %d", ran_now(), i);
        }
        prev = f;
    }
    // A guest-memory word written by a completion callback is visible to the waiter.
    volatile uint32_t *word = (volatile uint32_t *)(guest + 0x1000);
    *word = 0;
    static WordWrite ww;
    ww = (WordWrite){word, 0x5EC0DE01u};
    vk_completion_add(prev + 1, write_word, &ww);
    uint64_t f = b->submit(NULL, 0);
    CHECK(f == prev + 1, "word submission got fence %llu", (unsigned long long)f);
    CHECK(b->fence_wait(f, 5000000000ull), "fence_wait of the word submission timed out");
    CHECK(*word == 0x5EC0DE01u, "guest word is %#x after fence_wait", *word);
    prev = f;
    // finish drains everything.
    for (int i = 0; i < TAIL; i++) {
        vk_completion_add(prev + 1, note, (void *)(uintptr_t)(prev + 1));
        prev = b->submit(handles, 2);
    }
    b->finish();
    CHECK(b->fence_done() == prev, "fence_done %llu after finish, newest %llu", (unsigned long long)b->fence_done(),
          (unsigned long long)prev);
    CHECK(ran_now() == SUBMITS + TAIL, "%ld of %d callbacks ran after finish", ran_now(), SUBMITS + TAIL);
    for (int i = 0; i < SUBMITS + TAIL; i++) {
        uint64_t want = *first + (uint64_t)i + (i >= SUBMITS);   // the word submission sits between the two runs
        CHECK(ran[i] == want, "callback %d ran for fence %llu, expected %llu", i, (unsigned long long)ran[i],
              (unsigned long long)want);
    }
    CHECK(!on_main, "%ld callbacks ran on the submitting thread", on_main);
    *last = prev;
    return 0;
}

static uint8_t to_byte(float f) { return (uint8_t)(f * 255.0f + 0.5f); }

static int test_present(const GfxBackend *b, uint8_t *guest, int *w_out, int *h_out) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x100000, .size = 0x100000,
                   .game_mem = guest + 0x100000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool");
    tex_t t;
    memset(&t, 0, sizeof t);
    t.magic = NVN_TEX_MAGIC;
    t.d.target = NVN_TEXTURE_TARGET_2D, t.d.format = NVN_FORMAT_RGBA8, t.d.width = TEX_W, t.d.height = TEX_H;
    t.d.depth = 1, t.d.levels = 1;
    nvn_tex_layout(&t.d, &t.L);
    CHECK(t.L.size <= pool.size, "texture storage %#llx exceeds the pool", (unsigned long long)t.L.size);
    t.pool = &pool, t.off = 0, t.mem = pool.mem, t.serial = 1;
    const VkTexImage *img = vk_texture_get(&t);
    CHECK(img && img->format == VK_FORMAT_R8G8B8A8_UNORM, "vk_texture_get of an RGBA8 texture");
    vk_texture_gpu_written(&t);   // the clear below is its content, not the storage
    const float rgba[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    VkClearColorValue cv;
    memcpy(cv.float32, rgba, sizeof cv.float32);
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCommandBuffer cb = vk_upload_begin();
    vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rng);
    vk_upload_end();

    uint64_t frames = gfx_stats().frames;
    // the exact colour check needs the bilinear blit: FSR's approximate reciprocals move a flat colour by 1/255
    plat_setenv("XC2_UPSCALER", "bilinear");
    vk_present_capture(1);
    const int crop[4] = {0, 0, TEX_W, TEX_H};
    b->present(&t, crop, 0);
    b->finish();   // the queued present is submitted and, with capture on, read back
    const uint8_t *px;
    int w, h;
    VkFormat fmt;
    CHECK(vk_present_captured(&px, &w, &h, &fmt), "the present was not captured");
    CHECK(gfx_stats().frames == frames + 1, "frame counter %llu after one present from %llu",
          (unsigned long long)gfx_stats().frames, (unsigned long long)frames);
    uint8_t want[4] = {0, 0, 0, 255};
    if (fmt == VK_FORMAT_B8G8R8A8_UNORM) want[0] = to_byte(rgba[2]), want[1] = to_byte(rgba[1]), want[2] = to_byte(rgba[0]);
    else if (fmt == VK_FORMAT_R8G8B8A8_UNORM) want[0] = to_byte(rgba[0]), want[1] = to_byte(rgba[1]), want[2] = to_byte(rgba[2]);
    else CHECK(0, "swapchain format %d is not UNORM RGBA/BGRA", (int)fmt);
    uint32_t wv, bar;
    memcpy(&wv, want, 4);
    memcpy(&bar, (const uint8_t[4]){0, 0, 0, 255}, 4);
    // A window of another aspect ratio than the 16:9 texture shows it in gfx_fit_rect's rectangle, black bars around
    int to[4];
    gfx_fit_rect(TEX_W, TEX_H, w, h, to);
    const uint32_t *p = (const uint32_t *)px;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int in = x >= to[0] && x < to[0] + to[2] && y >= to[1] && y < to[1] + to[3];
            uint32_t v = p[(long)y * w + x], expect = in ? wv : bar;
            CHECK(v == expect, "presented pixel (%d, %d) is %08x, expected %08x (picture at %d,%d %dx%d of %dx%d)", x, y,
                  v, expect, to[0], to[1], to[2], to[3], w, h);
        }
    vk_present_capture(0);
    b->texture_release(&t);
    b->pool_fini(&pool);
    *w_out = w, *h_out = h;
    return 0;
}

// ---- clear part: a hand-built op stream through submit, read back with read_texels.

static uint8_t *put(uint8_t *p, uint32_t op, const void *payload, uint32_t n) {
    op_hdr h = {op, (uint32_t)((sizeof h + n + 7) & ~7u)};
    memset(p, 0, h.size);
    memcpy(p, &h, sizeof h);
    if (n) memcpy(p + sizeof h, payload, n);
    return p + h.size;
}

static void init_tex_wh(tex_t *t, pool_t *pool, int64_t off, int format, int w, int h) {
    memset(t, 0, sizeof *t);
    t->magic = NVN_TEX_MAGIC;
    t->d.target = NVN_TEXTURE_TARGET_2D, t->d.format = format, t->d.width = w, t->d.height = h;
    t->d.depth = 1, t->d.levels = 1;
    nvn_tex_layout(&t->d, &t->L);
    t->pool = pool, t->off = off, t->mem = pool->mem + off, t->serial = 100 + (uint64_t)off;
}
static void init_tex(tex_t *t, pool_t *pool, int64_t off, int format) { init_tex_wh(t, pool, off, format, TEX_W, TEX_H); }

// Pixel of the RGBA8 target after the stream: the scissored second clear inside the rectangle, the first elsewhere.
#define SC_X 40
#define SC_Y 24
#define SC_W 64
#define SC_H 32

static int test_clear(const GfxBackend *b, uint8_t *guest, int *n_inside) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x400000, .size = 0x200000,
                   .game_mem = guest + 0x400000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the clear part");
    static tex_t color, depth;   // payload texture pointers live as long as the submission
    init_tex(&color, &pool, 0, NVN_FORMAT_RGBA8);
    init_tex(&depth, &pool, 0x100000, NVN_FORMAT_DEPTH32F);
    CHECK(color.L.size <= 0x100000 && depth.L.size <= 0x100000, "clear-part textures exceed their pool halves");

    static _Alignas(8) uint8_t stream[4096];
    uint8_t *p = stream;
    p = put(p, OP_BEGIN, NULL, 0);
    p_set_rt rt;
    memset(&rt, 0, sizeof rt);
    rt.n = 1, rt.color[0] = &color, rt.depth = &depth;
    p = put(p, OP_SET_RT, &rt, sizeof rt);
    const p_clear_color c0 = {0, 0xf, {0.2f, 0.4f, 0.6f, 1.0f}};
    p = put(p, OP_CLEAR_COLOR, &c0, sizeof c0);   // whole target outside a pass: vkCmdClearColorImage
    const p_clear_ds d0 = {0.5f, 1, 0, 0xff};
    p = put(p, OP_CLEAR_DS, &d0, sizeof d0);      // vkCmdClearDepthStencilImage
    const p_scissor sc = {SC_X, SC_Y, SC_W, SC_H};
    p = put(p, OP_SCISSOR, &sc, sizeof sc);
    const p_clear_color c1 = {0, 0xf, {1.0f, 0.0f, 0.0f, 1.0f}};
    p = put(p, OP_CLEAR_COLOR, &c1, sizeof c1);   // scissored: a pass and vkCmdClearAttachments
    const p_clear_ds d1 = {0.25f, 1, 0, 0xff};
    p = put(p, OP_CLEAR_DS, &d1, sizeof d1);
    const p_scissor full = {0, 0, 1 << 20, 1 << 20};
    p = put(p, OP_SCISSOR, &full, sizeof full);   // leaves the replay's scissor unbounded for later submissions
    p = put(p, OP_END, NULL, 0);
    CHECK(p <= stream + sizeof stream, "op stream overflow");

    uint64_t handle = (uint64_t)(uintptr_t)stream;
    uint64_t f = b->submit(&handle, 1);
    CHECK(b->fence_wait(f, 5000000000ull), "fence_wait of the clear submission timed out");

    static uint32_t px[TEX_W * TEX_H];
    static float dpx[TEX_W * TEX_H];
    NVNcopyRegion r = {0, 0, 0, TEX_W, TEX_H, 1};
    b->read_texels(&color, NULL, &r, (uint8_t *)px, TEX_W * 4, TEX_W * TEX_H * 4);
    b->read_texels(&depth, NULL, &r, (uint8_t *)dpx, TEX_W * 4, TEX_W * TEX_H * 4);
    const uint8_t out_b[4] = {to_byte(0.2f), to_byte(0.4f), to_byte(0.6f), 255}, in_b[4] = {255, 0, 0, 255};
    uint32_t out_v, in_v;
    memcpy(&out_v, out_b, 4), memcpy(&in_v, in_b, 4);
    int inside = 0;
    for (int y = 0; y < TEX_H; y++)
        for (int x = 0; x < TEX_W; x++) {
            int in = x >= SC_X && x < SC_X + SC_W && y >= SC_Y && y < SC_Y + SC_H;
            inside += in;
            uint32_t want = in ? in_v : out_v;
            float dwant = in ? 0.25f : 0.5f;
            CHECK(px[y * TEX_W + x] == want, "colour texel (%d,%d) is %08x, expected %08x", x, y, px[y * TEX_W + x], want);
            CHECK(dpx[y * TEX_W + x] == dwant, "depth texel (%d,%d) is %g, expected %g", x, y, dpx[y * TEX_W + x], dwant);
        }
    b->texture_release(&color);
    b->texture_release(&depth);
    b->pool_fini(&pool);
    *n_inside = inside;
    return 0;
}

// ---- draw part: the pipeline self-test, then a hand-built stream that clears an RGBA8 target, binds a census
// vertex + fragment pair and draws one triangle; a census compute shader is dispatched. The census (GFX_CENSUS_DIR,
// tools/shader_census.py) is read the way vk_pipeline_selftest reads it: a pair is a fragment shader and a vertex
// shader of the same source file.

void vk_exec_draw_shutdown(void);   // vk_exec_draw.c: the stand-in resources; the device must be idle
void vk_exec_xfer_shutdown(void);   // vk_exec_xfer.c: the occlusion query pool; the device must be idle

typedef struct { const char *key, *c1, *src; size_t c1_len, src_len; int stage; uint32_t local[3]; } DrawEnt;

static const char *dr_field(const char *from, const char *to, const char *name, size_t *len) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\": \"", name);
    const char *p = strstr(from, pat);
    if (!p || p >= to) return NULL;
    p += strlen(pat);
    const char *e = memchr(p, '"', (size_t)(to - p));
    if (!e) return NULL;
    *len = (size_t)(e - p);
    return p;
}

static int dr_cmp(const void *x, const void *y) {
    const DrawEnt *a = x, *b = y;
    size_t n = a->src_len < b->src_len ? a->src_len : b->src_len;
    int c = memcmp(a->src, b->src, n);
    if (c) return c;
    if (a->src_len != b->src_len) return a->src_len < b->src_len ? -1 : 1;
    return a->stage - b->stage;
}

static int dr_parse(const char *jobs, DrawEnt *out, int max) {
    int n = 0;
    for (const char *p = jobs; n < max && (p = strstr(p, "\"job\": {")); p += 8) {
        const char *end = strstr(p, "\"origin\": ");
        if (!end) break;
        size_t kl, sl, cl, rl;
        const char *k = dr_field(p, end, "key", &kl), *s = dr_field(p, end, "stage", &sl), *c = dr_field(p, end, "c1", &cl),
                   *r = dr_field(p, end, "source", &rl);
        if (!k || !s || !c || !r || kl != 64) continue;
        DrawEnt *e = &out[n++];
        e->key = k, e->c1 = c, e->c1_len = cl / 2, e->src = r, e->src_len = rl;
        e->stage = !strncmp(s, "vs", sl) ? GFX_STAGE_VS : !strncmp(s, "ps", sl) ? GFX_STAGE_FS : GFX_STAGE_CS;
        e->local[0] = e->local[1] = e->local[2] = 1;
        const char *l = strstr(p, "\"local\": [");
        if (l && l < end) {
            l += 10;
            for (int i = 0; i < 3; i++) {
                while (*l == ' ' || *l == '\n' || *l == '\r' || *l == ',') l++;
                e->local[i] = (uint32_t)strtoul(l, (char **)&l, 10);
            }
        }
    }
    return n;
}

static char *dr_read(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *size = n;
    return b;
}

static int dr_root(char *root, size_t n) {
    char exe[PLAT_PATH_MAX * 2];
    int len = plat_exe_path(exe, sizeof exe);
    if (!len || len >= sizeof exe) return 0;
    for (char *cut = plat_last_sep(exe); cut; cut = plat_last_sep(exe)) {
        *cut = 0;
        char p[PLAT_PATH_MAX * 2 + 40];
        snprintf(p, sizeof p, "%s/" GFX_CENSUS_DIR "/jobs.json", exe);
        if (plat_file_exists(p)) return snprintf(root, n, "%s", exe) < (int)n;
    }
    return 0;
}

static int dr_hex(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

// A program of one census shader as the game hands it to NVN: control section, code with its 0x30-byte header.
static prog_t *dr_prog(const char *root, const DrawEnt *e[], int n) {
    static uint64_t serial = 0x7000;
    prog_t *p = calloc(1, sizeof *p);
    p->magic = NVN_PROG_MAGIC, p->serial = ++serial;
    for (int i = 0; i < n; i++) {
        char path[PLAT_PATH_MAX * 2 + 128];
        snprintf(path, sizeof path, "%s/" GFX_CENSUS_DIR "/code/%.64s.code", root, e[i]->key);
        long body;
        char *raw = dr_read(path, &body);
        if (!raw) return free(p), NULL;
        size_t skip = e[i]->stage == GFX_STAGE_CS ? 0 : 0x30, total = skip + (size_t)body;
        uint8_t *code = calloc(1, total), *ctl = calloc(1, 0x880);
        memcpy(code + skip, raw, (size_t)body);
        free(raw);
        uint32_t c1_off = (uint32_t)(total - e[i]->c1_len), length = (uint32_t)total;
        uint32_t nvn_stage = e[i]->stage == GFX_STAGE_VS ? 0 : e[i]->stage == GFX_STAGE_FS ? 1 : 5;
        memcpy(ctl + 0x700, &c1_off, 4), memcpy(ctl + 0x704, &length, 4), memcpy(ctl + 0x714, &nvn_stage, 4);
        if (nvn_stage == 5) memcpy(ctl + 0x748, e[i]->local, 12);
        p->sd[i].data = (uint64_t)(uintptr_t)code, p->sd[i].control = ctl;
        p->nstage++;
    }
    return p;
}

#define DR_ATTRS 8
#define DR_STRIDE (DR_ATTRS * 16)

static int layouts_equal;   // texels compared equal between the two layouts (0: not compared)
static int ind_draw_texels, ind_dispatches;   // indirect draws' texels compared, indirect dispatches run

// The draw part's stream up to the draw: target cleared, viewport, program, three RGBA32F vertices at 0x80000 of the
// pool, identity uniforms at 0x90000.
static uint8_t *draw_prefix(uint8_t *q, prog_t *prog, const pool_t *pool, tex_t *color) {
    q = put(q, OP_BEGIN, NULL, 0);
    p_set_rt rt;
    memset(&rt, 0, sizeof rt);
    rt.n = 1, rt.color[0] = color;
    q = put(q, OP_SET_RT, &rt, sizeof rt);
    const p_clear_color c0 = {0, 0xf, {0.2f, 0.4f, 0.6f, 1.0f}};
    q = put(q, OP_CLEAR_COLOR, &c0, sizeof c0);
    p_viewports vp;
    memset(&vp, 0, sizeof vp);
    vp.n = 1, vp.v[0][2] = color->d.width, vp.v[0][3] = color->d.height, vp.v[0][5] = 1.0f;   // the target's own size
    q = put(q, OP_VIEWPORTS, &vp, sizeof vp);
    const p_bind_program bp = {prog, 3};
    q = put(q, OP_BIND_PROGRAM, &bp, sizeof bp);
    p_bind_vattr va;
    memset(&va, 0, sizeof va);
    va.n = DR_ATTRS;
    for (int a = 0; a < DR_ATTRS; a++) {
        va_st s = {NVN_FORMAT_RGBA32F, (uint32_t)(16 * a), 0};
        memcpy(&va.st[a], &s, sizeof s);
    }
    q = put(q, OP_BIND_VATTR, &va, sizeof va);
    p_bind_vstream vs;
    memset(&vs, 0, sizeof vs);
    vs.n = 1, vs.st[0].stride = DR_STRIDE;
    q = put(q, OP_BIND_VSTREAM, &vs, sizeof vs);
    p_bind_vbuf vb;
    memset(&vb, 0, sizeof vb);
    vb.n = 1, vb.r[0].address = (uint64_t)(uintptr_t)pool->mem + 0x80000, vb.r[0].size = 3 * DR_STRIDE;
    q = put(q, OP_BIND_VBUF, &vb, sizeof vb);
    for (int stage = 0; stage < 2; stage++)
        for (int i = 0; i < 4; i++) {
            p_bind_buf bu = {stage, i, (uint64_t)(uintptr_t)pool->mem + 0x90000, 4096};
            q = put(q, OP_BIND_UBO, &bu, sizeof bu);
        }
    return q;
}

// ---- feedback part: a draw whose fragment shader samples the target it writes (vk_exec_draw.c feedback_snapshot).
// The draw reads that target as it was before the draw, overlapping fragments included. The reference draws the same six
// vertices into a second target while it samples a third one holding the same colour: the two must agree texel for
// texel. Only pairs whose output changes when the sampled colour changes are compared, so a copy that held the wrong
// contents would show. The narrow and full sync modes must agree too. The copy-off draw (XC2_FEEDBACK_SNAP=0) is reported, not required:
// it differs from the reference only where the GPU's live reads see the draw's own writes, which an NVIDIA GPU does not
// show (the AMD cloud-sea report does), so on NVIDIA this part cannot show that the copy is what makes the match.

#define FB_VERTS 6
#define FB_TEX_TARGET 0x3F000u   // texture slot ids of the feedback target and of the reference's sampled target
#define FB_TEX_REF 0x3F001u
#define FB_POOL_REF 0x1000000u   // guest offsets of the reference's sampled and drawn targets (the heap is 32 MiB)
#define FB_POOL_OUT 0x1040000u
#define FB_POOL_SIZE 0x40000u

// Six vertices of two overlapping triangles at 0x81000 of the draw part's pool (attribute 0 the position).
static void fb_vertices(const pool_t *pool) {
    static const float pos[FB_VERTS][4] = {{-0.8f, -0.8f, 0.0f, 1.0f}, {0.8f, -0.8f, 0.0f, 1.0f}, {0.0f, 0.8f, 0.0f, 1.0f},
                                           {-0.6f, -0.6f, 0.0f, 1.0f}, {0.9f, -0.5f, 0.0f, 1.0f}, {0.1f, 0.9f, 0.0f, 1.0f}};
    float *vtx = (float *)(pool->mem + 0x81000);
    for (int v = 0; v < FB_VERTS; v++)
        for (int a = 0; a < DR_ATTRS; a++)
            for (int c = 0; c < 4; c++) vtx[(v * DR_ATTRS + a) * 4 + c] = a == 0 ? pos[v][c] : c == 3 ? 1.0f : 0.5f;
}

// The six vertices drawn with every fragment-stage texture slot bound to texture id tex; no prefix and no end.
static uint8_t *fb_draw_body(uint8_t *q, const pool_t *pool, uint32_t tex) {
    p_bind_vbuf vb;
    memset(&vb, 0, sizeof vb);
    vb.n = 1, vb.r[0].address = (uint64_t)(uintptr_t)pool->mem + 0x81000, vb.r[0].size = FB_VERTS * DR_STRIDE;
    q = put(q, OP_BIND_VBUF, &vb, sizeof vb);
    p_bind_handles bt;
    memset(&bt, 0, sizeof bt);
    bt.stage = 1, bt.first = 0, bt.n = 8;   // NVN stage 1, the fragment stage (test_draw maps census stages the same way)
    for (int i = 0; i < 8; i++) bt.h[i] = tex;   // no sampler id: the default sampler
    q = put(q, OP_BIND_TEX, &bt, sizeof bt);
    const p_draw d = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, FB_VERTS, 0, 0};
    return put(q, OP_DRAW, &d, sizeof d);
}

// The draw part's prefix into color, then fb_draw_body, ended.
static uint8_t *fb_draw(uint8_t *q, prog_t *prog, const pool_t *pool, tex_t *color, uint32_t tex) {
    q = draw_prefix(q, prog, pool, color);
    q = fb_draw_body(q, pool, tex);
    return put(q, OP_END, NULL, 0);
}

static uint8_t fb_stream[8192];   // this part's op stream

// Submits fb_stream and waits for it.
static int fb_run(const GfxBackend *b) {
    uint64_t handle = (uint64_t)(uintptr_t)fb_stream;
    uint64_t f = b->submit(&handle, 1);
    return b->fence_wait(f, 20000000000ull);
}

static void fb_read(const GfxBackend *b, tex_t *t, uint32_t *px) {
    const NVNcopyRegion r = {0, 0, 0, TEX_W, TEX_H, 1};
    b->read_texels(t, NULL, &r, (uint8_t *)px, TEX_W * 4, TEX_W * TEX_H * 4);
}

static void fb_read_wh(const GfxBackend *b, tex_t *t, uint32_t *px, int w, int h) {
    const NVNcopyRegion r = {0, 0, 0, (uint32_t)w, (uint32_t)h, 1};
    b->read_texels(t, NULL, &r, (uint8_t *)px, (uint64_t)w * 4, (uint64_t)w * h * 4);
}

// The reference's sampled target is cleared to the draw part's colour (draw_prefix makes it) or to this one.
static const float fb_c1[4] = {0.9f, 0.1f, 0.3f, 1.0f};

// A submission that only clears t to rgba (a stream of its own: two clears of one target in one stream are a hazard).
static uint8_t *fb_clear_only(uint8_t *q, tex_t *t, const float rgba[4]) {
    q = put(q, OP_BEGIN, NULL, 0);
    p_set_rt rt;
    memset(&rt, 0, sizeof rt);
    rt.n = 1, rt.color[0] = t;
    q = put(q, OP_SET_RT, &rt, sizeof rt);
    const p_clear_color c = {0, 0xf, {rgba[0], rgba[1], rgba[2], rgba[3]}};
    return put(q, OP_CLEAR_COLOR, &c, sizeof c);
}

static int fb_differ(const uint32_t *a, const uint32_t *b) {
    int n = 0;
    for (int k = 0; k < TEX_W * TEX_H; k++) n += a[k] != b[k];
    return n;
}

// Feedback copies in flight: more keys than the pool holds (SNAP_MAX, vk_exec_draw.c), so the pool must drop entries that
// submissions still in flight use (snap_drop defers their images until those submissions finish). EV_N targets of
// different widths (each a key of its own) are drawn with the feedback draw of the pair found by test_feedback. First
// each draw is run and waited for, which gives its reference; then all are submitted, EV_GROUP targets at a time, with
// no wait between submissions. Every target must then read what its reference read.
#define FB_TEX_EV 0x3F100u   // texture ids of these targets: FB_TEX_EV + k
#define EV_N 64
#define EV_GROUP 32              // the snapshot cache holds 32 copies (SNAP_MAX): one submission draws at most this many
#define EV_BASE 0x1200000ull
#define EV_POOL 0x800000ull
#define EV_W(k) (32 + 2 * (k))   // widths 32 to 158
static uint8_t ev_stream[EV_N][8192];

static int test_feedback_inflight(const GfxBackend *b, uint8_t *guest, const pool_t *draw_pool, prog_t *prog) {
    static tex_t tt[EV_N];
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + EV_BASE, .size = EV_POOL, .game_mem = guest + EV_BASE};
    CHECK(b->pool_init(&pool), "pool_init of the in-flight feedback targets");
    static uint32_t *evref[EV_N], *evout[EV_N];
    int64_t off = 0;
    for (int k = 0; k < EV_N; k++) {
        init_tex_wh(&tt[k], &pool, off, NVN_FORMAT_RGBA8, EV_W(k), TEX_H);
        off += (int64_t)((tt[k].L.size + 0xfffull) & ~0xfffull);
        CHECK(off <= (int64_t)EV_POOL, "the in-flight feedback targets exceed their pool");
        vk_texture_register(FB_TEX_EV + k, &tt[k], NULL);
        evref[k] = calloc((size_t)EV_W(k) * TEX_H, 4), evout[k] = calloc((size_t)EV_W(k) * TEX_H, 4);
        CHECK(evref[k] && evout[k], "out of memory for the in-flight feedback readbacks");
        fb_draw(ev_stream[k], prog, draw_pool, &tt[k], FB_TEX_EV + (uint32_t)k);
    }
    // the reference: each draw alone, waited for
    for (int k = 0; k < EV_N; k++) {
        uint64_t h = (uint64_t)(uintptr_t)ev_stream[k];
        uint64_t f = b->submit(&h, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of a reference feedback draw timed out");
        fb_read_wh(b, &tt[k], evref[k], EV_W(k), TEX_H);
    }
    // all of them in flight, in submissions of EV_GROUP targets. The first draws the first group, which fills the cache
    // with its copies. The second draws the next group, and every copy it needs evicts a copy of the first group: the
    // least recently used ones, whose last submission the GPU may still be running (the flight bound allows that).
    uint64_t ev0 = vk_exec_frame.snap_evicted, busy0 = vk_exec_frame.snap_evicted_busy, fs0 = vk_exec_frame.fb_snap_draws,
             sk0 = vk_exec_frame.snap_skipped;
    uint64_t last = 0;
    for (int k0 = 0; k0 < EV_N; k0 += EV_GROUP) {
        uint64_t hs[EV_GROUP];
        int n = EV_N - k0 < EV_GROUP ? EV_N - k0 : EV_GROUP;
        for (int i = 0; i < n; i++) hs[i] = (uint64_t)(uintptr_t)ev_stream[k0 + i];
        last = b->submit(hs, n);
    }
    CHECK(b->fence_wait(last, 20000000000ull), "fence_wait of the in-flight feedback draws timed out");
    uint64_t evicted = vk_exec_frame.snap_evicted - ev0, busy = vk_exec_frame.snap_evicted_busy - busy0,
             draws = vk_exec_frame.fb_snap_draws - fs0, skipped = vk_exec_frame.snap_skipped - sk0;
    int differ = 0, first = -1;
    for (int k = 0; k < EV_N; k++) {
        fb_read_wh(b, &tt[k], evout[k], EV_W(k), TEX_H);
        for (int i = 0; i < EV_W(k) * TEX_H; i++)
            if (evout[k][i] != evref[k][i]) {
                if (first < 0) first = k;
                differ++;
            }
    }
    CHECK(skipped == 0, "%llu in-flight feedback draws were skipped for lack of a copy", (unsigned long long)skipped);
    CHECK(draws == EV_N, "%llu of %d in-flight feedback draws copied their target", (unsigned long long)draws, EV_N);
    CHECK(evicted > 0, "no copy was dropped by the %d in-flight feedback draws: the eviction path did not run", EV_N);
    CHECK(!differ, "in-flight feedback draws differ from their waited references in %d texels (first target %d)", differ, first);
    // busy counts the drops whose last submission the host had not seen finished: those images go through the deferred
    // destruction (vk_defer_image) instead of being freed at once. The second group's submission drops the first group's
    // copies right after the first was submitted, so on a GPU that is still running the first group, all of those drops
    // count here. A GPU that has finished it gives 0, so the count is printed rather than checked.
    printf("probe-vkexec: in-flight feedback part passed (%d targets of %d to %d wide: %llu copies dropped, %llu of them "
           "while their last submission was not known finished; every draw equal to its waited reference)\n", EV_N, EV_W(0),
           EV_W(EV_N - 1), (unsigned long long)evicted, (unsigned long long)busy);
    for (int k = 0; k < EV_N; k++) {
        b->texture_release(&tt[k]);
        free(evref[k]);
        free(evout[k]);
    }
    b->pool_fini(&pool);
    return 0;
}

// Samples passed across a render pass restart. The same draw is counted alone, then twice with a clear of an unrelated
// texture between the two draws: the clear ends the pass (as a snapshot copy does), so the first draw's occlusion query
// closes and the second draw begins one of its own. Two draws of one program over one target must count twice one.
#define SP_REPORT_OFF 0x3FF00u   // the report, in the output pool (FB_POOL_OUT) past the output target's 0x24000 bytes
static int samples_run(const GfxBackend *b, uint8_t *guest, const pool_t *pool, prog_t *prog, tex_t *color, tex_t *other,
                       int clear_between, uint64_t *count, uint64_t *passes) {
    NVNcounterData *rep = (NVNcounterData *)(guest + FB_POOL_OUT + SP_REPORT_OFF);
    memset(rep, 0, sizeof *rep);
    const p_reset_counter rs = {NVN_COUNTER_TYPE_SAMPLES_PASSED};
    uint8_t *q = put(fb_stream, OP_RESET_COUNTER, &rs, sizeof rs);
    q = draw_prefix(q, prog, pool, color);
    q = fb_draw_body(q, pool, FB_TEX_REF);
    if (clear_between) {
        p_clear_tex ct;
        memset(&ct, 0, sizeof ct);
        ct.tex = other, ct.r = (NVNcopyRegion){0, 0, 0, TEX_W, TEX_H, 1}, ct.mask = 0xf;
        ct.c.f[0] = 0.5f, ct.c.f[1] = 0.5f, ct.c.f[2] = 0.5f, ct.c.f[3] = 1.0f;
        q = put(q, OP_CLEAR_TEX, &ct, sizeof ct);
        q = fb_draw_body(q, pool, FB_TEX_REF);
    }
    const p_report_counter rc = {NVN_COUNTER_TYPE_SAMPLES_PASSED, (uint64_t)(uintptr_t)rep};
    q = put(q, OP_REPORT_COUNTER, &rc, sizeof rc);
    put(q, OP_END, NULL, 0);
    const uint64_t p0 = vk_exec_frame.passes;
    int ok = fb_run(b);
    *passes = vk_exec_frame.passes - p0;
    *count = rep->counter;
    return ok;
}

static int test_samples_restart(const GfxBackend *b, uint8_t *guest, const pool_t *pool, prog_t *prog, tex_t *color,
                                tex_t *other) {
    uint64_t alone = 0, pa = 0, twice = 0, pt = 0;
    int ok = samples_run(b, guest, pool, prog, color, other, 0, &alone, &pa);
    ok = samples_run(b, guest, pool, prog, color, other, 1, &twice, &pt) && ok;
    CHECK(ok, "fence_wait of the samples-passed draws timed out");
    CHECK(alone > 0, "one draw counted no samples passed");
    CHECK(pt > pa, "the clear between the two draws began no render pass after the first (%llu passes with it, %llu without)",
          (unsigned long long)pt, (unsigned long long)pa);
    CHECK(twice == 2 * alone, "two draws across a pass restart counted %llu samples passed, expected %llu (twice one draw)",
          (unsigned long long)twice, (unsigned long long)(2 * alone));
    printf("probe-vkexec: samples-passed part passed (%llu samples for one draw, %llu for two draws across a pass restart)\n",
           (unsigned long long)alone, (unsigned long long)twice);
    return 0;
}

static int test_feedback(const GfxBackend *b, uint8_t *guest, pool_t *pool, tex_t *color, const char *root,
                         const DrawEnt *e, int (*pairs)[2], int np) {
    static tex_t ref, out;
    pool_t pref = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + FB_POOL_REF, .size = FB_POOL_SIZE,
                   .game_mem = guest + FB_POOL_REF};
    pool_t pout = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + FB_POOL_OUT, .size = FB_POOL_SIZE,
                   .game_mem = guest + FB_POOL_OUT};
    CHECK(b->pool_init(&pref) && b->pool_init(&pout), "pool_init of the feedback part's targets");
    init_tex(&ref, &pref, 0, NVN_FORMAT_RGBA8);
    init_tex(&out, &pout, 0, NVN_FORMAT_RGBA8);
    CHECK(ref.L.size <= FB_POOL_SIZE && out.L.size <= FB_POOL_SIZE, "feedback part's targets exceed their pools");
    vk_texture_register(FB_TEX_TARGET, color, NULL);
    vk_texture_register(FB_TEX_REF, &ref, NULL);
    fb_vertices(pool);
    static uint32_t px_f[TEX_W * TEX_H], px_r[TEX_W * TEX_H], px_c[TEX_W * TEX_H], px_o[TEX_W * TEX_H], px_s[TEX_W * TEX_H];
    // The feedback draws are narrow whatever XC2_SNAP_SYNC says, so the narrow path is the one checked; the full run is made
    // explicitly below, and the environment's setting is put back at the end.
    int env_on, env_narrow;
    vk_exec_snap_mode_get(&env_on, &env_narrow);
    vk_exec_snap_mode(1, 1);
    int ok = 1, found = -1;
    uint64_t snapped = 0;
    prog_t *prog = NULL;
    int tried = 0;
    for (int t = 0; t < np && t < 64 && found < 0 && ok; t++) {
        const int *pr = pairs[(long long)t * np / (np < 64 ? np : 64)];
        if (t && pr == pairs[(long long)(t - 1) * np / (np < 64 ? np : 64)]) continue;
        const DrawEnt *both[2] = {&e[pr[0]], &e[pr[1]]};
        prog_t *p = dr_prog(root, both, 2);
        CHECK(p, "census code file of %.16s is missing", both[0]->key);
        tried++;
        // Y0: the six vertices into out, sampling ref cleared to fb_c0 (out is not sampled: no copy); Y1: the same with ref
        // cleared to fb_c1. Where they differ the output depends on what the draw samples, so the pair can show the copy.
        uint8_t *q = draw_prefix(fb_stream, p, pool, &ref);
        put(q, OP_END, NULL, 0);
        ok = fb_run(b) && ok;
        fb_draw(fb_stream, p, pool, &out, FB_TEX_REF);
        ok = fb_run(b) && ok;
        fb_read(b, &out, px_r);
        q = fb_clear_only(fb_stream, &ref, fb_c1);
        put(q, OP_END, NULL, 0);
        ok = fb_run(b) && ok;
        fb_draw(fb_stream, p, pool, &out, FB_TEX_REF);
        ok = fb_run(b) && ok;
        CHECK(ok, "fence_wait of the feedback part's reference timed out");
        fb_read(b, &out, px_s);
        if (!fb_differ(px_r, px_s)) continue;   // the output does not depend on what it samples
        // the feedback draw: color cleared to fb_c0 by the prefix, then the six vertices sampling color
        uint64_t s0 = vk_exec_frame.fb_snap_draws;
        fb_draw(fb_stream, p, pool, color, FB_TEX_TARGET);
        ok = fb_run(b) && ok;
        CHECK(ok, "fence_wait of the feedback draw timed out");
        uint64_t n = vk_exec_frame.fb_snap_draws - s0;
        if (!n) continue;   // the pair samples no target it writes
        fb_read(b, color, px_c);
        // the copy-off draw of the same pair, for the report: where live reads show a fragment's own write the two differ
        vk_exec_snap_mode(0, -1);
        fb_draw(fb_stream, p, pool, color, FB_TEX_TARGET);
        ok = fb_run(b) && ok;
        vk_exec_snap_mode(1, -1);
        fb_read(b, color, px_o);
        found = t, snapped = n, prog = p;
    }
    CHECK(found >= 0, "none of %d census pairs both sampled the target it writes and used what it sampled", tried);
    int differ = fb_differ(px_c, px_r);
    CHECK(!differ, "feedback draw of pair %d: %d of %d texels differ from the reference (the draw did not read the target as it was before the draw)",
          found, differ, TEX_W * TEX_H);
    // the same draw with the full barrier (px_o holds the copy-off draw of this pair, from the search)
    vk_exec_snap_mode(1, 0);
    fb_draw(fb_stream, prog, pool, color, FB_TEX_TARGET);
    ok = fb_run(b) && ok;
    fb_read(b, color, px_f);
    vk_exec_snap_mode(env_on, env_narrow);
    CHECK(ok, "fence_wait of the feedback part's variants timed out");
    int narrow_full = fb_differ(px_c, px_f), no_copy = fb_differ(px_o, px_r);
    CHECK(!narrow_full, "feedback draw: narrow and full sync differ in %d texels", narrow_full);
    CHECK(vk_exec_frame.snap_skipped == 0, "%llu feedback draws were skipped for lack of a copy",
          (unsigned long long)vk_exec_frame.snap_skipped);
    if (test_feedback_inflight(b, guest, pool, prog)) return 1;
    if (test_samples_restart(b, guest, pool, prog, color, &out)) return 1;
    printf("probe-vkexec: feedback part passed (pair %d of %d tried: %llu copies in its draw, %d of %d texels equal the reference "
           "in both sync modes; with the copy off %d texels differ from it (0 where this GPU's live reads show no same-draw "
           "writes); 0 skipped)\n",
           found, tried, (unsigned long long)snapped, TEX_W * TEX_H, TEX_W * TEX_H, no_copy);
    b->texture_release(&ref);
    b->texture_release(&out);
    b->pool_fini(&pref);
    b->pool_fini(&pout);
    return 0;
}

static int test_draw(const GfxBackend *b, uint8_t *guest, int *n_drawn, int *n_tries, int *n_cs, int *n_alpha) {
    char root[PLAT_PATH_MAX * 2], path[PLAT_PATH_MAX * 2 + 64];
    CHECK(dr_root(root, sizeof root), GFX_CENSUS_DIR "/jobs.json not found");
    snprintf(path, sizeof path, "%s/" GFX_CENSUS_DIR "/jobs.json", root);
    long size;
    char *jobs = dr_read(path, &size);
    DrawEnt *e = malloc(sizeof *e * 20000);
    int n = jobs && e ? dr_parse(jobs, e, 20000) : 0;
    CHECK(n > 0, "no census entries");
    qsort(e, (size_t)n, sizeof *e, dr_cmp);
    int (*pairs)[2] = malloc(sizeof *pairs * (size_t)n), np = 0, *cs = malloc(sizeof *cs * (size_t)n), nc = 0;
    for (int i = 0; i < n;) {
        int j = i, v0 = -1, nv = 0, nf = 0;
        for (; j < n && e[j].src_len == e[i].src_len && !memcmp(e[j].src, e[i].src, e[i].src_len); j++) {
            if (e[j].stage == GFX_STAGE_VS && v0 < 0) v0 = j;
            nv += e[j].stage == GFX_STAGE_VS;
            if (e[j].stage == GFX_STAGE_CS) cs[nc++] = j;
        }
        for (int k = i; k < j && nv; k++)
            if (e[k].stage == GFX_STAGE_FS) pairs[np][0] = v0 + nf++ % nv, pairs[np][1] = k, np++;
        i = j;
    }
    CHECK(np > 0 && nc > 0, "census has %d pairs and %d compute shaders", np, nc);

    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x700000, .size = 0xC0000,
                   .game_mem = guest + 0x700000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the draw part");
    static tex_t color;
    init_tex(&color, &pool, 0, NVN_FORMAT_RGBA8);
    CHECK(color.L.size <= 0x80000, "draw-part target exceeds its pool slot");
    // Vertex data at 0x80000: three vertices of DR_ATTRS RGBA32F attributes, attribute 0 the position. Uniform data at
    // 0x90000: identity matrices back to back, so that a vertex shader multiplying the position by one passes it through.
    float *vtx = (float *)(pool.mem + 0x80000);
    static const float pos[3][4] = {{-0.8f, -0.8f, 0.0f, 1.0f}, {0.8f, -0.8f, 0.0f, 1.0f}, {0.0f, 0.8f, 0.0f, 1.0f}};
    for (int v = 0; v < 3; v++)
        for (int a = 0; a < DR_ATTRS; a++)
            for (int c = 0; c < 4; c++) vtx[(v * DR_ATTRS + a) * 4 + c] = a == 0 ? pos[v][c] : c == 3 ? 1.0f : 0.5f;
    float *ub = (float *)(pool.mem + 0x90000);
    for (int m = 0; m < 256; m++)
        for (int i = 0; i < 16; i++) ub[m * 16 + i] = i % 5 == 0 ? 1.0f : 0.0f;

    const uint8_t clear_b[4] = {to_byte(0.2f), to_byte(0.4f), to_byte(0.6f), 255};
    uint32_t clear_v;
    memcpy(&clear_v, clear_b, 4);
    static uint32_t px[TEX_W * TEX_H];
    static _Alignas(8) uint8_t stream[8192];
    int tries = 0, drawn = 0;
    prog_t *hit = NULL;
    for (int t = 0; t < 150 && !drawn; t++) {
        const int *pr = pairs[(long long)t * np / (np < 150 ? np : 150)];
        if (t && pr == pairs[(long long)(t - 1) * np / (np < 150 ? np : 150)]) continue;
        const DrawEnt *both[2] = {&e[pr[0]], &e[pr[1]]};
        prog_t *prog = dr_prog(root, both, 2);
        CHECK(prog, "census code file of %.16s is missing", both[0]->key);
        tries++;
        uint8_t *q = draw_prefix(stream, prog, &pool, &color);
        const p_draw d = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 0, 0};
        q = put(q, OP_DRAW, &d, sizeof d);
        q = put(q, OP_END, NULL, 0);
        CHECK(q <= stream + sizeof stream, "draw op stream overflow");
        uint64_t handle = (uint64_t)(uintptr_t)stream;
        uint64_t f = b->submit(&handle, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the draw submission timed out");
        const NVNcopyRegion r = {0, 0, 0, TEX_W, TEX_H, 1};
        b->read_texels(&color, NULL, &r, (uint8_t *)px, TEX_W * 4, TEX_W * TEX_H * 4);
        int diff = 0;
        for (int k = 0; k < TEX_W * TEX_H; k++) diff += px[k] != clear_v;
        if (diff) drawn = diff, hit = prog;
        if (diff && vk_caps.gpl) {   // the same draw through the shared set 0 layout: texel-equal
            static uint32_t px2[TEX_W * TEX_H];
            int was = vk_pipeline_set_indep(0);
            f = b->submit(&handle, 1);
            CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the shared-layout draw timed out");
            b->read_texels(&color, NULL, &r, (uint8_t *)px2, TEX_W * 4, TEX_W * TEX_H * 4);
            vk_pipeline_set_indep(was);
            int ne = 0;
            for (int k = 0; k < TEX_W * TEX_H; k++) ne += px[k] != px2[k];
            CHECK(!ne, "%d of %d texels differ between independent and shared layouts", ne, TEX_W * TEX_H);
            layouts_equal = TEX_W * TEX_H;
        }
    }
    CHECK(drawn, "none of %d census pairs changed a pixel of the cleared target", tries);
    CHECK(!test_feedback(b, guest, &pool, &color, root, e, pairs, np), "feedback part failed");

    // The fixed-function alpha test (nvnColorStateSetAlphaTest's function against nvnCommandBufferSetAlphaRef's
    // reference, docs/maxwell.md 5.7): the same draw with an alpha test nothing passes must leave the target cleared,
    // and one everything passes must redraw it. A pair whose fragment shader writes no alpha (no target-0 alpha
    // component) passes neither: the scan moves on to the next pair.
    static color_st off, all, none, geq;
    // off is the default colour state (nvnColorStateSetDefaults: alpha test ALWAYS). A different state would build a new
    // pipeline here, and the indirect draw below (count 0x7fffffff, not a full-screen draw) is skipped while its pipeline builds.
    off = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    all = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_GREATER};
    none = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_NEVER};
    geq = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_GEQUAL};
    int alpha_pairs = 0, alpha_ok = 0;
    // pxp: the plain draw of the pair under test; px stays the direct draw of hit, which the indirect part compares with
    static uint32_t pxa[TEX_W * TEX_H], pxp[TEX_W * TEX_H];
    for (int t = 0; t < 150 && !alpha_ok; t++) {
        const int *pr = pairs[(long long)t * np / (np < 150 ? np : 150)];
        if (t && pr == pairs[(long long)(t - 1) * np / (np < 150 ? np : 150)]) continue;
        const DrawEnt *both[2] = {&e[pr[0]], &e[pr[1]]};
        prog_t *prog = dr_prog(root, both, 2);
        alpha_pairs++;
        // step 0: the plain draw (the colour state resets the alpha test a previous pair left on)
        uint8_t *q = draw_prefix(stream, prog, &pool, &color);
        p_bind_state bs = {1};
        memcpy(bs.bytes, &off, sizeof off);
        q = put(q, OP_BIND_STATE, &bs, sizeof bs);
        const p_alpha_ref ar0 = {0.0f};
        q = put(q, OP_ALPHA_REF, &ar0, sizeof ar0);
        const p_draw d = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3, 0, 0};
        q = put(q, OP_DRAW, &d, sizeof d);
        q = put(q, OP_END, NULL, 0);
        uint64_t handle = (uint64_t)(uintptr_t)stream;
        uint64_t f = b->submit(&handle, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the plain draw timed out");
        const NVNcopyRegion r = {0, 0, 0, TEX_W, TEX_H, 1};
        b->read_texels(&color, NULL, &r, (uint8_t *)pxp, TEX_W * 4, TEX_W * TEX_H * 4);
        int diff = 0;
        for (int k = 0; k < TEX_W * TEX_H; k++) diff += pxp[k] != clear_v;
        if (!diff) continue;   // the pair draws nothing: no alpha output to test
        // step 1: greater, ref 2: every fragment fails it (alpha is at most 1), the target stays cleared
        q = draw_prefix(stream, prog, &pool, &color);
        memcpy(bs.bytes, &all, sizeof all);
        q = put(q, OP_BIND_STATE, &bs, sizeof bs);
        const p_alpha_ref ar2 = {2.0f};
        q = put(q, OP_ALPHA_REF, &ar2, sizeof ar2);
        q = put(q, OP_DRAW, &d, sizeof d);
        q = put(q, OP_END, NULL, 0);
        f = b->submit(&handle, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the alpha-tested draw timed out");
        b->read_texels(&color, NULL, &r, (uint8_t *)pxa, TEX_W * 4, TEX_W * TEX_H * 4);
        int ne = 0;
        for (int k = 0; k < TEX_W * TEX_H; k++) ne += pxa[k] != clear_v;
        if (ne) continue;
        // step 2: greater, ref 0: the same pixels as the plain draw (opaque fragments pass, alpha 0 fails)
        q = draw_prefix(stream, prog, &pool, &color);
        memcpy(bs.bytes, &geq, sizeof geq);
        q = put(q, OP_BIND_STATE, &bs, sizeof bs);
        q = put(q, OP_ALPHA_REF, &ar0, sizeof ar0);   // the ref resets: step 1 set 2
        q = put(q, OP_DRAW, &d, sizeof d);
        q = put(q, OP_END, NULL, 0);
        f = b->submit(&handle, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the gequal-0 draw timed out");
        b->read_texels(&color, NULL, &r, (uint8_t *)pxa, TEX_W * 4, TEX_W * TEX_H * 4);
        ne = 0;
        for (int k = 0; k < TEX_W * TEX_H; k++) ne += pxa[k] != pxp[k];
        if (ne) continue;
        // step 3: never: nothing passes, the target stays cleared
        q = draw_prefix(stream, prog, &pool, &color);
        memcpy(bs.bytes, &none, sizeof none);
        q = put(q, OP_BIND_STATE, &bs, sizeof bs);
        q = put(q, OP_ALPHA_REF, &ar0, sizeof ar0);
        q = put(q, OP_DRAW, &d, sizeof d);
        q = put(q, OP_END, NULL, 0);
        f = b->submit(&handle, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the never-tested draw timed out");
        b->read_texels(&color, NULL, &r, (uint8_t *)pxa, TEX_W * 4, TEX_W * TEX_H * 4);
        ne = 0;
        for (int k = 0; k < TEX_W * TEX_H; k++) ne += pxa[k] != clear_v;
        if (!ne) alpha_ok = TEX_W * TEX_H;
    }
    CHECK(alpha_ok, "none of %d drawing census pairs passed the alpha test steps (draw, discard all, keep opaque, discard all)",
          alpha_pairs);
    {   // the colour state outlives the submission: clear the alpha test the scan left bound
        uint8_t *q = draw_prefix(stream, hit, &pool, &color);
        p_bind_state bs = {1};
        memcpy(bs.bytes, &off, sizeof off);
        q = put(q, OP_BIND_STATE, &bs, sizeof bs);
        const p_alpha_ref ar0 = {0.0f};
        q = put(q, OP_ALPHA_REF, &ar0, sizeof ar0);
        q = put(q, OP_END, NULL, 0);
        uint64_t handle = (uint64_t)(uintptr_t)stream;
        uint64_t f = b->submit(&handle, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the alpha-test reset timed out");
    }

    // DrawElementsIndirect (OP_DRAW_IDX_IND) with the pair that drew: indices {0, 1, 2} at 0xA0000; the arguments are
    // copied by the GPU (COPY_B2B, barrier) to 0xA0200 right before the draw, so the replay must read them on the GPU.
    // {3, 1, 0, 0, 0} draws the same triangle (texel-equal to the direct draw); count 0 draws nothing.
    uint32_t *ix = (uint32_t *)(pool.mem + 0xA0000), *args = (uint32_t *)(pool.mem + 0xA0100);
    ix[0] = 0, ix[1] = 1, ix[2] = 2;
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t want[5] = {pass ? 0u : 3u, 1, 0, 0, 0};
        memcpy(args, want, sizeof want);
        memset(pool.mem + 0xA0200, 0, 20);
        uint8_t *q = draw_prefix(stream, hit, &pool, &color);
        // The feedback part leaves its textures bound (fb_draw_body): unbind the fragment slots, as the first draw had them.
        p_bind_handles nt;
        memset(&nt, 0, sizeof nt);
        nt.stage = 1, nt.first = 0, nt.n = 8;
        q = put(q, OP_BIND_TEX, &nt, sizeof nt);
        const p_copy_b2b cp = {(uint64_t)(uintptr_t)args, (uint64_t)(uintptr_t)(pool.mem + 0xA0200), 20};
        q = put(q, OP_COPY_B2B, &cp, sizeof cp);
        const p_barrier bar = {0, 0};
        q = put(q, OP_BARRIER, &bar, sizeof bar);
        const p_draw_idx_ind di = {NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, (uint64_t)(uintptr_t)ix,
                                   (uint64_t)(uintptr_t)(pool.mem + 0xA0200)};
        q = put(q, OP_DRAW_IDX_IND, &di, sizeof di);
        q = put(q, OP_END, NULL, 0);
        uint64_t handle = (uint64_t)(uintptr_t)stream;
        uint64_t f = b->submit(&handle, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the indirect draw timed out");
        static uint32_t pxi[TEX_W * TEX_H];
        const NVNcopyRegion r = {0, 0, 0, TEX_W, TEX_H, 1};
        b->read_texels(&color, NULL, &r, (uint8_t *)pxi, TEX_W * 4, TEX_W * TEX_H * 4);
        int ne = 0;
        for (int k = 0; k < TEX_W * TEX_H; k++) ne += pxi[k] != (pass ? clear_v : px[k]);
        CHECK(!ne, "indirect draw with count %u: %d of %d texels differ from the %s", want[0], ne, TEX_W * TEX_H,
              pass ? "cleared target" : "direct draw");
        ind_draw_texels += TEX_W * TEX_H;
    }

    // Compute: a few census compute shaders, one dispatch each, resources unbound (zero buffers and stand-in images).
    int dispatched = 0;
    for (int t = 0; t < 4 && t < nc; t++) {
        const DrawEnt *one[1] = {&e[cs[(long long)t * nc / (nc < 4 ? nc : 4)]]};
        prog_t *prog = dr_prog(root, one, 1);
        CHECK(prog, "census code file of %.16s is missing", one[0]->key);
        uint8_t *q = stream;
        q = put(q, OP_BEGIN, NULL, 0);
        const p_bind_program bp = {prog, 1 << 5};
        q = put(q, OP_BIND_PROGRAM, &bp, sizeof bp);
        const p_dispatch dp = {1, 1, 1, 0};
        q = put(q, OP_DISPATCH, &dp, sizeof dp);
        // DispatchComputeIndirect: group counts {1, 1, 1} copied by the GPU to 0xA0500 first
        const uint32_t groups[3] = {1, 1, 1};
        memcpy(pool.mem + 0xA0400, groups, sizeof groups);
        memset(pool.mem + 0xA0500, 0, sizeof groups);
        const p_copy_b2b cp = {(uint64_t)(uintptr_t)(pool.mem + 0xA0400), (uint64_t)(uintptr_t)(pool.mem + 0xA0500), 12};
        q = put(q, OP_COPY_B2B, &cp, sizeof cp);
        const p_barrier bar = {0, 0};
        q = put(q, OP_BARRIER, &bar, sizeof bar);
        const p_dispatch_ind dpi = {(uint64_t)(uintptr_t)(pool.mem + 0xA0500), 0};
        q = put(q, OP_DISPATCH_IND, &dpi, sizeof dpi);
        q = put(q, OP_END, NULL, 0);
        uint64_t handle = (uint64_t)(uintptr_t)stream;
        uint64_t f = b->submit(&handle, 1);
        CHECK(b->fence_wait(f, 20000000000ull), "fence_wait of the dispatch submission timed out");
        dispatched++;
        ind_dispatches++;
    }
    b->texture_release(&color);
    b->pool_fini(&pool);
    free(pairs), free(cs), free(e), free(jobs);
    *n_drawn = drawn, *n_tries = tries, *n_cs = dispatched, *n_alpha = alpha_ok;
    return 0;
}

// ---- xfer part: copies, texture clear, event and counter through submit.

#define XF_W 64
#define XF_H 64
#define XF_BYTES (XF_W * XF_H * 4)

// Texture in the xfer pool at `off`.
static int xf_tex(tex_t *t, pool_t *pool, int64_t off, uint64_t serial) {
    init_tex(t, pool, off, NVN_FORMAT_RGBA8);
    t->d.width = XF_W, t->d.height = XF_H;
    nvn_tex_layout(&t->d, &t->L);
    t->serial = serial;
    return t->L.size <= 0x20000;
}

static int xf_submit(const GfxBackend *b, uint8_t *stream, uint8_t *end) {
    end = put(end, OP_END, NULL, 0);
    (void)stream;
    uint64_t handle = (uint64_t)(uintptr_t)stream;
    uint64_t f = b->submit(&handle, 1);
    return b->fence_wait(f, 5000000000ull);
}

static int test_xfer(const GfxBackend *b, uint8_t *guest, int *bytes) {
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x800000, .size = 0x100000,
                   .game_mem = guest + 0x800000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the xfer part");
    uint8_t *m = pool.mem;
    static tex_t ta, tb, tc;
    CHECK(xf_tex(&ta, &pool, 0x40000, 201) && xf_tex(&tb, &pool, 0x60000, 202) && xf_tex(&tc, &pool, 0x80000, 203),
          "xfer textures exceed their pool slots");
    // Linear buffers: B2B source 0x0, target 0x1000 (4 KiB); texture source 0x2000, read-backs 0x6000 and 0xA000.
    for (int i = 0; i < 4096; i++) m[i] = (uint8_t)(i * 7 + 3), m[0x1000 + i] = 0;
    uint32_t *img = (uint32_t *)(m + 0x2000);
    for (int i = 0; i < XF_W * XF_H; i++) img[i] = 0x01010101u * (uint32_t)(i & 0xff) + (uint32_t)i * 0x00010000u;
    memset(m + 0x6000, 0, XF_BYTES), memset(m + 0xA000, 0, XF_BYTES);
    volatile uint32_t *event = (volatile uint32_t *)(m + 0xE000);
    NVNcounterData *ctr = (NVNcounterData *)(m + 0xE100), *sp = (NVNcounterData *)(m + 0xE200);
    *event = 0, ctr->counter = 99, ctr->timestamp = 0;
    sp->counter = 99, sp->timestamp = 0;

    static _Alignas(8) uint8_t stream[4096];
    uint8_t *p = stream;
    p = put(p, OP_BEGIN, NULL, 0);
    const p_copy_b2b bb = {(uint64_t)(uintptr_t)m, (uint64_t)(uintptr_t)(m + 0x1000), 4096};
    p = put(p, OP_COPY_B2B, &bb, sizeof bb);
    p_copy_b2t up;
    memset(&up, 0, sizeof up);
    up.src = (uint64_t)(uintptr_t)(m + 0x2000), up.tex = &ta, up.r = (NVNcopyRegion){0, 0, 0, XF_W, XF_H, 1};
    p = put(p, OP_COPY_B2T, &up, sizeof up);
    p_copy_b2t down = up;
    down.src = (uint64_t)(uintptr_t)(m + 0x6000);
    p = put(p, OP_COPY_T2B, &down, sizeof down);
    p_copy_t2t tt;
    memset(&tt, 0, sizeof tt);
    tt.src = &ta, tt.dst = &tb, tt.sr = tt.dr = up.r;
    p = put(p, OP_COPY_T2T, &tt, sizeof tt);
    down.tex = &tb, down.src = (uint64_t)(uintptr_t)(m + 0xA000);
    p = put(p, OP_COPY_T2B, &down, sizeof down);
    p_clear_tex ct;
    memset(&ct, 0, sizeof ct);
    ct.tex = &tc, ct.r = (NVNcopyRegion){0, 0, 0, XF_W, XF_H, 1}, ct.mask = 0xf;
    ct.c.f[0] = 0.2f, ct.c.f[1] = 0.4f, ct.c.f[2] = 0.6f, ct.c.f[3] = 1.0f;
    p = put(p, OP_CLEAR_TEX, &ct, sizeof ct);
    const p_signal_event ev1 = {event, NVN_EVENT_SIGNAL_MODE_WRITE, 7}, ev2 = {event, NVN_EVENT_SIGNAL_MODE_ADD, 5};
    p = put(p, OP_SIGNAL_EVENT, &ev1, sizeof ev1);
    p = put(p, OP_SIGNAL_EVENT, &ev2, sizeof ev2);
    const p_report_counter rc = {NVN_COUNTER_TYPE_TIMESTAMP, (uint64_t)(uintptr_t)ctr};
    p = put(p, OP_REPORT_COUNTER, &rc, sizeof rc);
    // Samples passed: a reset, no draw, a report: {0, time} through the occlusion-query path.
    const p_reset_counter rs = {NVN_COUNTER_TYPE_SAMPLES_PASSED};
    p = put(p, OP_RESET_COUNTER, &rs, sizeof rs);
    const p_report_counter rsp = {NVN_COUNTER_TYPE_SAMPLES_PASSED, (uint64_t)(uintptr_t)sp};
    p = put(p, OP_REPORT_COUNTER, &rsp, sizeof rsp);
    CHECK(p <= stream + sizeof stream - sizeof(op_hdr), "xfer op stream overflow");
    CHECK(xf_submit(b, stream, p), "fence_wait of the xfer submission timed out");

    for (int i = 0; i < 4096; i++) CHECK(m[0x1000 + i] == m[i], "B2B byte %d is %02x, expected %02x", i, m[0x1000 + i], m[i]);
    for (int i = 0; i < XF_W * XF_H; i++) {
        CHECK(((uint32_t *)(m + 0x6000))[i] == img[i], "B2T then T2B texel %d is %08x, expected %08x", i,
              ((uint32_t *)(m + 0x6000))[i], img[i]);
        CHECK(((uint32_t *)(m + 0xA000))[i] == img[i], "T2T then T2B texel %d is %08x, expected %08x", i,
              ((uint32_t *)(m + 0xA000))[i], img[i]);
    }
    static uint32_t px[XF_W * XF_H];
    const NVNcopyRegion r = {0, 0, 0, XF_W, XF_H, 1};
    b->read_texels(&ta, NULL, &r, (uint8_t *)px, XF_W * 4, XF_BYTES);
    for (int i = 0; i < XF_W * XF_H; i++) CHECK(px[i] == img[i], "read_texels of the copied texture, texel %d is %08x", i, px[i]);
    b->read_texels(&tc, NULL, &r, (uint8_t *)px, XF_W * 4, XF_BYTES);
    const uint8_t cl[4] = {to_byte(0.2f), to_byte(0.4f), to_byte(0.6f), 255};
    uint32_t clv;
    memcpy(&clv, cl, 4);
    for (int i = 0; i < XF_W * XF_H; i++) CHECK(px[i] == clv, "CLEAR_TEX texel %d is %08x, expected %08x", i, px[i], clv);
    CHECK(*event == 12, "event word is %u after WRITE 7 and ADD 5", *event);
    CHECK(ctr->counter == 0 && ctr->timestamp != 0, "counter report is {%llu, %llu}", (unsigned long long)ctr->counter,
          (unsigned long long)ctr->timestamp);
    CHECK(sp->counter == 0 && sp->timestamp != 0, "samples-passed report after a reset and no draw is {%llu, %llu}",
          (unsigned long long)sp->counter, (unsigned long long)sp->timestamp);
    b->texture_release(&ta);
    b->texture_release(&tb);
    b->texture_release(&tc);
    b->pool_fini(&pool);
    *bytes = 4096 + 2 * XF_BYTES;
    return 0;
}

// c0 as vk_c0_fill builds it (CPU only): compute storage and uniform tables, dispatch size, unbound slots zero; the
// vertex stage's point size clamp and base instance.
static int test_c0(void) {
    static p_bind_buf ssbo[16], ubo[16];
    static uint8_t bank[VK_C0_BYTES];
    ssbo[1] = (p_bind_buf){5, 1, 0x12340000, 0x400};
    ubo[4] = (p_bind_buf){5, 4, 0x55660000, 0x100};
    const uint32_t dim[3] = {3, 2, 1};
    uint32_t u[4];
    uint64_t a;
    vk_c0_fill(bank, 5, ssbo, ubo, dim, 7);
    memcpy(u, bank + 0x200, 12);
    CHECK(u[0] == 3 && u[1] == 2 && u[2] == 1, "c0 dispatch size is %u %u %u", u[0], u[1], u[2]);
    memcpy(&a, bank + 0x320, 8), memcpy(u, bank + 0x328, 4);
    CHECK(a == 0x12340000 && u[0] == 0x400, "c0 0x320 (storage slot 1) is {%#llx, %#x}", (unsigned long long)a, u[0]);
    memcpy(&a, bank + 0x250, 8), memcpy(u, bank + 0x258, 4);
    CHECK(a == 0x55660000 && u[0] == 0x100, "c0 0x250 (uniform slot 4) is {%#llx, %#x}", (unsigned long long)a, u[0]);
    for (int i = 0; i < 16; i++) CHECK(!bank[0x310 + i], "c0 0x310 (unbound storage slot 0) byte %d is %02x", i, bank[0x310 + i]);
    CHECK(!bank[0x644] && !bank[0x610], "c0 of a compute stage has graphics words");
    vk_c0_fill(bank, 0, ssbo, ubo, dim, 61);
    float ps;
    memcpy(&ps, bank + 0x610, 4), memcpy(u, bank + 0x644, 4);
    CHECK(ps == 2047.0f && u[0] == 61, "vertex c0: point size %g at 0x610, base instance %u at 0x644", ps, u[0]);
    memcpy(&a, bank + 0x120, 8);
    CHECK(a == 0x12340000 && !bank[0x200], "vertex c0: storage slot 1 at 0x120 is %#llx", (unsigned long long)a);
    return 0;
}

// A texture in a physical pool's memory that nothing wrote reads zero, as Switch pool memory starts zeroed
// (XC2_FIX_ZERO; the driver may return zeros without it, then this is a guard).
static int test_zero(const GfxBackend *b, uint8_t *guest, int *bytes) {
    pool_t phys = {.magic = NVN_POOL_MAGIC, .flags = 0x189, .mem = (uint8_t *)(uintptr_t)RT_GPU_VA_BASE, .size = 0x100000,
                   .game_mem = guest + 0xA00000};
    pool_t cpu = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x800000, .size = 0x100000, .game_mem = guest + 0x800000};
    CHECK(b->pool_init(&phys), "pool_init of a physical pool");
    CHECK(b->pool_init(&cpu), "pool_init of a CPU-visible pool for the zero part");
    static tex_t t;
    init_tex(&t, &phys, 0, NVN_FORMAT_RGBA8);
    t.d.width = 64, t.d.height = 64;
    nvn_tex_layout(&t.d, &t.L);
    t.serial = 301;
    memset(cpu.mem, 0xab, 64 * 64 * 4);
    static _Alignas(8) uint8_t stream[512];
    uint8_t *p = put(stream, OP_BEGIN, NULL, 0);
    p_copy_b2t down;
    memset(&down, 0, sizeof down);
    down.src = (uint64_t)(uintptr_t)cpu.mem, down.tex = &t, down.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
    p = put(p, OP_COPY_T2B, &down, sizeof down);
    CHECK(xf_submit(b, stream, p), "fence_wait of the zero submission timed out");
    for (int i = 0; i < 64 * 64 * 4; i++) CHECK(!cpu.mem[i], "unwritten texel byte %d of a physical-pool texture is %02x", i, cpu.mem[i]);
    b->texture_release(&t);
    b->pool_fini(&cpu);
    b->pool_fini(&phys);
    *bytes = 64 * 64 * 4;
    return 0;
}

// ClearBuffer / ReportValue (OP_CLEAR_BUF), Downsample (a linear OP_COPY_T2T to half size) and the stencil view
// aspect. Fills: 64 bytes in a CPU-visible pool, a 4-byte report over the first word in the same submission (GPU order:
// the report wins), and 64 bytes in a CPU-invisible physical pool copied out with COPY_B2B. Downsample: a 64x64 RGBA8
// texture to 32x32, each texel the 2x2 average within 1. Stencil: views of a D24S8 texture pick the stencil aspect in
// STENCIL mode and depth otherwise, and a stencil-aspect sampled view of the image is created.
static int test_nvnx(const GfxBackend *b, uint8_t *guest, int *bytes) {
    pool_t cpu = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0x800000, .size = 0x100000, .game_mem = guest + 0x800000};
    pool_t phys = {.magic = NVN_POOL_MAGIC, .flags = 0x189, .mem = (uint8_t *)(uintptr_t)RT_GPU_VA_BASE, .size = 0x100000,
                   .game_mem = guest + 0xA00000};
    CHECK(b->pool_init(&cpu) && b->pool_init(&phys), "pool_init of the nvnx pools");
    uint8_t *m = cpu.mem;
    memset(m, 0x11, 0x400);
    static tex_t src, dst;
    init_tex(&src, &cpu, 0x40000, NVN_FORMAT_RGBA8);
    src.d.width = 64, src.d.height = 64, src.serial = 401;
    nvn_tex_layout(&src.d, &src.L);
    init_tex(&dst, &cpu, 0x60000, NVN_FORMAT_RGBA8);
    dst.d.width = 32, dst.d.height = 32, dst.serial = 402;
    nvn_tex_layout(&dst.d, &dst.L);
    CHECK(src.L.size <= 0x20000 && dst.L.size <= 0x20000, "downsample textures exceed their pool slots");
    uint8_t *img = m + 0x1000, *out = m + 0x8000;
    for (int i = 0; i < 64 * 64 * 4; i++) img[i] = (uint8_t)((i * 37 + (i >> 8) * 11) & 0xff);
    memset(out, 0, 32 * 32 * 4);

    static _Alignas(8) uint8_t stream[2048];
    uint8_t *p = put(stream, OP_BEGIN, NULL, 0);
    const p_clear_buf f1 = {(uint64_t)(uintptr_t)m, 64, 0xdeadbeefu, 0}, rv = {(uint64_t)(uintptr_t)m, 4, 0x1234u, 0};
    p = put(p, OP_CLEAR_BUF, &f1, sizeof f1);
    p = put(p, OP_CLEAR_BUF, &rv, sizeof rv);
    const p_clear_buf f2 = {RT_GPU_VA_BASE + 0x100, 64, 0xa5a5a5a5u, 0};
    p = put(p, OP_CLEAR_BUF, &f2, sizeof f2);
    const p_copy_b2b back = {RT_GPU_VA_BASE + 0x100, (uint64_t)(uintptr_t)(m + 0x200), 64};
    p = put(p, OP_COPY_B2B, &back, sizeof back);
    p_copy_b2t up;
    memset(&up, 0, sizeof up);
    up.src = (uint64_t)(uintptr_t)img, up.tex = &src, up.r = (NVNcopyRegion){0, 0, 0, 64, 64, 1};
    p = put(p, OP_COPY_B2T, &up, sizeof up);
    p_copy_t2t ds;   // as nvnCommandBufferDownsample records it
    memset(&ds, 0, sizeof ds);
    ds.src = &src, ds.dst = &dst, ds.sr = (NVNcopyRegion){0, 0, 0, 64, 64, 1}, ds.dr = (NVNcopyRegion){0, 0, 0, 32, 32, 1};
    ds.flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
    p = put(p, OP_COPY_T2T, &ds, sizeof ds);
    p_copy_b2t down;
    memset(&down, 0, sizeof down);
    down.src = (uint64_t)(uintptr_t)out, down.tex = &dst, down.r = (NVNcopyRegion){0, 0, 0, 32, 32, 1};
    p = put(p, OP_COPY_T2B, &down, sizeof down);
    CHECK(xf_submit(b, stream, p), "fence_wait of the nvnx submission timed out");

    uint32_t w0;
    memcpy(&w0, m, 4);
    CHECK(w0 == 0x1234u, "ReportValue word after the fill is %#x", w0);
    for (int i = 1; i < 16; i++) {
        uint32_t w;
        memcpy(&w, m + 4 * i, 4);
        CHECK(w == 0xdeadbeefu, "ClearBuffer word %d is %#x", i, w);
    }
    CHECK(m[64] == 0x11, "ClearBuffer wrote past its 64 bytes (%02x)", m[64]);
    for (int i = 0; i < 64; i++) CHECK(m[0x200 + i] == 0xa5, "physical-pool fill byte %d is %02x", i, m[0x200 + i]);
    int worst = 0;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            for (int ch = 0; ch < 4; ch++) {
                int sum = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) sum += img[((2 * y + dy) * 64 + 2 * x + dx) * 4 + ch];
                int d = abs(out[(y * 32 + x) * 4 + ch] - (sum + 2) / 4);
                if (d > worst) worst = d;
            }
    CHECK(worst <= 1, "downsampled texel off the 2x2 average by %d", worst);

    static tex_t dsx;
    init_tex(&dsx, &cpu, 0x80000, NVN_FORMAT_DEPTH24_STENCIL8);
    dsx.d.width = 64, dsx.d.height = 64, dsx.serial = 403;
    nvn_tex_layout(&dsx.d, &dsx.L);
    const VkTexImage *di = vk_texture_get(&dsx);
    CHECK(di && (di->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (di->aspect & VK_IMAGE_ASPECT_DEPTH_BIT), "D24S8 image with both aspects");
    view_t sv, dv;
    memset(&sv, 0, sizeof sv), memset(&dv, 0, sizeof dv);
    sv.set_dsmode = 1, sv.dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
    dv.set_dsmode = 1, dv.dsmode = NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
    CHECK(vk_exec_view_aspect(di, &sv, 1) == VK_IMAGE_ASPECT_STENCIL_BIT && vk_exec_view_aspect(di, &dv, 1) == VK_IMAGE_ASPECT_DEPTH_BIT &&
              vk_exec_view_aspect(di, &sv, 0) == VK_IMAGE_ASPECT_DEPTH_BIT, "view aspects of a D24S8 texture");
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = di->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = di->format,
                                .subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}};
    VkImageView view = VK_NULL_HANDLE;
    CHECK(vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) == VK_SUCCESS, "stencil-aspect view of the D24S8 image");
    vkDestroyImageView(vk_ctx()->device, view, NULL);
    b->texture_release(&src);
    b->texture_release(&dst);
    b->texture_release(&dsx);
    b->pool_fini(&cpu);
    b->pool_fini(&phys);
    *bytes = 64 + 64 + 32 * 32 * 4;
    return 0;
}

// VRAM mirror (vk_memory.c; skipped in import mode): a pool given its mirror as on a first buffer use, after which
// buffer copies and fills read and write the mirror. The first use copies the whole pool; CPU writes between
// submissions reach the mirror through write watch; GPU writes reach guest memory by the fence, and only the bytes
// written: CPU bytes beside a GPU fill keep their values.
static int test_mirror(const GfxBackend *b, uint8_t *guest, int *bytes) {
    *bytes = 0;
    if (!vk_mirror_on) return 0;
    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0xC00000, .size = 0x100000,
                   .game_mem = guest + 0xC00000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the mirror part");
    uint8_t *m = pool.mem;
    for (int i = 0; i < 4096; i++) m[i] = (uint8_t)(i * 13 + 1);
    memset(m + 0x1000, 0xEE, 0x5000);
    CHECK(vk_mem_mirror_probe((uint64_t)(uintptr_t)m), "the pool got no VRAM mirror");
    static _Alignas(8) uint8_t stream[1024];
    uint64_t a = (uint64_t)(uintptr_t)m;
    // 1: B2B in the mirror after its first-use copy, and a fill; both written back.
    uint8_t *p = put(stream, OP_BEGIN, NULL, 0);
    const p_copy_b2b c1 = {a, a + 0x1000, 4096};
    const p_clear_buf f1 = {a + 0x2000, 0x800, 0x5a5a5a5au, 0};
    p = put(p, OP_COPY_B2B, &c1, sizeof c1);
    p = put(p, OP_CLEAR_BUF, &f1, sizeof f1);
    CHECK(xf_submit(b, stream, p), "fence_wait of the first mirror submission timed out");
    for (int i = 0; i < 4096; i++) CHECK(m[0x1000 + i] == m[i], "mirror B2B byte %d is %02x, expected %02x", i, m[0x1000 + i], m[i]);
    for (int i = 0; i < 0x800; i++) CHECK(m[0x2000 + i] == 0x5a, "mirror fill byte %d is %02x", i, m[0x2000 + i]);
    for (int i = 0x800; i < 0x1000; i++) CHECK(m[0x2000 + i] == 0xEE, "byte %#x past the mirror fill is %02x", i, m[0x2000 + i]);
    // 2: the CPU rewrites the source; the next submission copies the new bytes.
    for (int i = 0; i < 4096; i++) m[i] = (uint8_t)(i * 7 + 5);
    p = put(stream, OP_BEGIN, NULL, 0);
    const p_copy_b2b c2 = {a, a + 0x3000, 4096};
    p = put(p, OP_COPY_B2B, &c2, sizeof c2);
    CHECK(xf_submit(b, stream, p), "fence_wait of the second mirror submission timed out");
    for (int i = 0; i < 4096; i++)
        CHECK(m[0x3000 + i] == m[i], "B2B byte %d after a CPU write is %02x, expected %02x (write not mirrored)", i,
              m[0x3000 + i], m[i]);
    // 3: CPU bytes in the page of a GPU fill, written before the submission, survive the write-back.
    memset(m + 0x4000, 0x11, 0x800);
    p = put(stream, OP_BEGIN, NULL, 0);
    const p_clear_buf f3 = {a + 0x4800, 0x800, 0x77777777u, 0};
    p = put(p, OP_CLEAR_BUF, &f3, sizeof f3);
    CHECK(xf_submit(b, stream, p), "fence_wait of the third mirror submission timed out");
    for (int i = 0; i < 0x800; i++) CHECK(m[0x4000 + i] == 0x11, "CPU byte %#x beside a fill is %02x", i, m[0x4000 + i]);
    for (int i = 0; i < 0x800; i++) CHECK(m[0x4800 + i] == 0x77, "fill byte %#x is %02x", i, m[0x4800 + i]);
    b->pool_fini(&pool);
    *bytes = 4096 + 0x1000 + 4096 + 0x1000;
    return 0;
}

// Transform feedback (docs/maxwell.md 5.10) with XC2's own shader: shd_post.wishp program 127, vertex stage only, stores
// two vec3 of 24 bytes. With p = (a0.x * a1.x, a0.y * a1.y, a0.z), output 0 is the rows c3[11..13] times p and output 1
// the rows c3[14..16] times p. Two draws between one Begin and End continue where the first stopped; the records reach
// guest memory and a GPU copy after the Barrier; a buffer of two records drops the third; an instanced draw stores its
// records instance by instance; discard leaves the target alone.
#define XFB_SHADER "a2f7735bb66a8479"
static int test_xfb(const GfxBackend *b, uint8_t *guest, int *n_floats) {
    *n_floats = 0;
    // The layouts of the five XC2 shaders add up; one that does not counts as none.
    uint8_t ctl[0x880] = {0}, words[GFX_XFB_WORDS];
    static const uint8_t comps52[4] = {3, 3, 3, 4};
    for (int i = 0; i < 4; i++) ctl[0xdc + i] = (uint8_t)(0x20 + i), ctl[0x4dc + i] = comps52[i];
    uint32_t stride = 52, count = 4;
    memcpy(ctl + 0x6dc, &stride, 4), memcpy(ctl + 0x6e8, &count, 4);
    CHECK(gfx_shader_xfb(ctl, words) == 52 && words[0] == 0x20 && words[2] == 0x22 && words[3] == 0x24 && words[9] == 0x2c &&
          words[12] == 0x2f, "transform feedback layout of 52 bytes");
    stride = 48;
    memcpy(ctl + 0x6dc, &stride, 4);
    CHECK(!gfx_shader_xfb(ctl, words), "a layout that does not add up to its stride was taken");
    if (!vk_ctx()->has_transform_feedback) return 0;

    char root[PLAT_PATH_MAX * 2], path[PLAT_PATH_MAX * 2 + 64];
    CHECK(dr_root(root, sizeof root), GFX_CENSUS_DIR "/jobs.json not found");
    snprintf(path, sizeof path, "%s/" GFX_CENSUS_DIR "/jobs.json", root);
    long size;
    char *jobs = dr_read(path, &size);
    DrawEnt *e = malloc(sizeof *e * 20000);
    int n = jobs && e ? dr_parse(jobs, e, 20000) : 0, at = -1;
    for (int i = 0; i < n && at < 0; i++)
        if (!memcmp(e[i].key, XFB_SHADER, sizeof XFB_SHADER - 1)) at = i;
    CHECK(at >= 0, "census has no shader " XFB_SHADER);
    const DrawEnt *one[1] = {&e[at]};
    prog_t *prog = dr_prog(root, one, 1);
    CHECK(prog, "census code file of " XFB_SHADER " is missing");
    uint8_t *pc = (uint8_t *)(uintptr_t)prog->sd[0].control;
    stride = 24, count = 2;
    pc[0xdc] = 0x20, pc[0xdd] = 0x21, pc[0x4dc] = pc[0x4dd] = 3;
    memcpy(pc + 0x6dc, &stride, 4), memcpy(pc + 0x6e8, &count, 4);

    pool_t pool = {.magic = NVN_POOL_MAGIC, .flags = 0x22, .mem = guest + 0xD00000, .size = 0x100000,
                   .game_mem = guest + 0xD00000};
    CHECK(b->pool_init(&pool), "pool_init of a CPU-visible pool for the transform feedback part");
    static tex_t color;
    init_tex(&color, &pool, 0, NVN_FORMAT_RGBA8);
    CHECK(color.L.size <= 0x80000, "transform feedback target exceeds its pool slot");
    float *vtx = (float *)(pool.mem + 0x80000), *c3 = (float *)(pool.mem + 0x90000);
    memset(vtx, 0, 3 * DR_STRIDE);
    for (int v = 0; v < 3; v++) {
        float *a0 = vtx + v * DR_ATTRS * 4, *a1 = a0 + 4;
        a0[0] = 1.0f + (float)v, a0[1] = 2.0f + (float)v, a0[2] = 3.0f + (float)v;
        a1[0] = 10.0f, a1[1] = 100.0f;
    }
    memset(c3, 0, 4096);
    for (int r = 0; r < 3; r++) c3[(11 + r) * 4 + r] = 1.0f, c3[(14 + r) * 4 + r] = 2.0f;
    uint8_t *rec = pool.mem + 0xA0000, *copy = pool.mem + 0xA1000, *small = pool.mem + 0xA2000, *inst = pool.mem + 0xA3000;
    memset(rec, 0xCC, 0x4000);
    uint64_t rec_a = (uint64_t)(uintptr_t)rec;

    static _Alignas(8) uint8_t stream[8192];
    uint8_t *q = draw_prefix(stream, prog, &pool, &color);
    const p_flag on = {1}, off = {0};
    const p_xfb_bind whole = {rec_a, 3 * 24}, two = {(uint64_t)(uintptr_t)small, 2 * 24},
                     four = {(uint64_t)(uintptr_t)inst, 4 * 24};
    const p_draw d0 = {NVN_DRAW_PRIMITIVE_POINTS, 0, 2, 0, 0}, d1 = {NVN_DRAW_PRIMITIVE_POINTS, 2, 1, 0, 0},
                 all = {NVN_DRAW_PRIMITIVE_POINTS, 0, 3, 0, 0}, twice = {NVN_DRAW_PRIMITIVE_POINTS, 0, 2, 0, 2};
    const p_copy_b2b cp = {rec_a, (uint64_t)(uintptr_t)copy, 3 * 24};
    q = put(q, OP_RASTER_DISCARD, &on, sizeof on);
    q = put(q, OP_XFB_BIND, &whole, sizeof whole);
    q = put(q, OP_XFB_ACTIVE, &on, sizeof on);
    q = put(q, OP_DRAW, &d0, sizeof d0);
    q = put(q, OP_DRAW, &d1, sizeof d1);
    q = put(q, OP_XFB_ACTIVE, &off, sizeof off);
    q = put(q, OP_BARRIER, NULL, 0);
    q = put(q, OP_COPY_B2B, &cp, sizeof cp);
    q = put(q, OP_XFB_BIND, &two, sizeof two);
    q = put(q, OP_XFB_ACTIVE, &on, sizeof on);
    q = put(q, OP_DRAW, &all, sizeof all);
    q = put(q, OP_XFB_ACTIVE, &off, sizeof off);
    q = put(q, OP_XFB_BIND, &four, sizeof four);   // instanced, as the game draws: records instance by instance
    q = put(q, OP_XFB_ACTIVE, &on, sizeof on);
    q = put(q, OP_DRAW, &twice, sizeof twice);
    q = put(q, OP_XFB_ACTIVE, &off, sizeof off);
    q = put(q, OP_RASTER_DISCARD, &off, sizeof off);
    q = put(q, OP_BARRIER, NULL, 0);
    q = put(q, OP_END, NULL, 0);
    CHECK(q <= stream + sizeof stream, "transform feedback op stream overflow");
    uint64_t handle = (uint64_t)(uintptr_t)stream;
    CHECK(b->fence_wait(b->submit(&handle, 1), 20000000000ull), "fence_wait of the transform feedback submission timed out");

    const float *got[4] = {(const float *)rec, (const float *)copy, (const float *)small, (const float *)inst};
    static const int records[4] = {3, 3, 2, 4}, wrap[4] = {3, 3, 3, 2};   // the fourth: vertices 0, 1 of each instance
    for (int s = 0; s < 4; s++)
        for (int v = 0; v < records[s]; v++) {
            const float u = (float)(v % wrap[s]);
            const float p[3] = {(1.0f + u) * 10.0f, (2.0f + u) * 100.0f, 3.0f + u};
            for (int k = 0; k < 3; k++) {
                CHECK(got[s][v * 6 + k] == p[k], "buffer %d record %d output 0 component %d is %g, expected %g", s, v, k, got[s][v * 6 + k], p[k]);
                CHECK(got[s][v * 6 + 3 + k] == 2.0f * p[k], "buffer %d record %d output 1 component %d is %g, expected %g", s, v, k, got[s][v * 6 + 3 + k], 2.0f * p[k]);
                *n_floats += 2;
            }
        }
    for (int i = 0; i < 24; i++)
        CHECK(rec[3 * 24 + i] == 0xCC && small[2 * 24 + i] == 0xCC && inst[4 * 24 + i] == 0xCC, "byte %d past the records was written", i);
    static uint32_t px[TEX_W * TEX_H];
    const NVNcopyRegion r = {0, 0, 0, TEX_W, TEX_H, 1};
    b->read_texels(&color, NULL, &r, (uint8_t *)px, TEX_W * 4, TEX_W * TEX_H * 4);
    const uint8_t clear_b[4] = {to_byte(0.2f), to_byte(0.4f), to_byte(0.6f), 255};
    uint32_t clear_v;
    memcpy(&clear_v, clear_b, 4);
    for (int k = 0; k < TEX_W * TEX_H; k++) CHECK(px[k] == clear_v, "texel %d changed under rasterizer discard", k);
    b->pool_fini(&pool);
    free(e), free(jobs);
    return 0;
}

int probe_vkexec(void) {
    layouts_equal = 0;
    main_tid = plat_thread_id();
    const GfxBackend *b = &gfx_vulkan_backend;
    uint64_t heap = 0;
    CHECK(mem_set_heap_size(32ull << 20, &heap) == 0 && heap, "SetHeapSize 32 MiB");   // the feedback part's targets sit above 16 MiB
    uint8_t *guest = (uint8_t *)(uintptr_t)heap;
    uint64_t first = 0, last = 0;
    int w = 0, h = 0, inside = 0, xfer_bytes = 0;
    int bad = test_sync(b, guest, &first, &last) || test_present(b, guest, &w, &h);
    int sync_bad = bad;
    if (!bad) bad = test_clear(b, guest, &inside);
    int draw_bad = 0, drawn = 0, tries = 0, dispatched = 0, alpha = 0;
    if (!bad) bad = draw_bad = vk_pipeline_selftest() || test_draw(b, guest, &drawn, &tries, &dispatched, &alpha);
    if (!bad) bad = test_xfer(b, guest, &xfer_bytes);
    int zero_bytes = 0;
    if (!bad) bad = test_c0() || test_zero(b, guest, &zero_bytes);
    int nvnx_bytes = 0, mirror_bytes = 0;
    if (!bad) bad = test_nvnx(b, guest, &nvnx_bytes);
    if (!bad) bad = test_mirror(b, guest, &mirror_bytes);
    int xfb_floats = 0;
    if (!bad) bad = test_xfb(b, guest, &xfb_floats);
    b->finish();
    vk_pipeline_shutdown();   // the pipelines and the draw part's stand-ins go before the device
    vk_exec_draw_shutdown();
    vk_exec_xfer_shutdown();
    vk_backend_shutdown();
    long errors = vk_validation_errors_total();
    if (bad || errors) {
        printf("probe-vkexec: FAILED (%s, %ld validation errors)\n",
               sync_bad ? "a sync check failed" : draw_bad ? "a draw check failed" : bad ? "a clear or xfer check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-vkexec: sync part passed (%d submissions, fences %llu..%llu, %d completion callbacks in submit order "
           "on the completion thread, guest word visible after fence_wait, finish drained, present %dx%d exact, "
           "0 validation errors)\n",
           SUBMITS + TAIL + 1, (unsigned long long)first, (unsigned long long)last, SUBMITS + TAIL, w, h);
    printf("probe-vkexec: clear part passed (RGBA8 + D32 %dx%d: image clears outside a pass, scissored attachment "
           "clears in a pass on %d texels, read back exact, 0 validation errors)\n",
           TEX_W, TEX_H, inside);
    printf("probe-vkexec: draw part passed (pipeline self-test; one triangle of census pair %d changed %d of %d texels of the "
           "cleared RGBA8 target; %d census compute shaders dispatched; alpha test greater ref 2 and never discard every "
           "fragment and gequal 0 keeps the draw (%d texels); 0 validation errors)\n",
           tries, drawn, TEX_W * TEX_H, dispatched, alpha);
    if (layouts_equal)
        printf("probe-vkexec: layout part passed (the same draw through independent sets 0 and 1 and the shared set 0: "
               "%d of %d texels equal)\n", layouts_equal, TEX_W * TEX_H);
    else
        printf("probe-vkexec: layout part skipped (no graphics pipeline library: shared set 0 only)\n");
    printf("probe-vkexec: xfer part passed (B2B between CPU-visible pools, B2T + T2B round trip and T2T of a %dx%d RGBA8 "
           "texture exact, CLEAR_TEX exact, event WRITE + ADD, timestamp and samples-passed reports in guest memory after "
           "fence_wait, %d bytes compared, 0 validation errors)\n",
           XF_W, XF_H, xfer_bytes);
    printf("probe-vkexec: indirect part passed (DrawElementsIndirect with GPU-copied arguments: count 3 texel-equal to the "
           "direct draw, count 0 draws nothing, %d texels compared; %d census compute shaders dispatched indirectly)\n",
           ind_draw_texels, ind_dispatches);
    printf("probe-vkexec: nvnx part passed (ClearBuffer then ReportValue in GPU order, a CPU-invisible pool fill copied out, "
           "Downsample 64x64 -> 32x32 within 1 of the 2x2 average, stencil view aspect of D24S8, %d bytes compared)\n",
           nvnx_bytes);
    printf("probe-vkexec: c0 and zero parts passed (c0 tables, dispatch size, point size and base instance at their offsets, "
           "unbound slots zero; %d bytes of an unwritten physical-pool texture read zero, XC2_FIX_ZERO=%s)\n",
           zero_bytes, cfg_value("XC2_FIX_ZERO"));
    if (vk_mirror_on)
        printf("probe-vkexec: mirror part passed (B2B and fill in a VRAM mirror written back by the fence, a CPU write "
               "between submissions mirrored, CPU bytes beside a fill kept, %d bytes compared)\n", mirror_bytes);
    else
        printf("probe-vkexec: mirror part skipped (memory mode import)\n");
    if (xfb_floats)
        printf("probe-vkexec: transform feedback part passed (XC2's shader " XFB_SHADER ": two draws in one Begin and End, the "
               "records in guest memory and through a GPU copy, a full buffer dropping the rest, an instanced draw, %d floats compared, target "
               "unchanged under discard)\n", xfb_floats);
    else
        printf("probe-vkexec: transform feedback part skipped (no VK_EXT_transform_feedback)\n");
    return 0;
}
