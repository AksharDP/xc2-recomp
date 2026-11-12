// The Vulkan graphics back end: gfx_vulkan_backend wires the memory and
// texture layer (vk_memory.c, vk_texture.c), submission and fences (vk_sync.c) and the present. Every entry starts the
// device on first use.
//
// Present: the game texture is resolved on the calling thread and its blit is handed to a present thread, so
// nvnQueuePresentTexture returns at once (section 5). Queue order does the rest: the submissions that wrote the texture
// are already on gfx_queue, and every call that may write or free GPU state the blit reads (submit, finish, pool and
// texture teardown, texel writes) first waits until the queued blit was submitted (vk_present_drain).
//
// Back-edge pacing: the present thread submits the blit at once, then holds the host present (vkQueuePresentKHR) until
// the frame's display time from the pacer (nvn_api_obj.c), so present-to-present intervals stay even whatever the
// replay took. The blit is already on the queue, so the wait blocks no submission.
//
// Asynchronous replay (XC2_REPLAY_ASYNC; by default on where the VRAM mirrors take submit-time copies, vk_replay_async):
// nvnQueueSubmitCommands and nvnQueuePresentTexture only queue; what the GPU reads from guest memory is copied in the
// submit call first (vk_be_submit). Each item gets its timeline value at enqueue (vk_sync_reserve), in queue
// order; the replay thread runs the synchronous submit and present bodies in that order, so every value is signalled
// by the submission it was handed out for. The game runs at most one frame ahead: queueing present N waits until the
// replay thread handled present N-1. Calls that read or free GPU state that queued work may use drain the queue first
// (replay_drain), each cause counted.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#define WEAK __attribute__((weak))

// XC2_STOP_FRAME=<n> (diagnostic): the process ends once frame n is the one being built, so a run stops at the frame a
// check needs instead of after a fixed time. A dump of frame n needs the frames after it, whose copies are written then:
// give the stop a few frames of margin past the dump frame.
static void stop_at_frame(uint64_t presented) {
    static uint64_t stop;   // 0: not read yet; ~0: off
    if (!stop) {
        const char *e = cfg_get("XC2_STOP_FRAME");
        stop = e && *e ? strtoull(e, NULL, 10) : 0;
        if (!stop) stop = ~0ull;
    }
    if (stop != ~0ull && presented + 1 >= stop) gfx_exit_saved("XC2_STOP_FRAME reached");
}

typedef struct { VkImage image; VkFormat format; int w, h, has_crop, crop[4]; uint64_t frame, at, queued, value; VkFsrFrame fsr; } PresentJob;

static PlatMutex pl = PLAT_MUTEX_INIT;
static PlatCond pcv = PLAT_COND_INIT;   // job queued, job taken, job submitted
static PresentJob job;
static int pending, busy, present_thread;

// Sleeps on a high-resolution timer to 1 ms before at, then yields until at: the timer alone wakes up to ~0.5 ms late.
static void wait_until(uint64_t at) {
    uint64_t now = kern_now_ns();
    if (at > now + 1500000) plat_wait_ns(at - now - 1000000);
    while (kern_now_ns() < at) plat_yield();
}

