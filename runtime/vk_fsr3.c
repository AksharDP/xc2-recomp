// AMD FidelityFX Super Resolution 3.1 in place of XC2's temporal anti-aliasing and final scaling (XC2_FSR3, applies at the
// next frame).
//
// XC2 renders with a jittered camera at the 3D render scale, and keeps that size through its temporal resolve, bloom and
// tone map. Only the first draw of the HUD pass stretches the picture to the output size. So FSR runs there, on the
// tone-mapped colour: earlier it could only work at the reduced size. Three steps, found by following the picture from
// image to image (vk_fsr3_note):
//
// 1. The resolve, one full-screen draw with five colour targets. It reads the scene colour (texture slot 0), a motion
//    target (slot 4), depth (slot 7), and in fragment block c4 the jitter ([2].xy, half the projection's offsets), the
//    rendered size ([3].xy), the factor from motion units to pixels ([4].xy), and far - near, far * near and far
//    ([5].xyz); vertex block c3 holds the tangent of half the vertical field of view at [2].y. After it depth and motion
//    are saved, as the game reuses those targets, and target 4, the picture the frame goes on with, gets the scene colour
//    in place of the game's result: FSR needs the jittered frames. The draw itself stays, so the game's history and its
//    other targets are untouched.
// 2. The tone map, the single-target draw that reads target 4. Its target is noted.
// 3. The stretch, the draw that reads that target. Once it is recorded the pass ends, FSR replaces what it drew, and the
//    next draw begins the pass again (it loads its targets): the HUD goes on top at full size.
//
// If FSR is unavailable step 1 is skipped and the game's own path runs. If the steps stop completing, the raw jittered
// picture would stay on screen, so after MAX_MISSES resolves without a stretch FSR turns itself off.
//
// The jitter comes from the submission's uniform snapshot: the game may already be writing the next frame's block while
// the replay records this one, and a jitter one frame off would blur the picture.
//
// amd_fidelityfx_vk.dll, AMD's signed build from the FidelityFX SDK, sits next to xc2.exe and is loaded when the setting
// first applies. Its Vulkan header is C++ only, so the one structure used from it is declared below. It records straight
// into the command buffer, so the recorder thread (vk_rec.h) is drained first.
#include "vk_exec.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "fsr3_prep_spv.h"
#include "host.h"
#include "platform/platform.h"

#ifndef _WIN32
#define __declspec(x)   // the headers export their prototypes; elsewhere the library is simply not found
#endif
#include "ffx_api/ffx_upscale.h"

// FidelityFX SDK 1.1.4, ffx-api/include/ffx_api/vk/ffx_api_vk.h, ffxCreateBackendVKDesc (MIT, ffx_api/LICENSE.txt)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 0x0000003u
struct ffxCreateBackendVKDesc {
    ffxCreateContextDescHeader header;
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
};

#define FFX_DLL "amd_fidelityfx_vk.dll"
#define SCENE_FORMAT VK_FORMAT_B10G11R11_UFLOAT_PACK32
#define PICTURE_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define FOV_DEFAULT 0.7853982f   // 45 degrees, what the dumped camera uses
#define MAX_MISSES 8

enum { IMG_COLOUR, IMG_DEPTH, IMG_MOTION, IMG_OUT, IMG_N };
enum { STAGE_NONE, STAGE_RESOLVED, STAGE_TONED };

static const VkFormat image_format[IMG_N] = {PICTURE_FORMAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                             VK_FORMAT_R16G16B16A16_SFLOAT};
static const uint32_t surface_format[IMG_N] = {FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_SURFACE_FORMAT_R32_FLOAT,
                                               FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                                               FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT};

static struct {
    int tried, ok;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch dispatch;
    ffxContext ctx;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline prep;
    VkSampler sampler;
    VkImage img[IMG_N];
    VmaAllocation alloc[IMG_N];
    VkImageView view[IMG_N];
    uint32_t w, h;                 // size of the three input images
    uint32_t ow, oh;               // size of the output image, and with w and h the context's
    uint64_t last_frame, last_ns;  // the last frame FSR ran in
    uint32_t warned;               // bits of warn_once
    int logged;
    // the frame being recorded
    int stage, misses;
    VkImageView in[2];             // the resolve's depth and motion
    VkImage scene;                 // the resolve's scene colour
    uint64_t scene_addr;           // address of its target 4, which the tone map reads
    VkImage tone;                  // the tone map's target
    uint64_t tone_addr;            // its address, which the stretch reads
    uint32_t rw, rh;               // rendered size
    float jitter[2];               // in pixels, as FSR takes it
    float to_pixels[2];
    float near_z, far_z, fov;
} F;

