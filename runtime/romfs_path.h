// From MacNativeGames games/totk/runtime/romfs_path.h @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// RomFS path validation shared by the runtime (romfs.c) and its probe (fs_host.c probe_fs).
#pragma once
#include <stddef.h>
#include <string.h>

// A RomFS-relative path: non-empty, no leading '/', no '\', no empty, "." or ".." component.
static inline int romfs_path_valid_relative(const char *path) {
    if (!path || !*path || *path == '/' || strchr(path, '\\')) return 0;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') || (size == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) break;
        part = end + 1;
    }
    return 1;
}

// A RomFS-relative path with one '/' between components, written to out. Repeated separators are collapsed, so
// "stream//bgm/m58.nop" becomes "stream/bgm/m58.nop" as the game's audio paths expect. Empty components are skipped;
// a leading '/', a backslash, "." and ".." are still rejected. Returns 1 on success, 0 on an invalid or too-long path.
static inline int romfs_path_normalize(const char *path, char *out, size_t cap) {
    if (!path || !*path || *path == '/' || strchr(path, '\\') || !out || !cap) return 0;
    size_t w = 0;
    for (const char *part = path; *part;) {
        while (*part == '/') ++part;
        if (!*part) break;
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if ((size == 1 && part[0] == '.') || (size == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (w) {
            if (w + 1 >= cap) return 0;
            out[w++] = '/';
        }
        if (w + size >= cap) return 0;
        memcpy(out + w, part, size);
        w += size;
        part = end ? end : part + size;
    }
    if (!w) return 0;
    out[w] = 0;
    return 1;
}

// "<mount>:/<path>" -> normalized "<path>" in out, or 0. Audio stream requests join a mount root and a leading-slash
// resource, producing "content://Sound/..." and XC2's field BGM "stream//bgm/m58.nop": repeated separators are
// collapsed; traversal and backslashes are still rejected.
static inline int romfs_path_relative(const char *mount, const char *path, char *out, size_t cap) {
    if (!mount || !*mount || !path || !out || !cap) return 0;
    size_t size = strlen(mount);
    if (strncmp(path, mount, size) || path[size] != ':' || path[size + 1] != '/') return 0;
    path += size + 2;
    while (*path == '/') ++path;
    return romfs_path_normalize(path, out, cap);
}
