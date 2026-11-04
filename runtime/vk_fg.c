// AMD FSR Frame Generation on the present (XC2_FRAME_GEN, off by default, needs XC2_FSR3 for its inputs).
//
// The frame generation swapchain of the AMD FSR SDK replaces ours: it destroys the VkSwapchainKHR handed to it and
// returns its own, and it replaces vkGetSwapchainImagesKHR, vkAcquireNextImageKHR and vkQueuePresentKHR. It then
// interpolates the picture of frame N-1 and frame N, presents both, and paces them with its own threads. Which provider
// runs is up to the API: ML FSR Frame Generation 4.0.2 on Radeon RX 9000 with Windows 11 and a new enough driver, the
// analytical FSR Frame Generation 3.1.7 everywhere else (Vulkan 1.2, every vendor).
//
// Every frame we hand it what FSR already decodes from the game's own resolve (vk_fsr3.c, vk_fsr3_frame_get): depth and
// motion vectors at the render size, the camera values, and the upscaled scene without the HUD. The HUD-less picture is
// what lets it compose the game's HUD onto the interpolated frame instead of interpolating the HUD's pixels, which for
// XC2's thin text would be very visible.
//
// The library submits on its own threads and refuses a swapchain unless its four queues are distinct, so vk_device.c asks
// for them where the device has them (pick_queues). Presentation goes through vk_fg_present, and vk_backend.c drops its
// own pacing while frame generation is on: two presents per frame are paced by the library.
//
// The upscaler runs on the tone-mapped picture at the game's output size, so the HUD-less picture is only the same size
// as the swapchain when output_res matches the window; otherwise this module scales it into one of its own images first
// (a blit, for UI extraction only).
#include "vk.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#ifndef _WIN32
#define __declspec(x)
#endif
#include "ffx_api/api/include/vk/ffx_api_vk.h"
// The SDK's Vulkan frame generation header names struct VkQueueInfoFFXAPI without the tag (C++ style); a C compiler needs
// the typedef before the include. The vendored headers stay as AMD ships them.
struct VkQueueInfoFFXAPI;
typedef struct VkQueueInfoFFXAPI VkQueueInfoFFXAPI;
#include "ffx_api/framegeneration/include/ffx_framegeneration.h"
#include "ffx_api/framegeneration/include/vk/ffx_api_framegeneration_vk.h"

#define FFX_LOADER "amd_fidelityfx_loader_vk.dll"   // loads amd_fidelityfx_framegeneration_vk.dll for this effect
#define MAX_IMAGES 16

static struct {
    int tried, ok, failed, on;
    void *lib;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch dispatch;
    PfnFfxQuery query;
    PfnFfxConfigure configure;
    ffxContext fg, sc;
    PFN_vkCreateSwapchainFFXAPI sc_create;
    PFN_vkDestroySwapchainFFXAPI sc_destroy;
    PFN_vkGetSwapchainImagesKHR sc_images;
    PFN_vkAcquireNextImageKHR sc_acquire;
    PFN_vkQueuePresentKHR sc_present;
    PFN_getLastPresentCountFFXAPI sc_count;
    VkSwapchainKHR swapchain;      // the handle this module owns while on
    uint32_t w, h;                 // display size
    VkFormat fmt;
    uint64_t frame_id;
    // the HUD-less picture at display size, one per frame parity, and its views
    VkImage hudless[2];
    VmaAllocation hudless_alloc[2];
    int par;
    int gap;                       // the last frame had nothing to interpolate from: the next one starts over
    int warned[4];
    int logged;
} G;

// The descriptors the library keeps pointers to until the context goes away (ffxCreateContext: "Pointers passed in desc
// must remain live until ffxDestroyContext is called").
static struct ffxCreateBackendVKDesc g_backend;
static struct ffxCreateContextDescFrameGenerationVersion g_fg_version;
static struct ffxCreateContextDescFrameGeneration g_fg_desc;
static struct ffxCreateContextDescFrameGenerationSwapChainVK g_sc_desc;
static struct ffxCreateContextDescFrameGenerationSwapChainVersionVK g_sc_version;

static void warn_once(int bit, const char *what) {
    if (G.warned[bit]) return;
    G.warned[bit] = 1;
    rt_log("vk: frame generation off: %s", what);
}

// XC2_TRACE=present: a line at each step of the frame generation path, for crashes inside its library.
static void trace(const char *what) {
    if (rt_trace & RT_TRACE_PRESENT) rt_log("fg: %s", what);
}