static void present_main(void *arg) {
    (void)arg;
    uint64_t last = 0, last_queued = 0;
    plat_mutex_lock(&pl);
    for (;;) {
        while (!pending) plat_cond_wait(&pcv, &pl);
        PresentJob j = job;
        pending = 0;
        busy = 1;
        plat_cond_broadcast(&pcv);
        plat_mutex_unlock(&pl);
        if (flight_on) vk_thumbs_frame(j.frame);
        vk_fsr3_frame_show(&j.fsr);
        vk_present_blit(j.image, VK_IMAGE_LAYOUT_GENERAL, j.format, j.w, j.h, j.has_crop ? j.crop : NULL);
        uint64_t f = vk_sync_submit(NULL, 0);   // a timeline value after the blit: deferred destruction waits for the blit too
        if (j.value && f != j.value)
            rt_fatal("vk: the present of frame %llu signalled %llu, queued as %llu", (unsigned long long)j.frame,
                     (unsigned long long)f, (unsigned long long)j.value);
        plat_mutex_lock(&pl);
        busy = 0;
        plat_cond_broadcast(&pcv);
        plat_mutex_unlock(&pl);
        uint64_t t0 = kern_now_ns();
        if (!vk_fg_active()) wait_until(j.at);   // frame generation paces its own two presents (vk_fg.c)
        uint64_t t1 = kern_now_ns();
        vk_present_flip();
        // XC2_TRACE=present: one line per present: the host present's interval, the interval of the game's present
        // calls (when the host present went out before back-edge pacing), the wait and how far past its target it went
        if (rt_trace & RT_TRACE_PRESENT)
            rt_log("present: frame %llu interval %.3f ms, game %.3f ms, waited %.3f ms, %.3f ms past target",
                   (unsigned long long)j.frame, last ? (t1 - last) / 1e6 : 0.0,
                   last_queued ? (j.queued - last_queued) / 1e6 : 0.0, (t1 - t0) / 1e6,
                   j.at && t1 > j.at ? (t1 - j.at) / 1e6 : 0.0);
        last = t1;
        last_queued = j.queued;
        vk_pipeline_autosave();   // starts the autosave thread once
        plat_mutex_lock(&pl);
    }
    return;
}

void vk_present_drain(void) {
    uint64_t p0 = perf_begin();
    plat_mutex_lock(&pl);
    while (pending || busy) plat_cond_wait(&pcv, &pl);
    plat_mutex_unlock(&pl);
    perf_end(PERF_DRAIN, p0);
}

