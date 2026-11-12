// The recorder of the captures (replay.h): a back end that logs each call and forwards it to the real one.
//
// Calls are logged in the order the back end receives them: one decorated call runs at a time (rec_lock), and its
// record is appended before the call is made, so the replay meets the same order. Submissions and pool calls first
// append the guest memory the CPU wrote since the previous record (vk_mem_record_pages), because the back end reads
// those bytes when it runs the submission. The record stays sequential on purpose: the replay is one thread, and a
// present is compared with what the record took after the same work.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_obj.h"
#include "nvn_tex.h"
#include "replay.h"
#include "rec_menu.h"
#include "vk.h"
#include "platform/platform.h"

typedef struct { const void *p; size_t n; } Part;
#define PUT(kind, ...) put(kind, (const Part[]){__VA_ARGS__}, sizeof((const Part[]){__VA_ARGS__}) / sizeof(Part))

static const GfxBackend *real;                 // the back end the calls go to; set once by rec_wrap
static PlatMutex rec_lock = PLAT_MUTEX_INIT;   // one decorated call at a time: the record keeps the calls' order
static PlatMutex log_lock = PLAT_MUTEX_INIT;   // leaf lock: memory.c's decommit appends under its frozen-region lock
// The log is written with positional writes through the platform's file layer, not stdio: a stdio write failed with
// EINVAL in the middle of a run, and the C library's descriptor state is not something the game's other file code
// should be able to touch. log_lock guards the file, its offset and the buffer.
#define LOG_BUF (1u << 20)
static PlatFile *log_f;                        // log_lock; NULL when no capture is open
static uint64_t log_off;                       // log_lock: where the buffer's first byte goes in the file
static uint8_t *log_buf;                       // log_lock: LOG_BUF bytes
static size_t log_len;                         // log_lock: bytes waiting in log_buf
static char shots_dir[PLAT_PATH_MAX];          // XC2_RECORD_SHOTS, "" when unset; set once by rec_wrap
static uint64_t frames_limit, presents;        // rec_lock: recorded presents (from the arm point on)
static uint32_t gen_since;                     // rec_lock: the page generation of the last memory record
static uint64_t flush_ns, flush_n;             // rec_lock: time in flush_memory and the flushes (close_log reports them)
static int armed;                              // log_lock: 1 while the record takes calls (from the arm point, rec_arm)
static uint64_t arm_at, presents_all;          // rec_lock: XC2_RECORD_FROM-1, the present the record arms at; all presents
static char arm_file[PLAT_PATH_MAX];           // XC2_RECORD_ARM_FILE: arms at the first present after this file appears
static uint64_t script_frames;                 // XC2_RECORD_FRAMES: the frames a scripted capture takes (0: one)
static int wrapped;                            // set once by rec_wrap: the recorder is on for the run, so the journal runs
static char rec_dir[PLAT_PATH_MAX];            // the folder the captures go to: XC2_RECORD, else captures
static char cur_dir[PLAT_PATH_MAX * 2];        // rec_lock: the folder of the capture that runs (frame-N or clip-N)
static uint64_t cur_first;                     // rec_lock: the number of the first frame that capture takes
static int cur_kind;                           // log_lock: REC_MENU_FRAME or REC_MENU_CLIP while a capture runs
static int write_failed;                       // log_lock: put could not write, and the capture was dropped (capture_reap)
static int req_kind;                           // the F1 menu's request (rec_menu_request), taken by the next present

static int rec_on(void) {
    plat_mutex_lock(&log_lock);
    int on = log_f != NULL && armed;
    plat_mutex_unlock(&log_lock);
    return on;
}

// The journal runs for the whole process: a capture that starts later needs the objects alive then.
static int journal_on(void) { return wrapped; }

// Writes the buffer out at log_off. log_lock held.
static int log_flush(void) {
    size_t at = 0;
    while (at < log_len) {
        int64_t k = plat_file_pwrite(log_f, log_buf + at, (uint32_t)(log_len - at), log_off);
        if (k <= 0) return 0;
        at += (size_t)k, log_off += (uint64_t)k;
    }
    log_len = 0;
    return 1;
}

// Appends n bytes to the log, through the buffer. log_lock held. A NULL p writes zeros: the record must not crash the
// game over a pointer the back end would not have read anyway.
static int log_bytes(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        if (log_len == LOG_BUF && !log_flush()) return 0;
        size_t k = LOG_BUF - log_len;
        if (k > n) k = n;
        if (b) memcpy(log_buf + log_len, b, k), b += k;
        else memset(log_buf + log_len, 0, k);
        log_len += k, n -= k;
    }
    return 1;
}

// Ends the log: the buffer goes out, then the file closes. log_lock held by the caller.
static void log_end(void) {
    if (!log_flush())
        rt_log("rec: the last part of the record did not reach the file (at %llu)", (unsigned long long)log_off);
    plat_file_close(log_f);
    log_f = NULL;
}

static void close_log(const char *why) {
    plat_mutex_lock(&log_lock);
    if (log_f) {
        log_end();
        armed = 0, cur_kind = 0;   // the next capture arms again
        rt_log("rec: recording stopped: %s; %llu memory flushes took %.1f s; log %llu bytes", why,
               (unsigned long long)flush_n, (double)flush_ns / 1e9, (unsigned long long)log_off);
    }
    plat_mutex_unlock(&log_lock);
    vk_mem_capture_hold(0);   // freezing resumes once no capture is open (outside log_lock: gen_lock is not nested in it)
}

// Opens a log at path and writes its header: the heap's place, as the replay maps it. log_lock held by the caller.
static int start_log(const char *path) {
    int err = 0;
    log_f = plat_fs_create(path, &err);
    if (!log_f) return 0;
    log_off = 0, log_len = 0;
    const RtGuestLayout *g = mem_layout();
    RecHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, REC_MAGIC, 8);
    h.version = REC_VERSION;
    h.sz_pool = sizeof(pool_t), h.sz_tex = sizeof(tex_t), h.sz_view = sizeof(view_t), h.sz_samp = sizeof(samp_par);
    h.sz_prog = sizeof(prog_t), h.sz_copy = sizeof(NVNcopyRegion);
    h.heap_base = g->heap.base, h.heap_size = g->heap.size;
    log_bytes(&h, sizeof h);
    return 1;
}

