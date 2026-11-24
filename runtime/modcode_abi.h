// What a mod library (tools/mod_code.py) and the game share. A mod library holds the lifted functions that the player's
// code patches and code cheats change, lifted again with the patches in and compiled at game start; modcode.c loads it
// and fnhook.c sends the calls of those functions to it. The library imports nothing: the game fills in the runtime
// services, the patch switches and the other guest functions it calls.
//
// MOD_ABI_VERSION, cpu.h, simd.h, simd_fp.h, this file, the generator, the compile flags and the patch table of the build
// are all in the key of tools/mod_abi.py, so a library only loads into the build it was made for.
#pragma once
#include "cpu.h"

#define MOD_ABI_VERSION 1

typedef struct {
    void (*dispatch)(Cpu *);
    void (*spin_gate)(Cpu *, uint64_t);
    void (*svc)(Cpu *, uint32_t, uint64_t);
    void (*brk)(Cpu *, uint32_t, uint64_t) __attribute__((noreturn));
    void (*udf)(Cpu *, uint64_t) __attribute__((noreturn));
    uint64_t (*ticks)(void);
    uint8_t *patch_on;   // the switches of the build's patch sets, then those of the library's
} ModRt;

typedef struct { const char *name, *env; int on; } ModSet;   // a patch set the build lacks; its switch is first_set + i

// A function the library replaces. sets: the switches (indices into patch_on) whose sites it holds; nsets 0: sites of the
// build itself that changed (a developer's edited patch table), which run whether or not a mod is on.
typedef struct { uint64_t addr; GuestFn fn; const uint16_t *sets; int nsets; } ModFn;

// One object of the library: its functions, and the guest functions they call, by address; the game fills imports.
typedef struct {
    const ModFn *fns;
    int nfns;
    const uint64_t *import_addrs;
    GuestFn *imports;
    int nimports;
} ModPart;

typedef struct {
    int version;       // MOD_ABI_VERSION
    const char *key;   // of the build it was made for
    int first_set, nsets;
    const ModSet *sets;
    const ModPart *const *parts;
    int nparts;
    ModRt *rt;         // the game fills it
} ModLib;

#ifndef MOD_LIBRARY
extern uint8_t rt_patch_on[];   // patches.c
// patches.c: adds a library's sets after the build's, each with its setting; 0 when they do not fit this build.
int rt_patches_add(int first, const ModSet *sets, int n);
#endif

#define MOD_ENTRY "xc2_mod_lib"
typedef const ModLib *(*ModEntry)(void);
