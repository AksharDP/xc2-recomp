// Adapted from totk-pc-recomp runtime/mods_probe.c @ ab1dee5b (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// --probe-mods: the mods folder scan, its order and switches, and the add-on content files of the mods (mods.c).
// Builds a mods folder of its own next to xc2.exe, scans it and removes it. Needs no game files.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL mods: %s\n", what); } } while (0)
#define EQ(a, b) ((a) && !strcmp((a), (b)))

// root/rel as a file of a few bytes (rel ends in '/': a folder), with every folder above it.
static void put(const char *root, const char *rel) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    char *slash = strrchr(p, '/');
    *slash = 0;
    plat_file_mkdir_p(p);
    *slash = '/';
    if (slash[1]) plat_file_write(p, "probe", 5);
}

static int remove_cb(const char *name, int kind, uint64_t size, void *ctx);
static void remove_tree(const char *dir) {
    plat_fs_list(dir, remove_cb, (void *)dir);
    plat_fs_rmdir(dir);
}
static int remove_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    char sub[2048];
    snprintf(sub, sizeof sub, "%s/%s", (const char *)ctx, name);
    if (kind == PLAT_FS_DIR) remove_tree(sub);
    else plat_fs_unlink(sub);
    return 1;
}

// s ends in tail, either path separator matching either.
static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    if (n < t) return 0;
    for (size_t i = 0; i < t; i++) {
        char a = s[n - t + i], b = tail[i];
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a != b) return 0;
    }
    return 1;
}

// The add-on content file mods_aoc_file gives for pack index and path rel ends in tail ("" for a folder, NULL for
// none).
static int aoc_is(int index, const char *rel, const char *tail) {
    char out[2048];
    int dir = -1;
    int found = mods_aoc_file(index, rel, out, sizeof out, &dir);
    if (!tail) return !found;
    if (!found) return 0;
    return *tail ? !dir && ends_with(out, tail) : dir == 1;
}

