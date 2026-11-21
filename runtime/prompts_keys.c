// Remappable keyboard prompts: tools/xc2_prompts.py writes keys.dat beside the keyboard set's layouts. It holds the
// encoded BC3 block of every bindable key in each style the set uses, and for each cell of each layout the offsets
// of the blocks that cell owns. Serving a layout copies the tile of the key bound to that action over those offsets.
// The game reads the layouts at boot, so a remap in the Controls tab takes effect on the next restart.
#include "prompts_keys.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "input_map.h"
#include "platform/platform.h"

enum { MAX_STYLES = 8, MAX_FILES = 16, MAX_PATCHES = 256, MAX_KEYS = 256, BLOCK = 16 };
typedef struct { uint16_t bytes, blocks; } Style;
typedef struct { uint16_t file, n; uint8_t action, style; const uint8_t *at; } Patch;

static struct {
    uint8_t *data;
    size_t size;
    int nkeys;
    int nstyles;
    Style styles[MAX_STYLES];
    const uint8_t *tiles[MAX_STYLES];
    int nfiles;
    char files[MAX_FILES][256];
    int npatches;
    Patch patches[MAX_PATCHES];
    uint8_t baked[16], ndef[16], defs[16][4];
    uint32_t tile_hash;
    int slot[MAX_KEYS];
} db;
static int loaded, failed;
static uint32_t bound_hash;
static char gen_dir[1200];
static PlatMutex lock = PLAT_MUTEX_INIT;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                                                (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static int cstr(const uint8_t *p, size_t cap, char *out, size_t n) {
    size_t i = 0;
    while (i < cap && i + 1 < n && p[i]) i++;
    if (!i || i == cap) return 0;
    memcpy(out, p, i);
    out[i] = 0;
    return 1;
}

static void load(const char *set_dir) {
    if (loaded || failed) return;
    loaded = 1;
    char path[1200];
    if (snprintf(path, sizeof path, "%s/keys.dat", set_dir) >= (int)sizeof path) { failed = 1; return; }
    uint64_t size = 0;
    size_t at = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (!data || size < 20) { free(data); failed = 1; return; }
    db.data = data;
    db.size = (size_t)size;
    if (memcmp(data, "XC2K", 4) || rd16(data + 4) != 2 || db.size < 116) goto bad;
    for (int a = 0; a < 16; a++) {
        db.baked[a] = data[20 + a];
        db.ndef[a] = data[36 + 5 * a];
        memcpy(db.defs[a], data + 37 + 5 * a, 4);
    }
    int nkeys = rd16(data + 6), nstyles = rd16(data + 8);
    db.nfiles = rd16(data + 10);
    db.npatches = rd16(data + 12);
    db.tile_hash = rd32(data + 16);
    if (!nkeys || nkeys > MAX_KEYS || !nstyles || nstyles > MAX_STYLES ||
        !db.nfiles || db.nfiles > MAX_FILES || db.npatches > MAX_PATCHES) goto bad;
    at = 116;
    if (at + (size_t)nkeys * 32 > db.size) goto bad;
    db.nkeys = nkeys;
    for (int k = 0; k < nkeys; k++) db.slot[k] = -1;
    for (int k = 0; k < nkeys; k++)
        for (int id = 1; id < KEY_COUNT; id++) {
            const char *name = input_key_name(id);
            if (name && !memcmp(data + at + k * 32, name, strlen(name) + 1)) db.slot[id] = k;
        }
    at += (size_t)nkeys * 32;
    if (at + (size_t)nstyles * 4 > db.size) goto bad;
    db.nstyles = nstyles;
    size_t tiles = 0;
    for (int s = 0; s < nstyles; s++, at += 4) {
        db.styles[s] = (Style){rd16(data + at), rd16(data + at + 2)};
        if (!db.styles[s].bytes || db.styles[s].bytes % BLOCK) goto bad;
        tiles += (size_t)db.styles[s].bytes * nkeys;
    }
    if (at + tiles + (size_t)db.nfiles * 256 > db.size) goto bad;
    for (int s = 0; s < nstyles; s++) db.tiles[s] = data + at, at += (size_t)db.styles[s].bytes * nkeys;
    for (int f = 0; f < db.nfiles; f++, at += 256)
        if (!cstr(data + at, 256, db.files[f], sizeof db.files[f])) goto bad;
    for (int p = 0; p < db.npatches; p++) {
        if (at + 8 > db.size) goto bad;
        Patch *q = &db.patches[p];
        *q = (Patch){rd16(data + at), rd16(data + at + 2), data[at + 4], data[at + 5], NULL};
        at += 8;
        if (q->file >= db.nfiles || q->style >= db.nstyles || !q->n || at + (size_t)q->n * 4 > db.size) goto bad;
        q->at = data + at;
        at += (size_t)q->n * 4;
    }
    return;
bad:
    failed = 1;
}

static uint8_t chosen[16];   // key index in keys.dat for each action, 255 when unbound

// The same keys in any order: a bind line may list an action's default keys its own way (input_map.c same_set).
static int same_keys(const uint8_t *a, const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) {
        int ca = 0, cb = 0;
        for (int j = 0; j < n; j++) ca += a[j] == a[i], cb += b[j] == a[i];
        if (ca != cb) return 0;
    }
    return 1;
}

