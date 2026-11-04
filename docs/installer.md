# Installer

A public release that turns the player's own *Xenoblade Chronicles 2* files into a playable folder. The release
carries no game data and no code derived from the game: the installer builds the game on the player's machine. The
installed folder holds only the game; the installer and its tools stay outside it. It is XCX Recomp's installer with
XC2's inputs; file names inside the game folder (`xc2.exe`, `xc2.ini`) are XC2 Recomp's own, not XCX Recomp's.

## What the player does

1. Downloads `XC2-Setup-<version>-windows.zip` (none is published), unpacks it anywhere and starts `XC2-Setup.exe`.
2. Picks:
   - the base game (XCI or NSP), the 3.0.1 update (NSP) and, optionally, the add-on content NSPs (the Expansion Pass
     packs 1 to 6 and *Torna ~ The Golden Country*, title ids 0100E95004039001..9006 and 0100E95004039063), of
     either release ([Releases](#releases));
   - their `prod.keys`;
   - the game language and the install folder.
3. Waits for the setup, one time. The window shows each step and its progress.
4. Plays `xc2.exe` from the install folder. The installer and the source NSP or XCI can be deleted.

The install folder (`main.c` `portable_paths()` reads this layout; it stays portable):

```
<install folder>/
  xc2.exe, SDL3.dll, libopus-0.dll, amd_fidelityfx_vk.dll, xc2_xlate.exe
  xc2.ini                       language; the other keys as commented defaults (xc2.ini.example)
  exefs/, romfs/                 extracted from the player's files (base merged with 3.0.1; RomFS about 13 GiB)
  aoc/<index>/                   each add-on's RomFS (about 4.6 GiB for all seven; packs 5 and 6 hold only a key file)
  profile/                       saves
  logs/                          xc2.log of the last run, xc2.1.log to xc2.4.log of the runs before it
  shader and pipeline caches     prepared by the setup
  build-cache/                   what the build has of the player's mods; objects/: the last build's generated objects
  xc2-install.json              what built this folder: release version, input hashes, finished steps
```

## Releases

Nintendo published XC2 under two titles (`tools/xc2_titles.py`):

| Release | Base | Update | Add-on content | Expected hashes | Build name |
|---|---|---|---|---|---|
| The Americas and Europe | 0100E95004038000 | 0100E95004038800 | 0100E9500403900N, ...9063 | `expected-xc2-<version>.json` | v301 |
| Japan | 0100F3400332C000 | 0100F3400332C800 | 0100F3400332D00N | `expected-xc2-jp-<version>.json` | v301jp |

A European dump is the American title: the same base game, update and add-ons, checked against the same hashes. The
Japanese release has its own code and data. The check takes the release from the base game's title id, then wants the
update and every add-on of that release (`check.region_mismatch`, `check.dlc_other_region`) and stops with
`check.region_unsupported` while `tools/data` has no expected files for it; `tools/extract.py --region jp --record
VERSION` writes them from a dump (docs/building.md). The updates a release builds are the ones its expected files
name (`installer/layout.py` `Layout.versions`), so its numbering may differ from 3.0.1.

Each release installs into its own folder, so both can sit side by side: `xc2-install.json` records the release
(`region`), the build bakes its title id into `xc2.exe` (`rt_title_id` in `modules.c`: the save data and program id
the game sees, and the title id folders `runtime/mods.c` looks in), and the code patch table is the release's
(`recomp/patches-v301jp.txt` for Japan; without one the game runs at the console's 30 fps and 1280x720, since the
runtime's frame rate and resolution code reads `main`'s data only with those patch sets). Mods may be laid out under
either title id.

## Updating the port

The player runs a newer installer on the same folder. It reads `xc2-install.json`, keeps `romfs/`, `exefs/`, `aoc/`,
`profile/` and `xc2.ini`, rebuilds `xc2.exe` and re-prepares the shaders. A newer release that changes nothing
game-derived skips the rebuild. Add-on content can be added later: only the add-on step runs again.

What keeps an update short:

- The RomFS is kept by the size and modification time recorded when the merge finished (the merge hashed every byte it
  wrote); the ExeFS modules are re-hashed (small). `--verify` reads the whole RomFS again.
- `build-cache/objects/` in the install folder holds the objects of the last build (`tools/build_game.py`
  `ObjectCache`, about 2 GB). The compile reuses every object whose generated C, compiler, flags and included headers
  are unchanged, so a release that changes a few lifted functions or only the runtime library compiles a few chunks
  or none and relinks. It lives in the install folder because each release folder has its own temp folder. Deleting
  it only costs a full compile on the next update.

## Setup steps

1. **Check** before writing anything: the files open with the keys, the base game, the update and the add-ons are one
   release's ([Releases](#releases)), the base game and the update are 1.0.0 and 3.0.1 (by the hashes in
   `tools/data/expected-xc2[-jp]-<version>.json`), the keys are new enough for the update, enough space
   is free on the target disk, the CPU has AVX2 (`-march=x86-64-v3`), a Vulkan 1.3 GPU exists. Each failure is one
   clear message.
2. **Extract** ExeFS and RomFS into the install folder and verify the hashes. `tools/xc2_merge_romfs.py` applies
   the update's BKTR patch while reading both game files in place: it writes the 14 GB RomFS once, decrypting on
   several threads and hashing as it writes, so the folder needs room for the RomFS only.
3. **Add-on content**: each add-on NSP's RomFS into `aoc/<index>/` (`tools/xc2_extract.py` `dlc_romfs`; add-on NCAs
   are encrypted with the title key from the NSP's ticket), up to three packs at once in processes of their own
   (`installer/steps.py` `DLC_JOBS`). Each pack's log is `logs/dlc-<index>.log` in the temp folder.
4. **Lift** the ExeFS to C (`recomp/gen.py`) into a temp folder next to the installer. A code patch table
   `recomp/patches-v301.txt` is passed when it exists (XC2's frame rate and game resolution). `patches-upd.txt` and the `v121`/`v143`
   tables are XCX's and never used.
5. **Compile** the generated C, `modules.c`, `romfs.c`, `patches.c` and `ov_zstd.c` (they bake the per-install layout
   and RomFS hashes) and link them with the prebuilt runtime library into `xc2.exe` (`build_game.py --runtime-lib`).
   Generated objects come from `build-cache/objects/` where they are current, and the new ones go into it
   (`build_game.py --object-cache`).
6. **Prepare**: puts the shader translator beside `xc2.exe`. XC2 has no shader list to translate ahead of play yet, so
   shaders are translated on first use and cached in `shader-cache/`.
7. **Clean up** the temp folder (generated C and objects).

A full first install from the base game, 3.0.1 and all seven add-ons took about 8 minutes on a 24-core machine.

A cancelled or failed setup resumes from the last finished step (`xc2-install.json`).

## Command line

`installer/` (stdlib only; `installer/layout.py` maps the release folder and, in a repository, the dev toolchain):

```
python -m installer --base FILE --update FILE --keys FILE [--dlc FILE ...] --lang CODE --dest DIR [--root DIR] [--temp DIR] [--verify]
```

In a repository checkout (dev mode, no release folder) the installer borrows Python, llvm-mingw, Ninja and
the translator from an unpacked XCX-Setup release: `XC2_SETUP_TOOLS`, else the newest `../tools/xcx-setup/XCX-Setup-*`
beside the repository (`installer/layout.py` `tools_fallback`). It needs the runtime library built once:
`tools/build_game.py --make-runtime-lib --out build/install-rtlib --expected tools/data/expected-xc2-3.0.1.json`
with that release's `--toolchain` and `--ninja`.

- On a folder with `xc2-install.json` only `--dest` is needed; the game files only when the extraction must run again.
- stdout: one JSON event per line, types `start`, `plan`, `step` (started, done, skipped), `progress` (step, phase,
  done/total/unit), `space` (bytes needed and free on the install disk and the temp disk), `warning`, `error`, `end` (`installer/events.py`). Messages are ids with parameters;
  `installer/text/en.json` holds their English text. The human-readable log goes to `<temp>/logs/`.
- Exit codes: 0 done, 1 failed, 2 bad command line, 3 cancelled. The window cancels with Ctrl+Break to the
  installer's process group.
- Each step records a key over its inputs (runner.py). A step with the recorded key and intact output is skipped, so
  resume and update are one rule; output kept from another release is re-hashed first.
- Peak space on the install disk is well above the finished folder's: the extraction copies both Program NCAs next to
  the merged RomFS before it deletes them. The check computes the exact figure.
- Window (`python -m installer.gui`, `installer/gui.py` draws, `installer/guilogic.py` decides): form, step list with
  progress, space line, collapsible log, Cancel (Ctrl+Break to the CLI's process group, then it waits for the exit),
  finish screen with Open folder. English or Russian, chosen from the Windows display language, switchable in the
  window; every sentence is an id in `installer/text/<en|ru>.json`. A folder with `xc2-install.json` needs only the
  folder. `--selftest [--shots DIR]` builds every screen in both languages and closes itself.
- `tests/installer_gui_test.py`: event model, form, catalogs, cancel, the self-closing window and one end-to-end run
  of the CLI in `--verify` mode (needs `build/install-test` and an extracted game; see its docstring).
- `tests/installer_test.py`: bookkeeping with fake steps. `tests/installer_e2e.py`: a dev-mode run with the files of
  `config.local.toml`.

## Release layout

The unpacked release; `tools/make_release.py` assembles it and `--zip` packs it:

```
XC2-Setup.exe        starts python/pythonw.exe -m installer.gui (installer/launcher/launcher.c, no console)
installer/            the installer: steps, CLI, window, en/ru text catalog
python/               Python 3.14.9 embeddable + numpy, lz4, cryptography, cffi, pycparser (+ tkinter for the window)
recomp/, tools/       the lifter and the build, extraction and shader scripts the steps run (copied from the repo)
llvm/                 llvm-mingw, x86_64-only subset (Apache-2.0 with LLVM exception; mingw-w64 parts permissive)
runtime/              libxc2rt.a and the headers the generated code and modules.c/romfs.c/patches.c include
bin/                  xc2_xlate.exe, SDL3.dll, libopus-0.dll, ninja.exe
THIRD_PARTY_NOTICES.md
xc2.ini.example, VERSION  the default ini; the release string (the repository's VERSION plus the commit)
```

- SDL3 comes from the upstream release, not MSYS2: MSYS2's DLL imports the LGPL libiconv.
- Every bundled third-party piece gets a row in `THIRD_PARTY_NOTICES.md`.
- The release contains nothing game-derived; `make_release.py` runs `tools/check_content.py` over the folder before it
  finishes.

## Linux release

Every tagged release carries the AppImage next to the Windows zip (`.github/workflows/release.yml`, job `linux`).
`--prove` builds the game from `generated/v301` with the folder's tools only.

`tools/make_release_linux.py` builds the same setup for Linux and packs it as `XC2-Setup-<version>-linux.AppImage` (the static
type-2 runtime: no libfuse2 needed on the host). The player makes it executable and starts it; on a Steam Deck, from
Desktop Mode. Building the release also needs `make` and `pkg-config`. The bundled Tcl/Tk 9.0.4 is rebuilt with
Xft against the sniper sysroot so Russian text uses system TrueType fonts. The setup builds the game into the folder they pick, as on Windows, and the game is `xc2` there with
`libSDL3.so.0` beside it. The folder inside the AppImage:

```
AppRun                starts python/bin/python3 -m installer.gui (no bytecode written into the read-only mount)
xc2-setup.desktop, xc2-setup.png
installer/, recomp/, tools/, data/, licenses/, THIRD_PARTY_NOTICES.md, xc2.ini.example, VERSION   as on Windows
python/               python-build-standalone 3.14 (with tkinter) + the installer's packages; a .pth puts the folder,
                      tools/ and recomp/ on sys.path
llvm/                 LLVM 23 (clang, lld, llvm-ar) and llvm/sysroot: the files of the Steam Runtime 3 "sniper" SDK the
                      game's compile and link read (about 130 files)
runtime/              libxc2rt.a, headers, the install-side sources, xc2.png; deps/ with the Vulkan headers, VMA and a
                      static zstd
bin/                  xc2_xlate, ninja, libSDL3.so.0
```

- What we compile is built against the sniper sysroot (glibc 2.31): the game, hostcheck and the translator
  need no newer glibc. The game links Opus, libvpx, zstd and libstdc++ statically and needs only glibc, the system's
  Vulkan loader and the SDL3 beside it. The setup itself needs glibc 2.34 (Ubuntu 22.04, Debian 12, Fedora 35,
  SteamOS 3): the official LLVM binaries and the `cryptography` wheel are built against it. `make_release_linux.py`
  checks every ELF file in the folder against that floor (`check_glibc`), and the setup's check step stops with one
  message on an older system.
- The setup's temp folder is `.setup-temp` inside the install folder (`--temp` changes it): the AppImage is mounted
  read-only, and a Steam Deck's `/home` is often too full for the generated C while the game goes to the SD card.
- The game keeps Mesa's shader cache in `mesa-cache` beside `xc2` unless `MESA_SHADER_CACHE_DIR` is set, for the
  same reason: RADV fills its default folder in `~/.cache` up to 1 GB.
- The window follows `LANG` for English or Russian, cancels through SIGTERM to the CLI's process group (the CLI handles
  it as Ctrl+Break on Windows) and opens the finished folder with `xdg-open`.
- Requirements beyond Windows': Linux 6.7 or later (the write watch, `docs/building.md`) and glibc 2.34.
- Building either release needs no game files: the runtime library takes main's base and the module limit from
  `runtime/dispatch.h` (`RT_MAIN_BASE`, `RT_MODULES_MAX`) instead of the generated `layout.h`, and each install's
  `modules.c` checks them against its layout. `.github/workflows/release.yml` builds both releases from a bare
  checkout when a `v<VERSION>` tag is pushed. Only `--prove` reads `generated/v301` and `build/extracted/v301`.

## Constraints

- The installer steps contain no Win32 calls: Linux runs the same steps with its own compiler ("Linux release").
- The keys file path is passed to the tools; key values never appear in the UI, logs or files.
- The window is a front end over the CLI (`python -m installer --base ... --update ... --keys ... --lang ... --dest
  ...`), which prints one JSON progress event per line. Tests and CI use the CLI.
- Paths may hold any Unicode characters (a Cyrillic user name puts the temp and install folders under it). Every exe
  the setup runs or builds reads its arguments and paths as UTF-8 through an `activeCodePage` manifest: xc2.exe (`runtime/xc2.rc`), ninja (upstream). The Python steps write
  their files and logs as UTF-8, and `build_game.py` quotes paths with spaces on its Ninja command lines.
- A game file whose data fails its hash checks or ends early stops the setup with `check.input_damaged`, which names
  the file and asks to copy it again (`extract.DamagedFile`). Merged game data whose
  checksum differs from the expected one stops it with `extract.romfs_mismatch`; copy both files again then.
- Unsigned exe: SmartScreen warns on first start.

## Later

- Re-warm the driver cache on start after a move, GPU or driver change.
- NSZ and XCZ input.
