// Hooks at the start of lifted functions (fnhook.c, cpu.h FN_ENTER). Set up before the guest runs: fnhook_count and
// fnhook_replace name the functions, fnhook_install then writes their sites once.
#pragma once
#include <stdint.h>

#include "cpu.h"

void fnhook_count(uint64_t addr);                 // fnprof_hit at each call of the function at guest address addr
void fnhook_replace(uint64_t addr, GuestFn fn);   // fn runs in place of the function at guest address addr
int fnhook_install(void);                         // writes the sites of the named functions; how many there were
long fnhook_sites(void);                          // every site of the build; 0 in one without RT_FN_HOOKS
int fnhook_probe(void);                           // 1 when every site still is the no-op it was built as (selftest)
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0);   // fnprof.c