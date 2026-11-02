# Mods and cheats

XC2 Recomp loads mods made for Switch emulators and Atmosphere: RomFS file replacements, ExeFS code patches (`.ips`,
`.pchtxt`) and Atmosphere cheat files. This page is for players who install mods and for people who make them. The
code is `runtime/mods.c` (the folder scan), `runtime/romfs.c` and `runtime/fs_host.c` (serving files), `runtime/cheats.c`
(cheats) and `tools/cheats.py` / `tools/exefs_patches.py` (code patches and code cheats built into the game).

## Installing a mod

1. Open the `mods` folder next to `xcx.exe`. The Mods tab of the in-game menu (F1) has **Open mods folder**, which
   creates it if it is missing.
2. Unpack each mod into a folder of its own: `mods\<any name>\...`. Keep the layout the mod ships with; all of these
   work:

   | Layout inside the mod's folder | Typical source |
   |---|---|
   | `romfs\`, `exefs\`, `cheats\` | yuzu / Ryujinx style mods (`load\<title id>\<mod>\` on an emulator) |
   | `atmosphere\contents\<title id>\romfs\` (and `exefs\`, `cheats\`) | mods made for the SD card of a Switch |
   | `contents\<title id>\romfs\`, `<title id>\romfs\` | the same, one or two folders shorter |
   | `atmosphere\exefs_patches\<name>\*.ips` | Atmosphere IPS patches |

3. Start the game. The Mods tab lists every folder with its switch and what it serves.

The title ids of Xenoblade Chronicles 2:

| Title id | Content |
|---|---|
| `0100E95004038000` | the base game (also used for the 3.0.1 update) |
| `0100E95004039001` | add-on content pack 1 (the folder XC2 mods use most, X2plus among them) |
| `0100E95004039002` to `...9006` | add-on content packs 2 to 6 |
| `0100E95004039063` | add-on content pack 99 (streamed audio and movies) |

Torna ~ The Golden Country is a separate game and is not supported.

### Order and switches

On the Mods tab, A (or a click, or left / right) switches a mod on or off. The top face button or Ctrl+Up moves it up,
the left face button or Ctrl+Down moves it down. When two mods give the same file, the higher one wins. Changes apply at
the next start. They are kept in `xcx.ini`:

```ini
[mods]
mods_order = Better Battles|X2plus|My Music   # top first, names separated by |
mods_disabled = My Music                      # switched off
# mods_dir = D:\XC2 mods                       # another mods folder (default: mods next to xcx.exe)
```

A folder `mods_order` does not name goes below the named ones, in name order.

## How XC2 finds its files (why mods use add-on content folders)

Almost all of XC2's data is packed in one archive, `bf2.arh` (the index) and `bf2.ard` (the data). Only `stream\`
(music, voices, movies) is loose. When the game needs a file of the archive, it first looks for that file in each
add-on content pack (`aoc1:/bdat/common.bdat`, `aoc2:/...`, up to pack 99), and only then reads it from `bf2.ard`. It
never looks for a loose copy in the base RomFS.

So a mod replaces a file of the archive by putting it in an add-on content title's `romfs` folder:

```
mods\
  My Balance Mod\
    atmosphere\contents\0100E95004039001\romfs\bdat\common.bdat
