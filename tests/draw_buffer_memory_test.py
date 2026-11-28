"""Check that CPU-written draw buffers get host-coherent memory, without a GPU or game files.

The draw scratch chunks and the zero stand-in buffer (runtime/vk_exec_draw.c) are written by the CPU and read by the GPU
with no flush in between, so runtime/vk_host_buffer.c has to ask for host-coherent memory and refuse anything else. The
test compiles that whole file against a stand-in vk_mem_alloc.h whose allocator prefers cached memory that is not
coherent, as VMA may for HOST_ACCESS_RANDOM on hardware that offers such a type.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Only the names runtime/vk_host_buffer.c uses; designated initializers make the field order irrelevant.
STUB_VMA = r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint64_t VkDeviceSize;
typedef int VkResult;
typedef uint32_t VkFlags;
typedef VkFlags VkBufferUsageFlags;
typedef VkFlags VkMemoryPropertyFlags;
enum { VK_SUCCESS = 0, VK_ERROR_OUT_OF_DEVICE_MEMORY = -2 };
enum { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12 };
enum { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 2, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 4,
       VK_MEMORY_PROPERTY_HOST_CACHED_BIT = 8 };
enum { VMA_MEMORY_USAGE_AUTO_PREFER_HOST = 9 };
enum { VMA_ALLOCATION_CREATE_MAPPED_BIT = 4, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT = 0x800 };
typedef struct VmaAllocator_T *VmaAllocator;
typedef struct VmaAllocation_T *VmaAllocation;
typedef struct VkBuffer_T *VkBuffer;
typedef struct { int sType; const void *pNext; VkFlags flags; VkDeviceSize size; VkBufferUsageFlags usage; }
    VkBufferCreateInfo;
typedef struct { VkFlags flags; int usage; VkMemoryPropertyFlags requiredFlags, preferredFlags; } VmaAllocationCreateInfo;
typedef struct { void *pMappedData; } VmaAllocationInfo;
VkResult vmaCreateBuffer(VmaAllocator vma, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai,
                         VkBuffer *buf, VmaAllocation *alloc, VmaAllocationInfo *info);
void vmaGetAllocationMemoryProperties(VmaAllocator vma, VmaAllocation alloc, VkMemoryPropertyFlags *flags);
'''

# The stand-in allocator and the cases. argv[1] picks how the allocator behaves:
#   pick      VMA's rule: the first type that has every required flag, cached non-coherent memory first.
#   ignore    hands back the non-coherent type whatever was required (a broken driver or allocator).
#   unmapped  allocates coherent memory but returns no mapping.
#   fail      the allocation fails.
# Only pick may succeed; the others must end in rt_fatal, which exits with 42.
FIXTURE = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk_host_buffer.h"

static const VkMemoryPropertyFlags types[] = {
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
struct VmaAllocation_T { VkMemoryPropertyFlags flags; uint8_t *host, *device; VkDeviceSize size; };
static const char *mode;
static VkBufferCreateInfo seen_bi;
static VmaAllocationCreateInfo seen_ai;

_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(42);
}

VkResult vmaCreateBuffer(VmaAllocator vma, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai,
                         VkBuffer *buf, VmaAllocation *alloc, VmaAllocationInfo *info) {
    (void)vma;
    seen_bi = *bi, seen_ai = *ai;
    if (!strcmp(mode, "fail")) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryPropertyFlags need = ai->requiredFlags | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    int t = 0;
    if (strcmp(mode, "ignore"))
        while (t < 2 && (types[t] & need) != need) t++;
    if (t == 2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    struct VmaAllocation_T *a = calloc(1, sizeof *a);
    a->flags = types[t], a->size = bi->size;
    a->host = calloc(1, (size_t)bi->size);
    // Without coherence the GPU reads its own copy, which a CPU write does not reach until a flush.
    a->device = types[t] & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? a->host : calloc(1, (size_t)bi->size);
    *alloc = a;
    *buf = (VkBuffer)a;
    info->pMappedData = strcmp(mode, "unmapped") ? a->host : NULL;
    return VK_SUCCESS;
}

void vmaGetAllocationMemoryProperties(VmaAllocator vma, VmaAllocation alloc, VkMemoryPropertyFlags *flags) {
    (void)vma;
    *flags = alloc->flags;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    mode = argv[1];
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *p = vk_host_buffer(NULL, 4096, 0x30, "test buffer", &buf, &alloc);
    if (seen_bi.size != 4096 || seen_bi.usage != 0x30 || !(seen_ai.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT)) {
        fprintf(stderr, "size, usage or MAPPED not passed to the allocator\n");
        return 3;
    }
    memcpy(p + 100, "visible", 7);
    if (memcmp(alloc->device + 100, "visible", 7)) {
        fprintf(stderr, "the GPU does not see the CPU write without a flush\n");
        return 1;
    }
    return 0;
}
'''


class HostBufferTest(unittest.TestCase):
    def test_cpu_written_buffers_are_host_coherent(self):
        default_cc = r'C:\msys64\ucrt64\bin\clang.exe' if os.name == 'nt' else 'clang'
        cc = shutil.which(os.environ.get('CC', default_cc))
        if not cc:
            self.skipTest('Clang required')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'vk_mem_alloc.h').write_text(STUB_VMA, encoding='utf-8')
            src, exe = tmp / 'host_buffer.c', tmp / 'host_buffer.exe'
            src.write_text(FIXTURE, encoding='utf-8')
            # The stand-in directory comes first, so vk_host_buffer.h finds the stand-in vk_mem_alloc.h.
            built = subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-I', str(tmp), '-I', str(ROOT / 'runtime'),
                                    str(src), str(ROOT / 'runtime' / 'vk_host_buffer.c'), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for mode, rc in (('pick', 0), ('ignore', 42), ('unmapped', 42), ('fail', 42)):
                with self.subTest(allocator=mode):
                    run = subprocess.run([str(exe), mode], capture_output=True, text=True, timeout=10)
                    self.assertEqual(run.returncode, rc, run.stdout + run.stderr)
                    if rc == 42:
                        self.assertIn('test buffer', run.stderr)


if __name__ == '__main__':
    unittest.main()