static uint32_t ffx_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
        default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static struct FfxApiResource resource(VkImage img, uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t state) {
    return (struct FfxApiResource){.resource = img, .state = state,
                                   .description = {.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, .format = fmt, .width = w,
                                                   .height = h, .depth = 1, .mipCount = 1, .usage = usage}};
}

static int init(void) {
    G.tried = 1;
    char path[1024];
    if (!plat_exe_path(path, sizeof path - sizeof FFX_LOADER)) return 0;
    char *end = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > end) end = back;
    if (!end) return 0;
    snprintf(end + 1, sizeof path - (size_t)(end + 1 - path), "%s", FFX_LOADER);
    G.lib = plat_lib_open(path);
    if (!G.lib) {
        rt_log("vk: frame generation off: " FFX_LOADER " is not next to the executable");
        G.failed = 1;
        return 0;
    }
    G.create = (PfnFfxCreateContext)plat_lib_sym(G.lib, "ffxCreateContext");
    G.destroy = (PfnFfxDestroyContext)plat_lib_sym(G.lib, "ffxDestroyContext");
    G.dispatch = (PfnFfxDispatch)plat_lib_sym(G.lib, "ffxDispatch");
    G.query = (PfnFfxQuery)plat_lib_sym(G.lib, "ffxQuery");
    G.configure = (PfnFfxConfigure)plat_lib_sym(G.lib, "ffxConfigure");
    if (!G.create || !G.destroy || !G.dispatch || !G.query || !G.configure) {
        rt_log("vk: frame generation off: %s lacks an ffx entry point", path);
        G.failed = 1;
        return 0;
    }
    return G.ok = 1;
}

// On: the setting is on, the library loaded, and the inputs frame generation needs exist (the upscaler's resolve hook).
int vk_fg_wanted(void) {
    if (!cfg_bool("XC2_FRAME_GEN")) return 0;
    if (G.failed) return 0;
    if (!G.tried && !init()) return 0;
    if (!G.ok) return 0;
    if (!cfg_bool("XC2_FSR3")) {
        warn_once(0, "it needs fsr3 = 1 for its depth, motion vectors and HUD-less picture");
        return 0;
    }
    return 1;
}

int vk_fg_active(void) { return G.on; }

static void free_hudless(VkCtx *c) {
    for (int i = 0; i < 2; i++) {
        if (G.hudless[i]) vmaDestroyImage(c->vma, G.hudless[i], G.hudless_alloc[i]);
        G.hudless[i] = VK_NULL_HANDLE;
    }
}

static int make_hudless(VkCtx *c) {
    if (G.hudless[0] && G.hudless[1]) return 1;
    free_hudless(c);
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = G.fmt, .extent = {G.w, G.h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_STORAGE_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &G.hudless[i], &G.hudless_alloc[i], NULL) != VK_SUCCESS) {
            G.hudless[i] = VK_NULL_HANDLE;
            free_hudless(c);
            return 0;
        }
    }
    return 1;
}

// The frame generation context: it interpolates and presents, and it is what we configure and prepare each frame.
static int make_fg_context(VkCtx *c, uint32_t w, uint32_t h, VkFormat fmt) {
    if (G.fg) return 1;
    memset(&g_backend, 0, sizeof g_backend);
    g_backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    g_backend.deviceContext.vkDevice = c->device;
    g_backend.deviceContext.vkPhysicalDevice = c->phys;
    g_backend.deviceContext.vkDeviceProcAddr = (PFN_vkGetDeviceProcAddr)vkGetDeviceProcAddr;
    memset(&g_fg_version, 0, sizeof g_fg_version);
    g_fg_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    g_fg_version.version = FFX_FRAMEGENERATION_VERSION;
    memset(&g_fg_desc, 0, sizeof g_fg_desc);
    g_fg_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    g_fg_desc.header.pNext = &g_fg_version.header;
    g_fg_version.header.pNext = &g_backend.header;
    g_fg_desc.flags = 0;
    g_fg_desc.displaySize = (struct FfxApiDimensions2D){w, h};
    g_fg_desc.maxRenderSize = (struct FfxApiDimensions2D){w, h};
    g_fg_desc.backBufferFormat = ffx_format(fmt);
    ffxReturnCode_t r = G.create(&G.fg, &g_fg_desc.header, NULL);
    if (r != FFX_API_RETURN_OK) {
        rt_log("vk: frame generation off: ffxCreateContext (frame generation) returned %u", r);
        G.fg = NULL;
        return 0;
    }
    return 1;
}

