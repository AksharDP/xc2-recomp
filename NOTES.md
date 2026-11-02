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

### Button prompts: Xbox, PlayStation and keyboard (2026-11-02)

`XCX_PROMPTS` (`prompts` in `[input]`, the *Button prompts* row on the Controls tab, after restart): `auto` (default),
`switch` (the game's own), `xbox`, `playstation`, `keyboard`. `auto` takes the gamepad's labels (cross on the south
button: PlayStation; B there: Nintendo, the game's own; else Xbox), and the keyboard without a gamepad. The Xbox and
PlayStation sets follow `XCX_PAD_LAYOUT`: in the Nintendo layout Switch A is the right button, so A/B and X/Y swap
(Xbox B, PlayStation circle for A).
- How XC2 draws them: no button font. `[ML:icon icon=<name>]` tags in the BDAT messages draw cells of an 8x4 atlas of
  64-pixel cells (UVs on that grid in the layout), the 512x256 BC3 texture of `menu/image/button.wilay`;
  `font_icon.wilay` has the same atlas at 24 pixels. A survey of the 177 image layouts whose names suggest buttons
  (`tools/xc2_prompts.py --survey`) found button art in five more: `mnu032_button_icon(_new)` (the battle palette's
  A B X Y spheres), `mnu032_button_push_main` (tilted A A B B X X Y Y discs, textures 2-9), `mnu032_btn_circle` (a large
  B in a ring) and `mnu001_btn` (plus). The others are not buttons (`mainmenu_keyinfo` is a placeholder bar).
- MIBL footer: the image starts `size` bytes before the footer's end, unless the swizzled image fills its pages
  exactly; then the footer has a 4 KiB page of its own (`button.wilay`: 131,072 bytes of image in 135,168). Block
  height as tegra_swizzle's `block_height_mip0` (one and a half times the height in blocks).
- `tools/xc2_prompts.py` reads `bf2.arh/ard` from the merged RomFS, pastes Kenney's Input Prompts (CC0, the 66 icons in
  `tools/data/kenney-input-prompts`) over each Switch button and re-encodes only those BC3 blocks; pure Python, the same
  bytes on 3.12 and 3.14, about 20-45 s. Five sets (`xbox`, `xbox_nintendo`, `playstation`, `playstation_nintendo`,
  `keyboard`) of seven layouts go to `build/v301/prompts/<set>/` with a `manifest.tsv`; `build_game.py` copies them next
  to `xcx.exe`, the installer's Prepare step writes them into the install. The atlas icons take the atlas's style
  (white face, blue letters, the face buttons in their brand colours, blue rim and glow); the battle palette's are
  solid in the brand colour (teal for keys) with white letters. The keyboard set shows `DEFAULT_KEYS` (E, Shift, Space,
  mouse left, Q, R, Z, F, Tab, M, Ctrl, C, WASD, mouse, 1-4), not rebinds. Home, capture, SL and SR stay where a set has
  no icon.
- Delivery (`runtime/prompts.c`, `fs_host.c` `prompts_aoc_path`): the game asks every add-on content mount for each menu
  file before it reads `bf2.ard`; those probes get the chosen set's file, and a player mod's file for the same path
  still wins. Without add-on content installed there is no mount to ask and the game keeps its own prompts.
- Checked in game (opening battle, New Game then PLUS and A to skip each scene): the tutorial's "press [A]", "press
  [RB]" and "holding down [RB] and pressing [B]" with `prompts = xbox`; ○, R1 and ✕ with `prompts = playstation` and
  `pad_layout = nintendo`; `auto` without a gamepad picks the keyboard set. The game tints text icons with the text
  colour (yellow there). The arts palette's letters were not caught on screen: the battle ended on auto-attacks first.
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