// A capture is running (its log is open and it takes calls).
static int capture_busy(void) {
    plat_mutex_lock(&log_lock);
    int b = armed;
    plat_mutex_unlock(&log_lock);
    return b;
}

static void rec_at_exit(void) { close_log("the game ended"); }

// Appends one record whose payload is the parts, in order.
static void put(uint32_t kind, const Part *parts, size_t count) {
    plat_mutex_lock(&log_lock);
    if (log_f && armed) {
        uint64_t total = 0;
        for (size_t i = 0; i < count; i++) total += parts[i].n;
        RecRecord h = {.kind = kind, .size = total};
        int ok = log_bytes(&h, sizeof h);
        for (size_t i = 0; ok && i < count; i++) ok = log_bytes(parts[i].p, parts[i].n);
        if (!ok) {   // a full disk, most likely: the capture is dropped, and the next present lets freezing resume
            rt_log("rec: cannot write the record (kind %u, %llu bytes at %llu); the capture is incomplete and stops here",
                   kind, (unsigned long long)total, (unsigned long long)log_off);
            plat_file_close(log_f);
            log_f = NULL;
            armed = 0, cur_kind = 0, write_failed = 1;
        }
    }
    plat_mutex_unlock(&log_lock);
}

// A capture that put dropped (a failed write) still holds freezing off: put runs under locks the hold may not be released
// under, so the next present releases it here. rec_lock held.
static void capture_reap(void) {
    plat_mutex_lock(&log_lock);
    int failed = write_failed;
    write_failed = 0;
    plat_mutex_unlock(&log_lock);
    if (failed) vk_mem_capture_hold(0);
}

// ---- the journal: the objects the back end holds, as the calls made them (rec_lock held). A capture that starts after
// present N-1 writes them first (rec_arm), so the replay starts with the objects the frame N finds. Slots and
// samplers keep their last registration; a pool keeps its init bytes and its maps until its fini.
typedef struct { int used, has_view; tex_t t; view_t v; } SlotRec;
static SlotRec *slots;
static size_t nslots, cslots;
typedef struct { int used; samp_par s; } SampRec;
static SampRec *samps;
static size_t nsamps, csamps;
typedef struct { uint64_t obj; pool_t p; uint8_t *maps; size_t maps_n, maps_cap; } PoolRec;
static PoolRec *pools;
static size_t npools, cpools;

#define JOURNAL_IDS (1u << 20)   // the back end's slot limit (vk_texture_register)

static void *grow_to(void *p, size_t *have, size_t need, size_t elt) {
    if (need <= *have) return p;
    size_t n = *have ? *have : 256;
    while (n < need) n *= 2;
    void *q = realloc(p, n * elt);
    if (!q) rt_fatal("rec: out of memory for the object journal");
    memset((char *)q + *have * elt, 0, (n - *have) * elt);
    *have = n;
    return q;
}

static void journal_slot(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || (uint32_t)id >= JOURNAL_IDS) return;
    slots = grow_to(slots, &cslots, (size_t)id + 1, sizeof *slots);
    if ((size_t)id + 1 > nslots) nslots = (size_t)id + 1;
    SlotRec *s = &slots[id];
    s->used = 1, s->has_view = v != NULL, s->t = *t;
    if (v) s->v = *v;
    else memset(&s->v, 0, sizeof s->v);
}

static void journal_samp(int id, const samp_par *s) {
    if (id < 0 || (uint32_t)id >= JOURNAL_IDS) return;
    samps = grow_to(samps, &csamps, (size_t)id + 1, sizeof *samps);
    if ((size_t)id + 1 > nsamps) nsamps = (size_t)id + 1;
    samps[id].used = 1, samps[id].s = *s;
}

static PoolRec *pool_find(uint64_t obj) {
    for (size_t i = 0; i < npools; i++)
        if (pools[i].obj == obj) return &pools[i];
    return NULL;
}

static void journal_pool_init(pool_t *p) {
    uint64_t obj = (uint64_t)(uintptr_t)p;
    PoolRec *r = pool_find(obj);
    if (!r) {
        if (npools == cpools) {
            size_t n = cpools ? cpools * 2 : 64;
            PoolRec *q = realloc(pools, n * sizeof *pools);
            if (!q) rt_fatal("rec: out of memory for the pool journal");
            pools = q, cpools = n;
        }
        r = &pools[npools++];
        memset(r, 0, sizeof *r);
        r->obj = obj;
    }
    r->maps_n = 0;   // a second init of a live pool starts with no maps
    r->p = *p;
}

static void journal_pool_fini(pool_t *p) {
    uint64_t obj = (uint64_t)(uintptr_t)p;
    for (size_t i = 0; i < npools; i++) {
        if (pools[i].obj != obj) continue;
        free(pools[i].maps);
        pools[i] = pools[--npools];
        memset(&pools[npools], 0, sizeof pools[npools]);
        return;
    }
}

static void journal_pool_map(pool_t *v, int n, const void *requests) {
    PoolRec *r = pool_find((uint64_t)(uintptr_t)v);
    if (!r || n < 0) return;
    size_t need = 8 + (size_t)n * 0x28;
    if (r->maps_n + need > r->maps_cap) {
        size_t cap = r->maps_cap ? r->maps_cap : 4096;
        while (cap < r->maps_n + need) cap *= 2;
        uint8_t *q = realloc(r->maps, cap);
        if (!q) rt_fatal("rec: out of memory for the pool maps");
        r->maps = q, r->maps_cap = cap;
    }
    uint32_t hd[2] = {(uint32_t)n, 0};
    memcpy(r->maps + r->maps_n, hd, 8);
    memcpy(r->maps + r->maps_n + 8, requests, (size_t)n * 0x28);
    r->maps_n += need;
}