// The library requires four distinct queues (FrameInterpolationSwapchainVK::init refuses anything else): the game's queue,
// one for presenting, one for its async compute and one for image acquisition. They come from the graphics family's spare
// queues, then a compute-only family's and a transfer-only one; without four distinct ones frame generation stays off.
static int pick_queues(VkCtx *c, VkQueue q[4], uint32_t fam[4]) {
    VkQueue pool[8];   // at most 4 graphics, 3 compute and 1 transfer (vk_device.c create_device)
    uint32_t fpool[8];
    int n = 0;
    pool[n] = c->gfx_queue, fpool[n++] = c->gfx_family;
    for (uint32_t i = 1; i < c->gfx_queue_count; i++) {
        vkGetDeviceQueue(c->device, c->gfx_family, i, &pool[n]);
        fpool[n++] = c->gfx_family;
    }
    for (uint32_t i = 0; i < c->comp_queue_count; i++) {
        vkGetDeviceQueue(c->device, c->comp_family, i, &pool[n]);
        fpool[n++] = c->comp_family;
    }
    if (!c->xfer_shared && c->xfer_queue) pool[n] = c->xfer_queue, fpool[n++] = c->xfer_family;
    if (n < 4) {
        rt_log("vk: frame generation off: this device offers %d distinct queues, its swapchain needs 4", n);
        return 0;
    }
    for (int i = 0; i < 4; i++) q[i] = pool[i], fam[i] = fpool[i];
    return 1;
}

// Installs the frame generation swapchain over *sc (which the library destroys and replaces) and creates or keeps the
// contexts. Called from gfx_present.c with the create info it would have used; vk.h has the return values.
int vk_fg_swapchain(VkCtx *c, VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    if (!vk_fg_wanted()) return 0;
    G.fmt = ci->imageFormat;
    G.w = ci->imageExtent.width;
    G.h = ci->imageExtent.height;
    VkQueue q[4];
    uint32_t fam[4];
    if (!G.sc && !pick_queues(c, q, fam)) {
        G.failed = 1;
        return 0;
    }
    if (!G.sc) {
        memset(&g_sc_version, 0, sizeof g_sc_version);
        g_sc_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
        g_sc_version.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
        memset(&g_sc_desc, 0, sizeof g_sc_desc);
        g_sc_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        g_sc_desc.header.pNext = &g_sc_version.header;
        g_sc_desc.physicalDevice = c->phys;
        g_sc_desc.device = c->device;
        g_sc_desc.swapchain = sc;
        g_sc_desc.allocator = NULL;
        g_sc_desc.createInfo = *ci;
        g_sc_desc.gameQueue = (VkQueueInfoFFXAPI){q[0], fam[0], NULL};
        g_sc_desc.asyncComputeQueue = (VkQueueInfoFFXAPI){q[2], fam[2], NULL};
        g_sc_desc.presentQueue = (VkQueueInfoFFXAPI){q[1], fam[1], NULL};
        g_sc_desc.imageAcquireQueue = (VkQueueInfoFFXAPI){q[3], fam[3], NULL};
        ffxReturnCode_t r = G.create(&G.sc, &g_sc_desc.header, NULL);
        if (r != FFX_API_RETURN_OK) {
            rt_log("vk: frame generation off: the swapchain context could not be created (%u)", r);
            G.sc = NULL;
            G.failed = 1;
            return -1;
        }
        struct ffxQueryDescSwapchainReplacementFunctionsVK q;
        memset(&q, 0, sizeof q);
        q.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        if (G.query(&G.sc, &q.header) != FFX_API_RETURN_OK || !q.pOutCreateSwapchainFFXAPI || !q.pOutDestroySwapchainFFXAPI ||
            !q.pOutGetSwapchainImagesKHR || !q.pOutAcquireNextImageKHR || !q.pOutQueuePresentKHR) {
            rt_log("vk: frame generation off: the swapchain context has no replacement functions");
            G.destroy(&G.sc, NULL);
            G.sc = NULL;
            G.failed = 1;
            return -1;
        }
        G.sc_create = q.pOutCreateSwapchainFFXAPI;
        G.sc_destroy = q.pOutDestroySwapchainFFXAPI;
        G.sc_images = q.pOutGetSwapchainImagesKHR;
        G.sc_acquire = q.pOutAcquireNextImageKHR;
        G.sc_present = q.pOutQueuePresentKHR;
        G.sc_count = q.pOutGetLastPresentCountFFXAPI;
        if (!make_fg_context(c, G.w, G.h, G.fmt) || !make_hudless(c)) {
            if (G.fg) G.destroy(&G.fg, NULL);
            G.fg = NULL;
            G.destroy(&G.sc, NULL);
            G.sc = NULL;
            G.failed = 1;
            return -1;
        }
        G.swapchain = *sc;
        G.on = 1;
        rt_log("vk: frame generation on: %ux%u, %s", G.w, G.h,
               "FSR Frame Generation (3.1.7 analytical on this hardware, 4.0.2 ML on Radeon RX 9000)");
        return 1;
    }
    // A new swapchain for a resize, a present-mode change or a fullscreen switch: the library rebuilds it in place.
    VkResult r = G.sc_create(c->device, ci, NULL, sc, G.sc);
    if (r != VK_SUCCESS) {
        rt_log("vk: frame generation off: recreating its swapchain failed (%d)", (int)r);
        vk_fg_shutdown(c);
        G.failed = 1;
        return -1;
    }
    G.swapchain = *sc;
    G.w = ci->imageExtent.width;
    G.h = ci->imageExtent.height;
    G.fmt = ci->imageFormat;
    free_hudless(c);
    make_hudless(c);
    return 1;
}

