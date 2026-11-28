#include <stdarg.h>
// Include the implementation so static helpers follow the production code.
#include "../runtime/vk_memory.c"
int callcap_level;
unsigned rt_trace;
volatile int vk_capture_on;
_Noreturn void fixture_unexpected(void) {
    fputs("unexpected external service\n", stderr);
    exit(91);
}
void vk_texture_jobs_wait(void) {}
static VkCtx ctx;
static VkBuffer in_flight;
// The fake queue retains uses until QueueWaitIdle, even after host recording ends.
static int waits, copies, destroys;
VkCtx *vk_ctx(void) { return &ctx; }
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    exit(90);
}
void rt_log(const char *fmt, ...) { (void)fmt; }
// Recording finishes before finalization, as required by the backend drain.
// The fixture is single-threaded; GPU progress is modeled independently below.
void plat_mutex_lock(PlatMutex *p) { (void)p; }
void plat_mutex_unlock(PlatMutex *p) { (void)p; }
void plat_mutex_lock_shared(PlatMutex *p) { (void)p; }
void plat_mutex_unlock_shared(PlatMutex *p) { (void)p; }
void plat_rmutex_lock(PlatRMutex *p) { (void)p; }
void plat_rmutex_unlock(PlatRMutex *p) { (void)p; }
void plat_mutex_init(PlatMutex *p) { (void)p; }
void plat_once(PlatOnce *o, void (*fn)(void *), void *a) {
    if (!o->state) {
        o->state = 1;
        fn(a);
    }
}
uint64_t vk_sync_fence_done(void) { return 0; }
uint64_t vk_sync_issued(void) { return 1; }
int vk_sync_fence_wait(uint64_t x, uint64_t y) {
    (void)x;
    (void)y;
    return 1;
}
void vk_texture_range_gone(uint64_t a, uint64_t b, VmaAllocation c) {
    (void)a;
    (void)b;
    (void)c;
}
void mem_thaw(uint64_t a, uint64_t b) {
    (void)a;
    (void)b;
}
void vk_queue_lock(void) {}
void vk_queue_unlock(void) {}
void vk_completion_add(uint64_t f, void (*cb)(void *), void *a) {
    (void)f;
    (void)cb;
    (void)a;
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n,
                                           const VkBufferCopy *r) {
    (void)cb;
    (void)src;
    (void)n;
    (void)r;
    copies++;
    in_flight = dst;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue q) {
    (void)q;
    waits++;
    in_flight = VK_NULL_HANDLE;
    return VK_SUCCESS;
}
void vmaDestroyBuffer(VmaAllocator a, VkBuffer b, VmaAllocation c) {
    (void)a;
    (void)c;
    destroys++;
    if (b == in_flight) {
        fprintf(stderr, "destroyed in-flight capture destination\n");
        exit(42);
    }
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    int staged = !strcmp(argv[1], "staged");
    int live = !strcmp(argv[1], "live");
    _Alignas(4096) uint8_t guest[4096] = {0}, shadow[4096] = {0};
    uint8_t arena[32] = {0};
    memset(guest, 0x5a, sizeof guest);
    pool_t key = {.mem = guest, .size = sizeof guest};
    PoolRec *r = calloc(1, sizeof *r);
    r->key = &key;
    r->kind = POOL_IMPORT;
    r->lo = (uintptr_t)guest;
    r->size = sizeof guest;
    r->import_lo = r->lo;
    r->import_hi = r->lo + r->size;
    r->smap = shadow;
    r->salloc = (VmaAllocation)(uintptr_t)3;
    r->buf = (VkBuffer)(uintptr_t)2;
    index_put(r);
    s_live_n = 1;
    s_live_bytes = sizeof guest;
    // This models a capture of an unrelated pool, with no call to resolve_locked.
    if (staged || live) {
        StageArena st = {.buf = (VkBuffer)(uintptr_t)1, .map = arena, .size = 32};
        CRange range = {r, 0, 32, live ? CAP_LIVE : 0};
        MCap cap = {.shadow = 1, .st = &st, .c = &range, .n = 1};
        apply_shadow((VkCommandBuffer)(uintptr_t)4, &cap, 1);
        if (live && memcmp(guest, shadow, 32)) {
            fputs("live capture did not copy the CPU bytes\n", stderr);
            return 44;
        }
    }
    // The backend has drained recording/submission before entering the finalizer.
    // Its hidden copy can still be in flight on the fake GPU.
    vk_pool_fini(&key);
    if (destroys != 1 || waits != staged || copies != staged) {
        fprintf(stderr, "wrong counts %d %d %d\n", destroys, waits, copies);
        return 43;
    }
    return 0;
}
