// Uniform blocks as of the submit call. NVN starts the GPU when nvnQueueSubmitCommands returns and the GPU reads the
// constants soon after; here the replay records for several milliseconds, and the GPU reads a pool's bytes when it
// runs the part that uses them (the VRAM mirror's sync copies run then, vk_memory.c "VRAM mirror"). Meanwhile the game,
// having handed the frame over, rewrites per-object uniform blocks on other threads with no fence: late draws read
// another object's matrix (phantom rocks, a flat far field; XCX 1.2.1's producer f_70008c6460, 14-24 ms after the
// submit). So at the start of a submission's recording every uniform range it binds is copied into an arena of its own,
// and the uniform bindings of the replay use the copy.
// - Only ranges of imported pools (memory the CPU writes) are copied, aligned out to 256 bytes (the largest
//   minUniformBufferOffsetAlignment) and at most 64 KiB from the bound address, overlapping ones merged.
// - A range the GPU wrote in a submission that had not completed when the copy was taken is left out (marked dropped
//   when the replay starts the submission): its bytes reach guest memory only with that submission's write-back, the
//   mirror holds them until then.
// - A GPU write within the submission (UpdateUniformBuffer, a storage buffer a shader writes, a buffer copy or fill,
//   an image-to-buffer copy: vk_usnap_drop) drops the copies it touches from then on, so later draws read the result.
// - The arena goes back to a free list once the submission's fence is done (completion thread).
// The design follows MacNativeGames' XCX_UBO_SNAPSHOT (games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m usnap_take,
// usnap_drop; MIT, Copyright (c) 2026 jhaizhou); the code is ours.
// The copy is taken in the submit call (vk_usnap_capture from vk_backend.c vk_be_submit), also with XC2_REPLAY_ASYNC,
// where the replay runs up to a frame later on its own thread. XC2_UBO_SNAPSHOT=0 binds live memory as before; =ab
// alternates (XC2_AB).
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "platform/platform.h"

#define ALIGN 256u
#define MAX_RANGE (64u << 10)
#define ARENA_MIN (4u << 20)
#define CALL_DEPTH 16

typedef struct Arena {
    struct Arena *next;
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *map;
    uint64_t size;
} Arena;

typedef struct { uint64_t lo, hi; } Rng;
typedef struct { uint64_t lo, hi, off; int dropped; } Reg;
typedef struct { uint64_t lo, hi, fence; } Wrote;

// A capture: the merged ranges and the arena holding their bytes as of the submit call.
typedef struct {
    Arena *arena;
    Reg *reg;
    int nreg;
    uint64_t done;              // vk_sync_fence_done before the copy
} UCap;

// The replay's state (the submission being recorded).
static struct {
    int on;                     // this submission has a snapshot
    uint64_t fence, gen;
    Arena *arena;
    Reg *reg;                   // merged, sorted by lo, disjoint
    int nreg;
    Wrote *wr;                  // GPU writes of submissions not known to be complete
    int nwr, cwr;
    // since the last summary
    uint64_t subs, hits, drops, inflight;
} U;

// The capture's state (submitting thread, cap_lock).
static PlatMutex cap_lock = PLAT_MUTEX_INIT;
static struct {
    Rng *col;                   // collected ranges
    int ncol, ccol;
    // since the last summary
    uint64_t subs, regions, bytes, copy_ns, refused, off_subs;
} C;
static volatile int umode;      // rt_knob + 1 (0: not read yet): 1 off, 2 on, 3 ab

static int mode(void) {
    int m = __atomic_load_n(&umode, __ATOMIC_RELAXED);
    if (!m) __atomic_store_n(&umode, m = rt_knob("UBO_SNAPSHOT", "1") + 1, __ATOMIC_RELAXED);
    return m - 1;
}

static Arena *free_arenas;
static PlatMutex free_lock = PLAT_MUTEX_INIT;

static void *grow(void *p, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 1024;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) rt_fatal("vk_usnap: out of memory");
    *cap = n;
    return q;
}

