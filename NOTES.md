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

### Audio: voices and effects on one side only (fixed 2026-11-02)

XC2 plays many sounds as a pair of mono voices, one mixed to the left and one to the right, and its SDK (audio
renderer REV3) sends unused voice slots and channel resources zeroed, id 0 included. `audren.c` applied every entry by
its id, so each unused entry reset voice 0 and zeroed channel resource 0 once per update: the left voice of a pair,
and mono voices such as speech that landed in those slots, went silent. Only in-use entries are applied now, as
Ryujinx's StateUpdater does, and a voice the game stops reporting is released. Measured at the title screen: left and
right energy were 15.6 against 68.0 before, 45.9 against 34.7 after. The 5.1 downmix also took the LFE coefficient for
the back pair; it now uses {front, centre, LFE, back}.

## Input settings from TotK Recomp (2026-11-02)

Ported from totk-pc-recomp (same lineage, MIT): per-stick round dead zones (`stick_deadzone`, `lstick_deadzone`,
`rstick_deadzone`, with the live "stick now" reading on the Controls tab), the keyboard walk key (`walk_key`,
`walk_scale`), bindable back paddles (`pad:lpaddle1` ... `pad:rpaddle2`), the post-menu input gate that also waits for
the sticks to centre, and `XCX_TEST_PAD` (a virtual SDL gamepad with fixed stick values, for runs without a
controller). Checked in game with `XCX_TEST_PAD=0,20000,0,0 XCX_HID_TRACE=1`: the game reads the left stick's down bit
with the default 8 % zone and nothing with `XCX_LSTICK_DEADZONE=30`. The walk key was not driven in game: the Win32
back end reads keys with `GetAsyncKeyState` while the window is foreground, which a headless run cannot press.
TotK's `stick_sens` is not ported: it rewrites TotK's camera parameter pack in the RomFS. An XC2 version needs XC2's
own camera speed data.

### Xbox button prompts: how XC2 draws them (investigated 2026-11-02, not implemented)

TotK's `prompts` setting rewrites button glyphs in a font. XC2 has no button font: its prompts are textures.
- Text prompts are `[ML:icon icon=<name>]` tags in the BDAT messages (names `A B X Y L R ZL ZR L3 R3 plus minus up down
  left right`; the English tables use 176 of them). They draw cells of one shared atlas, the 512x256 BC3 texture (MIBL
  format 68, block-linear) at the end of `menu/image/button.wilay` in `bf2.arh/ard`: round A B X Y, L R ZL ZR in two
  shapes, plus, minus, home, stick and D-pad icons, SL and SR.
- Layouts also carry their own button art, for example the battle palette's styled B X Y in
  `menu/image/mnu032_button_icon.wilay`. There are 5,064 image layouts; which others show buttons is still to be found.
- Delivery: the game asks every add-on content mount (`aoc1` to `aoc99`) for a menu file before it reads `bf2.ard`
  (`XCX_TRACE=fs` at boot shows the probes for `menu/image/button.wilay`). A patched layout served through one of those
  mounts, or through a RomFS overlay, replaces the archive's copy without touching `bf2.arh/ard`.
- So the TotK approach carries over: a tool reads the player's own archive (`arh1` header with a XOR key, a path trie,
  24-byte file entries; `xbc1` zlib blocks in the `.ard`), swaps the A/B and X/Y cells for the Nintendo layout, and
  draws LB RB LT RT Menu View over L R ZL ZR plus minus. Then it re-encodes the texture and writes the loose files under
  `build/`. Nothing from the game ships. Not done yet: the archive reader as a tool, the texture writer, the cell map,
  the survey of the other layouts, the runtime overlay, a `prompts` setting, and an in-game check on a screen that shows
  `[ML:icon]` text.

## Mods and cheats (ported from TotK Recomp, 2026-11-02)

How players install and make mods: [docs/mods.md](docs/mods.md). The notes below are the implementation's.