static void warn_once(uint32_t bit, const char *what) {
    if (F.warned & bit) return;
    F.warned |= bit;
    rt_log("vk: FSR 3.1 skipped, the game's own picture stays: %s", what);
}

static void on_message(uint32_t type, const wchar_t *message) {
    char b[256];
    size_t i = 0;
    for (; message && message[i] && i + 1 < sizeof b; i++) b[i] = message[i] < 128 ? (char)message[i] : '?';
    b[i] = 0;
    rt_log("vk: FSR 3.1 %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

static VkResult VKAPI_CALL no_object_name(VkDevice d, const VkDebugUtilsObjectNameInfoEXT *i) {
    (void)d, (void)i;
    return VK_SUCCESS;
}
static void VKAPI_CALL no_label_begin(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l) { (void)cb, (void)l; }
static void VKAPI_CALL no_label_end(VkCommandBuffer cb) { (void)cb; }

// The library's lookups of device functions. It calls what it gets without a check, so an empty answer crashes at address
// 0. It asks for the KHR names of functions that are core here, which a device only answers when that extension is
// enabled, and for debug labels, which need an instance extension the runtime does not enable and change no result.
static PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice device, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(device, name);
    size_t n = strlen(name);
    if (!f && n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(device, core);
    }
    if (!f && !strcmp(name, "vkSetDebugUtilsObjectNameEXT")) f = (PFN_vkVoidFunction)no_object_name;
    if (!f && !strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) f = (PFN_vkVoidFunction)no_label_begin;
    if (!f && !strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) f = (PFN_vkVoidFunction)no_label_end;
    if (!f) rt_log("vk: FSR 3.1: the device has no %s", name);
    return f;
}

// The library next to the executable: plat_lib_open searches only the system folder for a bare name.
static void *open_library(void) {
    char path[1024];
    if (!plat_exe_path(path, sizeof path - sizeof FFX_DLL)) return NULL;
    char *end = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > end) end = back;
    if (!end) return NULL;
    memcpy(end + 1, FFX_DLL, sizeof FFX_DLL);
    return plat_lib_open(path);
}

static int init(VkCtx *c) {
    F.tried = 1;
    void *lib = open_library();
    F.create = (PfnFfxCreateContext)plat_lib_sym(lib, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)plat_lib_sym(lib, "ffxDestroyContext");
    F.dispatch = (PfnFfxDispatch)plat_lib_sym(lib, "ffxDispatch");
    if (!F.create || !F.destroy || !F.dispatch) {
        rt_log("vk: FSR 3.1 off: " FFX_DLL " not found next to the executable");
        return 0;
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(c->phys, PICTURE_FORMAT, &fp);
    F.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || !F.push) {
        rt_log("vk: FSR 3.1 off: this device cannot blit to the picture format or push descriptors");
        return 0;
    }
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                              .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &F.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fsr3_prep_spv,
                                   .pCode = fsr3_prep_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &F.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &F.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &F.sampler) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: FSR 3.1 off: its pipeline objects could not be created");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = F.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &F.prep);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        F.prep = VK_NULL_HANDLE;
        rt_log("vk: FSR 3.1 off: its pipeline could not be created");
        return 0;
    }
    return F.ok = 1;
}

static void free_image(VkCtx *c, int i) {
    if (F.view[i]) vkDestroyImageView(c->device, F.view[i], NULL);
    if (F.img[i]) vmaDestroyImage(c->vma, F.img[i], F.alloc[i]);
    F.view[i] = VK_NULL_HANDLE;
    F.img[i] = VK_NULL_HANDLE;
}

static void free_all(VkCtx *c) {
    if (F.ctx) F.destroy(&F.ctx, NULL);
    F.ctx = NULL;
    for (int i = 0; i < IMG_N; i++) free_image(c, i);
    F.w = F.h = F.ow = F.oh = 0;
}