static Arena *arena_get(uint64_t need) {
    plat_mutex_lock(&free_lock);
    Arena **pp = &free_arenas, *a = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= need) {
            a = *pp, *pp = a->next;
            break;
        }
    plat_mutex_unlock(&free_lock);
    if (a) return a;
    a = calloc(1, sizeof *a);
    if (!a) rt_fatal("vk_usnap: out of memory");
    a->size = need < ARENA_MIN ? ARENA_MIN : (need + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = a->size,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    // Device-local through the BAR when the device has it (the GPU reads constants on every draw), else host memory.
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &a->buf, &a->alloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk_usnap: arena of %llu bytes failed (%d): this submission binds live memory",
               (unsigned long long)a->size, (int)res);
        free(a);
        return NULL;
    }
    a->map = info.pMappedData;
    rt_log("vk_usnap: new arena of %.1f MiB", a->size / 1048576.0);
    return a;
}

static void arena_done(void *arg) {   // completion thread: the GPU has read it
    Arena *a = arg;
    plat_mutex_lock(&free_lock);
    a->next = free_arenas, free_arenas = a;
    plat_mutex_unlock(&free_lock);
}

static void collect(const op_hdr *h, int depth) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk_usnap: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return;
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (h->op == OP_CALL && depth < CALL_DEPTH) collect(nvn_op_first(((const p_jump *)a)->target), depth + 1);
        if (h->op == OP_BIND_UBO) {
            const p_bind_buf *b = a;
            if (b->addr && b->size) {
                if (C.ncol == C.ccol) C.col = grow(C.col, &C.ccol, sizeof *C.col);
                uint64_t n = b->size < MAX_RANGE ? b->size : MAX_RANGE;
                C.col[C.ncol++] = (Rng){b->addr & ~(uint64_t)(ALIGN - 1), b->addr + n};
            }
        }
        h = nvn_op_next(h);
    }
}

static int by_lo(const void *a, const void *b) {
    uint64_t x = ((const Rng *)a)->lo, y = ((const Rng *)b)->lo;
    return x < y ? -1 : x > y;
}
static int wr_by_lo(const void *a, const void *b) {
    uint64_t x = ((const Wrote *)a)->lo, y = ((const Wrote *)b)->lo;
    return x < y ? -1 : x > y;
}

// Sorts the collected ranges by lo. A town frame binds about 10,000 uniform ranges and qsort took half a millisecond
// of the copy's 0.96 ms on the game thread, so they go through an LSD radix sort on (lo - lowest) / ALIGN, three
// passes of 11 bits; qsort is left for a span that does not fit in 33 bits (2 TiB).
#define RADIX_BITS 11
static void sort_col(void) {
    static Rng *tmp;
    static int ctmp;
    int n = C.ncol;
    if (n < 2) return;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < n; i++) {
        if (C.col[i].lo < lo) lo = C.col[i].lo;
        if (C.col[i].lo > hi) hi = C.col[i].lo;
    }
    if ((hi - lo) / ALIGN >> (3 * RADIX_BITS)) {
        qsort(C.col, (size_t)n, sizeof *C.col, by_lo);
        return;
    }
    if (ctmp < n) {
        free(tmp);
        ctmp = C.ccol;
        if (!(tmp = malloc((size_t)ctmp * sizeof *tmp))) rt_fatal("vk_usnap: out of memory");
    }
    Rng *src = C.col, *dst = tmp;
    for (int pass = 0; pass < 3; pass++) {
        int shift = pass * RADIX_BITS, count[1 << RADIX_BITS] = {0};
        for (int i = 0; i < n; i++) count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++;
        for (int k = 0, sum = 0; k < 1 << RADIX_BITS; k++) {
            int c = count[k];
            count[k] = sum, sum += c;
        }
        for (int i = 0; i < n; i++) dst[count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++] = src[i];
        Rng *t = src;
        src = dst, dst = t;
    }
    if (src != C.col) memcpy(C.col, src, (size_t)n * sizeof *src);   // three passes end in tmp
}