// The replay's copy of the heap starts zeroed, and only DELTA records fill it in. known_nz has one bit per heap page
// that may be non-zero in the replay: a page the CPU wrote as zeros goes into the record only when the replay still
// holds something there. That keeps the boot's zero-filled buffers out of the record. Atomic: decommit clears bits
// under the memory's frozen-region lock, the flush sets them under the generation lock.
static uint8_t *known_nz;
static uint64_t known_pages;
static uint64_t run_lo, run_hi;   // rec_lock: the pages of the DELTA record being gathered (run_hi 0: none)

static void run_flush(void) {
    if (!run_hi) return;
    uint64_t hd[2] = {run_lo, run_hi - run_lo};
    PUT(RR_DELTA, {hd, sizeof hd}, {(const void *)(uintptr_t)run_lo, (size_t)(run_hi - run_lo)});
    run_hi = 0;
}

static void known_set(uint64_t i, int nz) {
    if (nz) __atomic_fetch_or(&known_nz[i >> 3], (uint8_t)(1u << (i & 7)), __ATOMIC_RELAXED);
    else __atomic_fetch_and(&known_nz[i >> 3], (uint8_t)~(1u << (i & 7)), __ATOMIC_RELAXED);
}

// vk_mem_record_pages: one written page, in address order.
static void page_cb(void *ctx, uint64_t addr) {
    const RtGuestLayout *g = mem_layout();
    uint64_t i = (addr - g->heap.base) >> 12;
    if (i >= known_pages) return;   // outside the heap block (not written by the back end's view)
    const uint64_t *w = (const uint64_t *)(uintptr_t)addr;
    int nz = 0;
    for (int k = 0; k < 512 && !nz; k++) nz = w[k] != 0;
    int was = (__atomic_load_n(&known_nz[i >> 3], __ATOMIC_RELAXED) >> (i & 7)) & 1;
    if (!nz && !was) return;
    known_set(i, nz);
    if (run_hi && run_hi == addr) {
        run_hi += 0x1000;
    } else {
        run_flush();
        run_lo = addr, run_hi = addr + 0x1000;
    }
    (void)ctx;
}

// Memory the CPU wrote since the last record, as DELTA records: runs of consecutive pages, each with its bytes now.
// Only the committed part of the heap block is queried: the rest has never been committed, so the CPU wrote nothing there.
static void flush_memory(void) {
    const RtGuestLayout *g = mem_layout();
    uint64_t committed = mem_heap_committed();
    if (committed > g->heap.size) committed = g->heap.size;
    uint64_t t0 = plat_now_ns();
    gen_since = vk_mem_record_pages(gen_since, g->heap.base, g->heap.base + committed, page_cb, NULL);
    run_flush();
    uint64_t dt = plat_now_ns() - t0;
    flush_ns += dt, flush_n++;
    if (dt > 200000000u) rt_log("rec: a memory flush took %llu ms (flush %llu)", (unsigned long long)(dt / 1000000u),
                                (unsigned long long)flush_n);
}

// ---- the decorated back end

static int d_pool_init(pool_t *p) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) {
        flush_memory();
        uint64_t obj = (uint64_t)(uintptr_t)p;
        PUT(RR_POOL_INIT, {&obj, sizeof obj}, {p, sizeof *p});
    }
    int r = real->pool_init(p);
    if (journal_on()) journal_pool_init(p);
    plat_mutex_unlock(&rec_lock);
    return r;
}

static void d_pool_fini(pool_t *p) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) {
        flush_memory();
        uint64_t obj = (uint64_t)(uintptr_t)p;
        PUT(RR_POOL_FINI, {&obj, sizeof obj}, {p, sizeof *p});
    }
    real->pool_fini(p);
    if (journal_on()) journal_pool_fini(p);
    plat_mutex_unlock(&rec_lock);
}

static int d_pool_map_virtual(pool_t *v, int n, const void *requests) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) {
        uint64_t obj = (uint64_t)(uintptr_t)v;
        uint32_t hd[2] = {(uint32_t)n, 0};
        PUT(RR_POOL_MAP, {&obj, sizeof obj}, {hd, sizeof hd}, {requests, (size_t)n * 0x28});
    }
    int r = real->pool_map_virtual(v, n, requests);
    if (journal_on()) journal_pool_map(v, n, requests);
    plat_mutex_unlock(&rec_lock);
    return r;
}

static void d_texture_register(int id, const tex_t *t, const view_t *v) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) {
        // A NULL view is a slot without one (vk_texture_register): a flag says so, and the view's bytes are zero.
        uint32_t hd[2] = {(uint32_t)id, v != NULL};
        static const view_t no_view;
        PUT(RR_TEX_REG, {hd, sizeof hd}, {t, sizeof *t}, {v ? v : &no_view, sizeof no_view});
    }
    real->texture_register(id, t, v);
    if (journal_on()) journal_slot(id, t, v);
    plat_mutex_unlock(&rec_lock);
}

static void d_sampler_register(int id, const samp_par *s) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) {
        uint32_t hd[2] = {(uint32_t)id, 0};
        PUT(RR_SAMP_REG, {hd, sizeof hd}, {s, sizeof *s});
    }
    real->sampler_register(id, s);
    if (journal_on()) journal_samp(id, s);
    plat_mutex_unlock(&rec_lock);
}

static void d_texture_release(const tex_t *t) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) PUT(RR_TEX_REL, {t, sizeof *t});
    real->texture_release(t);
    plat_mutex_unlock(&rec_lock);
}

static void d_program_release(const prog_t *p) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) PUT(RR_PROG_REL, {p, sizeof *p});
    real->program_release(p);
    plat_mutex_unlock(&rec_lock);
}