static int make_image(VkCtx *c, int i, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = image_format[i], .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &F.img[i], &F.alloc[i], NULL) != VK_SUCCESS) {
        F.img[i] = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = F.img[i],
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image_format[i],
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &F.view[i]) != VK_SUCCESS) {
        F.view[i] = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

static void wait_idle(VkCtx *c) {   // earlier frames may still read the old images; as vk_fsr.c ensure_images
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
}

// The input images for w x h. The game's sizes are fixed for the run, so this and ensure_output make them once.
static int ensure_input(VkCtx *c, uint32_t w, uint32_t h) {
    if (F.img[IMG_COLOUR] && F.w == w && F.h == h) return 1;
    if (F.img[IMG_COLOUR]) wait_idle(c);
    free_all(c);
    for (int i = 0; i < IMG_OUT; i++)
        if (!make_image(c, i, w, h)) {
            free_all(c);
            return 0;
        }
    F.w = w;
    F.h = h;
    return 1;
}

// The output image and the context, once both sizes are known
static int ensure_output(VkCtx *c, uint32_t w, uint32_t h) {
    if (F.ctx && F.ow == w && F.oh == h) return 1;
    if (F.ctx) {
        wait_idle(c);
        F.destroy(&F.ctx, NULL);
        F.ctx = NULL;
    }
    free_image(c, IMG_OUT);
    F.ow = F.oh = 0;
    if (!make_image(c, IMG_OUT, w, h)) return 0;
    struct ffxCreateBackendVKDesc backend = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
                                             .vkDevice = c->device, .vkPhysicalDevice = c->phys,
                                             .vkDeviceProcAddr = device_proc};
    struct ffxCreateContextDescUpscale desc = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                                          .pNext = &backend.header},
                                               .flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE,
                                               .maxRenderSize = {F.w, F.h}, .maxUpscaleSize = {w, h},
                                               .fpMessage = on_message};
    ffxReturnCode_t r = F.create(&F.ctx, &desc.header, NULL);
    if (r != FFX_API_RETURN_OK) {
        rt_log("vk: FSR 3.1 off: ffxCreateContext returned %u", r);
        F.ctx = NULL;
        F.ok = 0;
        return 0;
    }
    F.ow = w;
    F.oh = h;
    return 1;
}

// n bytes of a bound uniform block as of the submit call, else as they are now
static const float *block_of(const p_bind_buf *u, uint64_t n) {
    if (!u->addr || (u->size && u->size < n)) return NULL;
    const void *p = vk_usnap_host(u->addr, n);
    VkMemSpan ms;
    if (!p && vk_mem_resolve(u->addr, n, &ms)) p = (const void *)(uintptr_t)u->addr;
    return p;
}

