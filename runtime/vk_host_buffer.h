#pragma once
// Mapped buffers the CPU writes in place and the GPU reads with no flush in between: the draw scratch chunks and the
// zero stand-in buffer (vk_exec_draw.c). Nothing flushes or invalidates them, so their memory has to be host-coherent.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"   // as in vk.h
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop

// Creates a mapped buffer of size bytes and usage in host-coherent memory and returns its mapping. Stops the game,
// naming `what`, when the allocation fails or the memory the allocator chose is not host-visible and host-coherent.
void *vk_host_buffer(VmaAllocator vma, VkDeviceSize size, VkBufferUsageFlags usage, const char *what, VkBuffer *buf,
                     VmaAllocation *alloc);
