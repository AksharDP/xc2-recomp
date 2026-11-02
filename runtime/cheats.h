// Adapted from totk-pc-recomp runtime/cheats.h @ ab1dee5b (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// Cheats of the mods' Atmosphere cheat files (cheats.c): mods\<mod>\cheats\<build id>.txt of each enabled mod,
// switched from the menu's Cheats tab. A cheat that writes the game's instructions is a code patch set that tools/cheats.py built into the game
// (switched with rt_patch_switch); one that writes data runs in the cheat VM every presented frame.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHEAT_HEADING,       // no opcodes: a title or a group label
    CHEAT_CODE,          // a patch set
    CHEAT_KEY,           // a patch set on while the buttons are held
    CHEAT_DATA,          // the cheat VM
    CHEAT_MASTER,        // {master}: always on
    CHEAT_RESTORE,       // writes the original instructions back: nothing to do
    CHEAT_REBUILD,       // writes instructions, but the game was built without this version of it
    CHEAT_UNSUPPORTED,   // note says why
} CheatKind;

typedef struct {
    const char *name;     // UTF-8, as in the file
    const char *mod;      // the mod's folder name; cheats come grouped by mod, in the Mods tab's order
    const char *note;     // why it is unsupported ("" otherwise)
    CheatKind kind;
    uint32_t buttons;     // CHEAT_KEY: HidNpadButton mask
    char key[24];         // its setting, XCX_CHEAT_<hash of "<mod>/<name>">
} CheatInfo;

void cheats_init(void);                    // after rt_patches_init: reads the mods' files and their build status
int cheats_count(void);
// Code cheats (CHEAT_REBUILD) and ExeFS patch files of enabled mods the build lacks: running the setup again builds them.
int cheats_unbuilt(void);
const CheatInfo *cheats_info(int i);
int cheats_on(int i);                      // the player's switch (1 for a master cheat)
void cheats_set(int i, int on);            // switches at once and sets the cheat's setting (the menu saves xcx.ini)
const char *cheats_file(void);             // "<build id>.txt", the name the files have for this game version
// Every presented frame (the present path): the data cheats' VM and the button state of CHEAT_KEY cheats.
void cheats_frame(uint64_t frame);
// The button names of a mask ("ZL + Right stick left"), into buf.
const char *cheats_buttons(uint32_t mask, char *buf, int n);

#ifdef __cplusplus
}
#endif
