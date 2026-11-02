// Xbox, PlayStation and keyboard button prompts (prompts.h).
#include "prompts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"

enum { MAX_FILES = 32 };

static PlatOnce once = PLAT_ONCE_INIT;
static char dir[1200];               // prompts/<set> of the chosen set, "" for the game's own prompts
static char *files[MAX_FILES];       // the set's paths from the manifest
static int nfiles;

// The set XC2_PROMPTS and the gamepad pick: a folder of tools/xc2_prompts.py, NULL for the game's own prompts.
static const char *pick(const char **why) {
    const char *v = cfg_value("XC2_PROMPTS");
    int south = PAD_LABEL_NONE, layout = PAD_LAYOUT_XBOX;
    int pad = input_pad_identity(&south, &layout);
    if (!pad) layout = input_pad_layout_pick(cfg_value("XC2_PAD_LAYOUT"), 0);
    const char *kind = v;
    *why = "prompts = ";
    if (!strcmp(v, "auto")) {
        kind = !pad ? "keyboard" : south == PAD_LABEL_CROSS ? "playstation" : south == PAD_LABEL_B ? "switch" : "xbox";
        *why = !pad ? "no gamepad" : south == PAD_LABEL_CROSS ? "a PlayStation gamepad"
                                   : south == PAD_LABEL_B     ? "a Nintendo gamepad" : "an Xbox or other gamepad";
    }
    if (!strcmp(kind, "switch")) return NULL;
    if (!strcmp(kind, "keyboard")) return "keyboard";
    if (layout != PAD_LAYOUT_NINTENDO) return kind;
    return !strcmp(kind, "playstation") ? "playstation_nintendo" : "xbox_nintendo";
}

static void init(void *arg) {
    (void)arg;
    const char *why = NULL, *set = pick(&why);
    const char *v = cfg_value("XC2_PROMPTS");
    if (!set) {
        rt_log("prompts: the game's own (%s%s)", why, !strcmp(why, "prompts = ") ? v : "");
        return;
    }
    char base[1100];
    if (!plat_exe_path(base, sizeof base)) return;
    char *sep = plat_last_sep(base);
    if (sep) sep[1] = 0;
    else strcpy(base, "./");
    char path[1200];
    snprintf(path, sizeof path, "%sprompts/manifest.tsv", base);
    uint64_t size = 0;
    char *m = (char *)plat_file_read(path, &size);
    if (!m) {
        rt_log("prompts: no %s (the setup writes it with tools/xc2_prompts.py); the game's own prompts", path);
        return;
    }
    // manifest lines: set, path, size, SHA-256, tab-separated
    char *text = malloc(size + 1);
    if (!text) rt_fatal("prompts manifest");
    memcpy(text, m, size);
    text[size] = 0;
    free(m);
    size_t sl = strlen(set);
    for (char *line = text, *next; *line && nfiles < MAX_FILES; line = next) {
        next = line + strcspn(line, "\r\n");
        if (*next) *next++ = 0;
        char *tab = strchr(line, '\t');
        if (!tab || (size_t)(tab - line) != sl || strncmp(line, set, sl)) continue;
        tab[1 + strcspn(tab + 1, "\t")] = 0;
        files[nfiles] = strdup(tab + 1);
        if (!files[nfiles]) rt_fatal("prompts manifest");
        nfiles++;
    }
    free(text);
    snprintf(dir, sizeof dir, "%sprompts/%s", base, set);
    rt_log("prompts: %s (%s%s), %d layouts in %s", set, why, !strcmp(why, "prompts = ") ? v : "", nfiles, dir);
}

int prompts_file(const char *rel, char *out, size_t cap) {
    plat_once(&once, init, NULL);
    if (!rel) return 0;
    while (*rel == '/') rel++;
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(files[i], rel)) {
            int n = snprintf(out, cap, "%s/%s", dir, rel);
            return n > 0 && (size_t)n < cap && plat_fs_kind(out) == PLAT_FS_FILE;
        }
    return 0;
}