// The present after the frame count: resolves the image and hands the blit to the present thread. queued: when the game
// presented; value: the timeline value queued for this present (asynchronous replay), 0 for the next one (synchronous).
static void present_image(const tex_t *t, const int crop[4], uint64_t at_ns, uint64_t queued, uint64_t frame,
                          uint64_t value) {
    static volatile int32_t n_skipped;
    vk_exec_frame_log(t, frame);
    vk_mem_census(frame);
    if (vk_dump_armed) vk_dump_present(frame);
    __atomic_store_n(&gfx_counters.validation_errors, (uint64_t)vk_validation_errors_total(), __ATOMIC_RELAXED);
    const VkTexImage *img = vk_texture_get(t);
    if (!img || !(img->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        if (plat_inc32(&n_skipped) <= 8)
            rt_log("vk: present of texture %p (format %#x) skipped: no colour image", (const void *)t, t ? t->d.format : 0);
        if (value) {   // the queued value is still signalled, in order
            vk_present_drain();
            uint64_t f = vk_sync_submit(NULL, 0);
            if (f != value) rt_fatal("vk: skipped present signalled %llu, queued as %llu", (unsigned long long)f, (unsigned long long)value);
        }
        return;
    }
    PresentJob j = {.image = img->image, .format = img->format, .w = (int)img->width, .h = (int)img->height, .frame = frame,
                    .at = at_ns, .queued = queued, .value = value};
    vk_fsr3_frame_take(&j.fsr);   // this frame's depth, motion and HUD-less scene, for frame generation at the present
    if (crop) {
        j.has_crop = 1;
        for (int i = 0; i < 4; i++) j.crop[i] = crop[i];
    }
    vk_upload_flush(0);   // an upload vk_texture_get just recorded goes before the blit
    plat_mutex_lock(&pl);
    if (!present_thread) {
        if (!plat_thread_spawn(present_main, NULL, "vk present", 0)) rt_fatal("vk: cannot start the present thread");
        present_thread = 1;
    }
    while (pending) plat_cond_wait(&pcv, &pl);   // one present in waiting at most
    job = j;
    pending = 1;
    plat_cond_broadcast(&pcv);
    plat_mutex_unlock(&pl);
    if (rdoc_active()) { vk_present_drain(); rdoc_after_present(frame); }
}

// ---- Asynchronous replay
enum { RI_SUBMIT, RI_PRESENT, RI_RELEASE };
typedef struct {
    int kind, n, has_crop, crop[4];
    uint64_t value, submit_no, at, queued;
    uint64_t *h;
    void *mcap, *ucap;  // RI_SUBMIT: the submit-time captures (vk_mem_capture, vk_usnap_capture)
    const void **progs; // RI_SUBMIT: the programs its replay may read (vk_usnap_programs, a copy); nprogs -1: any
    int nprogs;
    const tex_t *tex;   // RI_RELEASE: a copy the item owns (the game may reuse the object's memory)
} ReplayItem;
#define RQ_CAP 1024   // a frame queues 1 submit and 1 present, and up to a few hundred releases
enum { DR_POOL_FINI, DR_MAP_VIRTUAL, DR_TEX_RELEASE, DR_PROG_RELEASE, DR_FINISH, DR_READ_TEXELS, DR_WRITE_TEXELS,
       DR_GUEST_READ, DR_FRAME, DR_FULL, DR_CHECK, DR_N };
static const char *const dr_name[DR_N] = {
    "pool_fini", "map_virtual", "texture_release (queued)", "program_release", "finish", "read_texels (writer fence)",
    "write_texels", "guest_read (writer fence)", "frame bound", "queue full", "check drain"};

static PlatMutex rl = PLAT_MUTEX_INIT;
static PlatCond rcv = PLAT_COND_INIT;   // item queued, item taken, item done
static ReplayItem rq[RQ_CAP];
static int rq_head, rq_n, r_busy, r_started;
static uint64_t r_presents_queued, r_presents_done, r_submit_done, game_frames;
static ReplayItem r_cur;   // the item the replay thread runs while r_busy (its programs for vk_be_program_release)
// Under rl: calls, calls that waited and the time waited per cause, since the last replay_log line and over the run.
static uint64_t dr_calls[DR_N], dr_waits[DR_N], dr_ns[DR_N], dr_tcalls[DR_N], dr_twaits[DR_N], dr_tns[DR_N];
// XC2_REPLAY_CHECK: ReadTexels compared with a read after a full drain (under rl): checked, differing, differing
// although no submission recorded a write to the texture in between, differing with the queue idle at the first read.
static uint64_t ck_reads, ck_diff, ck_diff_unexplained, ck_diff_idle;

// On by default where every buffer the GPU reads is a submit-time copy (vk_mem_snapshots: the VRAM mirrors with staged
// captures): Goldmouth on an RTX 4070 Laptop, 75 fps on the submitting thread, 99 on the replay thread. Elsewhere the
// frame guard waits for the GPU each frame, and the replay thread only adds latency (63 fps there with the guard), so
// it stays off unless XC2_REPLAY_ASYNC is set.
int vk_replay_async(void) {
    static int on = -1;
    if (on < 0) {
        vk_sync_start();   // the memory mode is chosen with the device
        int set = cfg_is_set("XC2_REPLAY_ASYNC"), snap = vk_mem_snapshots();
        on = set ? cfg_bool("XC2_REPLAY_ASYNC") : snap;
        rt_log("vk: replay %s (XC2_REPLAY_ASYNC=%d%s)",
               on ? "on its own thread, at most one frame behind the game" : "on the submitting thread", on,
               set ? "" : snap ? ", auto: submit-time copies" : ", auto: the GPU reads guest memory live");
    }
    return on;
}

// The back end under the recorder's wrapper (replay_rec.c), or b itself.
const GfxBackend *rec_unwrap(const GfxBackend *b);

// The front end's view (nvn_api_core.c, nvn_api_cmd.c): replay is asynchronous only on this back end. gfx_backend() is
// the recorder's wrapper, so the check looks through it to the back end the calls go to.
int gfx_replay_async(void) { return rec_unwrap(gfx_backend()) == &gfx_vulkan_backend && vk_replay_async(); }

// The newest submit number (nvn_submit_count) whose replay is done: recording copies it used may be freed.
uint64_t gfx_replayed_submits(void) {
    if (!gfx_replay_async()) return nvn_submit_count();
    plat_mutex_lock_shared(&rl);
    uint64_t v = r_submit_done;
    plat_mutex_unlock_shared(&rl);
    return v;
}

static uint64_t submit_sync(const uint64_t *handles, int n) {
    vk_sync_start();
    vk_present_drain();
    vk_texture_new_submit();
    return vk_sync_submit(handles, n);
}

static void replay_main(void *arg) {
    (void)arg;
    perf_replay_thread();
    plat_mutex_lock(&rl);
    for (;;) {
        while (!rq_n) plat_cond_wait(&rcv, &rl);
        ReplayItem it = rq[rq_head];
        rq_head = (rq_head + 1) % RQ_CAP;
        rq_n--;
        r_busy = 1;
        r_cur = it;
        plat_cond_broadcast(&rcv);
        plat_mutex_unlock(&rl);
        if (it.kind == RI_SUBMIT) {
            if (it.n) vk_exec_captures(it.mcap, it.ucap);
            uint64_t f = submit_sync(it.h, it.n);
            if (f != it.value)
                rt_fatal("vk: replayed submission signalled %llu, queued as %llu", (unsigned long long)f,
                         (unsigned long long)it.value);
            free(it.h);
        } else if (it.kind == RI_RELEASE) {
            vk_present_drain();
            vk_texture_release(it.tex);
            free((void *)it.tex);
        } else {
            uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
            vk_capture_frame(frame);
            stop_at_frame(frame);
            present_image(it.tex, it.has_crop ? it.crop : NULL, it.at, it.queued, frame, it.value);
        }
        plat_mutex_lock(&rl);
        if (it.kind == RI_SUBMIT) r_submit_done = it.submit_no;
        else r_presents_done++;
        r_busy = 0;
        free(it.progs);   // under rl: vk_be_program_release reads r_cur's list under it
        r_cur = (ReplayItem){0};
        plat_cond_broadcast(&rcv);
    }
    return;
}

static void wait_counted(int why, uint64_t t0) {   // rl held
    uint64_t d = kern_now_ns() - t0;
    dr_waits[why]++, dr_twaits[why]++;
    dr_ns[why] += d, dr_tns[why] += d;
    perf_end(PERF_DRAIN, t0);
}

// A guest thread waiting for the replay gives its virtual core up, as around any blocking host wait (NVN_BLOCKING):
// guest_read (memcpy, pool invalidate), pool and texture finalize run without it, and a drain lasts up to a replay.
// Called and returning with rl held; rl is not held while the core is taken back.
static int core_away_unlocked(void) {
    plat_mutex_unlock(&rl);
    int away = kern_core_away();
    plat_mutex_lock(&rl);
    return away;
}
static void core_back_unlocked(int *away) {
    if (!*away) return;
    plat_mutex_unlock(&rl);
    kern_core_back(away);
    plat_mutex_lock(&rl);
}

// Queues an item (waits while the queue is full) and returns its value, reserved under rl so values follow queue order.
static uint64_t replay_enqueue(ReplayItem *it) {
    plat_mutex_lock(&rl);
    if (!r_started) {
        if (!plat_thread_spawn(replay_main, NULL, "vk replay", 0)) rt_fatal("vk: cannot start the replay thread");
        r_started = 1;
    }
    // rl is dropped while the core is taken back, and another thread may fill the queue again meanwhile: check again.
    while (rq_n == RQ_CAP) {
        uint64_t p0 = perf_begin();
        dr_calls[DR_FULL]++, dr_tcalls[DR_FULL]++;
        int away = core_away_unlocked();
        while (rq_n == RQ_CAP) plat_cond_wait(&rcv, &rl);
        wait_counted(DR_FULL, p0);
        core_back_unlocked(&away);
    }
    it->value = it->kind == RI_RELEASE ? 0 : vk_sync_reserve();   // a release signals nothing
    rq[(rq_head + rq_n) % RQ_CAP] = *it;
    rq_n++;
    plat_cond_broadcast(&rcv);
    plat_mutex_unlock(&rl);
    return it->value;
}

// Returns once every queued item was replayed and submitted. Back-end entries only: the replay thread never calls them.
static void replay_drain(int why) {
    if (!gfx_replay_async()) return;
    plat_mutex_lock(&rl);
    dr_calls[why]++, dr_tcalls[why]++;
    if (rq_n || r_busy) {
        uint64_t p0 = perf_begin();
        int away = core_away_unlocked();
        while (rq_n || r_busy) plat_cond_wait(&rcv, &rl);
        wait_counted(why, p0);
        core_back_unlocked(&away);
    }
    plat_mutex_unlock(&rl);
}

// What made the game wait for the replay: since the last line (every 1800 game frames), or over the run (at exit).
static void replay_log(uint64_t frame, int total) {
    char buf[1024];
    int k = 0;
    plat_mutex_lock(&rl);
    const uint64_t *c = total ? dr_tcalls : dr_calls, *w = total ? dr_twaits : dr_waits, *t = total ? dr_tns : dr_ns;
    for (int i = 0; i < DR_N && k < (int)sizeof buf - 96; i++)
        if (c[i] || w[i])
            k += snprintf(buf + k, sizeof buf - (size_t)k, "%s %s %llu calls, %llu waited %.1f ms", k ? ";" : "", dr_name[i],
                          (unsigned long long)c[i], (unsigned long long)w[i], t[i] / 1e6);
    if (ck_reads && k < (int)sizeof buf - 128)
        k += snprintf(buf + k, sizeof buf - (size_t)k, "; check (run): %llu reads, %llu differ, %llu without a recorded write, %llu with the queue idle",
                      (unsigned long long)ck_reads, (unsigned long long)ck_diff, (unsigned long long)ck_diff_unexplained,
                      (unsigned long long)ck_diff_idle);
    memset(dr_calls, 0, sizeof dr_calls);
    memset(dr_waits, 0, sizeof dr_waits);
    memset(dr_ns, 0, sizeof dr_ns);
    plat_mutex_unlock(&rl);
    rt_log("vk: replay queue %s frame %llu:%s", total ? "over the run to" : "at", (unsigned long long)frame,
           k ? buf : " no waits");
}

// The run total, once: the game exits through prof_write (boot.c _exit, the window close), probes through
// vk_backend_shutdown.
void gfx_exit_log(void) {
    static int32_t done;
    if (gfx_replay_async() && r_started && !plat_xchg32(&done, 1)) replay_log(game_frames, 1);
}

static void vk_be_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        uint64_t frame = __atomic_add_fetch(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
        vk_capture_frame(frame);
        stop_at_frame(frame);
        perf_present(frame);   // before vk_exec_frame_log, which resets the frame's exec counts the flight recorder reads
        present_image(t, crop, at_ns, kern_now_ns(), frame, 0);
        return;
    }
    // Frame bound: the replay thread handled the previous present before this one is queued.
    plat_mutex_lock(&rl);
    dr_calls[DR_FRAME]++, dr_tcalls[DR_FRAME]++;
    if (r_presents_done < r_presents_queued) {
        uint64_t p0 = perf_begin();
        while (r_presents_done < r_presents_queued) plat_cond_wait(&rcv, &rl);
        wait_counted(DR_FRAME, p0);
    }
    r_presents_queued++;
    plat_mutex_unlock(&rl);
    uint64_t frame = ++game_frames;
    perf_present(frame);
    if (frame % 1800 == 0) replay_log(frame, 0);
    ReplayItem it = {.kind = RI_PRESENT, .tex = t, .at = at_ns, .queued = kern_now_ns()};
    if (crop) {
        it.has_crop = 1;
        for (int i = 0; i < 4; i++) it.crop[i] = crop[i];
    }
    replay_enqueue(&it);
}

