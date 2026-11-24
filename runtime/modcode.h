// The code of the player's mods (modcode.c): their ExeFS patches and code cheats, compiled at game start into a mod
// library (modcode_abi.h, tools/mod_code.py) whose functions run in place of the game's.
#pragma once

// First in rt_boot, before anything reads the mods' files: runs tools/mod_code.py when an enabled mod has patches or
// cheats the last run did not see, and waits for it.
void modcode_prepare(void);
// 1 when what build-cache holds was made for this build: its status files count (mods.c, cheats.c).
int modcode_current(void);
// What build-cache holds is not usable after all: it no longer counts, and the next start makes it again.
void modcode_discard(const char *why);
// After rt_patches_init: loads the library and adds its patch sets (rt_patch_set finds them). A library that does not
// load leaves the mods that need it off as a whole (the mods are scanned again).
void modcode_load(void);
// A patch set of an enabled mod: its functions get hooked (mods.c, cheats.c, before modcode_hook).
void modcode_use(int set);
// After them, before the guest starts: hooks the functions of the sets in use; the other sets stay off.
void modcode_hook(void);
// Patches and cheats of enabled mods that are not built: the menu says so. modcode_error says why, unless mods_build is off
// (modcode_off), so that the game does not build them.
int modcode_unbuilt(void);
const char *modcode_error(void);
int modcode_off(void);