// ---- command blocks. The NVN layer keeps the command streams in host memory: a submitted handle is the address of a copy,
// and the copies' OP_JUMP and OP_CALL name other copies. The back end reads them by address, so the record takes the bytes
// of each block a submission reaches (replay.h, RR_CMD). A block runs from its first op to its OP_END or OP_JUMP.
#define CMD_MAX (16u << 20)
#define CMD_DEPTH 32
static uint64_t *cmd_seen;      // rec_lock: the block addresses recorded for the current submission
static size_t cmd_nseen, cmd_cap;

static int cmd_seen_has(uint64_t a) {
    for (size_t i = 0; i < cmd_nseen; i++)
        if (cmd_seen[i] == a) return 1;
    return 0;
}

static void cmd_block(uint64_t addr, int depth) {
    if (!addr || cmd_seen_has(addr)) return;
    if (cmd_nseen == cmd_cap) {
        cmd_cap = cmd_cap ? cmd_cap * 2 : 256;
        cmd_seen = realloc(cmd_seen, cmd_cap * sizeof *cmd_seen);
        if (!cmd_seen) rt_fatal("rec: out of memory for the command block list");
    }
    cmd_seen[cmd_nseen++] = addr;
    uint64_t total = 0;
    const op_hdr *h = (const op_hdr *)(uintptr_t)addr;
    for (;;) {
        if (!plat_mem_readable((uint64_t)(uintptr_t)h, sizeof *h)) break;
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_ || total + h->size > CMD_MAX) break;
        if (!plat_mem_readable((uint64_t)(uintptr_t)h, h->size)) break;
        total += h->size;
        const void *a = nvn_op_payload(h);
        if ((h->op == OP_JUMP || h->op == OP_CALL) && h->size >= sizeof *h + sizeof(p_jump)) {
            uint64_t target = ((const p_jump *)a)->target;
            if (h->op == OP_JUMP) {   // the stream continues in the target: the block ends here
                cmd_block(target, depth);
                break;
            }
            if (depth < CMD_DEPTH) cmd_block(target, depth + 1);
        }
        if (h->op == OP_END) break;
        h = nvn_op_next(h);
    }
    if (!total) return;
    uint64_t hd[2] = {addr, total};
    PUT(RR_CMD, {hd, sizeof hd}, {(const void *)(uintptr_t)addr, (size_t)total});
}

// ---- texture pins. A pin never changes once made, so each goes into the record once (RR_PIN, replay.h).
static nvn_hmap pin_sent = {.klen = 8};   // rec_lock: pin address -> 1

static void pin_send(const tex_t *pin, void *ctx) {
    (void)ctx;
    uint64_t a = (uint64_t)(uintptr_t)pin;
    if (nvn_hm_get(&pin_sent, &a)) return;
    nvn_hm_put(&pin_sent, &a, (void *)1);
    uint64_t hd[2] = {a, sizeof *pin};
    PUT(RR_PIN, {hd, sizeof hd}, {pin, sizeof *pin});
}

void rec_trace_submit(const char *who, const uint64_t *handles, int n) {
    const char *t = cfg_get("XC2_REC_TRACE");
    if (!t || !*t) return;
    for (int i = 0; i < n; i++) {
        uint64_t w[4] = {0, 0, 0, 0};
        plat_mem_read(handles[i], w, sizeof w);
        fprintf(stderr, "%s: submit %d handle %#llx: %016llx %016llx %016llx %016llx\n", who, i,
                (unsigned long long)handles[i], (unsigned long long)w[0], (unsigned long long)w[1],
                (unsigned long long)w[2], (unsigned long long)w[3]);
    }
}

static uint64_t d_submit(const uint64_t *handles, int n) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) {
        if (n > 0) {
            flush_memory();
            nvn_pins_each(pin_send, NULL);   // the pins before the blocks that point at them
            cmd_nseen = 0;
            for (int i = 0; i < n; i++) cmd_block(handles[i], 0);
        }
        rec_trace_submit("rec", handles, n);
        uint32_t hd[2] = {(uint32_t)n, 0};
        PUT(RR_SUBMIT, {hd, sizeof hd}, {handles, n > 0 ? (size_t)n * 8 : 0});
    }
    uint64_t f = real->submit(handles, n);
    plat_mutex_unlock(&rec_lock);
    return f;
}

static int d_fence_wait(uint64_t value, uint64_t timeout_ns) { return real->fence_wait(value, timeout_ns); }
static uint64_t d_fence_done(void) { return real->fence_done(); }

static void d_finish(void) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) PUT(RR_FINISH, {NULL, 0});
    real->finish();
    plat_mutex_unlock(&rec_lock);
}

static void rec_arm(void);   // starts the record at its preamble (below)

// Starts a capture, from the menu or from a script alike: its log goes to <rec_dir>/frame-N or clip-N, N the frame it
// starts from, and rec_arm writes the state that frame starts from. The frame after this present is the first one taken.
// limit is the number of presents it takes (0: until it is stopped). rec_lock held.
static void capture_start(int kind, uint64_t limit) {
    char path[PLAT_PATH_MAX * 2 + 16];
    close_log("replaced by a new capture");
    cur_first = presents_all + 1;
    snprintf(cur_dir, sizeof cur_dir, "%s/%s-%llu", rec_dir, kind == REC_MENU_FRAME ? "frame" : "clip",
             (unsigned long long)cur_first);
    snprintf(path, sizeof path, "%s/log.bin", cur_dir);
    plat_file_mkdir_p(cur_dir);
    plat_file_delete(path);   // a capture of the same frame from an earlier run is replaced
    plat_mutex_lock(&log_lock);
    armed = 0, write_failed = 0;
    int ok = start_log(path);
    if (ok) cur_kind = kind;
    plat_mutex_unlock(&log_lock);
    if (!ok) {
        rt_log("rec: cannot create %s: no capture", path);
        return;
    }
    presents = 0;
    frames_limit = limit;
    rt_log("rec: %s capture into %s", kind == REC_MENU_FRAME ? "frame" : "clip", cur_dir);
    rec_arm();
}

