# PC compatibility (Windows)

What a Windows PC needs to run XC2 Recomp, what has been tested and what has not. The game is native x86-64 code with a
Vulkan renderer; no emulator is involved. The setup builds the game on the player's PC, so it can build it for that
PC's processor, and its PC check (`installer/hostcheck.c`) applies the game's own rules, so it refuses what the game
would refuse and names what is missing.

**One PC has been tested** (below). "Accepted" in this file means the PC check lets a PC through; it does not mean such
a PC was tried.

## Evidence levels

| Level | Meaning |
| --- | --- |
| **Tested** | Ran on real hardware, with logs and screenshots kept. |
| **Path tested** | The code path ran on the tested PC by forcing it (for example hiding a Vulkan extension the driver has). Shows the code works; says nothing about a particular other driver. |
| **Emulated** | Ran under Intel SDE with an older processor's CPUID, SDE stopping at the first instruction that processor lacks. |
| **Check only** | The PC check's verdict in unit tests (`tests/installer_test.py` `HostVerdictTest`); nothing ran on such hardware. |
| **Unsupported** | Refused by design; the reason is given. |

## The tested PC

Intel Core i5-1240P (x86-64-v3), Intel Iris Xe (Vulkan 1.4.323, driver 32.0.101.7092), 16 GB RAM, Windows 11 Home
build 26300. No AMD or NVIDIA GPU, no AMD CPU, no CPU without AVX2 and no Windows 10 PC was available.

## Requirements

| | Requirement | Level |
| --- | --- | --- |
| Architecture | 64-bit x86 (Intel or AMD). Windows on Arm runs it only under Microsoft's x64 emulation, not natively; the PC check warns. | Check only (Arm) |
| Windows | 10 or 11. `xc2.exe` imports nothing newer than Windows 10 version 1507: `SetThreadDescription` (1607) is looked up at run time. Vulkan 1.3 drivers and the setup's Python ship for nothing older than Windows 10. | Tested on 11 (26300); Check only for 10 |
| Processor | x86-64-v3 (AVX2, FMA, BMI2: Intel Haswell 2013+, AMD Excavator 2015+ / Zen) gets the default build. x86-64-v2 (SSE4.2, POPCNT, CMPXCHG16B: Intel Nehalem 2008+, AMD Bulldozer 2011+, and Pentium and Celeron models up to 2020, which have no AVX) gets a build for that level. Older processors are refused. | v3 Tested; v2 Emulated |
| GPU API | Vulkan 1.3. | Tested |
| GPU extensions | `VK_KHR_swapchain`, `VK_KHR_push_descriptor`, `VK_EXT_depth_clip_control` (`runtime/vk_require.h`). `VK_EXT_external_memory_host` is optional: without it the game keeps host-visible copies of the GPU-shared memory (shadow storage, as on Linux), at about 1 GiB more committed memory in the opening on the tested PC (7.7 against 6.6 to 6.8 GiB). | Tested (with); Path tested (without) |
| GPU features | `timelineSemaphore`, `dynamicRendering`, `synchronization2`, `shaderStorageImageWriteWithoutFormat`, `depthClipControl`, `shaderInt64`, `samplerAnisotropy`, `independentBlend`, `geometryShader`, `textureCompressionBC`. A GPU lacking one is skipped, so a laptop whose discrete GPU lacks one plays on its integrated GPU. | Tested (all present) |
| Driver | The GPU maker's or PC maker's normal driver. No Vulkan SDK. | Tested (Intel) |
| RAM | 16 GB tested. The game alone held 3.1 to 5.8 GiB resident and 6.1 to 7.7 GiB private commit in the opening, depending on the build and path (below). Less RAM is untested; the game's own memory mode switches to import below 12 GB on an integrated GPU (`xc2.ini` `memory`). | Tested (16 GB only) |
| Disk | About 25 GiB free during setup, 20 GiB after. | Tested |

## What changed for this

1. **Processors without AVX2.** `tools/build_game.py --cpu v2` compiles the lifted code and the runtime for
   x86-64-v2; the setup does so on such a processor with the release's `runtime/libxc2rt-v2.a` (the release script
   builds both libraries). Before, the setup refused every processor without AVX2.
   - `runtime/kernel.c` `kern_ap_stub`, the hand-written stub that keeps the registers of preempted game code, saved
     YMM registers with AVX instructions in every build. A v2 build would have crashed a processor without AVX the
     first time game code was preempted. It now saves XMM registers with SSE instructions in a v2 build (v2 game code
     never holds data in the upper YMM halves).
   - `runtime/cpu_check.c` runs before `main` and before every other constructor, compiled for any x86-64 processor:
     a build started on a processor below its level (a v3 folder copied to an older PC) stops with a message and exit
     code 72 instead of an illegal-instruction crash.
   - A prebuilt runtime library of the other level does not link (`rt_cpu_level_v2` / `rt_cpu_level_v3` undefined),
     and `build_game.py --cpu v2` reads every runtime object with `tools/check_isa.py` before linking or archiving,
     stopping on any instruction above x86-64-v2.