// Keeps the GPU writes of submissions not done by done, merged into disjoint ranges sorted by lo (the terrain rewrites
// one block before every draw: hundreds of equal entries), each with the newest fence among its parts.
static void wr_prune(uint64_t done) {
    int n = 0;
    for (int i = 0; i < U.nwr; i++)
        if (U.wr[i].fence > done) U.wr[n++] = U.wr[i];
    qsort(U.wr, (size_t)n, sizeof *U.wr, wr_by_lo);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && U.wr[i].lo <= U.wr[m - 1].hi) {
            if (U.wr[i].hi > U.wr[m - 1].hi) U.wr[m - 1].hi = U.wr[i].hi;
            if (U.wr[i].fence > U.wr[m - 1].fence) U.wr[m - 1].fence = U.wr[i].fence;
        } else {
            U.wr[m++] = U.wr[i];
        }
    }
    U.nwr = m;
}
static int wr_hits(uint64_t lo, uint64_t hi) {   // wr_prune ran: disjoint, sorted
    int a = 0, b = U.nwr;
    while (a < b) {
        int k = (a + b) / 2;
        if (U.wr[k].hi <= lo) a = k + 1;
        else b = k;
    }
    return a < U.nwr && U.wr[a].lo < hi;
}

static void summary(void) {
    plat_mutex_lock(&cap_lock);
    if (C.subs + C.off_subs < 3600) {
        plat_mutex_unlock(&cap_lock);
        return;
    }
    double s = C.subs ? (double)C.subs : 1.0, r = U.subs ? (double)U.subs : 1.0;
    rt_log("vk_usnap: %llu submissions with a snapshot, %llu without; per submission %.0f ranges, %.1f KiB copied in "
           "%.3f ms, %.0f uniform bindings from the copy, %.1f dropped by GPU writes, %.1f left out (GPU writes in "
           "flight), %.1f refused (not an imported pool)",
           (unsigned long long)C.subs, (unsigned long long)C.off_subs, C.regions / s, C.bytes / s / 1024.0,
           C.copy_ns / s / 1e6, U.hits / r, U.drops / r, U.inflight / r, C.refused / s);
    C.subs = C.off_subs = C.regions = C.bytes = C.copy_ns = C.refused = 0;
    plat_mutex_unlock(&cap_lock);
    U.subs = U.hits = U.drops = U.inflight = 0;
}

// The submit call: the uniform ranges the submission binds, merged, those that lie wholly in one imported pool copied.
// NULL when the snapshot is off for this submission.
void *vk_usnap_capture(const uint64_t *handles, int n) {
    if (!RT_KNOB_ON(mode())) {
        plat_mutex_lock(&cap_lock);
        C.off_subs++;
        plat_mutex_unlock(&cap_lock);
        return NULL;
    }
    UCap *cap = calloc(1, sizeof *cap);
    if (!cap) rt_fatal("vk_usnap: out of memory");
    uint64_t t0 = kern_now_ns();
    plat_mutex_lock(&cap_lock);
    C.ncol = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) collect(nvn_op_first(handles[i]), 0);
    sort_col();
    int creg = 0;
    uint64_t total = 0;
    for (int i = 0; i < C.ncol;) {
        uint64_t lo = C.col[i].lo, hi = C.col[i].hi;
        for (i++; i < C.ncol && C.col[i].lo <= hi; i++)
            if (C.col[i].hi > hi) hi = C.col[i].hi;
        VkMemSpan sp;
        if (!vk_mem_resolve(lo, hi - lo, &sp) || sp.alloc || !sp.pool || sp.avail < hi - lo ||
            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC)) {
            C.refused++;
            continue;
        }
        if (cap->nreg == creg) cap->reg = grow(cap->reg, &creg, sizeof *cap->reg);
        cap->reg[cap->nreg++] = (Reg){lo, hi, total, 0};
        total += (hi - lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    }
    cap->done = vk_sync_fence_done();   // before the copy: a write-back done later is not in its bytes
    if (cap->nreg && (cap->arena = arena_get(total))) {
        for (int i = 0; i < cap->nreg; i++)
            memcpy(cap->arena->map + cap->reg[i].off, (const void *)(uintptr_t)cap->reg[i].lo,
                   cap->reg[i].hi - cap->reg[i].lo);
        vmaFlushAllocation(vk_ctx()->vma, cap->arena->alloc, 0, total);   // a no-op on coherent memory
        C.subs++, C.regions += (uint64_t)cap->nreg, C.bytes += total, C.copy_ns += kern_now_ns() - t0;
    }
    plat_mutex_unlock(&cap_lock);
    return cap;
}