// A scripted capture (XC2_RECORD_FROM, XC2_RECORD_ARM_FILE): one frame, or a clip of XC2_RECORD_FRAMES frames. rec_lock held.
static void script_start(void) {
    capture_start(script_frames > 1 ? REC_MENU_CLIP : REC_MENU_FRAME, script_frames ? script_frames : 1);
}

// info.txt of the capture's folder, written at its first present: what the capture is, for whoever opens the folder
// later. t is the presented texture and hash its picture's (rec_picture). rec_lock held.
static void write_info(const tex_t *t, uint64_t hash) {
    char path[PLAT_PATH_MAX * 2 + 16], text[1024];
    snprintf(path, sizeof path, "%s/info.txt", cur_dir);
    plat_mutex_lock(&log_lock);
    int kind = cur_kind;
    plat_mutex_unlock(&log_lock);
    int n = snprintf(text, sizeof text,
                     "kind: %s\nfirst frame: %llu\npicture: %dx%d, NVN format %#x\nfirst picture hash: %016llx\n"
                     "record version: %d\nbuild: %s\nreplay: xc2.exe --replay <this folder>\n"
                     "live-<n>.png is what the game presented; the replay writes replay-<n>.png next to it.\n",
                     kind == REC_MENU_FRAME ? "frame" : "clip", (unsigned long long)cur_first, (int)t->d.width,
                     (int)t->d.height, (unsigned)t->d.format, (unsigned long long)hash, REC_VERSION, rt_port_version);
    if (n < 0) return;
    if ((size_t)n >= sizeof text) n = (int)sizeof text - 1;
    plat_file_delete(path);
    int err = 0;
    PlatFile *f = plat_fs_create(path, &err);
    if (!f) return;   // the capture itself does not depend on this file
    plat_file_pwrite(f, text, (uint32_t)n, 0);
    plat_file_close(f);
}

static void d_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    plat_mutex_lock(&rec_lock);
    real->present(t, crop, at_ns);   // the frame's work is queued or done; the picture is read back below, after it
    presents_all++;
    if (rec_on()) {
        uint8_t *px = NULL;
        uint64_t hash = 0;
        size_t n = rec_picture(real, t, &px, &hash);
        presents++;
        uint32_t hc[6] = {crop ? 1u : 0u, 0, crop ? (uint32_t)crop[0] : 0, crop ? (uint32_t)crop[1] : 0,
                          crop ? (uint32_t)crop[2] : 0, crop ? (uint32_t)crop[3] : 0};
        uint64_t tail[3] = {at_ns, presents_all, hash};
        PUT(RR_PRESENT, {t, sizeof *t}, {hc, sizeof hc}, {tail, sizeof tail});
        if (presents == 1) write_info(t, hash);
        // The picture the game showed goes next to the record, so a replay's picture has something to be compared with
        // by eye: every present of a frame capture, the first of a clip (XC2_RECORD_SHOTS keeps them all).
        if (px && (presents == 1 || rec_menu_state() == REC_MENU_FRAME)) {
            char path[PLAT_PATH_MAX * 2 + 32];
            snprintf(path, sizeof path, "%s/live-%06llu.png", cur_dir, (unsigned long long)presents);
            rec_save_picture(path, t, px, n);
        }
        if (px && shots_dir[0]) {
            char path[PLAT_PATH_MAX * 2];
            snprintf(path, sizeof path, "%s/p%06llu.png", shots_dir, (unsigned long long)presents);
            rec_save_picture(path, t, px, n);
        }
        free(px);
        if (frames_limit && presents >= frames_limit) close_log("its frames are recorded");
    }
    capture_reap();
    // XC2_RECORD_FROM: the capture starts after present N-1, so frame N is the first one it takes.
    if (arm_at && presents_all == arm_at && !capture_busy()) script_start();
    // XC2_RECORD_ARM_FILE: "capture now". The file is created while the game shows the state; the next frame is the one
    // recorded, and the file is removed so a second capture needs a new one.
    if (arm_file[0] && !capture_busy() && plat_file_exists(arm_file)) {
        plat_file_delete(arm_file);
        rt_log("rec: %s found: capturing from the next frame", arm_file);
        script_start();
    }
    // The F1 menu's buttons (rec_menu.h): a request taken here starts or stops a capture after this present.
    int want = __atomic_exchange_n(&req_kind, 0, __ATOMIC_ACQ_REL);
    if (want == REC_MENU_STOP) close_log("stopped from the F1 menu");
    else if (want && wrapped && !capture_busy()) capture_start(want, want == REC_MENU_FRAME ? 1 : 0);
    plat_mutex_unlock(&rec_lock);
}

static void d_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                          uint64_t img) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) {
        uint64_t tail[3] = {row, img, vk_texture_texel_span(t, v, r, row, img)};
        PUT(RR_READ, {t, sizeof *t}, {v, sizeof *v}, {r, sizeof *r}, {tail, sizeof tail});
    }
    real->read_texels(t, v, r, dst, row, img);
    plat_mutex_unlock(&rec_lock);
}

static void d_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row,
                           uint64_t img) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) {
        uint64_t n = vk_texture_texel_span(t, v, r, row, img);
        uint64_t tail[3] = {row, img, n};
        PUT(RR_WRITE, {t, sizeof *t}, {v, sizeof *v}, {r, sizeof *r}, {tail, sizeof tail}, {src, (size_t)n});
    }
    real->write_texels(t, v, r, src, row, img);
    plat_mutex_unlock(&rec_lock);
}

static void d_guest(uint32_t which, uint64_t addr, uint64_t size) {
    uint32_t hd[2] = {which, 0};
    PUT(RR_GUEST, {hd, sizeof hd}, {&addr, sizeof addr}, {&size, sizeof size});
}

static void d_guest_written(uint64_t addr, uint64_t size) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) d_guest(0, addr, size);
    real->guest_written(addr, size);
    plat_mutex_unlock(&rec_lock);
}

static void d_guest_read(uint64_t addr, uint64_t size) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) d_guest(1, addr, size);
    real->guest_read(addr, size);
    plat_mutex_unlock(&rec_lock);
}

