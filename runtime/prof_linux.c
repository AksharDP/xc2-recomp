// Sampling profiler for Linux (XC2_PROFILE=<path>), in place of perf.c's weak stand-ins: one thread's CPU time, sampled
// every PERIOD_US of it by a thread CPU-time timer whose signal goes to that thread (SIGEV_THREAD_ID). The handler adds
// the interrupted instruction pointer to an open-addressing table; one thread is sampled, so it never runs twice at
// once. XC2_PROFILE_THREAD picks the thread: replay (default, the asynchronous replay), render or main.
// XC2_PROFILE_FROM / XC2_PROFILE_TO: the presents to sample between (default 1800 and 3600). The profile is text:
//   xc2-prof-linux 1 / thread <name> <tid> / period_us <n> / samples <n> / map <lo> <hi> <path> (executable mappings)
//   / rip <hex> <count>, most first. Addresses in the executable are as nm prints them (not position independent).
#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // REG_RIP
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "cfg.h"
#include "host.h"
#include "prof.h"

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif

#define SLOTS (1u << 16)
#define PERIOD_US 1000

static struct { uint64_t rip, n; } tab[SLOTS];
static volatile uint64_t nsamples, ndropped;
static volatile int in_handler;   // on_sample running: prof_sampler_write waits for it after deleting the timer
static int state;   // 0 off, 1 waiting for the first frame, 2 sampling, 3 written
static uint64_t from = 1800, to = 3600;
static int timer;   // the kernel's timer id (raw syscalls: the link's glibc keeps timer_* in librt)
static uint32_t tid;
static const char *path, *which;

// (rip, [rsp]) pairs: in a leaf such as memcpy or libc's ioctl wrapper the word at the stack pointer is the caller's
// return address, which tells whose copy or kernel call it was; elsewhere it is just a stack word (the report only
// uses it for samples outside the executable).
static struct { uint64_t rip, ret, req, n; } pairs[SLOTS];   // req: rsi when it looks like a DRM ioctl request
static uint64_t exe_lo, exe_hi;   // the executable's code (set before sampling): the frame walk stops at the first return into it

static void record(void *ctx);

static void on_sample(int sig, siginfo_t *si, void *ctx) {
    (void)sig, (void)si;
    __atomic_store_n(&in_handler, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&state, __ATOMIC_SEQ_CST) != 2) {   // the writer has started: leave the tables alone
        __atomic_store_n(&in_handler, 0, __ATOMIC_RELEASE);
        return;
    }
    record(ctx);
    __atomic_store_n(&in_handler, 0, __ATOMIC_RELEASE);
}

static void record(void *ctx) {
    uint64_t rip = (uint64_t)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
    uint64_t ret = *(const uint64_t *)(uintptr_t)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RSP];
    // An ioctl's request stays in rsi across the syscall: DRM ones have type 'd' and a direction.
    uint64_t rsi = (uint64_t)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RSI];
    uint32_t r32 = (uint32_t)rsi;   // callers pass the request as an int now and then: sign-extended
    uint64_t req = ((rsi >> 32) == 0 || (rsi >> 32) == 0xffffffffu) && ((r32 >> 8) & 0xff) == 0x64 && (r32 >> 30) ? r32 : 0;
    // Outside the executable, the first return into it along the frame-pointer chain (libc's ioctl and the drivers
    // keep frame pointers on SteamOS), when the chain stays on this stack: whose kernel call or driver work it was.
    if (exe_hi && !(rip >= exe_lo && rip < exe_hi)) {
        uint64_t sp = (uint64_t)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RSP];
        uint64_t bp = (uint64_t)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RBP];
        for (int d = 0; d < 12 && bp >= sp && bp < sp + (1u << 20) && !(bp & 7); d++) {
            uint64_t ra = ((const uint64_t *)(uintptr_t)bp)[1];
            if (ra >= exe_lo && ra < exe_hi) { ret = ra; break; }
            uint64_t nbp = ((const uint64_t *)(uintptr_t)bp)[0];
            if (nbp <= bp) break;
            bp = nbp;
        }
    }
    uint32_t hp = (uint32_t)(((rip >> 2) ^ (ret * 31) ^ (req * 7)) * 0x9E3779B97F4A7C15ull >> 48) & (SLOTS - 1);
    for (uint32_t k = 0; k < 64; k++, hp = (hp + 1) & (SLOTS - 1)) {
        if (pairs[hp].rip == rip && pairs[hp].ret == ret && pairs[hp].req == req) { pairs[hp].n++; break; }
        if (!pairs[hp].rip) { pairs[hp].rip = rip, pairs[hp].ret = ret, pairs[hp].req = req, pairs[hp].n = 1; break; }
    }
    uint32_t h = (uint32_t)((rip >> 2) * 0x9E3779B97F4A7C15ull >> 48) & (SLOTS - 1);
    for (uint32_t k = 0; k < 64; k++, h = (h + 1) & (SLOTS - 1)) {
        if (tab[h].rip == rip) { tab[h].n++; nsamples++; return; }
        if (!tab[h].rip) { tab[h].rip = rip, tab[h].n = 1; nsamples++; return; }
    }
    ndropped++;
}

static uint32_t target(uint64_t frame) {
    if (!strcmp(which, "render")) return prof_render_tid;
    if (!strcmp(which, "main")) return prof_main_tid;
    // With a synchronous replay (XC2_REPLAY_ASYNC=0) there is no replay thread: the render thread records instead.
    if (!prof_replay_tid && frame >= from + 600) {
        rt_log("prof: no replay thread %llu presents after XC2_PROFILE_FROM: sampling the render thread, which replays",
               (unsigned long long)(frame - from));
        which = "render";
        return prof_render_tid;
    }
    return prof_replay_tid;
}

