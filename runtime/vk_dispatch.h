// Command entry points shared by the core and extension renderer paths. No runtime dependencies: resolver tests
// supply a fake vkGetDeviceProcAddr. The extension path still requires the extensions to be enabled on the device.
#pragma once
#include <string.h>
#include <vulkan/vulkan.h>

typedef struct {
    PFN_vkCmdBeginRendering BeginRendering;
    PFN_vkCmdEndRendering EndRendering;
    PFN_vkCmdPipelineBarrier2 PipelineBarrier2;
    PFN_vkCmdWriteTimestamp2 WriteTimestamp2;
    PFN_vkCmdBindVertexBuffers2 BindVertexBuffers2;
    PFN_vkQueueSubmit2 QueueSubmit2;
} VkApiDispatch;

extern VkApiDispatch vk_api;

static inline int vk_api_load(VkApiDispatch *out, VkDevice device, PFN_vkGetDeviceProcAddr get, int extensions) {
    VkApiDispatch next = {0};
    memset(out, 0, sizeof *out);
    if (!get) return 0;
#define LOAD(field, core, ext) next.field = (PFN_##core)get(device, extensions ? #ext : #core)
    LOAD(BeginRendering, vkCmdBeginRendering, vkCmdBeginRenderingKHR);
    LOAD(EndRendering, vkCmdEndRendering, vkCmdEndRenderingKHR);
    LOAD(PipelineBarrier2, vkCmdPipelineBarrier2, vkCmdPipelineBarrier2KHR);
    LOAD(WriteTimestamp2, vkCmdWriteTimestamp2, vkCmdWriteTimestamp2KHR);
    LOAD(BindVertexBuffers2, vkCmdBindVertexBuffers2, vkCmdBindVertexBuffers2EXT);
    LOAD(QueueSubmit2, vkQueueSubmit2, vkQueueSubmit2KHR);
#undef LOAD
    if (!next.BeginRendering || !next.EndRendering || !next.PipelineBarrier2 || !next.WriteTimestamp2 ||
        !next.BindVertexBuffers2 || !next.QueueSubmit2) return 0;
    *out = next;
    return 1;
}