int probe_mods(void) {
    char root[1024];
    if (!plat_exe_path(root, sizeof root)) return 1;
    char *cut = plat_last_sep(root);
    if (!cut) return 1;
    strcpy(cut + 1, "mods-probe");
    remove_tree(root);
    plat_file_mkdir_p(root);
    static const char *const files[] = {
        "A/romfs/x.bin", "A/0100E95004039001/romfs/bdat/common.bdat",
        "B/atmosphere/contents/0100E95004038000/romfs/x.bin",
        "C/contents/0100E95004038000/romfs/y.bin", "C/contents/0100E95004039001/romfs/bdat/c.bdat",
        "D/exefs/main.pchtxt", "E/exefs/subsdk9", "F/romfs/",
        "G/romfs/z.bin", "G/exefs/1234.ips", "Ultra \xD0\x96/romfs/u.bin",
        "H/romfs/h.bin", "H/exefs/subsdk9", "I/romfs/i.bin", "I/exefs/main", "J/cheats/0123456789ABCDEF.txt",
        "K/atmosphere/contents/0100E95004039001/romfs/bdat/common.bdat",
        "K/atmosphere/contents/0100E95004039001/romfs/bdat/gb/common_ms.bdat",
        "K/0100e95004039063/romfs/stream/a.wav", "K/contents/0100E95004039009/notromfs/x.bin",
        "L/romfs/l.bin", "L/cheats/0123456789ABCDEF.txt", "L/exefs/main.npdm",
    };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(root, files[i]);
    // an IPS file for this game's main (any file counts as one when main cannot be read), in Atmosphere's layout
    char ips[160];
    snprintf(ips, sizeof ips, "M/atmosphere/exefs_patches/x/%s.ips", *mods_main_build_id() ? mods_main_build_id() : "0");
    put(root, ips);

    cfg_set("XC2_MODS_DIR", root);
    cfg_set("XC2_MODS_ORDER", "B|A|Gone");
    cfg_set("XC2_MODS_DISABLED", "C");
    mods_scan();

    static const char *const order[] = {"B", "A", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                        "Ultra \xD0\x96"};
    enum { N = sizeof order / sizeof *order };
    CHECK(mods_count() == N, "fourteen mod folders");
    int in_order = mods_count() == N;
    for (int i = 0; in_order && i < N; i++) in_order = EQ(mods_at(i)->name, order[i]);
    CHECK(in_order, "order: mods_order first, the rest by name");
    if (mods_count() == N && in_order) {
        ModInfo *a = mods_at(1), *b = mods_at(0), *c = mods_at(2), *d = mods_at(3), *e = mods_at(4), *f = mods_at(5),
                *g = mods_at(6), *hm = mods_at(7), *im = mods_at(8), *jm = mods_at(9), *km = mods_at(10),
                *lm = mods_at(11), *mm = mods_at(12), *u = mods_at(13);
        CHECK(hm->kind == (MOD_ROMFS | MOD_SUBSDK) && hm->problem == MOD_PLUGIN, "RomFS + subsdk plugin: unusable");
        CHECK(im->kind == (MOD_ROMFS | MOD_SUBSDK) && im->problem == MOD_PLUGIN, "RomFS + replaced exefs/main");
        CHECK(ends_with(b->romfs, "B/atmosphere/contents/0100E95004038000/romfs"), "Atmosphere RomFS root");
        CHECK(ends_with(c->romfs, "C/contents/0100E95004038000/romfs"), "contents RomFS root");
        CHECK(ends_with(a->romfs, "A/romfs") && a->kind == (MOD_ROMFS | MOD_AOC) && a->problem == MOD_OK,
              "plain RomFS mod with an add-on content folder");
        CHECK(!c->enabled && a->enabled && b->enabled && g->enabled && u->enabled, "mods_disabled switches C off");
        CHECK(d->kind == MOD_EXEFS && d->problem == MOD_EXEFS_OTHER && d->other_version == 1,
              "ExeFS patch mod without a build id: another version's");
        CHECK(e->problem == MOD_PLUGIN, "subsdk-only mod is a plugin mod");
        CHECK(f->problem == MOD_EMPTY, "empty RomFS folder");
        CHECK(g->kind == (MOD_ROMFS | MOD_EXEFS) && g->problem == MOD_OK && g->other_version == 1,
              "RomFS + another version's ExeFS patch: the RomFS is used");
        CHECK(mm->kind == MOD_EXEFS && mm->problem == MOD_EXEFS_LATER && mm->pending == 1,
              "an ExeFS patch for main the build lacks waits for the setup");
        CHECK(jm->kind == MOD_CHEATS && jm->problem == MOD_OK && ends_with(jm->cheats, "J/cheats"),
              "cheats-only mod");
        CHECK(km->kind == MOD_AOC && km->problem == MOD_OK && km->naoc == 2 && !km->romfs[0],
              "add-on content only mod: packs 1 and 99, a title folder without romfs left out");
        int k1 = km->naoc == 2 && km->aoc[0].index == 1 ? 0 : 1;
        CHECK(km->naoc == 2 && km->aoc[k1].index == 1 &&
              ends_with(km->aoc[k1].root, "K/atmosphere/contents/0100E95004039001/romfs") &&
              km->aoc[1 - k1].index == 99 && ends_with(km->aoc[1 - k1].root, "K/0100e95004039063/romfs"),
              "add-on content roots by title id, any case");
        CHECK(lm->kind == (MOD_ROMFS | MOD_CHEATS) && lm->problem == MOD_OK, "RomFS + cheats, main.npdm is no code");
        CHECK(u->kind == MOD_ROMFS && ends_with(u->romfs, "Ultra \xD0\x96/romfs"), "Unicode folder name");

        CHECK(aoc_is(1, "bdat/common.bdat", "A/0100E95004039001/romfs/bdat/common.bdat"),
              "add-on content file: the higher mod wins");
        CHECK(km->overridden == 1 && EQ(km->overridden_by, "A"), "the lower mod's file is counted as overridden");
        CHECK(aoc_is(1, "/BDAT/GB/Common_MS.bdat", "K/atmosphere/contents/0100E95004039001/romfs/bdat/gb/common_ms.bdat"),
              "add-on content path in another case, with a leading slash");
        CHECK(aoc_is(1, "bdat", "") && aoc_is(1, "bdat/gb", ""), "add-on content folders");
        CHECK(aoc_is(99, "stream/a.wav", "K/0100e95004039063/romfs/stream/a.wav"), "pack 99 (title ...9063)");
        CHECK(aoc_is(2, "bdat/common.bdat", NULL) && aoc_is(1, "bdat/missing.bdat", NULL) && aoc_is(1, "bda", NULL),
              "no file for another pack, a missing path or a part of a folder name");
        CHECK(aoc_is(1, "bdat/c.bdat", NULL), "a disabled mod's add-on content files are not served");
        CHECK(!mods_changed(), "nothing changed after the scan");

        mods_move(1, -1);
        CHECK(EQ(mods_at(0)->name, "A") && EQ(mods_at(1)->name, "B"), "move up");
        CHECK(EQ(cfg_get("XC2_MODS_ORDER"), "A|B|C|D|E|F|G|H|I|J|K|L|M|Ultra \xD0\x96"),
              "order written without the gone name");
        mods_move(0, -1);
        CHECK(EQ(mods_at(0)->name, "A"), "move up at the top does nothing");
        mods_move(N - 1, 1);
        CHECK(EQ(mods_at(N - 1)->name, "Ultra \xD0\x96"), "move down at the bottom does nothing");
        mods_set_enabled(2, 1);
        const char *dis = cfg_get("XC2_MODS_DISABLED");
        CHECK(mods_at(2)->enabled && (!dis || !*dis), "switching C on empties mods_disabled");
        mods_set_enabled(0, 0);
        CHECK(EQ(cfg_get("XC2_MODS_DISABLED"), "A"), "switching A off writes it");
        CHECK(mods_changed(), "changes seen");
        CHECK(aoc_is(1, "bdat/common.bdat", "A/0100E95004039001/romfs/bdat/common.bdat"),
              "menu changes apply at the next start");
    }
    remove_tree(root);
    printf("mods: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