int prof_sampler_start(void) {
    path = cfg_get("XC2_PROFILE");
    if (!path || !*path) return 0;
    which = cfg_get("XC2_PROFILE_THREAD");
    if (!which || !*which) which = "replay";
    const char *f = cfg_get("XC2_PROFILE_FROM"), *t = cfg_get("XC2_PROFILE_TO");
    if (f && *f) from = strtoull(f, NULL, 10);
    if (t && *t) to = strtoull(t, NULL, 10);
    __atomic_store_n(&state, 1, __ATOMIC_SEQ_CST);
    rt_log("prof: sampling the %s thread's CPU time every %d us from present %llu to %llu into %s", which, PERIOD_US,
           (unsigned long long)from, (unsigned long long)to, path);
    return 1;
}

static int cmp(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[1], y = ((const uint64_t *)b)[1];
    return x < y ? 1 : x > y ? -1 : 0;
}

void prof_sampler_write(void) {
    if (state != 2) return;
    __atomic_store_n(&state, 3, __ATOMIC_SEQ_CST);
    syscall(SYS_timer_delete, timer);
    // A signal delivered before the delete may still be in its handler on the sampled thread; one that starts later
    // sees state 3 and returns. Wait for the former before reading the tables.
    while (__atomic_load_n(&in_handler, __ATOMIC_SEQ_CST)) {
        struct timespec ts = {0, 100000};
        nanosleep(&ts, NULL);
    }
    FILE *o = fopen(path, "w");
    if (!o) { rt_log("prof: cannot write %s", path); return; }
    fprintf(o, "xc2-prof-linux 1\nthread %s %u\nperiod_us %d\nsamples %llu\ndropped %llu\n", which, tid, PERIOD_US,
            (unsigned long long)nsamples, (unsigned long long)ndropped);
    FILE *m = fopen("/proc/self/maps", "r");
    char line[512];
    while (m && fgets(line, sizeof line, m)) {
        unsigned long long lo, hi;
        char perm[8], p[400] = "";
        if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %399[^\n]", &lo, &hi, perm, p) >= 3 && perm[2] == 'x')
            fprintf(o, "map %llx %llx %s\n", lo, hi, p);
    }
    if (m) fclose(m);
    static uint64_t rows[SLOTS][2];
    int n = 0;
    for (uint32_t i = 0; i < SLOTS; i++)
        if (tab[i].rip) rows[n][0] = tab[i].rip, rows[n][1] = tab[i].n, n++;
    qsort(rows, (size_t)n, sizeof rows[0], cmp);
    for (int i = 0; i < n; i++) fprintf(o, "rip %llx %llu\n", (unsigned long long)rows[i][0], (unsigned long long)rows[i][1]);
    for (uint32_t i = 0; i < SLOTS; i++)
        if (pairs[i].rip && pairs[i].n > 2)
            fprintf(o, "pair %llx %llx %llx %llu\n", (unsigned long long)pairs[i].rip, (unsigned long long)pairs[i].ret,
                    (unsigned long long)pairs[i].req, (unsigned long long)pairs[i].n);
    fclose(o);
    rt_log("prof: %llu samples of the %s thread written to %s", (unsigned long long)nsamples, which, path);
}

void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d) {
    (void)now, (void)d;
    if (state == 1 && frame >= to) {   // the thread never came up inside the window
        rt_log("prof: no %s thread to sample before present %llu: no profile", which, (unsigned long long)to);
        __atomic_store_n(&state, 0, __ATOMIC_SEQ_CST);
    } else if (state == 1 && frame >= from) {
        tid = target(frame);
        if (!tid) return;   // the thread is not up yet
        FILE *m = fopen("/proc/self/maps", "r");   // the executable's code: the mapping that holds this function
        char line[512];
        while (m && fgets(line, sizeof line, m)) {
            unsigned long long lo, hi;
            if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && (uint64_t)(uintptr_t)&prof_sampler_frame >= lo &&
                (uint64_t)(uintptr_t)&prof_sampler_frame < hi)
                exe_lo = lo, exe_hi = hi;
        }
        if (m) fclose(m);
        struct sigaction sa = {.sa_sigaction = on_sample, .sa_flags = SA_SIGINFO | SA_RESTART};
        sigemptyset(&sa.sa_mask);
        int sig = SIGRTMIN + 6;
        sigaction(sig, &sa, NULL);
        struct sigevent ev = {.sigev_notify = SIGEV_THREAD_ID, .sigev_signo = sig};
        ev.sigev_notify_thread_id = (pid_t)tid;
        // The thread's CPU clock (the kernel's MAKE_THREAD_CPUCLOCK: CPUCLOCK_SCHED, per thread), shifted unsigned.
        clockid_t clk = (clockid_t)((~(uint32_t)tid << 3) | 6u);
        if (syscall(SYS_timer_create, clk, &ev, &timer)) {
            rt_log("prof: timer_create on thread %u failed", tid);
            __atomic_store_n(&state, 0, __ATOMIC_SEQ_CST);
            return;
        }
        struct itimerspec it = {{0, PERIOD_US * 1000}, {0, PERIOD_US * 1000}};
        syscall(SYS_timer_settime, timer, 0, &it, NULL);
        __atomic_store_n(&state, 2, __ATOMIC_SEQ_CST);   // the handler reads it
        rt_log("prof: sampling thread %u from present %llu", tid, (unsigned long long)frame);
    } else if (state == 2 && frame >= to) {
        prof_sampler_write();
    }
}
#endif
