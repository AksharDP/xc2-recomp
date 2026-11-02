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

- Boots, title screen, New Game, opening battle; sound, text, input, 30 fps.
- Lifter: `tests\diff\lift.py --exefs build\extracted\v301\exefs` (180k cases) and `tests\diff\simd.py` (160k
  cases) pass with 0 failures; only `udf` padding is rejected.
- Loader: `xcx.exe --probe-loader` passes; 172 host overrides bind (of 340 linked). The other 168 target SDK
  functions XC2's SDK 3.5.0 does not have.
- Version-dependent runtime paths: XC2 builds get `rt_game_version = 0`, so `RT_V143` branches take XCX 1.2.1's side.
  The only live one (`frame_rate.c` `player_pos`) runs under `XCX_SPEED_PROBE=1` only and reads through a guarded copy.
- XCX's frame-rate and resolution patch sites do not match XC2's `main`; the runtime declines them (native 30 fps,
  native resolution). 60 fps or a resolution option needs XC2's own sites.

## Known issues

### Opening battle: the floor under Rex is missing (open)

The cloud sea shows where the floor in front of the camera should be. Established so far (frame 3400-3401 of
`build\tmp\in_c4.txt`):

- The surface Rex stands on is absent from the depth pre-pass and the G-buffer: down a screen column beside Rex the
  distance grows from 6.7 units (crest) to 19 units (bottom edge); Rex is at 1.9. What shows through is lower terrain,
  and the cloud pass covers it correctly.
- Map meshes draw in the G-buffer with depth EQUAL and no depth writes, so a mesh absent from the pre-pass is absent
  from the G-buffer too.
- Ruled out: depth mode and ranges (one `SetDepthMode(NEAR_IS_MINUS_W)` at boot; every draw 0..1, no bias or clamp),
  the cloud pass (its `gl_FragDepth` is on the same scale as geometry), occlusion queries (`XCX_OQ_VISIBLE=1`),
  conditional rendering, pipeline or shader skips (every walked draw reaches the GPU), command ordering (record,
  submit and replay order match), skinning (bone matrices valid), vertex formats, instance buffer ranges and
  transforms (all map meshes valid), the VRAM mirror (`XCX_MEMORY=import`), callee-saved register trust
  (`gen.py --no-callee-saved-trust`), streaming (no failed or short archive reads).
- Next: draw the frame's map meshes one at a time to find the one that should cover the floor, then follow it.

### SDK 3.5.0 boundary

The 147 overrides that bind by name were written for SDK 19-20 structures. Input, oe, apm, applet and account were
adapted (`ov_input.c`, `ov_system.c`); the rest have not been audited struct by struct against SDK 3.5.0.

## Diagnostics added for XC2

| Setting | Effect |
|---|---|
| `XCX_TRACE=fs` | RomFS opens (and failed ones) and every RomFS read with offset, size and short-read flags |
| `XCX_DRAW_CALLERS=<frame>` | guest call stacks of every draw, recordings, inlined command lists and submissions |
| `XCX_DUMP=<frame>` | now also per-draw depth state, vertex inputs, instance streams, skipped draws, render conditions and replay order (`note` lines in `passes.txt`) |
| `XCX_OQ_VISIBLE=1` | every samples-passed report counts as visible |
| `XCX_HID_TRACE=1` | pad polling |