2. **GPUs without `VK_EXT_external_memory_host`.** It was required; now a device without it gets shadow storage
   (`runtime/vk_memory.c` `mirror_mode`). Windows drivers of NVIDIA, AMD and Intel have it, so this mostly matters for
   layers and translation drivers that hide it (RenderDoc, Mesa's Vulkan-on-Direct3D 12 "Dozen"). Untested on any of
   them.
3. **One list of GPU requirements.** `runtime/vk_require.h` holds the version, extensions and features; the game's
   device choice and device creation use it, and so does the setup's PC check. Before, the setup checked only
   "Vulkan 1.3", accepting GPUs the game would then refuse at start, and the game checked features only after choosing
   a GPU, stopping instead of trying the next one.
4. **Windows 10 before version 1607.** The static import of `SetThreadDescription` stopped the loader there with
   "entry point not found"; it is looked up now. The PC check refuses Windows before 10 with a plain message.

## Validation

| Check | Result | Level |
| --- | --- | --- |
| Baseline (this commit's parent): extract, lift, compile, shader preparation, boot | RomFS 3.0.1 hash matched (14,019,660,980 bytes, 28,376 files); 9,033 shader variants translated, 0 failed; title screen and the New Game opening rendered with sound; 18.0 fps in the first minute (cold caches), 26.2 in the second (the opening cutscene runs at 30 fps) | Tested |
| Changed v3 build, same boot, twice | Self-test passed; opening rendered; 24.1 / 26.6 and 26.9 / 29.9 fps (warmer caches); peak 3,120 and 3,991 MiB resident, 6,631 and 6,090 MiB private commit | Tested |
| Changed v3 build with `XC2_VK_HIDE_EXT=VK_EXT_external_memory_host` | Both GPUs logged "no VK_EXT_external_memory_host; CPU-visible pools will be shadow copies"; shadow storage on; opening rendered (screenshot inspected); 28.9 / 29.7 fps; peak 5,797 MiB resident, 7,708 MiB commit | Path tested |
| v2 build (`--cpu v2`) on the tested PC, twice | Self-test passed; opening rendered; 30.5 / 29.8 and 29.8 / 30.0 fps; peak 5,424 and 5,522 MiB resident, 6,837 and 7,329 MiB commit | Tested (on a v3 CPU) |
| `tools/check_isa.py` on the v2 `xc2.exe` (17.5 million instructions) | 0 functions above x86-64-v2 outside a CPU check; 138 libvpx functions behind libvpx's own CPU detection (the linker map places 3 symbol-less ones in libvpx's `*_avx2.c.o` objects); the SHA-extension path of `runtime/sha256.c` behind its check | Static |
| Negative control: `tools/check_isa.py` at level 2 on the v3 `xc2.exe` | 37,269 functions flagged | Static |
| v2 self-test under SDE, Nehalem (no AVX, no XSAVE) and Sandy Bridge (AVX, no AVX2) | Passed in both, 0 SDE errors; the build reads the emulated processors as x86-64-v2 | Emulated |
| v3 `xc2.exe` under SDE, Nehalem and Sandy Bridge | Stopped by `cpu_check.c` with exit code 72 and the message, before any AVX instruction (SDE reported none) | Emulated |
| Negative control: an AVX2 test program under SDE | Stopped at the first AVX2 instruction on Sandy Bridge and the first AVX instruction on Nehalem, also with SDE limited to that image | Emulated |
| v2 game under SDE, Sandy Bridge | 14 minutes: title screen, New Game, the opening cutscene into the dive (screenshots inspected), with sound; 2 to 3.5 fps under emulation; 0 SDE errors | Emulated |
| v2 game under SDE, Nehalem | As is, SDE stops about 20 s in with its own error "OSXSAVE bit is not set in CPUID", raised by code outside the three checked images (also with the null graphics back end and SDL's dummy audio driver), before the window opens; reaZelda's TotK runs stopped the same way. With `-xsave_by_host 1` (code outside the images may use the host's XSAVE; the images stay checked against Nehalem, and the AVX2 test program is still stopped): 8.5 minutes, title screen, New Game, the opening cutscene with sound, 464 preemptions of game code through the SSE stub, 0 invalid instructions. Then the Vulkan device was lost (exit 70), which no native or Sandy Bridge run showed; the Intel driver runs under the emulated Nehalem CPUID too. Not isolated | Emulated |
| Shader translator (`xc2_xlate.exe`, .NET NativeAOT) under SDE, Nehalem and Sandy Bridge, its own image checked | `tools/xlate_smoke.py` passed: the test shader translated to valid SPIR-V, 0 SDE errors. Inside the SDE game runs the game's translation requests failed ("translator unavailable", 3 and 7 times; SDE instruments that child process too); the prepared shader cache covered the opening | Emulated |
| Setup PC check (`installer/hostcheck.c`) on the tested PC | `cpu_level` 3, Windows 10.0.26300, x64; both Iris Xe entries usable with host import | Tested |
| PC check verdicts (`tests/installer_test.py`, 13 cases) | v3 and v2 processors, v2 without the v2 library, below v2, Windows 7 / 8.1 refused and 10 1507 accepted, Arm warning, no loader or device, Vulkan 1.2 only, missing extensions and features named per GPU, a laptop with an unusable discrete GPU, host import judged on the GPU the game picks, software renderer, an older host check's output | Check only |

The SDE runs check `xc2.exe`, `SDL3.dll` and `libopus-0.dll` (`-chip_check_image`), not Windows' own DLLs or the GPU
driver, which choose their instructions by processor themselves. Evidence files are in `build/compat/` of the
maintainer's checkout (game-derived, not in the repository).

## Limits that remain

| Barrier | Why | What it would take |
| --- | --- | --- |
| 32-bit Windows | The guest memory layout needs far more address space than a 32-bit process has. Windows 11 has no 32-bit edition. | Not possible. |
| No Vulkan driver (Microsoft Basic Display Adapter; Intel before Skylake; NVIDIA Fermi and older; AMD TeraScale) | The renderer is Vulkan only. | A Direct3D 12 back end, or Mesa's Vulkan-on-Direct3D 12 driver (smaller feature set, untried). |
| Vulkan below 1.3 (NVIDIA Kepler, GTX 600/700, on its last 470-series driver; AMD GCN 1 to 3 on the legacy driver) | Dynamic rendering, synchronization2 and the 1.3 feature structures are used throughout. | A Vulkan 1.2 path with those as extensions; not started. |
| Processors below x86-64-v2 (Core 2, AMD K10) | Refused; every such processor predates Windows 11 and its speed would be far below the game's needs. | A baseline build is possible in principle; not pursued. |
| Windows on Arm | x64 code runs only under emulation there; the runtime's platform layer is x64 specific. | A native ARM64 Windows build. |
| Windows 7, 8, 8.1 | No Vulkan 1.3 drivers; the setup's Python does not run there. | Unsupported. |
| Software Vulkan (lavapipe, SwiftShader) | Accepted with a warning; far too slow. | None. |
| The Linux release | Has no v2 runtime library yet, so its setup refuses processors without AVX2 as before (`check.cpu_no_avx2`). | `tools/make_release_linux.py` building `libxc2rt-v2.a` against its sysroot. |

## What cannot be claimed

- That any GPU other than the Iris Xe works with these changes. AMD, NVIDIA and other Intel generations were not
  available for this validation ([building.md](building.md) records earlier play on other GPUs, on Linux).
- That a v2 build runs on a physical processor without AVX2. Emulation is strong evidence for the code it ran, but it
  covered a self-test and minutes of the opening, not the game, and the Nehalem run needed `-xsave_by_host` and ended
  in a lost device (above).
- How much slower the v2 build is in play: the comparison above ran in a 30 fps cutscene on a v3 processor, where both
  builds hold the cap. Without FMA its fused multiply-adds are library calls.
- That any driver without `VK_EXT_external_memory_host` works: only the hidden extension on the Iris Xe was run.
- Why the v2 build holds more memory: 1.4 to 2.4 GiB more resident and 0.2 to 1.2 GiB more commit than the v3 build
  across both pairs of runs, while the game's own memory census (110 pools, 1,709.7 MiB) was the same in all of them. The
  difference lies outside the game's pools; not isolated.
- Anything about long sessions, demanding scenes or PCs with less than 16 GB of RAM. Two runs of the same build
  differed by up to 0.9 GiB resident and 0.5 GiB commit.

## How to check another PC

Start `XC2-Setup.exe` and press *Start*: the PC check runs first and writes `host: {...}` to the setup log, the JSON
of `installer/hostcheck.c` with the processor level, the Windows build and, for each GPU, `usable`, `missing` and
`host_import`. A refusal names what each GPU lacks. Please attach that line to a compatibility report.
