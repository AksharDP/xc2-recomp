// FSR 3.1 and FSR 3 frame interpolation built into the game (vk_ffx.cpp): the FidelityFX SDK 1.1.4's effects from source
// (tools/ffx_sdk.py), without a library of AMD's. vk_fsr3.c uses the upscaler where no library is next to the executable
// (Linux has none), vk_fg.c the interpolation where AMD's frame generation swapchain is not to be had.
#pragma once
#ifdef __cplusplus
#include <vulkan/vulkan.h>   // C++ parts of the headers below stay outside the C linkage
#include <vk_mem_alloc.h>
extern "C" {
#endif
#include "vk.h"

// 1 when this device can run the built-in shaders; else 0 with what it lacks in *why (a static string).
int vk_ffx_usable(const VkCtx *c, const char **why);

// ---- the upscaler (the replay thread)
typedef struct {
    VkImage colour;              // R8G8B8A8_UNORM, w x h, SHADER_READ_ONLY_OPTIMAL
    VkImage depth;               // R32_SFLOAT, w x h, SHADER_READ_ONLY_OPTIMAL
    VkImage motion;              // R16G16B16A16_SFLOAT in pixels, w x h, SHADER_READ_ONLY_OPTIMAL
    VkImage out;                 // R16G16B16A16_SFLOAT, ow x oh, GENERAL
    uint32_t w, h;               // size of the three input images
    uint32_t rw, rh;             // the rendered part of them
    uint32_t ow, oh;             // size of the output
    float jitter[2];             // in pixels
    float near_z, far_z, fov;
    float frame_ms;
    int reset;                   // the history does not belong to this picture
    uint64_t fence;              // the timeline value of the submission being recorded, 0 unknown
} VkFfxUpscale;

// A context for input images of w x h and an output of ow x oh, and its end. Both with the device idle or the context
// never used. 1 on success.
int vk_ffx_up_create(VkCtx *c, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh);
void vk_ffx_up_destroy(void);
// Whether the upscaler, the interpolation below, can be set up: a small context of its own, made and destroyed. Any thread.
int vk_ffx_up_check(VkCtx *c);
int vk_ffx_fg_check(VkCtx *c);
// Records the upscale into cb. The recorder thread must have nothing queued for cb (rec_drain). 1 on success.
int vk_ffx_up_dispatch(VkCommandBuffer cb, const VkFfxUpscale *d);

// ---- frame interpolation (the present thread)
// The contexts and the pictures for a w x h window texture and scenes of up to rw x rh, and their end, with the device
// idle or nothing recorded yet. 1 on success.
int vk_ffx_fg_create(VkCtx *c, uint32_t w, uint32_t h, uint32_t rw, uint32_t rh);
void vk_ffx_fg_destroy(void);
// 1 when the contexts exist and fit such a frame
int vk_ffx_fg_fits(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh);
// Records the interpolation between the last recorded frame and this one into cb: win is the window texture, the
// picture with the HUD (GENERAL; its UNORM format fmt), fr the frame's other inputs. reset: the last recorded frame is
// not the one before this (the result then is this frame). The generated picture (R8G8B8A8_UNORM, GENERAL), or
// VK_NULL_HANDLE on failure.
VkImage vk_ffx_fg_record(VkCommandBuffer cb, const VkFsrFrame *fr, VkImage win, VkFormat fmt, int reset);

#ifdef __cplusplus
}
#endif
