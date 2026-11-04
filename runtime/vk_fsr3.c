// AMD FSR upscaling (formerly FidelityFX Super Resolution, FSR 3.1 in FidelityFX SDK 1.1.4 and FSR Upscaling 4.x in
// the AMD FSR SDK 2.x) in place of XC2's temporal anti-aliasing and final scaling (XC2_FSR3, applies at the next frame).
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
// Two libraries are accepted, in this order:
//
// - amd_fidelityfx_loader_vk.dll, the AMD FSR SDK 2.x loader (effect code in amd_fidelityfx_upscaler_vk.dll next to
//   it). It offers FSR Upscaling 4.1.2 on Radeon RX 6000 and newer with Adrenalin 26.10.1, FSR 3.1.6 and 2.3.4
//   everywhere else, and it needs the version structure chained into the create description.
// - amd_fidelityfx_vk.dll, the FidelityFX SDK 1.1.4 library from earlier releases (and the name OptiScaler replaces).
//   It knows neither the version structures nor the newer backend descriptor, so it gets the deprecated one.
//
// XC2_FSR_VERSION (auto, 4, 3) pins a provider version through ffxOverrideVersion; auto takes what the API selects.
//
// The depth and motion images and FSR's output are double buffered by frame: frame generation (vk_fg.c) reads them at
// the present, which can run while the next frame is already on the GPU.
#include "vk_exec.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "fsr3_prep_spv.h"
#include "host.h"
#include "platform/platform.h"

#ifndef _WIN32
#define __declspec(x)   // the headers export their prototypes; elsewhere the library is simply not found
#endif
#include "ffx_api/api/include/vk/ffx_api_vk.h"
#include "ffx_api/upscalers/include/ffx_upscale.h"

#define FFX_LOADER "amd_fidelityfx_loader_vk.dll"   // AMD FSR SDK 2.x: the loader, with the upscaler beside it
#define FFX_LEGACY "amd_fidelityfx_vk.dll"          // FidelityFX SDK 1.1.4: FSR 3.1.4 in one library
#define SCENE_FORMAT VK_FORMAT_B10G11R11_UFLOAT_PACK32
#define PICTURE_FORMAT VK_FORMAT_R8G8B8A8_UNORM
#define FOV_DEFAULT 0.7853982f   // 45 degrees, what the dumped camera uses
#define MAX_MISSES 8

enum { API_NONE, API_LOADER, API_LEGACY };
enum { IMG_COLOUR, IMG_DEPTH, IMG_MOTION, IMG_OUT, IMG_N };
enum { STAGE_NONE, STAGE_RESOLVED, STAGE_TONED };

static const VkFormat image_format[IMG_N] = {PICTURE_FORMAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                             VK_FORMAT_R16G16B16A16_SFLOAT};
static const uint32_t surface_format[IMG_N] = {FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_SURFACE_FORMAT_R32_FLOAT,
                                               FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
                                               FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT};

