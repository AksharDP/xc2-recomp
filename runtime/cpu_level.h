// The x86-64 microarchitecture level a build is compiled for, and the level the processor runs.
//
// XC2_CPU_LEVEL comes from tools/build_game.py --cpu: 3 (-march=x86-64-v3: AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE; the
// default) or 2 (-march=x86-64-v2: SSE4.2, SSSE3, POPCNT, CMPXCHG16B, for processors without AVX2, such as Intel before
// Haswell, Pentium and Celeron models up to 2020, and AMD before Excavator). Both the lifted code and the runtime take
// it, so one build runs on processors of that level and above.
//
// cpu_level_host() reads the level from CPUID, the bits the psABI's level definitions list, with the OS saving the YMM
// state for level 3. The installer's host check (installer/hostcheck.c) includes this header too, so the installer and
// the game judge a processor by the same rule. Not __builtin_cpu_supports("x86-64-v3"): clang reads that from the C
// runtime's CPU model, and libgcc before GCC 12 has no level bits, so it answers false on every CPU.
//
// RT_CPU_MARK(n) names a symbol the runtime defines for the level it was compiled for (runtime/main.c). The start-up
// check (runtime/cpu_check.c, compiled per install) references the one of its own level, so linking a prebuilt runtime
// library of the other level into a build fails at the link with an undefined rt_cpu_level_v2 or rt_cpu_level_v3
// instead of starting.
#ifndef CPU_LEVEL_H
#define CPU_LEVEL_H

#include <cpuid.h>

#ifndef XC2_CPU_LEVEL
#define XC2_CPU_LEVEL 3
#endif
#if XC2_CPU_LEVEL != 2 && XC2_CPU_LEVEL != 3
#error "XC2_CPU_LEVEL must be 2 or 3"
#endif

#define RT_CPU_MARK_(n) rt_cpu_level_v##n
#define RT_CPU_MARK(n) RT_CPU_MARK_(n)

// 3, 2 or 1 (baseline x86-64: SSE2 only).
static inline int cpu_level_host(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 1;
    const unsigned sse3 = 1u << 0, ssse3 = 1u << 9, cx16 = 1u << 13, sse41 = 1u << 19, sse42 = 1u << 20,
                   popcnt = 1u << 23;
    const unsigned v2_ecx = sse3 | ssse3 | cx16 | sse41 | sse42 | popcnt;
    unsigned c1 = c;
    unsigned a8, b8, c8, d8;
    if (!__get_cpuid(0x80000001, &a8, &b8, &c8, &d8)) return 1;
    if ((c1 & v2_ecx) != v2_ecx || !(c8 & 1u)) return 1;   // c8 bit 0: LAHF/SAHF in 64-bit mode
    const unsigned fma = 1u << 12, movbe = 1u << 22, osxsave = 1u << 27, avx = 1u << 28, f16c = 1u << 29;
    const unsigned v3_ecx = fma | movbe | osxsave | avx | f16c;
    if ((c1 & v3_ecx) != v3_ecx) return 2;
    unsigned lo, hi;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    (void)hi;
    if ((lo & 6) != 6) return 2;   // XMM and YMM state enabled by the OS
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 2;
    const unsigned bmi1 = 1u << 3, avx2 = 1u << 5, bmi2 = 1u << 8;
    if ((b & (bmi1 | avx2 | bmi2)) != (bmi1 | avx2 | bmi2)) return 2;
    if (!(c8 & (1u << 5))) return 2;   // LZCNT (ABM)
    return 3;
}

#endif