VkResult vk_fg_images(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *imgs) {
    if (G.on && G.sc_images) {
        trace("images in");
        VkResult r = G.sc_images(d, sc, n, imgs);
        trace("images out");
        return r;
    }
    return vkGetSwapchainImagesKHR(d, sc, n, imgs);
}

VkResult vk_fg_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *idx) {
    if (G.on && G.sc_acquire) {
        trace("acquire in");
        VkResult r = G.sc_acquire(d, sc, timeout, sem, fence, idx);
        trace("acquire out");
        return r;
    }
    return vkAcquireNextImageKHR(d, sc, timeout, sem, fence, idx);
}

VkResult vk_fg_present(VkQueue q, const VkPresentInfoKHR *pi) {
    if (G.on && G.sc_present) {
        trace("present in");
        VkResult r = G.sc_present(q, pi);
        trace("present out");
        return r;
    }
    return vkQueuePresentKHR(q, pi);
}

// Before the device goes away, and when the setting is turned off: stop presenting through the library, then destroy in
// its documented order (frame generation context, swapchain context, swapchain).
void vk_fg_shutdown(VkCtx *c) {
    if (!G.sc) {
        G.on = 0;
        return;
    }
    if (G.fg) {
        struct ffxConfigureDescFrameGeneration cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = G.swapchain;
        cfg.frameGenerationEnabled = false;
        G.configure(&G.fg, &cfg.header);
    }
    struct ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait;
    memset(&wait, 0, sizeof wait);
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    G.dispatch(&G.sc, &wait.header);
    if (G.sc_count && G.swapchain)
        rt_log("vk: frame generation presented %llu frames for %llu drawn", (unsigned long long)G.sc_count(G.swapchain),
               (unsigned long long)G.frame_id);
    if (G.fg) G.destroy(&G.fg, NULL);
    G.fg = NULL;
    G.destroy(&G.sc, NULL);
    G.sc = NULL;
    G.on = 0;
    if (G.swapchain) G.sc_destroy(c->device, G.swapchain, NULL, NULL);
    G.swapchain = VK_NULL_HANDLE;
    free_hudless(c);
}

// The frame generation callback the library calls from its present: it asks the frame generation context to interpolate
// into the command buffer the library prepared for it.
static ffxReturnCode_t VKAPI_CALL on_generate(struct ffxDispatchDescFrameGeneration *params, void *ctx) {
    return G.dispatch((ffxContext *)ctx, &params->header);
}