static struct {
    int tried, ok, api;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch dispatch;
    PfnFfxQuery query;
    PfnFfxConfigure configure;
    ffxContext ctx;
    VkDeviceContext dev;           // the 2.x backend descriptor
    char version[64];              // what the context reports, for the log
    char pinned[8];                // the XC2_FSR_VERSION the context was created for
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline prep;
    VkSampler sampler;
    // IMG_COLOUR and the two images of a frame: only the depth, the motion and the output differ between the parities
    VkImage img[IMG_N][2];
    VmaAllocation alloc[IMG_N][2];
    VkImageView view[IMG_N][2];
    int par;                       // the pair the frame being recorded uses
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

// FR: the frame the replay last upscaled, for the next present (vk_fsr3_frame_take). FR_shown: the frame the present
// thread is working on (vk_fsr3_frame_show). The images stay readable until the next frame of the same parity is
// recorded, which is two frames later; images_gen counts the sets of images, as a job may outlive its set.
static VkFsrFrame FR, FR_shown;
static uint32_t images_gen;

static void warn_once(uint32_t bit, const char *what) {
    if (F.warned & bit) return;
    F.warned |= bit;
    rt_log("vk: FSR skipped, the game's own picture stays: %s", what);
}

static void on_message(uint32_t type, const wchar_t *message) {
    char b[256];
    size_t i = 0;
    for (; message && message[i] && i + 1 < sizeof b; i++) b[i] = message[i] < 128 ? (char)message[i] : '?';
    b[i] = 0;
    rt_log("vk: FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
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
    if (!f) rt_log("vk: FSR: the device has no %s", name);
    return f;
}

// The library next to the executable: plat_lib_open searches only the system folder for a bare name. *path receives the
// file it found, for the log.
static void *open_library(const char *dll, char *path, size_t size) {
    if (!plat_exe_path(path, size - strlen(dll) - 1)) return NULL;
    char *end = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > end) end = back;
    if (!end) return NULL;
    snprintf(end + 1, size - (size_t)(end + 1 - path), "%s", dll);
    return plat_lib_open(path);
}

static int init(VkCtx *c) {
    F.tried = 1;
    char path[1024];
    void *lib = open_library(FFX_LOADER, path, sizeof path);
    if (lib) {
        F.api = API_LOADER;
    } else {
        lib = open_library(FFX_LEGACY, path, sizeof path);
        if (lib) F.api = API_LEGACY;
    }
    if (!lib) {
        rt_log("vk: FSR off: neither " FFX_LOADER " nor " FFX_LEGACY " is next to the executable");
        return 0;
    }
    F.create = (PfnFfxCreateContext)plat_lib_sym(lib, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)plat_lib_sym(lib, "ffxDestroyContext");
    F.dispatch = (PfnFfxDispatch)plat_lib_sym(lib, "ffxDispatch");
    F.query = (PfnFfxQuery)plat_lib_sym(lib, "ffxQuery");
    F.configure = (PfnFfxConfigure)plat_lib_sym(lib, "ffxConfigure");
    if (!F.create || !F.destroy || !F.dispatch) {
        rt_log("vk: FSR off: %s has no ffxCreateContext, ffxDestroyContext or ffxDispatch", path);
        return 0;
    }
    F.dev.vkDevice = c->device;
    F.dev.vkPhysicalDevice = c->phys;
    F.dev.vkDeviceProcAddr = device_proc;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(c->phys, PICTURE_FORMAT, &fp);
    F.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || !F.push) {
        rt_log("vk: FSR off: this device cannot blit to the picture format or push descriptors");
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
        rt_log("vk: FSR off: its pipeline objects could not be created");
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
        rt_log("vk: FSR off: its pipeline could not be created");
        return 0;
    }
    rt_log("vk: FSR from %s (%s)", path, F.api == API_LOADER ? "AMD FSR SDK 2.x" : "FidelityFX SDK 1.1.4");
    return F.ok = 1;
}

// The version ids the upscaler offers, named like "4.1.2" or "3.1.6". Only the 2.x loader answers; 1.1.4 has no such
// query. *id receives the newest id of the wanted major version.
static int pick_version(int major, uint64_t *id) {
    if (F.api != API_LOADER || !F.query) return 0;
    struct ffxQueryDescGetVersions q;
    memset(&q, 0, sizeof q);
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &F.dev;
    uint64_t n = 0;
    q.outputCount = &n;
    if (F.query(NULL, &q.header) != FFX_API_RETURN_OK || !n || n > 64) return 0;
    uint64_t ids[64];
    const char *names[64];
    memset(names, 0, sizeof names);
    q.outputCount = &n;
    q.versionIds = ids;
    q.versionNames = names;
    if (F.query(NULL, &q.header) != FFX_API_RETURN_OK) return 0;
    char want[4];
    snprintf(want, sizeof want, "%d.", major);
    for (uint64_t i = 0; i < n && i < 64; i++)
        if (names[i] && !strncmp(names[i], want, 2)) {
            *id = ids[i];
            rt_log("vk: FSR version %s (id %#llx) is offered", names[i], (unsigned long long)ids[i]);
            return 1;
        }
    return 0;
}

static void free_image(VkCtx *c, int i, int p) {
    if (F.view[i][p]) vkDestroyImageView(c->device, F.view[i][p], NULL);
    if (F.img[i][p]) vmaDestroyImage(c->vma, F.img[i][p], F.alloc[i][p]);
    F.view[i][p] = VK_NULL_HANDLE;
    F.img[i][p] = VK_NULL_HANDLE;
}

static void forget_frames(void) {
    FR.valid = FR.fresh = 0;
    __atomic_add_fetch(&images_gen, 1, __ATOMIC_RELEASE);
}

static void free_all(VkCtx *c) {
    if (F.ctx) F.destroy(&F.ctx, NULL);
    F.ctx = NULL;
    for (int i = 0; i < IMG_N; i++)
        for (int p = 0; p < 2; p++) free_image(c, i, p);
    F.w = F.h = F.ow = F.oh = 0;
    F.pinned[0] = 0;
    forget_frames();
}