// The texture bound to fragment slot `slot`
static const VkTexImage *bound(VkExec *ex, int slot) {
    view_t v;
    int hv = 0;
    uint64_t h = ex->tex[1][slot];
    return h ? vk_texture_by_id(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
}

// A view of t when it is what the resolve uses: of format `want`, or with VK_FORMAT_UNDEFINED any format with `aspect`
// (the depth format depends on the device, vk_formats.c)
static VkImageView input_view(const VkTexImage *t, VkFormat want, VkImageAspectFlags aspect) {
    if (!t || t->samples > 1 || !(t->aspect & aspect) || (want && t->format != want)) return VK_NULL_HANDLE;
    return vk_exec_target_view(t, t->format, VK_IMAGE_VIEW_TYPE_2D, aspect, 0, 0, 1, 0);
}

// Steps 2 and 3: a single-target draw that reads the picture of the step before
static void note_picture(VkExec *ex) {
    if (ex->pass_color[0] != PICTURE_FORMAT || !ex->color[0].img) return;
    const VkTexImage *in = bound(ex, 0);
    if (!in) return;
    if (F.stage == STAGE_RESOLVED && in->addr == F.scene_addr) {
        F.tone = ex->color[0].image;
        F.tone_addr = ex->color[0].img->addr;
        F.stage = STAGE_TONED;
    } else if (F.stage == STAGE_TONED && in->addr == F.tone_addr) {
        F.stage = STAGE_NONE;
        F.misses = 0;
        if (ensure_output(vk_ctx(), ex->pass_w, ex->pass_h)) ex->fsr3_armed = VK_FSR3_UPSCALE;
        else warn_once(2, "its output could not be created");
    }
}

void vk_fsr3_note(VkExec *ex) {
    if (ex->pass_ncolor == 1) {
        if (F.stage) note_picture(ex);
        return;
    }
    if (ex->pass_ncolor != 5 || ex->pass_depth || ex->pass_color[0] != SCENE_FORMAT || ex->pass_color[1] != SCENE_FORMAT ||
        ex->pass_color[2] != PICTURE_FORMAT || ex->pass_color[3] != VK_FORMAT_R16_SFLOAT ||
        ex->pass_color[4] != SCENE_FORMAT)
        return;
    int unfinished = F.stage != STAGE_NONE;
    F.stage = STAGE_NONE;
    if (!cfg_bool("XC2_FSR3")) return;
    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    if (!F.ok) return;
    const VkTexImage *scene = bound(ex, 0);
    F.in[0] = input_view(bound(ex, 7), VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_DEPTH_BIT);
    F.in[1] = input_view(bound(ex, 4), VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!scene || scene->format != SCENE_FORMAT || !F.in[0] || !F.in[1] || !ex->color[4].img)
        return;   // not the resolve: a pass at boot has the same targets
    if (unfinished && ++F.misses >= MAX_MISSES) {
        rt_log("vk: FSR 3.1 off: the tone map or the stretch to the output size was not found");
        F.ok = 0;
        return;
    }
    const float *c4 = block_of(&ex->ubo[1][1], 96), *c3 = block_of(&ex->ubo[0][0], 48);
    if (!c4 || c4[3 * 4] < 1.0f || c4[3 * 4 + 1] < 1.0f || c4[5 * 4 + 2] <= 0.0f)
        return warn_once(1, "the resolve's uniform block is not readable");
    if (!ensure_input(c, ex->pass_w, ex->pass_h)) return warn_once(4, "its images could not be created");
    F.rw = (uint32_t)c4[3 * 4] < F.w ? (uint32_t)c4[3 * 4] : F.w;
    F.rh = (uint32_t)c4[3 * 4 + 1] < F.h ? (uint32_t)c4[3 * 4 + 1] : F.h;
    // The projection moves the picture by -2 * c4[2].x in x and -2 * c4[2].y in y, in device coordinates with y up. FSR
    // takes pixels, and its y runs down the picture. Checked in Goldmouth: of the four sign choices this is the sharpest.
    F.jitter[0] = -c4[2 * 4] * (float)F.rw;
    F.jitter[1] = c4[2 * 4 + 1] * (float)F.rh;
    F.to_pixels[0] = c4[4 * 4];
    F.to_pixels[1] = c4[4 * 4 + 1];
    F.far_z = c4[5 * 4 + 2];
    F.near_z = c4[5 * 4 + 1] / F.far_z;
    F.fov = c3 && c3[2 * 4 + 1] > 0.0f ? 2.0f * atanf(c3[2 * 4 + 1]) : FOV_DEFAULT;
    F.scene = scene->image;
    F.scene_addr = ex->color[4].img->addr;
    F.stage = STAGE_RESOLVED;
    ex->fsr3_armed = VK_FSR3_RESOLVE;
}

static void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

static void layouts(VkCommandBuffer cb, int first, int n, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b[IMG_N];
    for (int i = 0; i < n; i++)
        b[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                       .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = F.img[first + i],
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = (uint32_t)n,
                          .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &d);
}

static void copy_picture(VkCommandBuffer cb, VkImage src, VkImage dst, VkImageLayout dst_layout, const VkExecTarget *t) {
    VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t ? t->level : 0, t ? t->base_layer : 0, 1},
                     .extent = {F.rw, F.rh, 1}};
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, dst_layout, 1, &r);
}

// Step 1: depth and motion into the upscaler's images, the jittered scene into target 4
static void record_resolve(VkExec *ex) {
    const VkExecTarget *out = &ex->color[4];
    VkCommandBuffer cb = ex->cb;
    if (!out->image || ex->fbl_mask >> 4 & 1) {   // a target in the feedback layout: the game's picture goes on
        F.stage = STAGE_NONE;
        return;
    }
    memory_barrier(cb);
    layouts(cb, IMG_DEPTH, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w[4];
    for (uint32_t i = 0; i < 4; i++) {
        ii[i] = i < 2 ? (VkDescriptorImageInfo){F.sampler, F.in[i], VK_IMAGE_LAYOUT_GENERAL}
                      : (VkDescriptorImageInfo){VK_NULL_HANDLE, F.view[IMG_DEPTH + i - 2], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i, .descriptorCount = 1,
                                      .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .pImageInfo = &ii[i]};
    }
    struct { int32_t w, h; float x, y; } pc = {(int32_t)F.rw, (int32_t)F.rh, F.to_pixels[0], F.to_pixels[1]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.prep);
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 4, w);
    vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (F.rw + 7) / 8, (F.rh + 7) / 8, 1);
    copy_picture(cb, F.scene, out->image, VK_IMAGE_LAYOUT_GENERAL, out);
    layouts(cb, IMG_DEPTH, 2, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    memory_barrier(cb);
}