// One presented frame: the HUD-less picture scaled into our own image, the configuration, and the prepare dispatch into
// the present command buffer. rect is where the game picture sits in the swapchain image (the black bars stay bars).
int vk_fg_frame(VkCtx *c, VkCommandBuffer cb, const VkFsrFrame *fr, const int rect[4]) {
    if (!G.on || !G.fg) return 0;
    // A frame without its own depth and motion (a menu or a loading screen, FSR off) goes out as drawn: older ones would
    // interpolate this picture with the camera of another.
    int have = fr && fr->valid && fr->fresh, reset = G.gap;
    G.gap = !have;
    if (!G.logged) {
        G.logged = 1;
        rt_log("vk: frame generation will present about twice per frame: set fps_limit to about half the display rate");
    }
    G.par ^= 1;
    struct FfxApiResource hudless = {0};
    if (have && G.hudless[G.par]) {
        // The upscaler's picture is at the game's output size; the library wants it at the swapchain's, where the game's
        // picture sits in rect: it finds the HUD by comparing this image with the swapchain's, bars included.
        VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = G.hudless[G.par],
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                              .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(cb, &d);
        VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .srcOffsets = {{0, 0, 0}, {(int)fr->ow, (int)fr->oh, 1}},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstOffsets = {{rect[0], rect[1], 0}, {rect[0] + rect[2], rect[1] + rect[3], 1}}};
        if (rect[0] || rect[1] || (uint32_t)rect[2] != G.w || (uint32_t)rect[3] != G.h) {
            VkClearColorValue black = {0};
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, G.hudless[G.par], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                   .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
            VkDependencyInfo md = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
            vkCmdPipelineBarrier2(cb, &md);
        }
        // the source is the upscaler's output, which it left in GENERAL; it is read here and written next frame
        VkImageMemoryBarrier2 sb = b;
        sb.image = fr->hudless;
        sb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        sb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        sb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        sb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        sb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        sb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo sd = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &sb};
        vkCmdPipelineBarrier2(cb, &sd);
        vkCmdBlitImage(cb, fr->hudless, VK_IMAGE_LAYOUT_GENERAL, G.hudless[G.par], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &region, VK_FILTER_LINEAR);
        VkImageMemoryBarrier2 ab = b;
        ab.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        ab.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ab.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ab.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        ab.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ab.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;   // what the state below tells the library
        VkDependencyInfo ad = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &ab};
        vkCmdPipelineBarrier2(cb, &ad);
        hudless = resource(G.hudless[G.par], ffx_format(G.fmt), G.w, G.h, FFX_API_RESOURCE_USAGE_READ_ONLY,
                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }
    struct ffxConfigureDescFrameGeneration cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = G.swapchain;
    cfg.frameGenerationEnabled = have != 0;   // no depth or motion this frame: present it as drawn
    cfg.allowAsyncWorkloads = false;
    cfg.HUDLessColor = hudless;
    cfg.flags = 0;
    cfg.onlyPresentGenerated = false;
    cfg.generationRect = (struct FfxApiRect2D){rect[0], rect[1], rect[2], rect[3]};
    cfg.frameID = ++G.frame_id;
    cfg.frameGenerationCallback = on_generate;
    cfg.frameGenerationCallbackUserContext = &G.fg;
    trace("configure in");
    ffxReturnCode_t r = G.configure(&G.fg, &cfg.header);
    trace("configure out");
    if (r != FFX_API_RETURN_OK) {
        warn_once(1, "configuring it failed");
        return 0;
    }
    if (!have) return 0;   // nothing to interpolate from: the frame goes out as drawn
    struct ffxDispatchDescFrameGenerationPrepareV2 prep;
    memset(&prep, 0, sizeof prep);
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prep.frameID = G.frame_id;
    prep.commandList = cb;
    prep.renderSize = (struct FfxApiDimensions2D){fr->rw, fr->rh};
    prep.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};   // the upscaler's prep shader already writes pixels
    prep.reset = reset != 0;
    {
        prep.depth = resource(fr->depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, fr->rw, fr->rh, FFX_API_RESOURCE_USAGE_READ_ONLY,
                              FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.motionVectors = resource(fr->motion, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, fr->rw, fr->rh,
                                      FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.jitterOffset = (struct FfxApiFloatCoords2D){fr->jitter[0], fr->jitter[1]};
        prep.cameraNear = fr->near_z;
        prep.cameraFar = fr->far_z;
        prep.cameraFovAngleVertical = fr->fov;
        prep.viewSpaceToMetersFactor = 1.0f;
    }
    prep.frameTimeDelta = fr->frame_ms > 0.0f ? fr->frame_ms : 16.7f;
    trace("prepare in");
    r = G.dispatch(&G.fg, &prep.header);
    trace("prepare out");
    if (r != FFX_API_RETURN_OK) {
        warn_once(2, "its prepare dispatch failed");
        return 0;
    }
    return 1;
}

void vk_fg_release(VkCtx *c) {
    if (!c) return;
    if (G.tried) vk_fg_shutdown(c);
    memset(&G, 0, sizeof G);
}