static void d_cpu_synced(uint64_t fence) {
    plat_mutex_lock(&rec_lock);
    if (rec_on()) PUT(RR_CPU_SYNCED, {&fence, sizeof fence});
    real->cpu_synced(fence);
    plat_mutex_unlock(&rec_lock);
}

static const GfxBackend rec_be = {
    .name = "record",
    .pool_init = d_pool_init,
    .pool_fini = d_pool_fini,
    .pool_map_virtual = d_pool_map_virtual,
    .texture_register = d_texture_register,
    .sampler_register = d_sampler_register,
    .texture_release = d_texture_release,
    .program_release = d_program_release,
    .submit = d_submit,
    .fence_wait = d_fence_wait,
    .fence_done = d_fence_done,
    .present = d_present,
    .finish = d_finish,
    .read_texels = d_read_texels,
    .write_texels = d_write_texels,
    .guest_written = d_guest_written,
    .guest_read = d_guest_read,
    .cpu_synced = d_cpu_synced,
};

// ---- entry points

void rec_note_commit(uint64_t addr, uint64_t n) {
    if (!n) return;   // put does nothing when not recording
    uint64_t hd[2] = {addr, n};
    PUT(RR_COMMIT, {hd, sizeof hd});
}

void rec_note_decommit(uint64_t addr, uint64_t n) {
    if (!n) return;
    const RtGuestLayout *g = mem_layout();
    if (known_nz && addr >= g->heap.base && addr < g->heap.base + g->heap.size) {   // the replay zeroes these pages
        uint64_t lo = (addr - g->heap.base) >> 12, hi = ((addr + n + 0xfff) - g->heap.base) >> 12;
        if (hi > known_pages) hi = known_pages;
        for (uint64_t i = lo; i < hi; i++) known_set(i, 0);
    }
    uint64_t hd[2] = {addr, n};
    PUT(RR_DECOMMIT, {hd, sizeof hd});
}

size_t rec_picture(const GfxBackend *b, const tex_t *t, uint8_t **px, uint64_t *hash) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    *px = NULL, *hash = 0;
    if (!bpb || t->d.width <= 0 || t->d.height <= 0) return 0;
    NVNcopyRegion r = {.width = (int)t->d.width, .height = (int)t->d.height, .depth = 1};
    uint64_t row = (uint64_t)((r.width + bw - 1) / bw) * (uint64_t)bpb;
    uint64_t img = row * (uint64_t)((r.height + bh - 1) / bh);
    uint8_t *buf = malloc(img);
    if (!buf) rt_fatal("replay: out of memory for a picture");
    view_t v;
    memset(&v, 0, sizeof v);
    b->read_texels(t, &v, &r, buf, row, img);
    uint64_t h = 1469598103934665603ull;   // FNV-1a over 8-byte words, then the tail bytes
    uint64_t i = 0;
    for (; i + 8 <= img; i += 8) {
        uint64_t w;
        memcpy(&w, buf + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < img; i++) h = (h ^ buf[i]) * 1099511628211ull;
    *px = buf, *hash = h;
    return (size_t)img;
}

void rec_save_picture(const char *path, const tex_t *t, const uint8_t *px, size_t n) {
    uint32_t w = (uint32_t)t->d.width, h = (uint32_t)t->d.height;
    if (n != 4ull * w * h) return;   // only 4-byte texels are pictures here
    int bgra = t->d.format == NVN_FORMAT_BGRA8 || t->d.format == NVN_FORMAT_BGRA8_SRGB;
    if (!bgra && t->d.format != NVN_FORMAT_RGBA8 && t->d.format != NVN_FORMAT_RGBA8_SRGB) return;
    if (!bgra) {
        rt_write_png(path, px, w, h);
        return;
    }
    uint8_t *rgba = malloc(n ? n : 1);   // the caller's picture stays as it is: it may be saved twice
    if (!rgba) return;
    for (size_t i = 0; i + 4 <= n; i += 4)
        rgba[i] = px[i + 2], rgba[i + 1] = px[i + 1], rgba[i + 2] = px[i], rgba[i + 3] = px[i + 3];
    rt_write_png(path, rgba, w, h);
    free(rgba);
}

// The module images (loader.c) lie outside the heap, and the replay does not run the loader: the back end reads shader
// control blocks and constants out of their data sections. The record takes those sections once, as they are when it
// starts, with a COMMIT for their whole range. The code is never read, so it is left out. Writes the game makes to the
// sections later are not in the write-watch the record reads, so they do not reach the replay.
static void snap_module_data(void) {
    int n = ldr_count();
    for (int i = 0; i < n; i++) {
        const RtLoaded *m = ldr_module(i);
        // The text is included too: a shader's binary is read from its stage's data pointer (vk_exec_draw.c find_stage),
        // which points into the text, so a replay without it translates every shader from zeros and skips its draws.
        uint64_t lo = (m->base + 0xfffull) & ~0xfffull, hi = (m->base + m->size + 0xfffull) & ~0xfffull;
        if (hi <= lo) continue;
        rec_note_commit(lo, hi - lo);
        uint64_t a = 0, b = 0;   // the run of non-zero pages being gathered: [a, b), b 0 for none
        for (uint64_t p = lo; p <= hi; p += 0x1000) {
            int nz = 0;
            if (p < hi && plat_mem_readable(p, 0x1000)) {
                const uint64_t *w = (const uint64_t *)(uintptr_t)p;
                for (int k = 0; k < 512 && !nz; k++) nz = w[k] != 0;
            }
            if (nz && b == p) {
                b += 0x1000;
                continue;
            }
            if (b) {
                uint64_t hd[2] = {a, b - a};
                PUT(RR_DELTA, {hd, sizeof hd}, {(const void *)(uintptr_t)a, (size_t)(b - a)});
                b = 0;
            }
            if (nz) a = p, b = p + 0x1000;
        }
    }
}