// vk_pipeline.c drops a released program's pipelines here.
WEAK void vk_program_release(const prog_t *p) { (void)p; }

static int vk_be_pool_init(pool_t *p) { vk_sync_start(); return vk_pool_init(p); }
// Submit-time captures (vk_be_submit) name the mirrored pools' records: a pool is finalized with the capture lock held,
// after the queued submissions have used their captures, so none is taken or queued with a record that goes.
static PlatMutex cap_lock = PLAT_MUTEX_INIT;

static void vk_be_pool_fini(pool_t *p) {
    vk_sync_start();
    plat_mutex_lock(&cap_lock);
    replay_drain(DR_POOL_FINI);
    vk_present_drain();
    vk_pool_fini(p);
    plat_mutex_unlock(&cap_lock);
}
static int vk_be_pool_map_virtual(pool_t *v, int n, const void *requests) {
    vk_sync_start();
    replay_drain(DR_MAP_VIRTUAL);
    vk_present_drain();
    return vk_pool_map_virtual(v, n, requests);
}
static void vk_be_texture_register(int id, const tex_t *t, const view_t *v) { vk_sync_start(); vk_texture_register(id, t, v); }
static void vk_be_sampler_register(int id, const samp_par *s) { vk_sync_start(); vk_sampler_register(id, s); }
// Asynchronous: the release is queued behind the submissions before it, so their replay still resolves the texture's
// slots to its image (a released slot follows another live texture at the storage), and the ones after it see it
// released, the order of the synchronous replay. vk_texture_release reads only the object's key fields: a copy.
static void vk_be_texture_release(const tex_t *t) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        vk_present_drain();
        vk_texture_release(t);
        return;
    }
    if (!t || !t->mem) return;
    tex_t *copy = malloc(sizeof *copy);
    if (!copy) rt_fatal("vk: out of memory (replay queue)");
    *copy = *t;
    ReplayItem it = {.kind = RI_RELEASE, .tex = copy};
    plat_mutex_lock(&rl);
    dr_calls[DR_TEX_RELEASE]++, dr_tcalls[DR_TEX_RELEASE]++;
    plat_mutex_unlock(&rl);
    replay_enqueue(&it);
}
// rl held: the replay of submission item q may read program p.
static int item_reads(const ReplayItem *q, const prog_t *p) {
    if (q->kind != RI_SUBMIT) return 0;
    if (q->nprogs < 0) return 1;
    int a = 0, b = q->nprogs;   // sorted by address
    while (a < b) {
        int k = (a + b) / 2;
        if ((uintptr_t)q->progs[k] < (uintptr_t)p) a = k + 1;
        else b = k;
    }
    return a < q->nprogs && q->progs[a] == (const void *)p;
}

