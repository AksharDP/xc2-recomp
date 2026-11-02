# XC2 Recomp: status notes

Static recompilation of *Xenoblade Chronicles 2* (Switch) to Windows x86-64, forked from XCX Recomp v0.1.11
(https://github.com/jmars125/xcx-recomp). Everything under `build\` and `generated\` is derived from the game: keep it
private.

## Inputs

| Content | Title id | Notes |
|---|---|---|
| base | `0100E95004038000` | |
| update 3.0.1 | `0100E95004038800` | BKTR-merged into one RomFS (`tools/xc2_merge_romfs.py`) |
| add-on content 1-6, 99 | `0100E95004039001`-`...9006`, `...9063` | extracted per pack to `build\extracted\v301\aoc\<n>` |

- Modules: `main`, `sdk`, `subsdk0`; NintendoSDK **3.5.0** (XCX's runtime was written against SDK 19-20).
- DLC packs: 1 ships its own archive (`aoc1.arh`/`.ard`), 2-4 loose files, 99 streams (audio/movies), 5 and 6 only a
  64-byte `key.bin` (entitlement-only packs, no data; their 664-byte RomFS verifies). The game mounts all seven,
  probes every pack for a file before the base archive (`bf2.arh`/`bf2.ard`), and opens `aoc1.arh` itself.

## Build

```powershell
.venv\Scripts\python recomp\gen.py --exefs build\extracted\v301\exefs --out generated\v301
.venv\Scripts\python tools\build_game.py --toolchain <XCX-Setup>\llvm --ninja <XCX-Setup>\bin\ninja.exe
```

- `--version` defaults to `v301`; the profile defaults to `build\profile-v301`.
- The build copies `xcx_xlate.exe` and `xcx_xlate.id` next to `xcx.exe` (from `build\xlate`, else the XCX setup's
  `bin\`). Without a translator every shader fails and the game draws nothing while sound plays.
- If `llvm-windres` fails with "Unable to create temp file", point `TEMP`/`TMP` at a writable folder.

## State (2026-11-01)

- Boots, title screen, New Game, opening battle with its scene complete; sound, text, input, 30 fps.
- Lifter: `tests\diff\lift.py --exefs build\extracted\v301\exefs` (180k cases) and `tests\diff\simd.py` (160k
  cases) pass with 0 failures; only `udf` padding is rejected.
- Loader: `xcx.exe --probe-loader` passes; 172 host overrides bind (of 340 linked). The other 168 target SDK
  functions XC2's SDK 3.5.0 does not have.
- Version-dependent runtime paths: XC2 builds get `rt_game_version = 0`, so `RT_V143` branches take XCX 1.2.1's side.
  The only live one (`frame_rate.c` `player_pos`) runs under `XCX_SPEED_PROBE=1` only and reads through a guarded copy.
- XCX's frame-rate and resolution patch sites do not match XC2's `main`; the runtime declines them (native 30 fps,
  native resolution). 60 fps or a resolution option needs XC2's own sites.

## Fixed

### Opening battle: missing floor, dark props (front face, fixed 2026-11-01)

XC2 sets the upper-left window origin (`nvnDeviceSetWindowOriginMode`), and the replay mapped NVN's front face to
Vulkan unchanged. With the replay's negative-height viewports Vulkan judges winding with NDC +y up, NVN in window
coordinates running downwards, so every camera-facing triangle of a back-face-culled draw was culled. Gramps' body
(drawn in world coordinates with one matrix in `c5`) lost the floor in front of the camera, and the cloud sea showed
through; props drew their far sides lit from behind and looked like dark silhouettes. `vk_pipeline_set_dynamic`
now mirrors the front face for the upper-left origin. Verified by rasterizing Gramps' two body meshes on the CPU from
the dumped triangles and `c5` (`XCX_DUMP_MESH`, `XCX_ONLY_PROG`) against the GPU's pre-pass depth: 164,578 pixels
differed before, 4 after.

### Movies: host VP9 decoder never bound (fixed 2026-11-01)

XC2's SDK 3.5 movie library constructs `movie::Decoder(DecoderType, DecoderOutputFormat)` without XCX's mode and
priority arguments, and numbers packed NV12 3 rather than 4. `ov_movie.c` bound only XCX's constructor, so the first of
the 154 movies (`stream/movie/*.webm`, VP9 1280x720 + Vorbis) would have run the library's NVDEC path and aborted. The
host decoder now binds XC2's constructor and format. The Vorbis tracks stay on the library's CPU decoder (SoftVorbis),
which constructs in the running game without NvRm. `xcx.exe --probe-movie` decodes an XC2 movie and passes; a movie
has not been watched in game yet.

## Known issues

### SDK 3.5.0 boundary

The overrides that bind by name were written for SDK 19-20 structures. Input, oe, apm, applet and account were
adapted (`ov_input.c`, `ov_system.c`). Of the 172 that bind, 60 run between boot and the opening battle; every one of
those that writes a structure into guest memory matches XC2's own SDK code (calendar additional info 0x18, NpadFullKeyState
0x28, TouchScreenState<16> 0x290, the save fs calls). The rest are unaudited. `--probe-fs` passes (165 checks; run it
with TEMP pointing at a writable folder).

## Diagnostics added for XC2

| Setting | Effect |
|---|---|
| `XCX_TRACE=fs` | RomFS opens (and failed ones) and every RomFS read with offset, size and short-read flags |
| `XCX_DRAW_CALLERS=<frame>` | guest call stacks of every draw, recordings, inlined command lists and submissions |
| `XCX_DUMP=<frame>` | now also per-draw depth state, vertex inputs, instance streams, vertex position bounds, skipped draws, render conditions and replay order (`note` lines in `passes.txt`) |
| `XCX_DUMP_MESH=<index count>[,...]` | with `XCX_DUMP`: the triangles (`mesh_<count>_<n>.bin`, float triples) and vertex indices (`.idx`) of draws with that index count |
| `XCX_ONLY_PROG=<program serial>` | with `XCX_DUMP`: only that program's draws are made in the dump frame |
| `XCX_OQ_VISIBLE=1` | every samples-passed report counts as visible |
| `XCX_HID_TRACE=1` | pad polling |
