// The replayer of xc2.exe --replay DIR (replay.h): the calls of a recording, made on the Vulkan back end of this build,
// with no game and no window. The guest memory the record gives back is written at its addresses (the heap block is
// reserved by mem_init, so the pages are committed here when a record writes them), and every present is compared with
// the hash the record took of its picture.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn.h"
#include "nvn_api.h"
#include "nvn_obj.h"
#include "replay.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

typedef struct { const uint8_t *p; uint64_t left; } Cur;

static const void *take(Cur *c, uint64_t n) {
    if (n > c->left) rt_fatal("replay: a record is shorter than its kind needs");
    const void *p = c->p;
    c->p += n, c->left -= n;
    return p;
}

static uint64_t take64(Cur *c) {
    uint64_t v;
    memcpy(&v, take(c, 8), 8);
    return v;
}

static uint32_t take32(Cur *c) {
    uint32_t v;
    memcpy(&v, take(c, 4), 4);
    return v;
}

// The pages of [a, a+n), committed (the record writes them, so they exist in the recording's process).
static void commit_bytes(uint64_t a, uint64_t n) {
    uint64_t lo = a & ~0xfffull, hi = (a + n + 0xfff) & ~0xfffull;
    if (n && !plat_mem_commit(lo, hi - lo))
        rt_fatal("replay: cannot commit %#llx+%#llx: the heap layout differs from the record's", (unsigned long long)lo,
                 (unsigned long long)(hi - lo));
}

// Decommits the committed runs of [a, a+n): what the game gave back, read zero from now on.
static void decommit_committed(uint64_t a, uint64_t n) {
    uint64_t lo = a & ~0xfffull, hi = (a + n + 0xfff) & ~0xfffull;
    for (uint64_t p = lo; p < hi;) {
        uint64_t end = p;
        int st = plat_mem_state(p, hi, &end);
        if (end <= p) end = p + 0x1000;
        if (st >= PLAT_PROT_NONE) plat_mem_decommit(p, end - p);
        p = end;
    }
}

static void write_object(uint64_t obj, const void *bytes, size_t n) {
    commit_bytes(obj, n);
    memcpy((void *)(uintptr_t)obj, bytes, n);
}

// The command blocks of the submissions (RR_CMD): each is copied to a new address, which is what the submission's
// handle becomes. The copies are kept: the back end may read a stream after the call that submitted it.
typedef struct { uint64_t old; uint8_t *p; uint64_t n; } CmdCopy;
static CmdCopy *cmds;
static size_t ncmds, ccmds;

// The texture pins (RR_PIN): old address -> the replay's copy. They persist for the replay, and are sorted when a
// relocation needs them.
typedef struct { uint64_t old; uint64_t now; } PinCopy;
static PinCopy *pins;
static size_t npins, cpins;
static int pins_sorted = 1;

static int by_old(const void *a, const void *b) {
    uint64_t x = ((const PinCopy *)a)->old, y = ((const PinCopy *)b)->old;
    return x < y ? -1 : x > y;
}

static int cmd_by_old(const void *a, const void *b) {
    uint64_t x = ((const CmdCopy *)a)->old, y = ((const CmdCopy *)b)->old;
    return x < y ? -1 : x > y;
}

static uint64_t find_old(const PinCopy *t, size_t n, uint64_t v) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t[mid].old < v) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && t[lo].old == v ? t[lo].now : 0;
}

// The replay's address for a recorded host object: a pin, or a block of the current submission (cmds, sorted by old).
static uint64_t relocated(uint64_t v) {
    if (!pins_sorted) qsort(pins, npins, sizeof *pins, by_old), pins_sorted = 1;
    uint64_t now = find_old(pins, npins, v);
    if (now) return now;
    size_t lo = 0, hi = ncmds;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cmds[mid].old < v) lo = mid + 1;
        else hi = mid;
    }
    return lo < ncmds && cmds[lo].old == v ? (uint64_t)(uintptr_t)cmds[lo].p : 0;
}