static int make_image(VkCtx *c, int i, int p, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = image_format[i], .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &F.img[i][p], &F.alloc[i][p], NULL) != VK_SUCCESS) {
        F.img[i][p] = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = F.img[i][p],
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image_format[i],
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &F.view[i][p]) != VK_SUCCESS) {
        F.view[i][p] = VK_NULL_HANDLE;
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
    if (F.img[IMG_COLOUR][0] && F.w == w && F.h == h) return 1;
    if (F.img[IMG_COLOUR][0]) wait_idle(c);
    free_all(c);
    for (int i = 0; i < IMG_OUT; i++)   // ensure_output makes the output at its own size
        for (int p = 0; p < (i == IMG_COLOUR ? 1 : 2); p++)
            if (!make_image(c, i, p, w, h)) {
                free_all(c);
                return 0;
            }
    F.w = w;
    F.h = h;
    return 1;
}

// Both output images, the context, and the version the setting asks for. A changed setting (a new version, or auto
// where a version was pinned) makes a new context.
static int ensure_output(VkCtx *c, uint32_t w, uint32_t h) {
    const char *want = cfg_value("XC2_FSR_VERSION");
    if (F.img[IMG_OUT][0] && F.img[IMG_OUT][1] && F.ow == w && F.oh == h && !strcmp(want, F.pinned)) return 1;
    if (F.ctx) {
        wait_idle(c);
        F.destroy(&F.ctx, NULL);
        F.ctx = NULL;
    } else if (F.img[IMG_OUT][0]) {
        wait_idle(c);
    }
    forget_frames();   // frame generation must not read the images freed here
    for (int p = 0; p < 2; p++) free_image(c, IMG_OUT, p);
    F.ow = F.oh = 0;
    F.pinned[0] = 0;
    for (int p = 0; p < 2; p++)
        if (!make_image(c, IMG_OUT, p, w, h)) return 0;
    struct ffxCreateBackendVKDesc backend = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
                                             .deviceContext = F.dev};
    struct ffxCreateBackendVKDesc_DEPRECATED backend_old = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED},
        .vkDevice = F.dev.vkDevice, .vkPhysicalDevice = F.dev.vkPhysicalDevice, .vkDeviceProcAddr = F.dev.vkDeviceProcAddr};
    struct ffxCreateContextDescUpscale desc = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE},
                                               .flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE,
                                               .maxRenderSize = {F.w, F.h}, .maxUpscaleSize = {w, h},
                                               .fpMessage = on_message};
    struct ffxCreateContextDescUpscaleVersion ver = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION}, .version = FFX_UPSCALER_VERSION};
    struct ffxOverrideVersion ov = {.header = {.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION}, .versionId = 0};
    int pinned_major = 0;
    if (!strcmp(want, "4")) pinned_major = 4;
    else if (!strcmp(want, "3")) pinned_major = 3;
    int have_pin = pinned_major && pick_version(pinned_major, &ov.versionId);
    if (pinned_major && !have_pin)
        rt_log("vk: FSR version %d is not offered here; the newest one is used", pinned_major);
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    for (int attempt = 0; attempt < 2; attempt++) {   // second: the newest 3.x, if the selected provider failed
        if (F.api == API_LOADER) {
            ov.header.pNext = &backend.header;
            ver.header.pNext = have_pin ? &ov.header : &backend.header;
            desc.header.pNext = &ver.header;
        } else {
            desc.header.pNext = &backend_old.header;
        }
        r = F.create(&F.ctx, &desc.header, NULL);
        if (r == FFX_API_RETURN_OK) break;
        rt_log("vk: FSR: ffxCreateContext returned %u%s", r, attempt ? "" : ", trying FSR 3");
        F.ctx = NULL;
        if (F.api != API_LOADER || !pick_version(3, &ov.versionId)) break;
        have_pin = 1;
    }
    if (r != FFX_API_RETURN_OK) {
        rt_log("vk: FSR off: no upscaler context could be created");
        F.ok = 0;
        return 0;
    }
    F.pinned[0] = 0;
    snprintf(F.pinned, sizeof F.pinned, "%s", want);
    F.version[0] = 0;
    if (F.api == API_LOADER && F.query) {
        struct ffxQueryGetProviderVersion pv = {.header = {.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
        if (F.query(&F.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
            snprintf(F.version, sizeof F.version, "%s", pv.versionName);
    }
    if (!F.version[0]) snprintf(F.version, sizeof F.version, "%s", F.api == API_LOADER ? "auto" : "3.1.4");
    if (F.query) {
        struct FfxApiEffectMemoryUsage mem;
        struct ffxQueryDescUpscaleGetGPUMemoryUsage q = {
            .header = {.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE}, .gpuMemoryUsageUpscaler = &mem};
        if (F.query(&F.ctx, &q.header) == FFX_API_RETURN_OK)
            rt_log("vk: FSR %s holds %.1f MiB of GPU memory", F.version, mem.totalUsageInBytes / 1048576.0);
    }
    F.ow = w;
    F.oh = h;
    F.logged = 0;
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
    if (!cfg_bool("XC2_FSR3")) return;   // frame generation is off without it too (vk_fg_wanted)
    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    if (!F.ok) return;
    const VkTexImage *scene = bound(ex, 0);
    F.in[0] = input_view(bound(ex, 7), VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_DEPTH_BIT);
    F.in[1] = input_view(bound(ex, 4), VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!scene || scene->format != SCENE_FORMAT || !F.in[0] || !F.in[1] || !ex->color[4].img)
        return;   // not the resolve: a pass at boot has the same targets
    if (unfinished && ++F.misses >= MAX_MISSES) {
        rt_log("vk: FSR off: the tone map or the stretch to the output size was not found");
        F.ok = 0;
        return;
    }
    const float *c4 = block_of(&ex->ubo[1][1], 96), *c3 = block_of(&ex->ubo[0][0], 48);
    if (!c4 || c4[3 * 4] < 1.0f || c4[3 * 4 + 1] < 1.0f || c4[5 * 4 + 2] <= 0.0f)
        return warn_once(1, "the resolve's uniform block is not readable");
    if (!ensure_input(c, ex->pass_w, ex->pass_h)) return warn_once(4, "its images could not be created");
    F.par ^= 1;   // this frame's depth, motion and output; the other pair stays readable for the present
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

static void image_layout(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
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
    image_layout(cb, F.img[IMG_DEPTH][F.par], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    image_layout(cb, F.img[IMG_MOTION][F.par], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w[4];
    for (uint32_t i = 0; i < 4; i++) {
        ii[i] = i < 2 ? (VkDescriptorImageInfo){F.sampler, F.in[i], VK_IMAGE_LAYOUT_GENERAL}
                      : (VkDescriptorImageInfo){VK_NULL_HANDLE, F.view[IMG_DEPTH + i - 2][F.par], VK_IMAGE_LAYOUT_GENERAL};
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
    image_layout(cb, F.img[IMG_DEPTH][F.par], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_layout(cb, F.img[IMG_MOTION][F.par], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    memory_barrier(cb);
}

static struct FfxApiResource resource(int i, int p, uint32_t usage, uint32_t state) {
    return (struct FfxApiResource){.resource = F.img[i][p], .state = state,
                                   .description = {.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, .format = surface_format[i],
                                                   .width = i == IMG_OUT ? F.ow : F.w, .height = i == IMG_OUT ? F.oh : F.h,
                                                   .depth = 1, .mipCount = 1, .usage = usage}};
}

// The upscaler's dispatch for the frame F describes. reset clears its history (a camera cut, or the first frame).
static ffxReturnCode_t dispatch_upscale(VkCommandBuffer cb, int reset, float ms) {
    struct ffxDispatchDescUpscale d = {
        .header = {.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE}, .commandList = cb,
        .color = resource(IMG_COLOUR, 0, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .depth = resource(IMG_DEPTH, F.par, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .motionVectors = resource(IMG_MOTION, F.par, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .output = resource(IMG_OUT, F.par, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
        .jitterOffset = {F.jitter[0], F.jitter[1]}, .motionVectorScale = {1.0f, 1.0f}, .renderSize = {F.rw, F.rh},
        .upscaleSize = {F.ow, F.oh}, .frameTimeDelta = ms, .preExposure = 1.0f, .reset = reset != 0, .cameraNear = F.near_z,
        .cameraFar = F.far_z, .cameraFovAngleVertical = F.fov, .viewSpaceToMetersFactor = 1.0f,
        .flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB};
    rec_drain();   // the library's calls come after everything queued so far, on this thread
    return F.dispatch(&F.ctx, &d.header);
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
        rt_log("vk: FSR %s upscales %ux%u to %ux%u: near %g far %g, fov %.1f degrees, jitter %.3f %.3f", F.version, F.rw, F.rh,
               F.ow, F.oh, F.near_z, F.far_z, F.fov * 57.29578f, F.jitter[0], F.jitter[1]);
    }
    memory_barrier(cb);
    image_layout(cb, F.img[IMG_COLOUR][0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy_picture(cb, F.tone, F.img[IMG_COLOUR][0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, NULL);
    image_layout(cb, F.img[IMG_COLOUR][0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    image_layout(cb, F.img[IMG_OUT][F.par], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    ffxReturnCode_t r = dispatch_upscale(cb, reset, ms);
    if (r != FFX_API_RETURN_OK) {
        rt_log("vk: FSR off: ffxDispatch returned %u", r);
        F.ok = 0;
        return;
    }
    memory_barrier(cb);
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{0, 0, 0}, {(int)F.ow, (int)F.oh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, out->level, out->base_layer, 1},
                          .dstOffsets = {{0, 0, 0}, {(int)F.ow, (int)F.oh, 1}}};
    vkCmdBlitImage(cb, F.img[IMG_OUT][F.par], VK_IMAGE_LAYOUT_GENERAL, out->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region,
                   VK_FILTER_NEAREST);
    memory_barrier(cb);
    // This frame, for frame generation: the depth and motion it reads, and the scene without the HUD it interpolates.
    FR = (VkFsrFrame){.valid = 1, .fresh = 1, .gen = __atomic_load_n(&images_gen, __ATOMIC_ACQUIRE),
                      .depth = F.img[IMG_DEPTH][F.par], .motion = F.img[IMG_MOTION][F.par], .hudless = F.img[IMG_OUT][F.par],
                      .rw = F.rw, .rh = F.rh, .ow = F.ow, .oh = F.oh, .jitter = {F.jitter[0], F.jitter[1]},
                      .near_z = F.near_z, .far_z = F.far_z, .fov = F.fov, .frame_ms = ms};
}

void vk_fsr3_resolve(VkExec *ex) {
    int step = ex->fsr3_armed;
    ex->fsr3_armed = 0;
    if (step == VK_FSR3_RESOLVE) record_resolve(ex);
    else record_upscale(ex);
    ex->recorded += 8;
    vk_exec_draw_new_cb();   // the pipelines and descriptors of this work replaced what the replay had bound
}

void vk_fsr3_frame_take(VkFsrFrame *out) {
    *out = FR;
    FR.fresh = 0;
}

void vk_fsr3_frame_show(const VkFsrFrame *fr) {
    FR_shown = *fr;
    if (fr->gen != __atomic_load_n(&images_gen, __ATOMIC_ACQUIRE)) FR_shown.valid = 0;
}

int vk_fsr3_frame_get(VkFsrFrame *out) {
    if (!FR_shown.valid) return 0;
    *out = FR_shown;
    return 1;
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
    memset(&FR, 0, sizeof FR);
    memset(&FR_shown, 0, sizeof FR_shown);
}

// ---- --probe-fsr: the upscaler alone, without the game

static float half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)h >> 15, e = ((uint32_t)h >> 10) & 0x1f, m = (uint32_t)h & 0x3ff;
    float v;
    if (!e) v = (float)m * (1.0f / 1024.0f);   // subnormal, close enough to zero here
    else if (e == 31) v = 1.0f;
    else {
        v = 1.0f + (float)m * (1.0f / 1024.0f);
        for (int i = 0; i < (int)e - 15; i++) v *= 2.0f;
        for (int i = 0; i < 15 - (int)e; i++) v *= 0.5f;
    }
    return s ? -v : v;
}

// --probe-fsr (host.h): creates a 1280x720 -> 1920x1080 context from the library next to the executable, dispatches two
// frames of a flat colour and checks the upscaled result is that colour. The provider version the API selected (FSR
// Upscaling 4.1.2 on Radeon RX 6000 and newer with a new enough driver, FSR 3.1.6 elsewhere) goes to the log and to the
// output. Needs no game files.
int probe_fsr(void) {
    if (!vk_init()) return 1;
    VkCtx *c = vk_ctx();
    const uint32_t RW = 1280, RH = 720, OW = 1920, OH = 1080;
    const float flat[3] = {0.25f, 0.5f, 0.75f};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation bmem = VK_NULL_HANDLE;
    VmaAllocationInfo binfo;
    int rc = 1;
    if (!F.tried) init(c);
    if (!F.ok) {
        printf("probe-fsr: skipped (no FSR library next to the executable)\n");
        vk_shutdown();
        return 0;
    }
    if (!ensure_input(c, RW, RH) || !ensure_output(c, OW, OH)) {
        printf("probe-fsr: FAILED (no %ux%u -> %ux%u context)\n", RW, RH, OW, OH);
        goto out;
    }
    F.rw = RW, F.rh = RH, F.ow = OW, F.oh = OH;
    F.jitter[0] = F.jitter[1] = 0.0f;
    F.near_z = 0.1f, F.far_z = 1000.0f, F.fov = 0.7853982f;
    {
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pci, NULL, &pool) != VK_SUCCESS) goto out;
        VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(c->device, &cbi, &cb) != VK_SUCCESS) goto out;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)OW * OH * 8,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        if (vmaCreateBuffer(c->vma, &bi, &ai, &buf, &bmem, &binfo) != VK_SUCCESS || !binfo.pMappedData) goto out;
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(c->device, &fi, NULL, &fence) != VK_SUCCESS) goto out;
    }
    {
        VkCommandBufferBeginInfo bci = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bci);
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue colour = {.float32 = {flat[0], flat[1], flat[2], 1.0f}};
        image_layout(cb, F.img[IMG_COLOUR][0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, F.img[IMG_COLOUR][0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &all);
        image_layout(cb, F.img[IMG_COLOUR][0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkClearColorValue far = {.float32 = {1.0f, 0, 0, 0}};
        image_layout(cb, F.img[IMG_DEPTH][F.par], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, F.img[IMG_DEPTH][F.par], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &all);
        image_layout(cb, F.img[IMG_DEPTH][F.par], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkClearColorValue zero = {.float32 = {0, 0, 0, 0}};
        image_layout(cb, F.img[IMG_MOTION][F.par], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, F.img[IMG_MOTION][F.par], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
        image_layout(cb, F.img[IMG_MOTION][F.par], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image_layout(cb, F.img[IMG_OUT][F.par], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        for (int i = 0; i < 2; i++)   // two frames: the second one exercises the history
            if (dispatch_upscale(cb, i == 0, 16.7f) != FFX_API_RETURN_OK) {
                vkEndCommandBuffer(cb);
                printf("probe-fsr: FAILED (ffxDispatch returned an error)\n");
                goto out;
            }
        image_layout(cb, F.img[IMG_OUT][F.par], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {OW, OH, 1}};
        vkCmdCopyImageToBuffer(cb, F.img[IMG_OUT][F.par], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &dep);
        vkEndCommandBuffer(cb);
    }
    {
        VkCommandBufferSubmitInfo cbsi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbsi};
        vk_queue_lock();
        VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, fence);
        vk_queue_unlock();
        if (r != VK_SUCCESS || vkWaitForFences(c->device, 1, &fence, VK_TRUE, 30000000000ull) != VK_SUCCESS) {
            printf("probe-fsr: FAILED (the dispatch did not complete, %d)\n", (int)r);
            goto out;
        }
        vmaInvalidateAllocation(c->vma, bmem, 0, VK_WHOLE_SIZE);
    }
    {
        const uint16_t *p = binfo.pMappedData;
        float worst = 0.0f;
        int worst_at = 0;
        double sum[3] = {0, 0, 0};
        for (size_t i = 0; i < (size_t)OW * OH; i++) {
            for (int ch = 0; ch < 3; ch++) {
                float v = half_to_float(p[i * 4 + ch]);
                float d = v - flat[ch];
                if (d < 0) d = -d;
                sum[ch] += v;
                if (d > worst) worst = d, worst_at = (int)i;
            }
        }
        double n = (double)OW * OH;
        printf("probe-fsr: %s, %ux%u -> %ux%u, two flat frames %.3f %.3f %.3f\n", F.version, RW, RH, OW, OH, flat[0], flat[1],
               flat[2]);
        printf("probe-fsr: mean %.4f %.4f %.4f, worst deviation %.4f at pixel %d\n", sum[0] / n, sum[1] / n, sum[2] / n, worst,
               worst_at);
        rc = worst <= 0.05f ? 0 : 1;
        printf("probe-fsr: %s\n", rc ? "FAILED (the upscaled flat colour differs)" : "passed");
    }
out:
    if (buf) vmaDestroyBuffer(c->vma, buf, bmem);
    if (fence) vkDestroyFence(c->device, fence, NULL);
    if (pool) vkDestroyCommandPool(c->device, pool, NULL);
    vk_fsr3_release();
    vk_shutdown();
    return rc;
}