static struct FfxApiResource resource(int i, uint32_t usage, uint32_t state) {
    return (struct FfxApiResource){.resource = F.img[i], .state = state,
                                   .description = {.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, .format = surface_format[i],
                                                   .width = i == IMG_OUT ? F.ow : F.w, .height = i == IMG_OUT ? F.oh : F.h,
                                                   .depth = 1, .mipCount = 1, .usage = usage}};
}

// Step 3: the tone-mapped picture to the output size, over what the stretch drew
static void record_upscale(VkExec *ex) {
    const VkExecTarget *out = &ex->color[0];
    VkCommandBuffer cb = ex->cb;
    if (!F.ctx || !out->image || ex->fbl_mask & 1) return;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), now = kern_now_ns();
    // A gap means the setting was off or the game drew no scene for a while: the history would not match the picture
    int reset = !F.last_frame || frame > F.last_frame + 2;
    float ms = reset ? 16.7f : (float)(now - F.last_ns) / 1e6f;
    F.last_frame = frame;
    F.last_ns = now;
    if (!F.logged) {
        F.logged = 1;
        rt_log("vk: FSR 3.1 upscales %ux%u to %ux%u: near %g far %g, fov %.1f degrees, jitter %.3f %.3f", F.rw, F.rh, F.ow,
               F.oh, F.near_z, F.far_z, F.fov * 57.29578f, F.jitter[0], F.jitter[1]);
    }
    memory_barrier(cb);
    layouts(cb, IMG_COLOUR, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_picture(cb, F.tone, F.img[IMG_COLOUR], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, NULL);
    layouts(cb, IMG_COLOUR, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    layouts(cb, IMG_OUT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    struct ffxDispatchDescUpscale d = {
        .header = {.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE}, .commandList = cb,
        .color = resource(IMG_COLOUR, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .depth = resource(IMG_DEPTH, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .motionVectors = resource(IMG_MOTION, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .output = resource(IMG_OUT, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
        .jitterOffset = {F.jitter[0], F.jitter[1]}, .motionVectorScale = {1.0f, 1.0f}, .renderSize = {F.rw, F.rh},
        .upscaleSize = {F.ow, F.oh}, .frameTimeDelta = ms, .preExposure = 1.0f, .reset = reset != 0, .cameraNear = F.near_z,
        .cameraFar = F.far_z, .cameraFovAngleVertical = F.fov, .viewSpaceToMetersFactor = 1.0f,
        .flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB};
    rec_drain();   // the library's calls come after everything queued so far, on this thread
    ffxReturnCode_t r = F.dispatch(&F.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        rt_log("vk: FSR 3.1 off: ffxDispatch returned %u", r);
        F.ok = 0;
        return;
    }
    memory_barrier(cb);
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{0, 0, 0}, {(int)F.ow, (int)F.oh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, out->level, out->base_layer, 1},
                          .dstOffsets = {{0, 0, 0}, {(int)F.ow, (int)F.oh, 1}}};
    vkCmdBlitImage(cb, F.img[IMG_OUT], VK_IMAGE_LAYOUT_GENERAL, out->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region,
                   VK_FILTER_NEAREST);
    memory_barrier(cb);
}

void vk_fsr3_resolve(VkExec *ex) {
    int step = ex->fsr3_armed;
    ex->fsr3_armed = 0;
    if (step == VK_FSR3_RESOLVE) record_resolve(ex);
    else record_upscale(ex);
    ex->recorded += 8;
    vk_exec_draw_new_cb();   // the pipelines and descriptors of this work replaced what the replay had bound
}

void vk_fsr3_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !F.tried) return;
    free_all(c);
    if (F.prep) vkDestroyPipeline(c->device, F.prep, NULL);
    if (F.sampler) vkDestroySampler(c->device, F.sampler, NULL);
    if (F.layout) vkDestroyPipelineLayout(c->device, F.layout, NULL);
    if (F.dsl) vkDestroyDescriptorSetLayout(c->device, F.dsl, NULL);
    memset(&F, 0, sizeof F);
}