// Finalize and SetShaders on a program: the replay reads a bound program's control blocks and code from guest memory
// (vk_exec_draw.c find_stage, shader_for), which the game may free or reuse once the call returns. Asynchronous: the
// call waits until every queued submission that may read p (it binds p, or starts with p bound: vk_usnap_programs) was
// replayed, not for the whole queue. While leaving or entering an area the game releases programs every frame, and
// waiting for the whole queue each time held it for 1.65 s over two minutes in Goldmouth.
static void vk_be_program_release(const prog_t *p) {
    if (gfx_replay_async()) {
        plat_mutex_lock(&rl);
        dr_calls[DR_PROG_RELEASE]++, dr_tcalls[DR_PROG_RELEASE]++;
        uint64_t need = 0;
        for (int i = 0; i < rq_n; i++) {
            const ReplayItem *q = &rq[(rq_head + i) % RQ_CAP];
            if (item_reads(q, p) && q->submit_no > need) need = q->submit_no;
        }
        if (r_busy && item_reads(&r_cur, p) && r_cur.submit_no > need) need = r_cur.submit_no;
        if (need > r_submit_done) {
            uint64_t p0 = perf_begin();
            int away = core_away_unlocked();
            while (r_submit_done < need) plat_cond_wait(&rcv, &rl);
            wait_counted(DR_PROG_RELEASE, p0);
            core_back_unlocked(&away);
        }
        plat_mutex_unlock(&rl);
    }
    vk_program_release(p);
}
// A submission's submit-time captures (its uniform arenas, the mirror staging) live until its fence completes, and
// a GPU that falls behind lets them pile up: a paused game kept submitting while the menu was open, and issue 27
// held 888 MiB in 222 arenas that way. Past XC2_FLIGHT_MAX values queued or in flight (a value per submission and per
// present) the submitting thread waits for the oldest, which bounds the backlog without the per-frame GPU wait the
// frame guard would add in the steady state. 0 turns it off.
#define FLIGHT_MAX_DEFAULT 6   // values: a frame queues one submit and one present, so this is about three frames
static void flight_bound(void) {
    static int max = -1;
    if (max < 0) {
        const char *e = cfg_get("XC2_FLIGHT_MAX");
        max = e && *e ? atoi(e) : FLIGHT_MAX_DEFAULT;
    }
    if (max <= 0) return;
    uint64_t ahead = vk_sync_reserved(), done = vk_sync_fence_done();
    if (ahead > done + (uint64_t)max) vk_sync_fence_wait(ahead - (uint64_t)max, UINT64_MAX);
}