// Every 8-byte word of the copies that is the address of a recorded host object (a pin, or a block of this submission)
// becomes the replay's address of that object: the record's jumps, calls and pin pointers. A word that is no such
// address stays as it is: the host addresses the record holds are far above any guest address or small integer, so a
// match by accident is not a practical risk. A target the record did not hold stays too (cmd_block in replay_rec.c
// stops at its depth limit; that stream then reads the record's address, which the replay does not have).
static void cmd_relocate(void) {
    qsort(cmds, ncmds, sizeof *cmds, cmd_by_old);
    for (size_t k = 0; k < ncmds; k++) {
        uint8_t *p = cmds[k].p;
        uint64_t n = cmds[k].n;
        for (uint64_t off = 0; off + 8 <= n; off += 8) {
            uint64_t w;
            memcpy(&w, p + off, 8);
            uint64_t now = relocated(w);
            if (now && now != w) memcpy(p + off, &now, 8);
        }
    }
}

int replay_main(const char *dir) {
    char path[PLAT_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/log.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("replay: cannot open %s\n", path);
        return 1;
    }
    setvbuf(f, NULL, _IOFBF, 1u << 20);
    RecHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, REC_MAGIC, 8) || h.version != REC_VERSION) {
        printf("replay: %s is not a version %d recording\n", path, REC_VERSION);
        fclose(f);
        return 1;
    }
    if (h.sz_pool != sizeof(pool_t) || h.sz_tex != sizeof(tex_t) || h.sz_view != sizeof(view_t) ||
        h.sz_samp != sizeof(samp_par) || h.sz_prog != sizeof(prog_t) || h.sz_copy != sizeof(NVNcopyRegion)) {
        printf("replay: %s was recorded by a build with other structure sizes: record again with this build\n", path);
        fclose(f);
        return 1;
    }
    const RtGuestLayout *g = mem_layout();
    if (h.heap_base != g->heap.base || h.heap_size != g->heap.size) {
        printf("replay: the guest heap is at %#llx (%llu MiB) but the recording's is at %#llx (%llu MiB): the address "
               "layout differs\n", (unsigned long long)g->heap.base, (unsigned long long)(g->heap.size >> 20),
               (unsigned long long)h.heap_base, (unsigned long long)(h.heap_size >> 20));
        fclose(f);
        return 1;
    }
    // The back end runs as it did when the frames were made, but with nothing else going on: synchronous (the replay
    // thread would let the calls return before the work they describe), and no page freezing (replay.c, rec_wrap).
    plat_setenv("XC2_REPLAY_ASYNC", "0");
    plat_setenv("XC2_FREEZE_IDLE", "0");
    // The game's boot (boot.c rt_boot) makes the pipelines recorded in earlier runs ready before the guest starts, and
    // draws whose pipeline is still building are skipped. The replay makes the same boot-time pipelines ready first, so
    // the frame's draws are not skipped for want of a build the game had already finished.
    vk_pipeline_async_enable();
    vk_pipeline_prewarm();
    // The game skips a draw whose shader or pipeline is still building, and which of them that is depends on the frame
    // clock. The replay waits for each one instead (vk_exec_deterministic), so a draw is never dropped by timing.
    vk_exec_deterministic(1);
    const char *shots = cfg_get("XC2_REPLAY_SHOTS");
    if (shots && *shots) plat_file_mkdir_p(shots);
    const char *lim = cfg_get("XC2_REPLAY_FRAMES");
    uint64_t limit = lim ? strtoull(lim, NULL, 10) : 0;
    int verify = cfg_get("XC2_REPLAY_VERIFY") && *cfg_get("XC2_REPLAY_VERIFY") == '1';
    uint64_t writes_checked = 0, writes_bad = 0;
    // XC2_REPLAY_VERIFY: each WriteTexels, kept with the hash of its texels, is read back at every present.
    typedef struct { tex_t t; view_t v; NVNcopyRegion r; uint64_t row, img, n, hash; } Kept;
    Kept *kept = NULL;
    size_t nvw = 0, cvw = 0;

    const GfxBackend *b = &gfx_vulkan_backend;
    uint64_t t0 = plat_now_ns(), records = 0, mem_bytes = 0, presents = 0, matched = 0, bad = 0, first_bad = 0;
    uint8_t *buf = NULL;
    uint64_t cap = 0;
    int ended_early = 0;
    RecRecord rh;
    while (fread(&rh, sizeof rh, 1, f) == 1) {
        records++;
        if (rh.kind == RR_DELTA) {   // the bytes go straight to guest memory
            uint64_t hd[2];
            if (fread(hd, sizeof hd, 1, f) != 1) { ended_early = 1; break; }
            commit_bytes(hd[0], hd[1]);
            if (fread((void *)(uintptr_t)hd[0], 1, hd[1], f) != hd[1]) { ended_early = 1; break; }
            mem_bytes += hd[1];
            continue;
        }
        if (rh.size > cap) {
            free(buf);
            buf = malloc(rh.size ? rh.size : 1);
            if (!buf) rt_fatal("replay: out of memory for a record of %llu bytes", (unsigned long long)rh.size);
            cap = rh.size;
        }
        if (rh.size && fread(buf, 1, rh.size, f) != rh.size) { ended_early = 1; break; }
        Cur c = {buf, rh.size};
        int stop = 0;
        switch (rh.kind) {
        case RR_DECOMMIT: {
            uint64_t a = take64(&c), n = take64(&c);
            decommit_committed(a, n);
            break;
        }
        case RR_COMMIT: {
            uint64_t a = take64(&c), n = take64(&c);
            commit_bytes(a, n);
            break;
        }
        case RR_POOL_INIT: {
            uint64_t obj = take64(&c);
            write_object(obj, take(&c, sizeof(pool_t)), sizeof(pool_t));
            b->pool_init((pool_t *)(uintptr_t)obj);
            break;
        }
        case RR_POOL_FINI: {
            uint64_t obj = take64(&c);
            write_object(obj, take(&c, sizeof(pool_t)), sizeof(pool_t));
            b->pool_fini((pool_t *)(uintptr_t)obj);
            break;
        }
        case RR_POOL_MAP: {
            uint64_t obj = take64(&c);
            uint32_t n = take32(&c);
            take32(&c);
            b->pool_map_virtual((pool_t *)(uintptr_t)obj, (int)n, take(&c, (uint64_t)n * 0x28));
            break;
        }
        case RR_TEX_REG: {
            int id = (int)take32(&c);
            int has_view = take32(&c) != 0;
            tex_t t;
            view_t v;
            memcpy(&t, take(&c, sizeof t), sizeof t);
            memcpy(&v, take(&c, sizeof v), sizeof v);
            b->texture_register(id, &t, has_view ? &v : NULL);
            break;
        }
        case RR_SAMP_REG: {
            int id = (int)take32(&c);
            take32(&c);
            samp_par s;
            memcpy(&s, take(&c, sizeof s), sizeof s);
            b->sampler_register(id, &s);
            break;
        }
        case RR_TEX_REL: {
            tex_t t;
            memcpy(&t, take(&c, sizeof t), sizeof t);
            b->texture_release(&t);
            break;
        }
        case RR_PROG_REL: {
            prog_t p;
            memcpy(&p, take(&c, sizeof p), sizeof p);
            b->program_release(&p);
            break;
        }
        case RR_PIN: {
            uint64_t a = take64(&c), n = take64(&c);
            if (n != sizeof(tex_t)) rt_fatal("replay: a pin of %llu bytes, the texture object is %zu bytes",
                                             (unsigned long long)n, sizeof(tex_t));
            tex_t *copy = malloc(sizeof *copy);
            if (!copy) rt_fatal("replay: out of memory for a texture pin");
            memcpy(copy, take(&c, n), n);
            if (npins == cpins) {
                cpins = cpins ? cpins * 2 : 1024;
                pins = realloc(pins, cpins * sizeof *pins);
                if (!pins) rt_fatal("replay: out of memory for the texture pins");
            }
            pins[npins++] = (PinCopy){a, (uint64_t)(uintptr_t)copy};
            pins_sorted = 0;
            break;
        }
        case RR_CMD: {
            uint64_t a = take64(&c), n = take64(&c);
            uint8_t *p = malloc(n ? n : 1);
            if (!p) rt_fatal("replay: out of memory for a command block of %llu bytes", (unsigned long long)n);
            memcpy(p, take(&c, n), n);
            if (ncmds == ccmds) {
                ccmds = ccmds ? ccmds * 2 : 256;
                cmds = realloc(cmds, ccmds * sizeof *cmds);
                if (!cmds) rt_fatal("replay: out of memory for the command blocks");
            }
            cmds[ncmds++] = (CmdCopy){a, p, n};
            break;
        }
        case RR_SUBMIT: {
            uint32_t n = take32(&c);
            take32(&c);
            const uint64_t *hs = take(&c, (uint64_t)n * 8);
            cmd_relocate();
            uint64_t *mapped = malloc(n ? (size_t)n * 8 : 8);
            if (!mapped) rt_fatal("replay: out of memory for the submission's handles");
            for (uint32_t i = 0; i < n; i++) {
                uint64_t m = hs[i] ? relocated(hs[i]) : 0;
                if (hs[i] && !m) rt_fatal("replay: a submission names command block %#llx the record does not hold",
                                          (unsigned long long)hs[i]);
                mapped[i] = m;
            }
            rec_trace_submit("replay", mapped, (int)n);
            b->submit(mapped, (int)n);
            free(mapped);
            ncmds = 0;   // the copies stay allocated (the back end may read a stream after its submit): see CmdCopy
            break;
        }
        case RR_PRESENT: {
            tex_t t;
            memcpy(&t, take(&c, sizeof t), sizeof t);
            take(&c, 24);   // has_crop, the crop: the replay's picture is the whole texture, as the record's was
            uint64_t tail[3];
            memcpy(tail, take(&c, sizeof tail), sizeof tail);
            presents++;
            if (verify) {   // the written images must still hold what the record wrote, when the frame is presented
                size_t moved = 0;
                for (size_t k = 0; k < nvw; k++) {
                    uint8_t *back = malloc(kept[k].img ? kept[k].img : 1);
                    if (!back) rt_fatal("replay: out of memory for a verify read");
                    b->read_texels(&kept[k].t, &kept[k].v, &kept[k].r, back, kept[k].row, kept[k].img);
                    uint64_t h = 1469598103934665603ull;
                    for (uint64_t q = 0; q < kept[k].n; q++) h = (h ^ back[q]) * 1099511628211ull;
                    if (h != kept[k].hash) {
                        if (!moved) printf("replay: at present %llu, written images changed since their WRITE:", (unsigned long long)presents);
                        if (moved < 8) printf(" #%zu(%ux%u fmt %#x)", k, (unsigned)kept[k].t.d.width, (unsigned)kept[k].t.d.height, (unsigned)kept[k].t.d.format);
                        moved++;
                    }
                    free(back);
                }
                if (moved) printf(" (%zu of %zu)\n", moved, nvw);
                else printf("replay: at present %llu, all %zu written images hold their WRITE content\n", (unsigned long long)presents, nvw);
            }
            uint8_t *px = NULL;
            uint64_t got = 0;
            size_t n = rec_picture(b, &t, &px, &got);
            if (got == tail[2]) {
                matched++;
            } else {
                if (!bad) first_bad = presents;
                bad++;
                printf("replay: present %llu (frame %llu) differs: recorded %016llx, replayed %016llx\n",
                       (unsigned long long)presents, (unsigned long long)tail[1], (unsigned long long)tail[2],
                       (unsigned long long)got);
            }
            if (px && shots && *shots) {
                char out[PLAT_PATH_MAX * 2];
                snprintf(out, sizeof out, "%s/p%06llu.png", shots, (unsigned long long)presents);
                rec_save_picture(out, &t, px, n);
            }
            free(px);
            if (limit && presents >= limit) stop = 1;
            break;
        }
        case RR_FINISH:
            b->finish();
            break;
        case RR_READ: {
            tex_t t;
            view_t v;
            NVNcopyRegion r;
            memcpy(&t, take(&c, sizeof t), sizeof t);
            memcpy(&v, take(&c, sizeof v), sizeof v);
            memcpy(&r, take(&c, sizeof r), sizeof r);
            uint64_t row = take64(&c), img = take64(&c), need = take64(&c);
            uint8_t *dst = malloc(need ? need : 1);
            if (!dst) rt_fatal("replay: out of memory for a read of %llu bytes", (unsigned long long)need);
            b->read_texels(&t, &v, &r, dst, row, img);
            free(dst);
            break;
        }
        case RR_WRITE: {
            tex_t t;
            view_t v;
            NVNcopyRegion r;
            memcpy(&t, take(&c, sizeof t), sizeof t);
            memcpy(&v, take(&c, sizeof v), sizeof v);
            memcpy(&r, take(&c, sizeof r), sizeof r);
            uint64_t row = take64(&c), img = take64(&c), n = take64(&c);
            const uint8_t *src = take(&c, n);
            b->write_texels(&t, &v, &r, src, row, img);
            if (verify) {   // XC2_REPLAY_VERIFY=1: the image must read back what the record wrote, now and at each present
                uint8_t *back = malloc(img ? img : 1);
                if (!back) rt_fatal("replay: out of memory for a verify read");
                b->read_texels(&t, &v, &r, back, row, img);
                uint64_t diff = 0;
                for (uint64_t k = 0; k < img && k < n; k++) diff += back[k] != src[k];
                writes_checked++;
                if (diff) writes_bad++, printf("replay: WRITE %llu (%ux%u fmt %#x) reads back %llu of %llu bytes differently\n",
                    (unsigned long long)writes_checked, (unsigned)t.d.width, (unsigned)t.d.height, (unsigned)t.d.format,
                    (unsigned long long)diff, (unsigned long long)n);
                free(back);
                if (nvw == cvw) {
                    cvw = cvw ? cvw * 2 : 64;
                    kept = realloc(kept, cvw * sizeof *kept);
                    if (!kept) rt_fatal("replay: out of memory for the verify list");
                }
                kept[nvw].t = t, kept[nvw].v = v, kept[nvw].r = r, kept[nvw].row = row, kept[nvw].img = img;
                kept[nvw].n = n, kept[nvw].hash = 1469598103934665603ull;
                for (uint64_t k = 0; k < n; k++) kept[nvw].hash = (kept[nvw].hash ^ src[k]) * 1099511628211ull;
                nvw++;
            }
            break;
        }
        case RR_GUEST: {
            uint32_t which = take32(&c);
            take32(&c);
            uint64_t addr = take64(&c), size = take64(&c);
            if (which == 0) b->guest_written(addr, size);
            else b->guest_read(addr, size);
            break;
        }
        case RR_CPU_SYNCED:
            b->cpu_synced(take64(&c));
            break;
        default:
            printf("replay: unknown record kind %u\n", rh.kind);
            ended_early = 1;
            stop = 1;
            break;
        }
        if (stop) break;
    }
    if (ended_early) printf("replay: the recording ends early (a record is cut short)\n");
    b->finish();
    double secs = (double)(plat_now_ns() - t0) / 1e9;
    fclose(f);
    free(buf);
    if (!presents) printf("replay: no present in the recording\n");
    printf("replay: %llu presents, %llu match, %llu differ%s; %llu records, %.1f MiB of guest memory, %.2f s\n",
           (unsigned long long)presents, (unsigned long long)matched, (unsigned long long)bad,
           bad ? "" : " (all pictures identical)", (unsigned long long)records, mem_bytes / 1048576.0, secs);
    if (bad) printf("replay: first difference at present %llu\n", (unsigned long long)first_bad);
    return bad || ended_early || !presents ? 1 : 0;
}