static void read_bindings(void) {
    for (int a = 0; a < 16; a++) {
        uint8_t keys[4];
        int n = input_map_keys_of(&input_map, a, keys, (int)(sizeof keys / sizeof *keys));
        chosen[a] = 255;
        for (int i = 0; i < n; i++) keys[i] = (uint8_t)db.slot[keys[i]];   // ids to keys.dat indices
        if (n == db.ndef[a] && same_keys(keys, db.defs[a], n)) chosen[a] = db.baked[a];
        else if (n) chosen[a] = keys[0];
    }
}

static uint32_t bindings(void) {
    return fnv(fnv(2166136261u, &db.tile_hash, sizeof db.tile_hash), chosen, sizeof chosen);
}

int prompts_keys_file(const char *rel, const char *set_dir, const char *src, char *out, size_t cap) {
    if (!rel || !set_dir || !src) return 0;
    plat_mutex_lock(&lock);
    load(set_dir);
    if (failed || !rt_config()->profile) goto done;
    read_bindings();
    uint32_t hash = bindings();
    if (!gen_dir[0] || hash != bound_hash) {
        if (snprintf(gen_dir, sizeof gen_dir, "%s/prompts/keyboard-%08x", rt_config()->profile, hash) >=
            (int)sizeof gen_dir) goto done;
        bound_hash = hash;
    }
    int file = -1;
    for (int f = 0; f < db.nfiles; f++)
        if (!strcmp(db.files[f], rel)) { file = f; break; }
    if (file < 0) goto done;

    if (snprintf(out, cap, "%s/%s", gen_dir, rel) >= (int)cap) goto done;
    // this hash has already been written, from a source of the same size (a regenerated layout may differ)
    if (plat_fs_kind(out) == PLAT_FS_FILE && plat_file_size(out) == plat_file_size(src)) goto served;
    uint64_t n = 0;
    uint8_t *data = plat_file_read(src, &n);
    if (!data || n > 0xffffffffu) { free(data); goto done; }
    int ok = 1;
    for (int p = 0; p < db.npatches && ok; p++) {
        const Patch *q = &db.patches[p];
        if (q->file != file) continue;
        int slot = chosen[q->action];
        if (slot == 255) continue;   // no key bound: keep the icon the set was built with
        const Style *st = &db.styles[q->style];
        const uint8_t *tile = db.tiles[q->style] + (size_t)slot * st->bytes;
        for (uint16_t i = 0; i < q->n; i++) {
            uint32_t off = rd32(q->at + 4 * i);
            if ((uint64_t)off + BLOCK > n) { ok = 0; break; }
            memcpy(data + off, tile + (size_t)i * BLOCK, BLOCK);
        }
    }
    char parent[1200];
    snprintf(parent, sizeof parent, "%s", out);
    char *sep = strrchr(parent, '/');
    if (sep) *sep = 0;
    plat_file_mkdir_p(parent);
    plat_file_mkdir_p(gen_dir);
    if (ok && !plat_file_write_atomic(out, data, (size_t)n)) ok = 0;   // a half-written copy must never be served
    free(data);
    if (!ok) goto done;
served:
    plat_mutex_unlock(&lock);
    return 1;
done:
    plat_mutex_unlock(&lock);
    return 0;
}