void vk_usnap_capture_drop(void *p) {
    UCap *cap = p;
    if (!cap) return;
    if (cap->arena) arena_done(cap->arena);
    free(cap->reg);
    free(cap);
}

// The replay, at the start of a submission's recording: the capture's copies serve its uniform bindings, but for the
// ranges a GPU write of a submission that was not done at the capture touches (the copy lacks its bytes).
void vk_usnap_begin(void *p, uint64_t fence) {
    UCap *cap = p;
    U.on = 0, U.fence = fence, U.gen++;
    summary();
    free(U.reg);
    U.reg = NULL, U.nreg = 0;
    wr_prune(cap ? cap->done : vk_sync_fence_done());
    if (!cap) return;
    U.reg = cap->reg, U.nreg = cap->nreg, U.arena = cap->arena;
    free(cap);
    if (!U.arena) return;
    for (int i = 0; i < U.nreg; i++)
        if (wr_hits(U.reg[i].lo, U.reg[i].hi)) U.reg[i].dropped = 1, U.inflight++;
    U.on = 1;
    U.subs++;
}

void vk_usnap_end(uint64_t fence) {
    if (U.arena) vk_completion_add(fence, arena_done, U.arena), U.arena = NULL;
    U.on = 0, U.gen++;
}
// First region whose end is above a.
static int first_above(uint64_t a) {
    int lo = 0, hi = U.nreg;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (U.reg[m].hi <= a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    if (!U.on || !n) return 0;
    int i = first_above(addr);
    if (i >= U.nreg) return 0;
    const Reg *r = &U.reg[i];
    if (r->dropped || addr < r->lo || addr + n > r->hi) return 0;
    uint64_t off = r->off + (addr - r->lo);
    if (off % align) return 0;
    *out = (VkDescriptorBufferInfo){U.arena->buf, off, n};
    if (vk_capture_on) vk_capture_note(addr, n);   // XC2_CAPTURE: a uniform block read from the submit snapshot
    U.hits++;
    return 1;
}

// The bytes [addr, addr+n) as the submission's snapshot holds them, copied to dst; 0 when the snapshot does not hold all of
// them (not captured, dropped by a GPU write, or outside the captured ranges).
int vk_usnap_read(uint64_t addr, uint64_t n, void *dst) {
    if (!U.on || !n) return 0;
    int i = first_above(addr);
    if (i >= U.nreg) return 0;
    const Reg *r = &U.reg[i];
    if (r->dropped || addr < r->lo || addr + n > r->hi) return 0;
    memcpy(dst, U.arena->map + r->off + (addr - r->lo), n);
    return 1;
}

void vk_usnap_drop(uint64_t addr, uint64_t n) {
    if (!__atomic_load_n(&umode, __ATOMIC_RELAXED) || !n) return;
    if (U.nwr == U.cwr) U.wr = grow(U.wr, &U.cwr, sizeof *U.wr);
    U.wr[U.nwr++] = (Wrote){addr, addr + n, U.fence};
    if (!U.on) return;
    for (int i = first_above(addr); i < U.nreg && U.reg[i].lo < addr + n; i++)
        if (!U.reg[i].dropped) U.reg[i].dropped = 1, U.drops++, U.gen++;
}

uint64_t vk_usnap_gen(void) { return U.gen; }

// The arenas back on the free list (vk_exec_draw_shutdown: the device is idle, every completion ran).
void vk_usnap_shutdown(void) {
    plat_mutex_lock(&free_lock);
    for (Arena *a = free_arenas, *next; a; a = next) next = a->next, vmaDestroyBuffer(vk_ctx()->vma, a->buf, a->alloc), free(a);
    free_arenas = NULL;
    plat_mutex_unlock(&free_lock);
}