// Everything the GPU reads from guest memory for a submission is copied here, in the game's submit call (the mirrors'
// written pages, the uniform ranges it binds: vk_mem_capture, vk_usnap_capture), under the capture lock so captures
// follow queue order, and handed to the replay with it: the game rewrites vertex, storage and uniform data for its next
// frame with no fence, while the replay may record this one up to a frame later. Asynchronous: the handles
// (nvn_submit_prepare_async ran under the front end's submit lock) are copied and queued; the value returned is the one
// the replay will signal.
static uint64_t vk_be_submit(const uint64_t *handles, int n) {
    vk_sync_start();
    plat_mutex_lock(&cap_lock);
    void *mcap = n ? vk_mem_capture() : NULL, *ucap = n ? vk_usnap_capture(handles, n) : NULL;
    if (!gfx_replay_async()) {
        if (n) vk_exec_captures(mcap, ucap);
        uint64_t f = submit_sync(handles, n);
        plat_mutex_unlock(&cap_lock);
        flight_bound();
        return f;
    }
    ReplayItem it = {.kind = RI_SUBMIT, .n = n, .submit_no = nvn_submit_count(), .mcap = mcap, .ucap = ucap};
    if (n) {   // copied before the queue: the replay frees the capture
        const void *const *pl;
        it.nprogs = vk_usnap_programs(ucap, &pl);
        if (it.nprogs > 0) {
            if (!(it.progs = malloc(sizeof *it.progs * (size_t)it.nprogs))) rt_fatal("vk: out of memory (replay queue)");
            memcpy(it.progs, pl, sizeof *it.progs * (size_t)it.nprogs);
        }
    }
    if (n > 0) {
        it.h = malloc(sizeof *it.h * (size_t)n);
        if (!it.h) rt_fatal("vk: out of memory (replay queue)");
        memcpy(it.h, handles, sizeof *it.h * (size_t)n);
    }
    uint64_t v = replay_enqueue(&it);
    plat_mutex_unlock(&cap_lock);
    flight_bound();
    return v;
}
static void vk_be_finish(void) { vk_sync_start(); replay_drain(DR_FINISH); vk_present_drain(); vk_sync_finish(); }
static int replay_check(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("XC2_REPLAY_CHECK");
        on = e && strcmp(e, "0");
        if (on) rt_log("vk: XC2_REPLAY_CHECK: every ReadTexels is read again after a full replay drain and compared");
    }
    return on;
}

