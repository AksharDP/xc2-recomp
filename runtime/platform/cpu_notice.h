// CPU rejection runs before the processor can safely call the rest of the runtime. Keep this helper inline in the
// baseline-compiled cpu_check.c rather than calling a platform object compiled for the unsupported CPU level.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

static inline _Noreturn void plat_cpu_unsupported(const char *message) {
    fprintf(stderr, "error: %s", message);
    fflush(stderr);
#ifdef _WIN32
    // Same console-ownership rule as plat_fatal_notice in platform/win32.c; only ASCII startup messages use this.
    DWORD pids[2];
    if (GetConsoleWindow() && GetConsoleProcessList(pids, 2) == 1)
        MessageBoxA(NULL, message, "XC2 Recomp", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    ExitProcess(72);
#else
    _Exit(72);
#endif
}