A `mods` folder next to `xcx.exe` (`XCX_MODS_DIR`) takes mods as emulators take them, one folder per mod: `romfs/`,
`exefs/` (`.ips`, `.pchtxt`), `cheats/<build id>.txt`, or an SD card mod's `atmosphere/` folder. Order and switches
are `mods_order` / `mods_disabled` in `xcx.ini` and the menu's Mods tab; cheats switch on the Cheats tab.
`xcx.exe --probe-mods` checks the scan, order, switches and add-on content lookup without game files.

How XC2 finds a file (measured with `XCX_TRACE=fs` at boot): everything but `stream/` sits in `bf2.arh` / `bf2.ard`.
For a file of the archive the game reads `bf2.ard` at the archive's offset and never tries a loose `rom:/` path; before
that it looks the file up in each add-on content pack (`aoc1:/bdat/common.bdat`, ... `aoc99:/...`). It opens a loose
`rom:/` path only for files the archive lacks (`stream/`, `bdat/jp.bin`, a few effects and maps). So:

| Mod folder | Served over | Use |
|---|---|---|
| `<mod>/romfs`, `<mod>/atmosphere/contents/0100E95004038000/romfs` (and `contents/`, `<title>/`) | the base RomFS (`runtime/romfs.c`) | `stream/` music, voices and movies; a whole replaced `bf2.arh` / `bf2.ard` |
| `<mod>/atmosphere/contents/0100E950040390NN/romfs` (and `contents/`, `<title>/`; NN = pack in hex, 63 = pack 99) | that add-on content pack (`runtime/fs_host.c` `mod_aoc_path`) | files of the archive, as XC2 mods ship them (X2plus: `bdat/` for `...9001`) |

A base `romfs/bdat/common.bdat` is served if asked but XC2 never asks for it, as on an emulator. Add-on content files
of mods are not listed by `OpenDirectory` on a pack (the game opens them by path).

ExeFS patches and code cheats are lifted into the game: the installer's Lift step runs `tools/cheats.py` and
`tools/exefs_patches.py` over `<install>/mods` and writes `build-cache/cheats.status` / `exefs.status`, which the
runtime reads. A developer build does the same by hand:

```powershell
.venv\Scripts\python tools\cheats.py --exefs build\extracted\v301\exefs --mods build\v301\mods --out build\tmp\p1.txt --status build\v301\build-cache\cheats.status
.venv\Scripts\python tools\exefs_patches.py --exefs build\extracted\v301\exefs --mods build\v301\mods --patches build\tmp\p1.txt --out build\tmp\p2.txt --status build\v301\build-cache\exefs.status
.venv\Scripts\python recomp\gen.py --exefs build\extracted\v301\exefs --out generated\v301 --patches build\tmp\p2.txt
```

Unlike TotK Recomp, the game does not restart into the setup when a mod's code is unbuilt: the log, the Mods tab and
the Cheats tab say to run the setup again, and such a mod stays off. XC2 3.0.1's main build id is
`4F5C96E2D97F45AC288BC67613EBEF0A`; cheat files are named `4F5C96E2D97F45AC.txt`. 3.0.0 / 3.0.1 are recent and no
public 60 fps or resolution patch for them was found; emulator patches for 2.0.0 carry another build id and are listed
as made for another version.

## Known issues

### Probes that need XCX data

`--probe-vkexec` (draw checks) and `--probe-shader` read a shader census of XCX (`build/v121/shaders/jobs.json`) and
fail without it; XC2 has no census yet. `--probe-vkexec`'s present check also needs a 16:9 window
(`XCX_WINDOW_SIZE=1280x720`) in environments that size it otherwise. `make_release.py --prove` runs these probes.

### Settings that do nothing in XC2

The output resolution and 3D render scale (`resolution.c`) rewrite XCX's frame-size table and code; XC2's are not
found, so the game keeps 1920x1080 and logs why. The frame-rate patches (`frame_rate.c`) are XCX patch sets, absent
for XC2.

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
