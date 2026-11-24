// Guest function call counts (XC2_FNPROF=<addr>[,<addr>...], guest addresses of lifted functions, at most FP_FUNCS).
// Every lifted function starts with FN_ENTER (cpu.h), whose site the listed functions' hooks (fnhook.c) turn into a call
// of fnprof_hit. For each listed function it counts the calls per first argument (x0), per caller (the return address) and per calling guest thread.
// With a * before the address it also reads the 32-bit word x0 points at when the call starts: for the SDK's critical
// section (sdk+0x8fc90, the word is the owner's handle with bit 30 for waiters, 0 when free) that tells how often the lock
// was already held and by whom. The report goes to the log at exit, per presented frame.
// On the rec4 route the main thread entered critical sections about 19,000 times a frame, all but 0.1 % free.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fnhook.h"
#include "host.h"
#include "platform/platform.h"

#define FP_FUNCS 8
#define FP_SLOTS (1u << 16)   // per table, open addressing; a full table counts the call as dropped
// used: 0 free, 1 being claimed, 2 key valid; separate from key so every 64-bit argument is its own group
typedef struct { uint64_t key, used; uint64_t n, held, waiters; } FpSlot;
typedef struct { uint64_t addr, calls, dropped; int word; FpSlot *by_x0, *by_lr, *by_tid, *by_owner; } FpFunc;

static volatile int g_fnprof;
static FpFunc fp[FP_FUNCS];
static int nfp;
static uint64_t fp_presents;

uint32_t kern_cur_handle(void);

static FpSlot *slot(FpSlot *t, uint64_t key) {
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 48);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (FP_SLOTS - 1)) {
        uint64_t u = __atomic_load_n(&t[i].used, __ATOMIC_ACQUIRE);
        if (!u) {
            uint64_t z = 0;
            if (__atomic_compare_exchange_n(&t[i].used, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                t[i].key = key;
                __atomic_store_n(&t[i].used, 2, __ATOMIC_RELEASE);
                return &t[i];
            }
            u = z;
        }
        while (u == 1) u = __atomic_load_n(&t[i].used, __ATOMIC_ACQUIRE);   // another thread is writing the key
        if (t[i].key == key) return &t[i];
    }
    return NULL;
}

static void bump(FpFunc *f, FpSlot *t, uint64_t key, int held, int waiters) {
    FpSlot *s = slot(t, key);
    if (!s) { __atomic_add_fetch(&f->dropped, 1, __ATOMIC_RELAXED); return; }
    __atomic_add_fetch(&s->n, 1, __ATOMIC_RELAXED);
    if (held) __atomic_add_fetch(&s->held, 1, __ATOMIC_RELAXED);
    if (waiters) __atomic_add_fetch(&s->waiters, 1, __ATOMIC_RELAXED);
}

void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    if (!g_fnprof) return;
    FpFunc *f = NULL;
    for (int i = 0; i < nfp; i++)
        if (fp[i].addr == addr) { f = &fp[i]; break; }
    if (!f) return;
    __atomic_add_fetch(&f->calls, 1, __ATOMIC_RELAXED);
    uint32_t w = 0, me = kern_cur_handle();
    if (f->word && x0 >= 0x10000 && !(x0 & 3) && x0 < RT_ADDRESS_LIMIT) w = __atomic_load_n((uint32_t *)(uintptr_t)x0, __ATOMIC_RELAXED);
    uint32_t owner = w & ~0x40000000u;
    int held = owner && owner != me, waiters = (w >> 30) & 1;
    bump(f, f->by_x0, x0, held, waiters);
    bump(f, f->by_lr, lr, held, waiters);
    bump(f, f->by_tid, me, held, waiters);
    if (held) bump(f, f->by_owner, (uint64_t)me << 32 | owner, 1, waiters);
}

void fnprof_init(void) {
    const char *e = cfg_get("XC2_FNPROF");
    if (!e || !*e) return;
    for (const char *p = e; *p && nfp < FP_FUNCS;) {
        int word = *p == '*';   // *<addr>: x0 points at a lock word to read at each call
        if (word) p++;
        char *end;
        uint64_t a = strtoull(p, &end, 16);
        if (end == p) break;
        FpFunc *f = &fp[nfp];
        f->addr = a, f->word = word;
        f->by_x0 = calloc(FP_SLOTS, sizeof(FpSlot)), f->by_lr = calloc(FP_SLOTS, sizeof(FpSlot));
        f->by_tid = calloc(FP_SLOTS, sizeof(FpSlot)), f->by_owner = calloc(FP_SLOTS, sizeof(FpSlot));
        if (!f->by_x0 || !f->by_lr || !f->by_tid || !f->by_owner) rt_fatal("fnprof: out of memory");
        fnhook_count(a);
        nfp++;
        p = *end == ',' ? end + 1 : end;
    }
    rt_log("fnprof: counting the calls of %d guest functions", nfp);
    __atomic_store_n(&g_fnprof, nfp > 0, __ATOMIC_RELEASE);
}

void fnprof_frame(uint64_t frame) {
    (void)frame;
    if (g_fnprof) fp_presents++;
}

static int by_n(const void *a, const void *b) {
    uint64_t x = ((const FpSlot *)a)->n, y = ((const FpSlot *)b)->n;   // free slots have n 0
    return x < y ? 1 : x > y ? -1 : 0;
}

static void top(const char *what, FpSlot *t, double frames, int n, int owners) {
    qsort(t, FP_SLOTS, sizeof *t, by_n);
    for (int i = 0; i < n && t[i].n; i++) {
        uint64_t k = t[i].key;
        char d[128] = "";
        if (owners) snprintf(d, sizeof d, "thread %#x waits on owner %#x", (uint32_t)(k >> 32), (uint32_t)k);
        else if (!strcmp(what, "caller")) ldr_describe(k, d, sizeof d);
        else snprintf(d, sizeof d, "%#llx", (unsigned long long)k);
        rt_log("fnprof:   %-7s %-48s %10.1f calls/frame, held by another %5.1f %%, waiters %5.1f %%", what, d,
               t[i].n / frames, 100.0 * t[i].held / t[i].n, 100.0 * t[i].waiters / t[i].n);
    }
}

void fnprof_report(void) {
    if (!g_fnprof) return;
    g_fnprof = 0;
    double frames = fp_presents ? (double)fp_presents : 1;   // presents while counting
    for (int i = 0; i < nfp; i++) {
        FpFunc *f = &fp[i];
        char d[128];
        ldr_describe(f->addr, d, sizeof d);
        rt_log("fnprof: %s: %llu calls over %.0f frames, %.1f per frame (%llu not tabled)", d,
               (unsigned long long)f->calls, frames, f->calls / frames, (unsigned long long)f->dropped);
        top("x0", f->by_x0, frames, 12, 0);
        top("caller", f->by_lr, frames, 12, 0);
        top("thread", f->by_tid, frames, 8, 0);
        top("owner", f->by_owner, frames, 8, 1);
    }
}
