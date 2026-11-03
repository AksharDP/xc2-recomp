# Mods and cheats

XC2 Recomp loads the same mods as Switch emulators and Atmosphere: replaced game files (RomFS), code patches (`.ips`,
`.pchtxt`) and Atmosphere cheat files.

## Install a mod

1. Open the `mods` folder next to `xc2.exe`. In game, press F1, go to the Mods tab and choose **Open mods folder**. The
   game creates the folder if it does not exist.
2. Unpack each mod into its own folder, `mods\<any name>\`. Keep the folders the mod came with. These layouts all work:

   | Inside the mod's folder | Where it comes from |
   |---|---|
   | `romfs\`, `exefs\`, `cheats\` | yuzu and Ryujinx mods (`load\<title id>\<mod>\` on the emulator) |
   | `atmosphere\contents\<title id>\romfs\` (also `exefs\`, `cheats\`) | mods for a Switch SD card |
   | `contents\<title id>\romfs\` or `<title id>\romfs\` | SD card mods unpacked one or two levels down |
   | `atmosphere\exefs_patches\<name>\*.ips` | Atmosphere IPS patches |

3. Start the game. The Mods tab lists each folder, its switch and what it changes.

XC2's title ids:

| Title id | What it is |
|---|---|
| `0100E95004038000` | the base game, including the 3.0.1 update |
| `0100E95004039001` | add-on content pack 1, the one most XC2 mods use (X2plus, for example) |
| `0100E95004039002` to `...9006` | add-on content packs 2 to 6 |
| `0100E95004039063` | add-on content pack 99: streamed audio and movies |

Torna ~ The Golden Country is a separate game. It is not supported.

### Order and switches

On the Mods tab:

- A, a click, or left / right turns a mod on or off.
- The top face button or Ctrl+Up moves a mod up. The left face button or Ctrl+Down moves it down.
- If two mods change the same file, the higher one wins.
- Changes take effect the next time you start the game.

The menu saves the list to `xc2.ini`:

```ini
[mods]
mods_order = Better Battles|X2plus|My Music   # top first, names separated by |
mods_disabled = My Music                      # turned off
# mods_dir = D:\XC2 mods                       # a different mods folder (default: mods next to xc2.exe)
```

Folders missing from `mods_order` go below the listed ones, sorted by name.

## Where XC2 looks for files

XC2 packs nearly all its data into one archive: `bf2.arh` is the index and `bf2.ard` holds the data. Only `stream\`
(music, voices, movies) sits outside it as loose files.

When the game needs a file from the archive, it checks each add-on content pack first (`aoc1:/bdat/common.bdat`,
`aoc2:/...`, up to pack 99). If no pack has the file, it reads it from `bf2.ard`. It never checks the base game's
`romfs` for a loose copy.

That is why XC2 mods put replacement files in an add-on content title's `romfs` folder:

```
mods\
  My Balance Mod\
    atmosphere\contents\0100E95004039001\romfs\bdat\common.bdat