// The live heap at the arm point: every committed page that is not zero now goes in as DELTA records, and the replay's
// zeroed heap then holds what the game holds. This is a full copy of the live bytes, so it is the size of the heap's
// non-zero part, not of the frame's reads: the later captures cut it down to the pages frame N reads.
static void snapshot_heap(uint64_t committed) {
    const RtGuestLayout *g = mem_layout();
    uint64_t lo = g->heap.base, hi = lo + committed;
    uint64_t a = 0, b = 0;   // the run of non-zero pages being gathered: [a, b), b 0 for none
    for (uint64_t p = lo; p <= hi; p += 0x1000) {
        int nz = 0;
        if (p < hi && plat_mem_readable(p, 0x1000)) {
            const uint64_t *w = (const uint64_t *)(uintptr_t)p;
            for (int k = 0; k < 512 && !nz; k++) nz = w[k] != 0;
        }
        if (nz) known_set((p - lo) >> 12, 1);
        if (nz && b == p) {
            b += 0x1000;
            continue;
        }
        if (b) {
            uint64_t hd[2] = {a, b - a};
            PUT(RR_DELTA, {hd, sizeof hd}, {(const void *)(uintptr_t)a, (size_t)(b - a)});
            b = 0;
        }
        if (nz) a = p, b = p + 0x1000;
    }
}

static void no_page(void *ctx, uint64_t addr) { (void)ctx, (void)addr; }

// vk_texture_snapshot: one level and layer of an image the guest bytes do not hold, as an IMAGE record. A part that
// cannot be read back is counted, so the arm line says what the capture lacks.
typedef struct { size_t parts, unread; uint64_t bytes; uint64_t last_serial; size_t images; } ImageSnap;

static void image_cb(void *ctx, const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *px, size_t n,
                     int owned) {
    ImageSnap *s = ctx;
    if (t->serial != s->last_serial) s->last_serial = t->serial, s->images++;   // the parts of one image come in a row
    if (!px) {
        if (s->unread++ < 8)
            rt_log("rec: image %#llx (%dx%d, NVN format %#x, level %u, layer %u) cannot be read back: the replay starts "
                   "without its content", (unsigned long long)(uintptr_t)t->mem, (int)t->d.width, (int)t->d.height,
                   (unsigned)t->d.format, (unsigned)v->base_level, (unsigned)v->min_layer);
        return;
    }
    uint32_t hd[2] = {owned != 0, 0};
    uint64_t len = n;
    PUT(RR_IMAGE, {t, sizeof *t}, {v, sizeof *v}, {r, sizeof *r}, {hd, sizeof hd}, {&len, sizeof len}, {px, n});
    s->parts++, s->bytes += n;
}

// Starts the open capture: from here the record takes calls. Its preamble is the state the next frame starts from: the
// committed heap (snapshot_heap), the module images, the device modes, then every pool, texture slot and sampler the
// journal holds, in the order the calls made them, and the images the guest bytes do not hold. rec_lock held.
static void rec_arm(void) {
    plat_mutex_lock(&log_lock);
    if (!log_f || armed) {
        plat_mutex_unlock(&log_lock);
        return;
    }
    armed = 1;
    plat_mutex_unlock(&log_lock);
    vk_mem_capture_hold(1);   // no frozen page may take a write unstamped while the capture runs
    const RtGuestLayout *g = mem_layout();
    uint64_t committed = mem_heap_committed();
    if (committed > g->heap.size) committed = g->heap.size;
    // What an earlier capture of this run left behind does not describe this one: its replay starts from a zeroed heap
    // and holds no pin, so every pin is sent again and the page map is rebuilt by snapshot_heap.
    nvn_hm_clear(&pin_sent, NULL);
    memset(known_nz, 0, (size_t)((known_pages + 7) / 8));
    run_hi = 0;
    // The write-watch baseline is taken before the heap is copied, not after: the game's other threads run while the
    // copy is made, and a page they write after its copy must still count as written. Such a page goes out again with
    // the first flush; taken after the copy, the baseline would hide that write and the replay would keep the old bytes.
    gen_since = vk_mem_record_pages(0, g->heap.base, g->heap.base + committed, no_page, NULL);
    rec_note_commit(g->heap.base, committed);
    snapshot_heap(committed);
    snap_module_data();
    // The device modes the game set at boot (nvn_api_core.c): the back end reads them on each draw, and the calls that set
    // them are not in the record, so the replay takes their values from here (RR_NVN_STATE).
    uint64_t dev[2] = {(uint64_t)nvn_depth_minus_w(), (uint64_t)nvn_origin_upper_left()};
    PUT(RR_NVN_STATE, {dev, sizeof dev});
    size_t np = 0, nm = 0, nhost = 0;
    for (size_t i = 0; i < npools; i++) {
        PoolRec *r = &pools[i];
        uint64_t obj = r->obj;
        // A pool whose bytes are host storage, or lie outside the heap block, is not in the heap copy above.
        uint64_t mem = (uint64_t)(uintptr_t)r->p.mem;
        if (r->p.host || (mem && (mem < g->heap.base || mem + r->p.size > g->heap.base + g->heap.size))) nhost++;
        PUT(RR_POOL_INIT, {&obj, sizeof obj}, {&r->p, sizeof r->p});
        np++;
    }
    // Every pool's init comes before any map, so replay resolves the pool a map names whatever the order of pools[].
    for (size_t i = 0; i < npools; i++) {
        PoolRec *r = &pools[i];
        uint64_t obj = r->obj;
        for (size_t k = 0; k < r->maps_n;) {
            uint32_t n;
            memcpy(&n, r->maps + k, 4);
            uint32_t hd[2] = {n, 0};
            PUT(RR_POOL_MAP, {&obj, sizeof obj}, {hd, sizeof hd}, {r->maps + k + 8, (size_t)n * 0x28});
            k += 8 + (size_t)n * 0x28;
            nm++;
        }
    }
    size_t ns = 0, nsm = 0;
    static const view_t no_view;
    for (size_t id = 0; id < nslots; id++) {
        const SlotRec *s = &slots[id];
        if (!s->used) continue;
        uint32_t hd[2] = {(uint32_t)id, (uint32_t)s->has_view};
        PUT(RR_TEX_REG, {hd, sizeof hd}, {&s->t, sizeof s->t}, {s->has_view ? &s->v : &no_view, sizeof no_view});
        ns++;
    }
    for (size_t id = 0; id < nsamps; id++) {
        if (!samps[id].used) continue;
        uint32_t hd[2] = {(uint32_t)id, 0};
        PUT(RR_SAMP_REG, {hd, sizeof hd}, {&samps[id].s, sizeof samps[id].s});
        nsm++;
    }
    // The images the GPU owns, and those without CPU storage, hold content the guest bytes do not have: every level and
    // layer is read back and goes in as an IMAGE record, after the slots, so the replay's back end knows the textures
    // by the time their texels arrive. The work the back end has queued is done first, so the images are complete.
    real->finish();
    ImageSnap snap = {0};
    vk_texture_snapshot(image_cb, &snap);
    rt_log("rec: armed at present %llu: %zu pools (%zu maps), %zu texture slots, %zu samplers, %zu images in %zu parts "
           "(%.1f MiB) in the preamble", (unsigned long long)presents_all, np, nm, ns, nsm, snap.images, snap.parts,
           snap.bytes / (1024.0 * 1024.0));
    if (snap.unread)
        rt_log("rec: %zu image parts could not be read back (multisampled, packed depth-stencil or ASTC): a frame that "
               "reads them before writing them differs in the replay", snap.unread);
    if (nhost)
        rt_log("rec: %zu pools keep their bytes outside the guest heap: what the frame reads from them is not in the "
               "capture", nhost);
}