```

The port serves that file whenever the game asks pack 1 for `bdat/common.bdat`. This is the same thing that happens on
a Switch or an emulator, so mods made for those work unchanged. The base `romfs\` folder of a mod is still used for
files the game opens loose: `stream\bgm\*.nop` (music), `stream\voice\...`, `stream\movie\*.webm`, and a complete
replacement of `bf2.arh` / `bf2.ard`. A file such as `romfs\bdat\common.bdat` under the base title is ignored by the
game, as on an emulator.

The add-on content packs you own must be installed for their mod folders to be read (the setup installs every pack you
give it). Paths are matched without regard to upper and lower case.

## Cheats

A mod's cheats are an Atmosphere cheat file in its `cheats` folder, named after the first 16 hex digits of the game's
build id. For XC2 3.0.1 that is:

```
mods\<mod>\cheats\4F5C96E2D97F45AC.txt
```

The file name's case does not matter. Files for other game versions (another build id) are not read. Cheats appear on
the Cheats tab, grouped by mod, and are off until you switch them on; each switch is kept in `xcx.ini` under `[cheats]`
(`cheat_<hash> = 1`). What each kind of cheat does here:

| Cheat | Effect |
|---|---|
| writes memory (heap, game data, pointer chains, button conditions) | runs in the cheat VM every frame while on, as on Atmosphere |
| changes the game's code (opcode 0 into main's code) | built into the game by the setup; switches at once |
| changes code while buttons are held (`8...` / `21000000` / `20000000`) | built in; on while the buttons are held |
| `{master}` code | always on |

The game's code is compiled C, not ARM instructions, so a code cheat cannot be written into memory as it runs. The setup
compiles every code cheat of the mods folder into the game when it builds it. A code cheat added or edited after that is
listed as waiting for the setup: run the setup again on the same folder (it keeps your saves and settings) and it
becomes switchable. Extended opcodes `C...` and `F...` are not supported and are listed with the reason.

## Code patches (ExeFS)

`.pchtxt` (IPSwitch) and `.ips` / IPS32 files in a mod's `exefs` folder, or `atmosphere\exefs_patches\<name>\*.ips`,
patch the game's main program. A file is used when it is for this game version:

- a `.pchtxt` file's `@nsobid-` line names main's build id, `4F5C96E2D97F45AC288BC67613EBEF0A` for 3.0.1 (trailing zeros
  may be left out);
- an `.ips` file's name is that build id.

Like code cheats, code patches are built into the game by the setup: after adding or changing one, run the setup again
on the install folder. Until then the whole mod stays off (its other files may expect the patch), and the Mods tab says
so. A patch file the port cannot apply makes the whole mod stay off with the reason shown, as does a mod that ships its
own code modules (`exefs\subsdk*` plugins or a replaced `exefs\main`): the port has no loader for those.

Patches made for 2.0.0 or other versions carry another build id and are listed as made for another version. Their
offsets do not match 3.0.1's code, so they cannot simply be renamed.

## Making a mod

### Replacing or adding a file

1. Find which file you want to change and where the game looks for it. `XCX_TRACE=fs` (environment variable, or
   `trace = fs` under `[diagnostics]` in `xcx.ini`) writes every file the game opens to the log, for example
   `read aoc1:/bdat/common.bdat` followed by reads of `bf2.ard`.
2. Get the original. Loose files (`stream\`) are in the installed `romfs\romfs.bin`; archived files are inside
   `bf2.arh` / `bf2.ard` and need an XC2 archive extractor from the modding community. Add-on content files are in the
   install's `aoc\<pack>\` folders.
3. Put your file at the same path under `mods\<your mod>\atmosphere\contents\<title id>\romfs\`, with the add-on content
   title id for archived files and `0100E95004038000` for loose ones (or `romfs\` directly for loose ones).
4. Restart the game and check the log: `mods: <your mod>: on, ...` lists what was found, and with `XCX_TRACE=fs`
   `read from a mod <path>` (add-on content) or `rom from an overlay <path>` (base RomFS) shows the game reading your
   file.

### Writing a code patch

An IPSwitch file for 3.0.1:

```
@nsobid-4F5C96E2D97F45AC288BC67613EBEF0A
@flag offset_shift 0x100
@enabled
// <offset in main> <bytes in memory order>
00708B4C 81020091
@stop
```

With `offset_shift 0x100` offsets are main's own; without it they count the 0x100-byte NSO header, as IPS files do. The
setup turns each instruction you replace into a switchable alternative in the compiled code. Rules (the setup reports a
file that breaks one as unsupported):

- only main's code can be patched, not its data;
- the replaced instruction may be anything but `br`, `blr` or `ret`;
- a replacement may end in a branch (`b`, `bl`, `b.cond`, `cbz`, `tbz`, `ret`); earlier words of it may not branch;
- a replacement must set the CPU flags the way the original does;
- a branch into a code cave the file writes into main's zero padding is inlined at the branch, without branches or
  pc-relative instructions inside the cave.

## When something does not work

The log (Advanced tab, **Open log folder**) has a line per mod at start:

| Log line | Meaning |
|---|---|
| `mods: <name>: on, romfs aoc 1, ok` | used: base RomFS files and add-on content pack 1 files |
| `... empty` | no `romfs`, `exefs` or `cheats` files in the places listed above (check the folder depth) |
| `... ExeFS patches the game was built without (run the setup again)` | run the setup on the install folder |
| `... its ExeFS patches are for another game version` | the patch is not for 3.0.1 |
| `... has a code module (subsdk or main), not supported` | the mod needs a code plugin the port cannot load |
| `mods: <name>: N add-on content files (...), M overridden by <other>` | a higher mod gives M of the same files |
| `cheats: ... N wait for a rebuild` | code cheats new since the build: run the setup again |

A developer build without the setup builds code patches and cheats by hand; the commands are in
[NOTES.md](../NOTES.md) ("Mods and cheats").