```

When the game asks pack 1 for `bdat/common.bdat`, it gets this file. A Switch and the emulators behave the same way, so
mods made for them work without changes.

A mod's base `romfs\` folder works for files the game opens directly:

- music: `stream\bgm\*.nop`
- voices: `stream\voice\...`
- movies: `stream\movie\*.webm`
- a full replacement of `bf2.arh` and `bf2.ard`

The game never asks for something like `romfs\bdat\common.bdat` under the base title, so a file there does nothing. The
emulators behave the same.

A pack's mod files load only if you installed that pack. The setup installs every pack you give it. Upper and lower case
in paths do not matter.

## Cheats

Put the Atmosphere cheat file in the mod's `cheats` folder. Its name is the first 16 hex digits of the game's build id.
For 3.0.1:

```
mods\<mod>\cheats\4F5C96E2D97F45AC.txt
```

Upper or lower case in the name does not matter. Files for other game versions have another build id and are not read.

The Cheats tab lists the cheats by mod. Every cheat starts off. Your choices are saved in `xc2.ini` under `[cheats]` as
`cheat_<hash> = 1`.

| Kind of cheat | What happens |
|---|---|
| writes memory (heap, game data, pointer chains, button conditions) | runs every frame while on, as on Atmosphere |
| changes the game's code (opcode 0 into main's code) | built into the game by the setup; turns on and off at once |
| changes code while you hold buttons (`8...` / `21000000` / `20000000`) | built in; active while the buttons are held |
| `{master}` code | always on |

The port runs the game as compiled C, not as ARM instructions, so a cheat cannot rewrite code while the game runs.
Instead, the setup compiles every code cheat in the mods folder into the game. If you add or edit a code cheat later,
the Cheats tab marks it as waiting. Run the setup again on the same folder to build it in. Your saves and settings stay.

Extended opcodes (`C...`, `F...`) are not supported. The Cheats tab shows them greyed out with the reason.

## Code patches (ExeFS)

The game reads code patches from a mod's `exefs` folder (`.pchtxt`, `.ips`, IPS32) and from
`atmosphere\exefs_patches\<name>\*.ips`. A patch is used only if it targets this game version:

- In a `.pchtxt` file, the `@nsobid-` line must name main's build id. For 3.0.1 that is
  `4F5C96E2D97F45AC288BC67613EBEF0A`. Trailing zeros can be left out.
- An `.ips` file must be named after that build id.

The setup builds code patches into the game, the same way as code cheats. After you add or change a patch, run the setup
again. Until you do, the whole mod stays off, because its other files may depend on the patch. The Mods tab tells you
when this happens.

The whole mod also stays off when:

- the port cannot apply one of its patch files (the Mods tab says why), or
- it brings its own code modules (`exefs\subsdk*` plugins or a replaced `exefs\main`). The port cannot load those.

Patches for 2.0.0 or any other version have a different build id, and the Mods tab lists them as made for another
version. Renaming them does not help: their offsets point at different code in 3.0.1.

## Make a mod

### Replace or add a file

1. Find the file and where the game looks for it. Set `XC2_TRACE=fs` as an environment variable, or `trace = fs` under
   `[diagnostics]` in `xc2.ini`. The log then lists every file the game opens, for example `read aoc1:/bdat/common.bdat`
   followed by reads of `bf2.ard`.
2. Get the original file:
   - Loose files (`stream\`) are in the installed `romfs\romfs.bin`.
   - Archived files are inside `bf2.arh` / `bf2.ard`. You need an XC2 archive extractor from the modding community.
   - Add-on content files are in the install's `aoc\<pack>\` folders.
3. Put your file at the same path under `mods\<your mod>\atmosphere\contents\<title id>\romfs\`. For an archived file,
   use an add-on content title id. For a loose file, use `0100E95004038000`, or put it straight under `romfs\`.
4. Restart the game and read the log. `mods: <your mod>: on, ...` lists what the game found. With `XC2_TRACE=fs`, the
   line `read from a mod <path>` (add-on content) or `rom from an overlay <path>` (base game) shows the game reading
   your file.

### Write a code patch

An IPSwitch file for 3.0.1:

```
@nsobid-4F5C96E2D97F45AC288BC67613EBEF0A
@flag offset_shift 0x100
@enabled
// <offset in main> <bytes in memory order>
00708B4C 81020091
@stop
```

`offset_shift 0x100` makes offsets count from the start of main. Without it, offsets include the 0x100-byte NSO header,
as in IPS files.

The setup turns each replaced instruction into a switchable alternative in the compiled code. A patch must follow these
rules, or the setup marks the file unsupported:

- Patch main's code only, not its data.
- Do not replace a `br`, `blr` or `ret` instruction.
- A replacement may end in a branch (`b`, `bl`, `b.cond`, `cbz`, `tbz`, `ret`). Only its last instruction may branch.
- A replacement must set the CPU flags the same way as the original.
- A branch into a code cave (code the file writes into main's zero padding) is copied in place of the branch. The cave
  itself may not contain branches or pc-relative instructions.

## Troubleshooting

To find the log, open the Advanced tab and choose **Open log folder**. At start the log has one line per mod:

| Log line | Meaning |
|---|---|
| `mods: <name>: on, romfs aoc 1, ok` | in use: base game files and add-on content pack 1 files |
| `... empty` | no `romfs`, `exefs` or `cheats` files where the game looks. Check the folder depth. |
| `... ExeFS patches the game was built without (run the setup again)` | run the setup on the install folder |
| `... its ExeFS patches are for another game version` | the patch is not for 3.0.1 |
| `... has a code module (subsdk or main), not supported` | the mod needs a code plugin the port cannot load |
| `mods: <name>: N add-on content files (...), M overridden by <other>` | a higher mod replaces M of these files |
| `cheats: ... N wait for a rebuild` | code cheats added since the last build: run the setup again |

## For developers

The mod loader is in `runtime/mods.c` (finding mods), `runtime/romfs.c` and `runtime/fs_host.c` (serving their files)
and `runtime/cheats.c` (cheats). `tools/cheats.py` and `tools/exefs_patches.py` build code cheats and patches into the
game. To build them without the setup, use the commands shown above in this file.