// Sets up the wrapper the first time; rec_wrap calls it under its lock, so it runs once.
static void rec_wrap_setup(const GfxBackend *b) {
    // The captures go to XC2_RECORD when it names a folder, else to captures/ beside xc2.exe. A relative XC2_RECORD is
    // taken from the executable's folder too, as XC2_PROFILE_DIR is (main.c portable_path), so a shortcut or the working
    // folder the game was started in does not move the captures. Nothing is written there until a capture starts.
    char exe[PLAT_PATH_MAX * 2];
    int en = plat_exe_path(exe, sizeof exe);
    char *cut = en > 0 && en < (int)sizeof exe ? plat_last_sep(exe) : NULL;
    if (cut) *cut = 0;
    const char *dir = cfg_get("XC2_RECORD");
    if (dir && *dir) {
        int abs_path = dir[0] == '\\' || dir[0] == '/' || dir[1] == ':';
        if (abs_path || !cut) snprintf(rec_dir, sizeof rec_dir, "%s", dir);
        else snprintf(rec_dir, sizeof rec_dir, "%s/%s", exe, dir);
    } else if (cut) {
        snprintf(rec_dir, sizeof rec_dir, "%s/captures", exe);
    } else {
        snprintf(rec_dir, sizeof rec_dir, "captures");   // the executable's path is unknown: the working folder
    }
    log_buf = malloc(LOG_BUF);
    if (!log_buf) rt_fatal("rec: out of memory for the log buffer");
    atexit(rec_at_exit);   // a capture still open when the game ends gets its last bytes out
    real = b;
    const RtGuestLayout *g = mem_layout();
    known_pages = g->heap.size >> 12;
    known_nz = calloc((known_pages + 7) / 8, 1);
    if (!known_nz) rt_fatal("rec: out of memory for the page map");
    wrapped = 1;   // the journal runs from here on, whether or not a capture is taken
    // XC2_RECORD_FROM=N and XC2_RECORD_ARM_FILE=<path> (scripts) ask for a capture without the menu: it starts at that
    // present the way a menu capture does (capture_start), in a folder of its own. Nothing is written before then.
    const char *from = cfg_get("XC2_RECORD_FROM");
    arm_at = from && *from ? strtoull(from, NULL, 10) : 0;
    if (arm_at) arm_at--;   // frame 1 has no present before it to start after: XC2_RECORD_FROM=1 takes nothing
    const char *af = cfg_get("XC2_RECORD_ARM_FILE");
    if (af && *af) snprintf(arm_file, sizeof arm_file, "%s", af);
    const char *lim = cfg_get("XC2_RECORD_FRAMES");
    script_frames = lim && *lim ? strtoull(lim, NULL, 10) : 0;
    const char *shots = cfg_get("XC2_RECORD_SHOTS");
    if (shots && *shots) {
        snprintf(shots_dir, sizeof shots_dir, "%s", shots);
        plat_file_mkdir_p(shots_dir);
    }
    rt_log("rec: %s back end; captures (F1 menu, Advanced tab: Capture frame, Record frames) go to %s", b->name, rec_dir);
}

// Called by gfx_backend() on every thread that asks. The first caller arms the journal; a caller that arrives meanwhile
// waits for it, so no caller sees the recorder half set up.
const GfxBackend *rec_wrap(const GfxBackend *b) {
    static PlatMutex init_lock = PLAT_MUTEX_INIT;
    plat_mutex_lock(&init_lock);
    if (!real) rec_wrap_setup(b);
    plat_mutex_unlock(&init_lock);
    return &rec_be;
}

// The back end under the recorder's wrapper, or b itself when b is not the wrapper.
const GfxBackend *rec_unwrap(const GfxBackend *b) { return b == &rec_be && real ? real : b; }

// ---- the capture's state for the F1 menu (rec_menu.h). A scripted capture shows here as well, so the menu's buttons
// wait for it.

int rec_menu_state(void) {
    plat_mutex_lock(&log_lock);
    int k = log_f && armed ? cur_kind : 0;
    plat_mutex_unlock(&log_lock);
    return k;
}

void rec_menu_request(int kind) { __atomic_store_n(&req_kind, kind, __ATOMIC_RELEASE); }

int rec_menu_folder(char *out, size_t cap) {
    if (!rec_dir[0] || cap == 0) return 0;
    snprintf(out, cap, "%s", rec_dir);
    return 1;
}
