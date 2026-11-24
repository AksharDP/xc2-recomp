// Hooks at the start of lifted functions (cpu.h FN_ENTER): the sites of a hooked function become jumps to their cold
// paths, which come here. Two users: the function-entry profiler (fnprof.c) counts a call and lets the function run, and
// the code of the player's mods (modcode.c) runs another function in its place.
//
// Sites are written by fnhook_install at start, before any guest thread runs: a site is five bytes of code that another
// thread may be executing.
#include "fnhook.h"

#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "host.h"
#include "platform/platform.h"

typedef struct { int32_t site, cold; uint64_t addr; } Key;   // cpu.h FN_ENTER; the offsets are from their own fields

#ifdef _WIN32
__attribute__((section(".fnkey$a"), used, aligned(16))) static const Key key_start[1] = {{0, 0, 0}};
__attribute__((section(".fnkey$z"), used)) static const char key_stop[1] = {0};
#define KEYS (key_start + 1)
#define KEYS_END ((const Key *)key_stop)
#else
extern const Key __start_fnkey[] __attribute__((weak)), __stop_fnkey[] __attribute__((weak));
#define KEYS __start_fnkey
#define KEYS_END __stop_fnkey
#endif

// The hooked functions: an open hash by guest address, written before the guest starts and only read after.
typedef struct { uint64_t addr; GuestFn fn; int count; } Hook;
static Hook *hooks;
static uint32_t cap, used;   // cap: a power of two

static uint32_t slot_of(const Hook *t, uint32_t n, uint64_t addr) {
    uint32_t i = (uint32_t)((addr >> 2) * 0x9E3779B1u) & (n - 1);
    while (t[i].addr && t[i].addr != addr) i = (i + 1) & (n - 1);
    return i;
}

static Hook *hook_at(uint64_t addr) {
    if (used * 2 >= cap) {
        uint32_t n = cap ? cap * 2 : 64;
        Hook *t = (Hook *)calloc(n, sizeof *t);
        if (!t) rt_fatal("fnhook: out of memory");
        for (uint32_t i = 0; i < cap; i++)
            if (hooks[i].addr) t[slot_of(t, n, hooks[i].addr)] = hooks[i];
        free(hooks);
        hooks = t, cap = n;
    }
    Hook *h = &hooks[slot_of(hooks, cap, addr)];
    if (!h->addr) h->addr = addr, used++;
    return h;
}

void fnhook_count(uint64_t addr) { hook_at(addr)->count = 1; }
void fnhook_replace(uint64_t addr, GuestFn fn) { hook_at(addr)->fn = fn; }

int rt_fn_hook(Cpu *c, uint64_t addr) {
    const Hook *h = &hooks[slot_of(hooks, cap, addr)];
    if (h->count) fnprof_hit(addr, c->x[30], c->x[0]);
    if (!h->fn) return 0;
    c->pc = addr;
    return 1;
}

void rt_fn_run(Cpu *c) { __attribute__((musttail)) return hooks[slot_of(hooks, cap, c->pc)].fn(c); }

int fnhook_install(void) {
    int sites = 0;
    if (!used) return 0;
    for (const Key *k = KEYS; k < KEYS_END; k++) {
        const Hook *h = &hooks[slot_of(hooks, cap, k->addr)];
        if (!h->addr) continue;
        uint8_t *site = (uint8_t *)&k->site + k->site, *cold = (uint8_t *)&k->cold + k->cold, jump[5] = {0xE9};
        int32_t rel = (int32_t)(cold - (site + 5));
        memcpy(jump + 1, &rel, 4);
        if (!plat_code_write(site, jump, sizeof jump)) rt_fatal("fnhook: cannot write the code at %p", (void *)site);
        sites++;
    }
    return sites;
}

long fnhook_sites(void) { return (long)(KEYS_END - KEYS); }

int fnhook_probe(void) {
    static const uint8_t nop[5] = {0x0F, 0x1F, 0x44, 0, 0};
    for (const Key *k = KEYS; k < KEYS_END; k++)
        if (memcmp((const uint8_t *)&k->site + k->site, nop, 5) || !k->addr) return 0;
    return 1;
}