// ReadTexels with the replay queued: waits for the newest submission recorded to write the texture (replayed and
// completed), not for the whole queue. The readback copy goes through the upload batch on the same queue, after every
// submission the replay has submitted. Submissions still queued or being recorded run after the copy, as on hardware
// when the GPU has not reached them yet: the game only sees their texels after waiting on a sync, and every sync
// value covers replay and GPU, so a read the game synchronised sees them. XC2_REPLAY_CHECK compares with a full drain.
static void vk_be_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                              uint64_t img) {
    vk_sync_start();
    if (!gfx_replay_async()) {
        vk_read_texels(t, v, r, dst, row, img);
        return;
    }
    uint64_t f = vk_texture_last_write(t);
    plat_mutex_lock(&rl);
    dr_calls[DR_READ_TEXELS]++, dr_tcalls[DR_READ_TEXELS]++;
    int idle = !rq_n && !r_busy;
    plat_mutex_unlock(&rl);
    if (f && vk_sync_fence_done() < f) {
        uint64_t t0 = kern_now_ns();
        vk_sync_fence_wait(f, UINT64_MAX);   // counts as a fence wait in perf
        uint64_t d = kern_now_ns() - t0;
        plat_mutex_lock(&rl);
        dr_waits[DR_READ_TEXELS]++, dr_twaits[DR_READ_TEXELS]++;
        dr_ns[DR_READ_TEXELS] += d, dr_tns[DR_READ_TEXELS] += d;
        plat_mutex_unlock(&rl);
    }
    vk_read_texels(t, v, r, dst, row, img);
    if (!replay_check()) return;
    size_t n = vk_texture_texel_span(t, v, r, row, img);
    uint8_t *again = n ? malloc(n) : NULL;
    if (!again) return;
    memcpy(again, dst, n);   // bytes between the rows stay the caller's
    uint64_t f0 = vk_texture_last_write(t);
    replay_drain(DR_CHECK);
    uint64_t f1 = vk_texture_last_write(t);
    vk_read_texels(t, v, r, again, row, img);
    int differ = memcmp(again, dst, n) != 0;
    static volatile int32_t n_logged;
    if (differ && plat_inc32(&n_logged) <= 40) {
        size_t at = 0;
        while (at < n && again[at] == dst[at]) at++;
        rt_log("vk: replay check: ReadTexels of %p (format %#x, %dx%dx%d at %d,%d,%d) differs at byte %zu of %zu; writer "
               "fence %llu at the read, %llu after the drain, done %llu; queue %s at the read",
               (void *)t->mem, t->d.format, r->width, r->height, r->depth, r->xoffset, r->yoffset, r->zoffset, at, n,
               (unsigned long long)f0, (unsigned long long)f1, (unsigned long long)vk_sync_fence_done(), idle ? "idle" : "busy");
    }
    plat_mutex_lock(&rl);
    ck_reads++;
    if (differ) {
        ck_diff++;
        if (f1 == f0) ck_diff_unexplained++;
        if (idle) ck_diff_idle++;
    }
    plat_mutex_unlock(&rl);
    free(again);
}
static void vk_be_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row,
                               uint64_t img) {
    vk_sync_start();
    replay_drain(DR_WRITE_TEXELS);
    vk_present_drain();
    vk_write_texels(t, v, r, src, row, img);
}
static void vk_be_guest_written(uint64_t addr, uint64_t size) { vk_sync_start(); vk_guest_written(addr, size); }
// A CPU read of GPU-written mirrored storage (memcpy, pool invalidate). With the replay queued it waits for the newest
// submission recorded to write the textures there, not for the whole queue: the write-back copy then runs after their
// renders, and clears gpu_dirty only for content that is on the queue. Submissions not replayed yet render after the
// copy, as on hardware when the GPU has not reached them; they mark the texture dirty again for a later read.
static void vk_be_guest_read(uint64_t addr, uint64_t size) {
    if (!vk_texture_cpu_stale(addr, size)) return;
    vk_sync_start();
    if (gfx_replay_async()) {
        uint64_t f = vk_texture_dirty_fence(addr, size);
        plat_mutex_lock(&rl);
        dr_calls[DR_GUEST_READ]++, dr_tcalls[DR_GUEST_READ]++;
        plat_mutex_unlock(&rl);
        if (f && vk_sync_fence_done() < f) {
            uint64_t t0 = kern_now_ns();
            NVN_BLOCKING();
            vk_sync_fence_wait(f, UINT64_MAX);
            uint64_t d = kern_now_ns() - t0;
            plat_mutex_lock(&rl);
            dr_waits[DR_GUEST_READ]++, dr_twaits[DR_GUEST_READ]++;
            dr_ns[DR_GUEST_READ] += d, dr_tns[DR_GUEST_READ] += d;
            plat_mutex_unlock(&rl);
        }
    }
    vk_present_drain();
    vk_texture_write_back(addr, size);
}
// After a fence wait: the value is done, so its replay is too; a later submission the replay records meanwhile marks
// its targets with a later fence, which write_back_small leaves alone. No drain.
static void vk_be_cpu_synced(uint64_t fence) {
    if (!vk_texture_small_dirty()) return;
    vk_sync_start();
    vk_present_drain();
    vk_texture_write_back_small(fence);
}

const GfxBackend gfx_vulkan_backend = {
    .name = "vulkan",
    .pool_init = vk_be_pool_init,
    .pool_fini = vk_be_pool_fini,
    .pool_map_virtual = vk_be_pool_map_virtual,
    .texture_register = vk_be_texture_register,
    .sampler_register = vk_be_sampler_register,
    .texture_release = vk_be_texture_release,
    .program_release = vk_be_program_release,
    .submit = vk_be_submit,
    .fence_wait = vk_sync_fence_wait,
    .fence_done = vk_sync_fence_done,
    .present = vk_be_present,
    .finish = vk_be_finish,
    .read_texels = vk_be_read_texels,
    .write_texels = vk_be_write_texels,
    .guest_written = vk_be_guest_written,
    .guest_read = vk_be_guest_read,
    .cpu_synced = vk_be_cpu_synced,
};

void vk_backend_shutdown(void) {
    replay_drain(DR_FINISH);
    gfx_exit_log();
    vk_present_drain();
    vk_sync_shutdown();
    vk_mem_shutdown();
    vk_shutdown();
}
