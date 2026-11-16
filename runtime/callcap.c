// Slow-frame call capture: see callcap.h. The calls of the frame in progress go into one growing buffer under a lock;
// at a present the buffer goes to the writer thread when the frame was slow, or is emptied. The writer keeps at most
// CC_QUEUE frames waiting; a slow frame that finds the queue full is dropped (counted in the log line at exit).
#include <stdio.h>
#include <stdlib.h>

#include "callcap.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

int callcap_level;

#define CC_QUEUE 8
typedef struct { uint8_t *buf; uint64_t n, frame, interval_ns, nrec, dropped; } CcFrame;

static struct {
    PlatMutex lock;                // the buffer below
    uint8_t *buf;
    uint64_t n, cap, nrec, dropped, limit;
    PlatMutex qlock;               // the writer's queue
    PlatCond qcv;
    CcFrame q[CC_QUEUE];
    int qhead, qn, writing;        // writing: the writer holds a frame it has not finished (qlock)
    uint64_t thr_ns, files, files_max, kept, lost, written_bytes;
    char dir[PLAT_PATH_MAX];
} C = {.lock = PLAT_MUTEX_INIT, .qlock = PLAT_MUTEX_INIT, .qcv = PLAT_COND_INIT};

uint64_t callcap_now(void) { return plat_now_ns(); }

static void writer(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&C.qlock);
        while (!C.qn) plat_cond_wait(&C.qcv, &C.qlock);
        CcFrame fr = C.q[C.qhead];
        C.qhead = (C.qhead + 1) % CC_QUEUE;
        C.qn--;
        C.writing = 1;
        plat_mutex_unlock(&C.qlock);
        char path[PLAT_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/f%06llu.ccap", C.dir, (unsigned long long)fr.frame);
        FILE *f = fopen(path, "wb");
        if (f) {
            CcFileHdr h = {.frame = fr.frame, .interval_ns = fr.interval_ns, .nrec = fr.nrec, .dropped = fr.dropped};
            memcpy(h.magic, CC_MAGIC, 8);
            int ok = fwrite(&h, sizeof h, 1, f) == 1;
            ok &= fwrite(fr.buf, 1, (size_t)fr.n, f) == (size_t)fr.n;
            ok &= fclose(f) == 0;
            if (ok) {
                __atomic_add_fetch(&C.written_bytes, fr.n, __ATOMIC_RELAXED);
            } else {   // a short file would claim records it does not hold
                rt_log("callcap: writing %s failed; removed", path);
                remove(path);
            }
        } else {
            rt_log("callcap: cannot write %s", path);
        }
        free(fr.buf);
        plat_mutex_lock(&C.qlock);
        C.writing = 0;
        plat_cond_broadcast(&C.qcv);
        plat_mutex_unlock(&C.qlock);
    }
}

void callcap_finish(void) {
    if (!C.dir[0]) return;
    __atomic_store_n(&callcap_level, 0, __ATOMIC_RELEASE);
    uint64_t t0 = plat_now_ns();
    plat_mutex_lock(&C.qlock);
    while ((C.qn || C.writing) && plat_now_ns() - t0 < 5000000000ull) {   // the writer broadcasts after each file
        plat_mutex_unlock(&C.qlock);
        plat_sleep_ms(5);
        plat_mutex_lock(&C.qlock);
    }
    int left = C.qn + C.writing;
    plat_mutex_unlock(&C.qlock);
    if (left) rt_log("callcap: %d frames not written at exit (5 s)", left);
}

void callcap_init(void) {
    const char *d = cfg_get("XC2_CALLCAP");
    if (!d || !*d) return;
    snprintf(C.dir, sizeof C.dir, "%s", d);
    plat_file_mkdir_p(C.dir);
    const char *e = cfg_get("XC2_CALLCAP_MS");
    C.thr_ns = (uint64_t)(e && *e ? atof(e) * 1e6 : 10e6);
    e = cfg_get("XC2_CALLCAP_MB");
    C.limit = (uint64_t)(e && *e ? atoi(e) : 256) << 20;
    e = cfg_get("XC2_CALLCAP_FILES");
    C.files_max = (uint64_t)(e && *e ? atoi(e) : 300);
    if (!plat_thread_spawn(writer, NULL, "callcap", 0)) {
        rt_log("callcap: no writer thread; off");
        return;
    }
    __atomic_store_n(&callcap_level, 1, __ATOMIC_RELEASE);
    rt_log("callcap: the calls of frames over %.1f ms to %s (at most %llu files, %llu MiB a frame)", C.thr_ns / 1e6, C.dir,
           (unsigned long long)C.files_max, (unsigned long long)(C.limit >> 20));
}

// C.lock held: room for n more bytes, 0 when the frame's records would pass the limit.
static int reserve(uint64_t n) {
    if (C.n + n > C.limit) return 0;
    if (C.n + n > C.cap) {
        uint64_t cap = C.cap ? C.cap : 1 << 20;
        while (cap < C.n + n) cap *= 2;
        uint8_t *b = realloc(C.buf, (size_t)cap);
        if (!b) return 0;
        C.buf = b, C.cap = cap;
    }
    return 1;
}

void callcap_add(uint32_t kind, uint64_t ns, const CcBlob *b, int nb) {
    uint64_t need = sizeof(CcRec);
    for (int i = 0; i < nb; i++) need += 8 + ((b[i].n + 7) & ~7ull);
    CcRec r = {.kind = kind, .nblob = (uint32_t)nb, .ns = ns, .tid = plat_thread_id()};
    plat_mutex_lock(&C.lock);
    if (!reserve(need)) {
        C.dropped++;
        plat_mutex_unlock(&C.lock);
        return;
    }
    uint8_t *o = C.buf + C.n;
    memcpy(o, &r, sizeof r), o += sizeof r;
    for (int i = 0; i < nb; i++) {
        uint64_t n = b[i].n, pad = ((n + 7) & ~7ull) - n;
        memcpy(o, &n, 8), o += 8;
        if (n) memcpy(o, b[i].p, (size_t)n), o += n;
        memset(o, 0, (size_t)pad), o += pad;
    }
    C.n += need, C.nrec++;
    plat_mutex_unlock(&C.lock);
}

void callcap_frame(uint64_t frame, uint64_t interval_ns) {
    if (!callcap_on()) return;
    plat_mutex_lock(&C.lock);
    int keep = interval_ns > C.thr_ns && C.nrec && C.files < C.files_max;
    CcFrame fr = {C.buf, C.n, frame, interval_ns, C.nrec, C.dropped};
    if (keep) {
        plat_mutex_lock(&C.qlock);
        if (C.qn < CC_QUEUE) {
            C.q[(C.qhead + C.qn) % CC_QUEUE] = fr;
            C.qn++;
            plat_cond_signal(&C.qcv);
            C.buf = NULL, C.cap = 0;   // the writer owns it now
            C.files++, C.kept++;
        } else {
            C.lost++;
        }
        plat_mutex_unlock(&C.qlock);
    }
    C.n = 0, C.nrec = 0, C.dropped = 0;
    if (C.files == C.files_max && keep) {
        __atomic_store_n(&callcap_level, 0, __ATOMIC_RELEASE);   // the last file: no more copies
        rt_log("callcap: %llu files written, capture off", (unsigned long long)C.files);
    }
    plat_mutex_unlock(&C.lock);
}
