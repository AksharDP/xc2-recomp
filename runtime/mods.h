// Adapted from totk-pc-recomp runtime/mods.h @ ab1dee5b (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// Player mods (mods.c): one folder per mod under mods/ next to xc2.exe (XC2_MODS_DIR), unpacked as emulators take
// them: <mod>/romfs, <mod>/exefs and <mod>/cheats, or the Atmosphere layout (atmosphere/contents/<title id>/romfs,
// atmosphere/exefs_patches/<name>/*.ips). The RomFS of an add-on content title (<title id> 0100E950040390NN, in Japan
// 0100F3400332D0NN, pack NN in hex) goes over that pack: XC2 looks a file up in every add-on content pack before its
// base archive, which is how XC2 mods replace files inside bf2.arh / bf2.ard. ExeFS patch files (.ips, .pchtxt) for the game's main are patch sets the
// setup builds into the game (tools/exefs_patches.py), switched with their mod (mods_exefs_switch). The list is scanned
// once at start (romfs.c overlay_init); its order and switches live in xc2.ini [mods] (XC2_MODS_ORDER: names top
// first, separated by '|'; XC2_MODS_DISABLED: names switched off). The top mod wins a file two mods replace.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MOD_ROMFS = 1, MOD_EXEFS = 2, MOD_SUBSDK = 4, MOD_CHEATS = 8, MOD_AOC = 16 };   // ModInfo.kind bits
enum { MOD_AOC_MAX = 8 };   // add-on content RomFS roots one mod may have (XC2 has seven packs)
typedef enum {
    MOD_OK,
    MOD_EMPTY,          // no RomFS file, no ExeFS patch, no cheat file
    MOD_PLUGIN,         // has code modules (exefs/subsdk*, a replaced exefs/main): the port has no module loader, so the
                        // whole mod stays off (its RomFS files may expect that code)
    MOD_EXEFS_LATER,    // has ExeFS patches for this game's main that the game was built without: running the setup
                        // again builds them in; the whole mod stays off until then
    MOD_EXEFS_UNSUPPORTED,   // an ExeFS patch file the port cannot apply (note says why): the whole mod stays off
    MOD_EXEFS_OTHER,    // only ExeFS patches, all for another game version
    MOD_BAD_NAME,       // its folder name holds '|', the separator of the order keys
} ModProblem;

typedef struct {
    char name[256];        // folder name, UTF-8
    char romfs[2048];      // UTF-8 path of its base RomFS root; "" none
    char exefs[2048];      // UTF-8 path of its ExeFS folder; "" none
    char cheats[2048];     // UTF-8 path of its cheats folder; "" none
    int naoc;
    struct { int index; char root[2048]; } aoc[MOD_AOC_MAX];   // add-on content RomFS roots (index: the pack)
    int kind, enabled;
    ModProblem problem;
    uint32_t replaced, added, overridden;   // romfs.c: base RomFS files served (replacing / added); files a higher mod
                                            // serves (base and add-on content)
    uint32_t aoc_replaced, aoc_added;       // mods.c: add-on content files served (replacing a pack's own / added)
    uint32_t other_version;                 // ExeFS patches for another build, not used
    char overridden_by[256];                // the first higher mod that took one of its files
    char note[256];                         // MOD_EXEFS_UNSUPPORTED: why
    uint32_t pending, pending_key;          // ExeFS patch files the build lacks; FNV-1a of their names and contents
    int nsets;
    uint32_t sets[16];                      // the built patch sets mod_<hash> of its ExeFS patch files
    char overrides[256];                    // the port's own patch sets its patches replace, comma separated
} ModInfo;

void mods_scan(void);              // once; later calls return at once
const char *mods_dir(void);        // the folder scanned (default: <dir of xc2.exe>/mods)
int mods_count(void);
ModInfo *mods_at(int i);           // 0 = top of the order; NULL out of range
// 1 when mod i was on and usable at start: its files are served and its patches switched on until the next start.
int mods_started(int i);
// Menu edits (present path). They change the list mods_at shows and write XC2_MODS_ORDER / XC2_MODS_DISABLED with
// cfg_set; romfs.c used the start state, so they apply at the next start.
void mods_set_enabled(int i, int on);
void mods_move(int i, int delta);  // -1 up, +1 down; nothing at the ends
int mods_changed(void);            // 1 when the order or a switch differs from the start
// After rt_patches_init: the patch sets of the enabled mods' ExeFS patches on, the port's own sets they replace off.
void mods_exefs_switch(void);
// The ExeFS patch files of the enabled mods that the build lacks, their FNV-1a mixed into *key.
int mods_exefs_pending(uint32_t *key);
// The hex of the game main's build id (64 digits), "" when exefs/main cannot be read.
const char *mods_main_build_id(void);
// The host file that add-on content pack index serves at its RomFS path rel (no leading '/'), from the highest enabled
// mod that has one, into out; 0 when no mod has it. dir: rel names a folder some mod has files under (*is_dir = 1).
int mods_aoc_file(int index, const char *rel, char *out, size_t cap, int *is_dir);

#ifdef __cplusplus
}
#endif
