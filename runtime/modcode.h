// The code of the player's mods (modcode.c): their ExeFS patches and code cheats, compiled at game start into a mod
// library (modcode_abi.h, tools/mod_code.py) whose functions run in place of the game's.
#pragma once

// First in rt_boot, before anything reads the mods' files: runs tools/mod_code.py when an enabled mod has patches or
// cheats the last run did not see, and waits for it.
void modcode_prepare(void);
// 1 when what build-cache holds was made for this build: its status files count (mods.c, cheats.c).
int modcode_current(void);
// After rt_patches_init: loads the library and adds its patch sets (rt_patch_set finds them).
void modcode_load(void);
// A patch set of an enabled mod: its functions get hooked (mods.c, cheats.c, before modcode_hook).
void modcode_use(int set);
// After them, before the guest starts: hooks the functions of the sets in use; the other sets stay off.
void modcode_hook(void);
// Patches and cheats of enabled mods that could not be built: the menu says so. modcode_error says why ("" unknown).
int modcode_unbuilt(void);
const char *modcode_error(void);
