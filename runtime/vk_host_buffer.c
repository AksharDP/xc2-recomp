// Host-coherent mapped buffers for CPU writes the GPU reads without a flush (vk_host_buffer.h).
// tests/draw_buffer_memory_test.py compiles this file alone against a stand-in allocator, so it includes only the
// allocator header and dispatch.h (rt_fatal).
#include "vk_host_buffer.h"
#include "dispatch.h"

void *vk_host_buffer(VmaAllocator vma, VkDeviceSize size, VkBufferUsageFlags usage, const char *what, VkBuffer *buf,
                     VmaAllocation *alloc) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    // RANDOM access lets the allocator pick cached memory, which may be non-coherent; requiring coherence rules that out.
    // Coherent memory also keeps a flush from writing back over ranges a shader wrote when a later split submission
    // appends to the same scratch chunk.
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(vma, &bi, &ai, buf, alloc, &info);
    if (res != VK_SUCCESS) rt_fatal("vk: %s of %llu bytes failed (%d)", what, (unsigned long long)size, (int)res);
    // The desktop drivers offer cached memory that is also coherent, so the allocator picks the same memory with or
    // without requiredFlags there; this check catches a driver or allocator that hands back something else.
    VkMemoryPropertyFlags flags = 0;
    vmaGetAllocationMemoryProperties(vma, *alloc, &flags);
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if ((flags & need) != need || !info.pMappedData)
        rt_fatal("vk: %s of %llu bytes got memory without host-visible coherent access (flags %#x, mapped %d)", what,
                 (unsigned long long)size, (unsigned)flags, info.pMappedData != NULL);
    return info.pMappedData;
}
