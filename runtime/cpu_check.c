// Start-up check that this processor runs the instruction set the build was compiled for (runtime/cpu_level.h).
//
// A build for x86-64-v3 started on a processor without AVX2 (a portable folder copied to an older PC, say) would stop
// at the first AVX2 instruction with an illegal-instruction fault and no word of why. This constructor runs before
// main and before the other constructors (priority 101; the C++ static initializers of the menu have the default
// one), says which level the build needs and how to get one for this processor, and exits with code 72.
//
// tools/build_game.py compiles this file with -march=x86-64 after the build's own -march, so the check itself runs on
// any x86-64 processor; it calls nothing of the rest of the runtime for the same reason. It is an install-side file
// (build_game.py INSTALL_SIDE), compiled with the level of the install's generated code, and references the runtime's
// RT_CPU_MARK of that level, so a prebuilt runtime library of another level does not link.
#include <stdio.h>
#include <stdlib.h>

#include "cpu_level.h"

#ifdef _WIN32
#include <windows.h>
#endif

extern const int RT_CPU_MARK(XC2_CPU_LEVEL);

static const char k_message[] =
#if XC2_CPU_LEVEL == 3
    "This build of XC2 Recomp is compiled for x86-64-v3 processors (AVX2, FMA, BMI2), and this processor lacks them.\n"
    "Run XC2-Setup on this PC with the same install folder: it builds the game for this processor (x86-64-v2).\n";
#elif XC2_CPU_LEVEL == 2
    "This build of XC2 Recomp is compiled for x86-64-v2 processors (SSE4.2, POPCNT, CMPXCHG16B), and this processor "
    "lacks them.\nXC2 Recomp needs at least an Intel Core (Nehalem, 2008) or AMD Bulldozer (2011) class processor.\n";
#else
    "This experimental build of XC2 Recomp requires baseline x86-64 (SSE2).\n";
#endif

__attribute__((constructor(101))) static void cpu_check(void) {
    int have = cpu_level_host();
    if (have >= RT_CPU_MARK(XC2_CPU_LEVEL)) return;
    fprintf(stderr, "error: %s", k_message);
    fflush(stderr);
#ifdef _WIN32
    // As plat_fatal_notice (runtime/platform/win32.c): a box only when the game has its console to itself, which a
    // double-click makes and which disappears when it exits; a shared console (cmd, a test harness) keeps the line.
    DWORD pids[2];
    if (GetConsoleWindow() && GetConsoleProcessList(pids, 2) == 1)
        MessageBoxA(NULL, k_message, "XC2 Recomp", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    ExitProcess(72);
#else
    _Exit(72);
#endif
}
