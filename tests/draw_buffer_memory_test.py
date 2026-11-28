"""Exercise the draw buffer allocation paths with a simulated VMA memory selector.

The C fixture compiles scratch() and dummies_init()'s allocation/zeroing prefix
from the actual source. It stops before image/view creation; no Vulkan loader,
GPU, game files or third-party headers are needed. The allocator prefers cached
noncoherent memory unless the caller explicitly requires coherent memory.
DRAW_BUFFER_TEST_SOURCE may select an unchanged source for before/after checks.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef uint64_t VkDeviceSize;
typedef int VkResult;
enum { VK_SUCCESS, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
enum { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 1, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 2,
       VK_MEMORY_PROPERTY_HOST_CACHED_BIT = 4 };
enum { VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 2,
       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT = 8,
       VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT = 32,
       VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT = 64, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT = 128 };
enum { VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT = 1,
       VMA_ALLOCATION_CREATE_MAPPED_BIT = 2 };
typedef struct Allocation { uint8_t *host, *device; uint64_t size; unsigned type; } *VmaAllocation;
typedef VmaAllocation VkBuffer;
typedef struct { int sType; uint64_t size; unsigned usage; } VkBufferCreateInfo;
typedef struct { int usage; unsigned flags, requiredFlags; } VmaAllocationCreateInfo;
typedef struct { void *pMappedData; } VmaAllocationInfo;
typedef struct { void *vma; } VkCtx;
typedef struct { uint64_t fence; } VkExec;
static VkCtx ctx;
static VkCtx *vk_ctx(void) { return &ctx; }
static _Noreturn void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
static void plat_mutex_lock(int *m) { (void)m; }
static void plat_mutex_unlock(int *m) { (void)m; }
static VkResult vmaCreateBuffer(void *vma, const VkBufferCreateInfo *bi,
        const VmaAllocationCreateInfo *ai, VkBuffer *buf, VmaAllocation *alloc, VmaAllocationInfo *info) {
    (void)vma;
    // Both are valid host-visible choices. RANDOM access prefers the cached one.
    const unsigned types[] = { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT };
    unsigned required = ai->requiredFlags;
    if (ai->flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) required |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    unsigned selected = 0;
    while (selected < 2 && (types[selected] & required) != required) selected++;
    if (selected == 2 || !(ai->flags & VMA_ALLOCATION_CREATE_MAPPED_BIT)) abort();
    VmaAllocation a = calloc(1, sizeof *a);
    a->size = bi->size; a->type = selected;
    a->host = malloc((size_t)a->size);
    a->device = selected == 1 ? a->host : malloc((size_t)a->size);
    memset(a->host, 0xa5, (size_t)a->size);
    if (a->device != a->host) memset(a->device, 0x5a, (size_t)a->size);
    *buf = *alloc = a; info->pMappedData = a->host;
    return VK_SUCCESS;
}
/* SOURCE_DECLARATIONS */
typedef struct { Chunk *chunks; int nchunks, cchunks; } CbRes;
static CbRes resources;
static CbRes *cb_res(VkExec *ex) { (void)ex; return &resources; }
static int spare_lock, nspare_chunk;
static Chunk spare_chunk[1];
static struct { int ready; VkBuffer zbuf; VmaAllocation zalloc; } Z;
/* SOURCE_FUNCTIONS */
static int same(VkBuffer buf, uint64_t off, const void *p, size_t n) {
    if (off + n > buf->size) abort();
    return !memcmp(buf->device + off, p, n);
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "zero")) {
        dummies_init();
        for (unsigned i = 0; i < ZERO_BYTES; i++)
            if (Z.zbuf->device[i]) { fprintf(stderr, "GPU zero buffer is not initialized\n"); return 1; }
        return Z.zbuf->type != 1;
    }
    VkExec ex = {.fence = 1}; VkBuffer first, second; VkDeviceSize off;
    uint8_t *p = scratch(&ex, 7, &first, &off);
    if (off || resources.nchunks != 1) return 2;
    memcpy(p, "support", 7);
    if (!same(first, off, "support", 7)) { fprintf(stderr, "GPU cannot see CPU scratch writes\n"); return 1; }
    if (!strcmp(argv[1], "reused")) {
        spare_chunk[nspare_chunk++] = resources.chunks[0];
        free(resources.chunks); memset(&resources, 0, sizeof resources);
        p = scratch(&ex, 9, &second, &off);
        if (off || second != first || nspare_chunk) return 2;
        memcpy(p, "recycled!", 9);
        if (!same(second, off, "recycled!", 9)) return 1;
    } else {
        // Model a completed earlier split's GPU write, then append a CPU allocation.
        first->device[0] = 0x36;
        p = scratch(&ex, 17, &second, &off);
        if (second != first || off != 256) return 2;
        memset(p, 0x7b, 17);
        if (!same(second, off, p, 17) || first->device[0] != 0x36) return 1;
        p = scratch(&ex, CHUNK_BYTES + 1, &second, &off);
        if (off || second == first || resources.nchunks != 2) return 2;
        p[CHUNK_BYTES] = 0x42;
        if (second->device[CHUNK_BYTES] != 0x42) return 1;
    }
    return first->type != 1 || second->type != 1;
}
'''


class DrawBufferMemoryTest(unittest.TestCase):
    def test_cpu_initialized_draw_buffers_use_visible_coherent_memory(self):
        default_cc = r'C:\msys64\ucrt64\bin\clang.exe' if os.name == 'nt' else 'clang'
        cc = shutil.which(os.environ.get('CC', default_cc))
        if not cc:
            self.skipTest('Clang required')
        path = Path(os.environ.get('DRAW_BUFFER_TEST_SOURCE', ROOT / 'runtime/vk_exec_draw.c'))
        source = path.read_text(encoding='utf-8')
        chunk = next(line for line in source.splitlines() if line.startswith('typedef struct { VkBuffer buf;'))
        defines = '\n'.join(line for line in source.splitlines()
                            if line.startswith(('#define CHUNK_BYTES ', '#define ZERO_BYTES ')))
        grow = source[source.index('static void *grow('):source.index('// ---- resources of one command buffer')]
        scratch = source[source.index('static uint8_t *scratch('):source.index('static void note_written(')]
        # The remaining dummies_init code creates views, samplers and images; this
        # test executes its buffer allocation and zeroing before that first view.
        zero = source[source.index('static void dummies_init('):source.index('    VkBufferViewCreateInfo bv =')]
        fixture = FIXTURE.replace('/* SOURCE_DECLARATIONS */', defines + '\n' + chunk)
        fixture = fixture.replace('/* SOURCE_FUNCTIONS */', grow + scratch + zero + '}\n')
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'buffers.c', Path(tmp) / 'buffers.exe'
            src.write_text(fixture, encoding='utf-8')
            built = subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for mode in ('fresh', 'reused', 'zero'):
                with self.subTest(path=mode):
                    run = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=10)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
