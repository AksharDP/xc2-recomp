// FSR 3.1 and FSR 3 frame interpolation built into the game: the FidelityFX SDK 1.1.4's upscaler, optical flow and frame
// interpolation contexts (MIT; tools/ffx_sdk.py fetches it) on the SDK's Vulkan backend, driven directly, without the ffx-api DLL
// layer and without the SDK's frame interpolation swapchain (which exists for Windows only and would own the present).
//
// Ported from Savage Wind Recomp (https://github.com/Revivedx/SavageWindRecomp @
// bf6146c14c37fee8781a601e5a544b93dff6d75e, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai), runtime/nvn/vulkan/fsr3.cpp and framegen.cpp, by way of TotK Recomp
// (https://github.com/valentderah/totk-pc-recomp @ 7da078d1aba7ff3ce4d4c98e9520332d3361ef2e, MIT, Copyright (c) 2026
// Valiantsin Dzerakh), runtime/vk_fsr3.cpp and vk_fg.cpp: the blob lookup, the backend set-up, the contexts as the SDK's
// ffx-api providers make them (the images the effects share come from a backend context of their own), the resource
// wrapping and the parameters of the dispatches.
//
// What differs here: the upscaler's input is XC2's tone-mapped picture (vk_fsr3.c says why), so its shaders take SDR
// colour. The interpolation is recorded at the present, into the present thread's own command buffer, from the same
// per-frame record AMD's library gets (VkFsrFrame): depth, motion and the upscaled picture before the HUD.
#include "vk_ffx.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <ffx_shader_blobs.h>

extern "C" {
#include "host.h"
#include "platform/platform.h"
}

// The shaders: tools/ffx_shaders.py's blobs in place of the SDK's FidelityFX_SC permutation tables
#include "ffx_fsr3upscaler_vk.h"
#include "ffx_shaders_vk.h"

enum { FU_PERM_FP16 = 1 << 7 };  // ffx_fsr3upscaler_private.h FSR3UPSCALER_SHADER_PERMUTATION_ALLOW_FP16

static bool fp16;   // the device has what the 16-bit shaders declare (Float16, Int16); the blob lookup of any thread reads it

// The shader of a pass. The 16-bit upscaler shaders are taken where the SDK asks for them (the backend found
// shaderFloat16) and the device also has shaderInt16; vk_device.c enables both wherever the driver reports them.
extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage, uint32_t options,
                                                     FfxShaderBlob *out) {
    const FfxShaderBlob *b = nullptr;
    if (effect == FFX_EFFECT_FRAMEINTERPOLATION && pass < sizeof ffx_fi_blobs / sizeof ffx_fi_blobs[0]) b = &ffx_fi_blobs[pass];
    if (effect == FFX_EFFECT_OPTICALFLOW && pass < sizeof ffx_of_blobs / sizeof ffx_of_blobs[0]) b = &ffx_of_blobs[pass];
    if (effect == FFX_EFFECT_FSR3UPSCALER && pass < sizeof ffx_fu_blobs[0] / sizeof ffx_fu_blobs[0][0])
        b = &ffx_fu_blobs[(options & FU_PERM_FP16) && __atomic_load_n(&fp16, __ATOMIC_RELAXED)][pass];
    if (!b || !b->data) {
        rt_log("vk: FSR: no built-in shader for effect %d pass %d", (int)effect, (int)pass);
        return FFX_ERROR_INVALID_ARGUMENT;
    }
    memcpy(out, b, sizeof *out);
    return FFX_OK;
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &wave64) {
    wave64 = false;  // only wave32 blobs: the backend then asks for no subgroup size
    return FFX_OK;
}

// The backend's hook into its frame interpolation swapchain, which is not built
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *) { return FFX_OK; }

// The backend calls a few extension functions unchecked: the debug labels (VK_EXT_debug_utils, an instance extension
// the runtime enables only with validation) and vkGetBufferMemoryRequirements2KHR (core since 1.1 without the suffix).
static VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
static VKAPI_ATTR void VKAPI_CALL no_label_end(VkCommandBuffer) {}

// A set-up that fails leaves the backend with objects it keeps no handle of (a pipeline's samplers and layout, an image
// without its memory), and its destroy functions take whole contexts only. The backend makes and destroys its objects
// through these functions: they note what one set-up made (track_begin), so that a failed one is undone by destroying
// that. None of it has reached a command buffer yet. live counts every object of the backend, for vk_ffx_probe.
enum Kind { K_POOL, K_SAMPLER, K_SET_LAYOUT, K_BUFFER, K_BUFFER_VIEW, K_IMAGE, K_IMAGE_VIEW, K_MODULE, K_LAYOUT, K_PIPELINE, K_MEMORY };
struct Made {
    Kind kind;
    uint64_t handle;
};
struct Track {
    Made *made;
    size_t n, cap;
    bool failed;   // a creation did: the optical flow reports success all the same, without the pipeline
};
static thread_local Track *track;      // the set-up this thread is in
static thread_local int fail_in = -1;  // vk_ffx_probe: the creation that fails, counted down
static int live;

static bool fails(void) { return fail_in >= 0 && fail_in-- == 0; }

static VkResult note_result(VkResult r) {
    if (r != VK_SUCCESS && track) track->failed = true;
    return r;
}

// Room to note n more objects, before they are made: one that could not be noted would be left over.
static bool reserve(size_t n) {
    if (!track || track->n + n <= track->cap) return true;
    size_t cap = track->cap * 2 + 512 + n;
    Made *p = (Made *)realloc(track->made, cap * sizeof *p);
    if (!p) return false;
    track->made = p, track->cap = cap;
    return true;
}

static void note_made(Kind kind, uint64_t handle) {
    __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
    if (track) track->made[track->n++] = {kind, handle};
}

static void note_gone(Kind kind, uint64_t handle) {
    if (!handle) return;
    __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
    for (size_t i = track ? track->n : 0; i-- > 0;)
        if (track->made[i].kind == kind && track->made[i].handle == handle) {
            memmove(track->made + i, track->made + i + 1, (--track->n - i) * sizeof *track->made);
            return;
        }
}

#define OBJECT(kind, T, Info, create, destroy)                                                                         \
    static PFN_##create real_##create;                                                                                 \
    static PFN_##destroy real_##destroy;                                                                               \
    static VKAPI_ATTR VkResult VKAPI_CALL t_##create(VkDevice d, const Info *i, const VkAllocationCallbacks *a, T *out) { \
        if (fails() || !reserve(1)) return *out = VK_NULL_HANDLE, note_result(VK_ERROR_OUT_OF_DEVICE_MEMORY);          \
        VkResult r = __atomic_load_n(&real_##create, __ATOMIC_RELAXED)(d, i, a, out);                                  \
        if (r == VK_SUCCESS) note_made(kind, (uint64_t)*out);                                                          \
        return note_result(r);                                                                                         \
    }                                                                                                                  \
    static VKAPI_ATTR void VKAPI_CALL t_##destroy(VkDevice d, T h, const VkAllocationCallbacks *a) {                   \
        note_gone(kind, (uint64_t)h);                                                                                  \
        __atomic_load_n(&real_##destroy, __ATOMIC_RELAXED)(d, h, a);                                                   \
    }
#define OBJECTS(X)                                                                                                     \
    X(K_POOL, VkDescriptorPool, VkDescriptorPoolCreateInfo, vkCreateDescriptorPool, vkDestroyDescriptorPool)           \
    X(K_SAMPLER, VkSampler, VkSamplerCreateInfo, vkCreateSampler, vkDestroySampler)                                    \
    X(K_SET_LAYOUT, VkDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, vkCreateDescriptorSetLayout,               \
      vkDestroyDescriptorSetLayout)                                                                                    \
    X(K_BUFFER, VkBuffer, VkBufferCreateInfo, vkCreateBuffer, vkDestroyBuffer)                                         \
    X(K_BUFFER_VIEW, VkBufferView, VkBufferViewCreateInfo, vkCreateBufferView, vkDestroyBufferView)                    \
    X(K_IMAGE, VkImage, VkImageCreateInfo, vkCreateImage, vkDestroyImage)                                              \
    X(K_IMAGE_VIEW, VkImageView, VkImageViewCreateInfo, vkCreateImageView, vkDestroyImageView)                         \
    X(K_MODULE, VkShaderModule, VkShaderModuleCreateInfo, vkCreateShaderModule, vkDestroyShaderModule)                 \
    X(K_LAYOUT, VkPipelineLayout, VkPipelineLayoutCreateInfo, vkCreatePipelineLayout, vkDestroyPipelineLayout)         \
    X(K_MEMORY, VkDeviceMemory, VkMemoryAllocateInfo, vkAllocateMemory, vkFreeMemory)
OBJECTS(OBJECT)

static PFN_vkCreateComputePipelines real_vkCreateComputePipelines;
static PFN_vkDestroyPipeline real_vkDestroyPipeline;
static VKAPI_ATTR VkResult VKAPI_CALL t_vkCreateComputePipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                                 const VkComputePipelineCreateInfo *i,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out) {
    if (fails() || !reserve(n)) {
        memset(out, 0, n * sizeof *out);
        return note_result(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    VkResult r = __atomic_load_n(&real_vkCreateComputePipelines, __ATOMIC_RELAXED)(d, cache, n, i, a, out);
    for (uint32_t k = 0; k < n; k++)
        if (out[k]) note_made(K_PIPELINE, (uint64_t)out[k]);
    return note_result(r);
}
static VKAPI_ATTR void VKAPI_CALL t_vkDestroyPipeline(VkDevice d, VkPipeline h, const VkAllocationCallbacks *a) {
    note_gone(K_PIPELINE, (uint64_t)h);
    __atomic_load_n(&real_vkDestroyPipeline, __ATOMIC_RELAXED)(d, h, a);
}

// The tracking function for name, which calls f
static PFN_vkVoidFunction tracked(const char *name, PFN_vkVoidFunction f) {
#define PAIR(fn)                                                                                                       \
    if (!strcmp(name, #fn)) {                                                                                          \
        __atomic_store_n(&real_##fn, (PFN_##fn)f, __ATOMIC_RELAXED);                                                   \
        return (PFN_vkVoidFunction)t_##fn;                                                                             \
    }
#define BOTH(kind, T, Info, create, destroy) PAIR(create) PAIR(destroy)
    OBJECTS(BOTH)
    PAIR(vkCreateComputePipelines)
    PAIR(vkDestroyPipeline)
    return f;
}

static void track_begin(Track *t) {
    *t = {};
    track = t;
}

// keep: the set-up is done and its objects are the contexts' own. Otherwise what is left of it is destroyed, the newest
// first, so that a view goes before its image and an image before its memory. How many that were.
static size_t track_end(VkCtx *c, bool keep) {
    Track *t = track;
    track = nullptr;
    size_t n = keep ? 0 : t->n;
    for (size_t i = n; i-- > 0;) {
        uint64_t h = t->made[i].handle;
        switch (t->made[i].kind) {
#define GONE(kind, T, Info, create, destroy) case kind: t_##destroy(c->device, (T)h, nullptr); break;
            OBJECTS(GONE)
            case K_PIPELINE: t_vkDestroyPipeline(c->device, (VkPipeline)h, nullptr); break;
        }
    }
    free(t->made);
    return n;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    if (f) return tracked(name, f);
    if (!strcmp(name, "vkGetBufferMemoryRequirements2KHR")) return vkGetDeviceProcAddr(d, "vkGetBufferMemoryRequirements2");
    if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label;
    if (!strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label_end;
    return nullptr;
}

int vk_ffx_usable(const VkCtx *c, const char **why) {
    *why = "";
    VkPhysicalDeviceSubgroupProperties sg = {};
    sg.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 p2 = {};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &sg;
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    VkPhysicalDeviceVulkan12Features f12 = {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    __atomic_store_n(&fp16, f12.shaderFloat16 && f2.features.shaderInt16, __ATOMIC_RELAXED);
    // What the SPIR-V of the blobs declares
    const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                       VK_SUBGROUP_FEATURE_QUAD_BIT;
    if (!f2.features.shaderStorageImageExtendedFormats) *why = "shaderStorageImageExtendedFormats";
    else if (!f2.features.shaderStorageImageWriteWithoutFormat) *why = "shaderStorageImageWriteWithoutFormat";
    else if (!(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || (sg.supportedOperations & ops) != ops)
        *why = "subgroup arithmetic and quad operations in compute shaders";
    return !**why;
}

// The backend's scratch memory holds its context, whose members the compiler aligns past malloc's 16 bytes.
static void *scratch_alloc(size_t n) {
#ifdef _WIN32
    void *p = _aligned_malloc(n, 256);
#else
    void *p = aligned_alloc(256, n);
#endif
    if (p) memset(p, 0, n);
    return p;
}

static void scratch_free(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

// The device as the SDK takes it. ffxGetDeviceVK keeps one copy for the process, which every set-up reads, and the menu's
// check sets up beside the game: it is written once per device.
static FfxDevice sdk_device(VkCtx *c) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static VkDevice have;
    static FfxDevice dev;
    plat_mutex_lock(&lock);
    if (have != c->device) {
        VkDeviceContext dc = {c->device, c->phys, device_proc};
        dev = ffxGetDeviceVK(&dc);
        have = c->device;
    }
    FfxDevice d = dev;
    plat_mutex_unlock(&lock);
    return d;
}

static bool make_backend(VkCtx *c, FfxInterface *i, void **scratch, size_t contexts) {
    size_t n = (ffxGetScratchMemorySizeVK(c->phys, contexts) + 255) & ~(size_t)255;
    if (!(*scratch = scratch_alloc(n))) {
        rt_log("vk: FSR: no memory for the built-in backend (%zu bytes)", n);
        return false;
    }
    FfxErrorCode e = ffxGetInterfaceVK(i, sdk_device(c), *scratch, n, contexts);
    if (e != FFX_OK) rt_log("vk: FSR: the built-in backend failed (%#x)", (unsigned)e);
    return e == FFX_OK;
}

static void message(FfxMsgType type, const wchar_t *msg) {
    static int n;
    if (n++ >= 32) return;   // the checks repeat every frame
    char b[256];
    size_t i = 0;
    for (; msg && msg[i] && i + 1 < sizeof b; i++) b[i] = msg[i] < 128 ? (char)msg[i] : '?';
    b[i] = 0;
    rt_log("vk: FSR %s: %s", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

#define TRY(x, what)                                                                                                   \
    do {                                                                                                               \
        FfxErrorCode e_ = (x);                                                                                         \
        if (e_ != FFX_OK) {                                                                                            \
            rt_log("vk: FSR: %s failed (%#x)", what, (unsigned)e_);                                                    \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

// An image of the runtime as the SDK sees it. The backend takes it in the layout of state and returns it there after
// the dispatch (UnregisterResourcesVK): COMPUTE_READ is SHADER_READ_ONLY_OPTIMAL, GENERIC_READ and UNORDERED_ACCESS are
// GENERAL.
static FfxResource res(VkImage img, VkFormat f, uint32_t w, uint32_t h, FfxResourceStates state, const wchar_t *name) {
    bool write = state == FFX_RESOURCE_STATE_UNORDERED_ACCESS;
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (write ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    return ffxGetResourceVK((void *)img, ffxGetImageResourceDescriptionVK(img, ci, write ? FFX_RESOURCE_USAGE_UAV
                            : FFX_RESOURCE_USAGE_READ_ONLY), name, state);
}

// ---------------------------------------------------------------- the upscaler
static struct Up {
    bool live;
    FfxFsr3UpscalerContext ctx;
    FfxInterface iface;
    void *scratch;
    FfxUInt32 shared_id;
    FfxResourceInternal shared[3];   // dilated depth, dilated motion vectors, the previous frame's nearest depth
    uint64_t ring[FFX_MAX_QUEUED_FRAMES];   // the timeline values of the last dispatches (wait_ring)
    uint32_t n;
    uint64_t waits;
    bool check;   // vk_ffx_up_check's, no log
} U;

static bool up_create(Up &u, VkCtx *c, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    enum { CONTEXTS = FFX_FSR3UPSCALER_CONTEXT_COUNT + 1 };   // and the shared images'
    FfxFsr3UpscalerContextDescription cd = {};
    if (!make_backend(c, &cd.backendInterface, &u.scratch, CONTEXTS)) return false;
    // SDR colour, render-size motion vectors without the jitter and standard depth with a finite far plane are the
    // defaults; the render size changes while the game runs.
    cd.flags = FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION;
    cd.maxRenderSize = {w, h};
    cd.maxUpscaleSize = {ow, oh};
    cd.fpMessage = message;
    TRY(ffxFsr3UpscalerContextCreate(&u.ctx, &cd), "the upscaler's context");
    u.iface = cd.backendInterface;
    u.live = true;   // from here vk_ffx_up_destroy takes the context down
    FfxFsr3UpscalerSharedResourceDescriptions sd = {};
    TRY(ffxFsr3UpscalerGetSharedResourceDescriptions(&u.ctx, &sd), "the shared images' descriptions");
    TRY(u.iface.fpCreateBackendContext(&u.iface, FFX_EFFECT_SHAREDRESOURCES, nullptr, &u.shared_id), "the shared context");
    const FfxCreateResourceDescription *rd[3] = {&sd.dilatedDepth, &sd.dilatedMotionVectors, &sd.reconstructedPrevNearestDepth};
    for (int i = 0; i < 3; i++)
        TRY(u.iface.fpCreateResource(&u.iface, rd[i], u.shared_id, &u.shared[i]), "a shared image");
    FfxEffectMemoryUsage mu = {};
    ffxFsr3UpscalerContextGetGpuMemoryUsage(&u.ctx, &mu);
    if (!u.check)
        rt_log("vk: FSR 3.1.4 (built in, %s floats) holds %.1f MiB of GPU memory",
               __atomic_load_n(&fp16, __ATOMIC_RELAXED) ? "16-bit" : "32-bit", (double)mu.totalUsageInBytes / 1048576.0);
    return true;
}

static void up_free(Up &u) {
    scratch_free(u.scratch);
    memset(&u, 0, sizeof u);
}

static void up_destroy(Up &u) {
    if (u.live) {
        if (u.waits) rt_log("vk: FSR: %llu upscales waited for an earlier submission", (unsigned long long)u.waits);
        for (int i = 0; i < 3; i++)
            if (u.shared[i].internalIndex) u.iface.fpDestroyResource(&u.iface, u.shared[i], u.shared_id);
        if (u.shared_id) u.iface.fpDestroyBackendContext(&u.iface, u.shared_id);
        ffxFsr3UpscalerContextDestroy(&u.ctx);
    }
    up_free(u);
}

// A failed set-up is undone object by object: the SDK's own destroy functions need a whole context.
static bool up_try(Up &u, VkCtx *c, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    Track t;
    track_begin(&t);
    bool ok = up_create(u, c, w, h, ow, oh) && !t.failed;
    track_end(c, ok);
    if (!ok) up_free(u);
    return ok;
}

int vk_ffx_up_create(VkCtx *c, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) { return up_try(U, c, w, h, ow, oh); }

void vk_ffx_up_destroy(void) { up_destroy(U); }

int vk_ffx_up_check(VkCtx *c) {
    Up *u = (Up *)scratch_alloc((sizeof *u + 255) & ~(size_t)255);   // the context is large for a stack
    if (!u) return 0;
    u->check = true;
    bool ok = up_try(*u, c, 320, 180, 640, 360);
    up_destroy(*u);
    scratch_free(u);
    return ok;
}

// The backend writes a dispatch's descriptor sets and constants into rings FFX_MAX_QUEUED_FRAMES deep and destroys the
// image views of the oldest dispatch at the end of each one, without knowing the GPU. With the game's frames in flight
// that dispatch is long done; should it not be, its submission is waited for here (counted).
static void wait_ring(uint64_t fence) {
    if (U.n >= FFX_MAX_QUEUED_FRAMES - 1) {
        uint64_t back = U.ring[(U.n + 1) % FFX_MAX_QUEUED_FRAMES];   // the dispatch FFX_MAX_QUEUED_FRAMES - 1 back
        if (back && vk_sync_fence_done() < back) {
            if (!U.waits++) rt_log("vk: FSR waits for a submission %d upscales back", FFX_MAX_QUEUED_FRAMES - 1);
            vk_sync_fence_wait(back, 2000000000ull);
        }
    }
    U.ring[U.n % FFX_MAX_QUEUED_FRAMES] = fence;
    U.n++;
}

int vk_ffx_up_dispatch(VkCommandBuffer cb, const VkFfxUpscale *f) {
    if (!U.live) return 0;
    wait_ring(f->fence);
    FfxFsr3UpscalerDispatchDescription d = {};
    d.commandList = ffxGetCommandListVK(cb);
    d.color = res(f->colour, VK_FORMAT_R8G8B8A8_UNORM, f->w, f->h, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Color");
    d.depth = res(f->depth, VK_FORMAT_R32_SFLOAT, f->w, f->h, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Depth");
    d.motionVectors = res(f->motion, VK_FORMAT_R16G16B16A16_SFLOAT, f->w, f->h, FFX_RESOURCE_STATE_COMPUTE_READ, L"FSR3_Motion");
    d.output = res(f->out, VK_FORMAT_R16G16B16A16_SFLOAT, f->ow, f->oh, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"FSR3_Output");
    d.dilatedDepth = U.iface.fpGetResource(&U.iface, U.shared[0]);
    d.dilatedMotionVectors = U.iface.fpGetResource(&U.iface, U.shared[1]);
    d.reconstructedPrevNearestDepth = U.iface.fpGetResource(&U.iface, U.shared[2]);
    d.jitterOffset = {f->jitter[0], f->jitter[1]};
    d.motionVectorScale = {1.0f, 1.0f};   // the vectors are in pixels already
    d.renderSize = {f->rw, f->rh};
    d.upscaleSize = {f->ow, f->oh};
    d.frameTimeDelta = f->frame_ms;
    d.preExposure = 1.0f;
    d.reset = f->reset != 0;
    d.cameraNear = f->near_z;
    d.cameraFar = f->far_z;
    d.cameraFovAngleVertical = f->fov;
    d.viewSpaceToMetersFactor = 1.0f;
    FfxErrorCode e = ffxFsr3UpscalerContextDispatch(&U.ctx, &d);
    if (e != FFX_OK) rt_log("vk: FSR: the built-in upscale failed (%#x)", (unsigned)e);
    return e == FFX_OK;
}

// ---------------------------------------------------------------- frame interpolation
enum { R_OF_VECTOR, R_OF_SCD, R_DILATED_DEPTH, R_DILATED_MV = R_DILATED_DEPTH + 2, R_PREV_DEPTH = R_DILATED_MV + 2,
       R_COUNT = R_PREV_DEPTH + 2 };

struct Img {
    VkImage img;
    VmaAllocation mem;
};

static struct Fg {
    bool live;
    uint32_t w, h, rw, rh;
    FfxInterface shared, iface;
    void *scratch_shared, *scratch;
    uint32_t shared_id;
    FfxOpticalflowContext of;
    FfxFrameInterpolationContext fi;
    bool of_live, fi_live, shared_live;
    FfxResourceInternal res[R_COUNT];
    int n_res;
    unsigned toggle;
    uint64_t frame_id;
    Img hudless, out;
    bool fresh;   // the two images have not been in a command buffer yet
    bool check;   // vk_ffx_fg_check's, no log
} G;

static bool make_image(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, ii.imageType = VK_IMAGE_TYPE_2D, ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1}, ii.mipLevels = 1, ii.arrayLayers = 1, ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL, ii.usage = usage, ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->mem, NULL) == VK_SUCCESS) return true;
    m->img = VK_NULL_HANDLE;
    rt_log("vk: frame generation: no memory for a %ux%u picture", w, h);
    return false;
}

static bool fg_create(Fg &g, VkCtx *c, uint32_t w, uint32_t h, uint32_t rw, uint32_t rh) {
    g.w = w, g.h = h, g.rw = rw, g.rh = rh;
    if (!make_backend(c, &g.shared, &g.scratch_shared, 1) || !make_backend(c, &g.iface, &g.scratch, 2)) return false;
    TRY(g.shared.fpCreateBackendContext(&g.shared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &g.shared_id),
        "frame generation's shared context");
    g.shared_live = true;
    FfxOpticalflowContextDescription od = {};
    od.backendInterface = g.iface;
    od.resolution = {w, h};
    TRY(ffxOpticalflowContextCreate(&g.of, &od), "the optical flow context");
    g.of_live = true;
    FfxFrameInterpolationContextDescription fd = {};   // flags 0: standard depth with a finite far plane
    fd.backendInterface = g.iface;
    fd.maxRenderSize = {rw, rh};
    fd.displaySize = {w, h};
    fd.backBufferFormat = ffxGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);
    fd.previousInterpolationSourceFormat = ffxGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);   // the HUD-less copy's
    TRY(ffxFrameInterpolationContextCreate(&g.fi, &fd), "the frame interpolation context");
    g.fi_live = true;
    FfxOpticalflowSharedResourceDescriptions ord = {};
    TRY(ffxOpticalflowGetSharedResourceDescriptions(&g.of, &ord), "the optical flow's descriptions");
    FfxFrameInterpolationSharedResourceDescriptions frd = {};
    TRY(ffxFrameInterpolationGetSharedResourceDescriptions(&g.fi, &frd), "the frame interpolation's descriptions");
    const FfxCreateResourceDescription *rd[R_COUNT] = {};
    rd[R_OF_VECTOR] = &ord.opticalFlowVector;
    rd[R_OF_SCD] = &ord.opticalFlowSCD;
    for (int i = 0; i < 2; i++) {
        rd[R_DILATED_DEPTH + i] = &frd.dilatedDepth;
        rd[R_DILATED_MV + i] = &frd.dilatedMotionVectors;
        rd[R_PREV_DEPTH + i] = &frd.reconstructedPrevNearestDepth;
    }
    for (; g.n_res < R_COUNT; g.n_res++)
        TRY(g.shared.fpCreateResource(&g.shared, rd[g.n_res], g.shared_id, &g.res[g.n_res]), "frame generation's shared image");
    const VkImageUsageFlags copy = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!make_image(c, &g.hudless, w, h, copy) || !make_image(c, &g.out, w, h, copy | VK_IMAGE_USAGE_STORAGE_BIT)) return false;
    FfxEffectMemoryUsage a = {}, b = {};
    ffxFrameInterpolationContextGetGpuMemoryUsage(&g.fi, &a);
    ffxOpticalflowContextGetGpuMemoryUsage(&g.of, &b);
    if (!g.check)
        rt_log("vk: frame generation (FSR 3.1.4, built in): %ux%u from scenes up to %ux%u, %.1f MiB of GPU memory", w, h, rw,
               rh, (double)(a.totalUsageInBytes + b.totalUsageInBytes + 2ull * w * h * 4) / 1048576.0);
    g.fresh = g.live = true;
    return true;
}

static void fg_free(Fg &g) {
    VkCtx *c = vk_ctx();
    scratch_free(g.scratch_shared);
    scratch_free(g.scratch);
    if (c && g.hudless.img) vmaDestroyImage(c->vma, g.hudless.img, g.hudless.mem);
    if (c && g.out.img) vmaDestroyImage(c->vma, g.out.img, g.out.mem);
    memset(&g, 0, sizeof g);
}

static void fg_destroy(Fg &g) {
    for (int i = 0; i < g.n_res; i++) g.shared.fpDestroyResource(&g.shared, g.res[i], g.shared_id);
    if (g.fi_live) ffxFrameInterpolationContextDestroy(&g.fi);
    if (g.of_live) ffxOpticalflowContextDestroy(&g.of);
    if (g.shared_live) g.shared.fpDestroyBackendContext(&g.shared, g.shared_id);
    fg_free(g);
}

static bool fg_try(Fg &g, VkCtx *c, uint32_t w, uint32_t h, uint32_t rw, uint32_t rh) {   // as up_try
    Track t;
    track_begin(&t);
    bool ok = fg_create(g, c, w, h, rw, rh) && !t.failed;
    track_end(c, ok);
    if (!ok) fg_free(g);
    return ok;
}

int vk_ffx_fg_create(VkCtx *c, uint32_t w, uint32_t h, uint32_t rw, uint32_t rh) {
    fg_destroy(G);
    return fg_try(G, c, w, h, rw, rh);
}

void vk_ffx_fg_destroy(void) { fg_destroy(G); }

void vk_ffx_fg_lost(void) { G.w = 0; }

int vk_ffx_fg_check(VkCtx *c) {
    Fg *g = (Fg *)scratch_alloc((sizeof *g + 255) & ~(size_t)255);
    if (!g) return 0;
    g->check = true;
    bool ok = fg_try(*g, c, 640, 360, 320, 180);
    fg_destroy(*g);
    scratch_free(g);
    return ok;
}

int vk_ffx_live(void) { return __atomic_load_n(&live, __ATOMIC_RELAXED); }

int vk_ffx_probe(VkCtx *c) {
    const int before = vk_ffx_live();
    int bad = 0;
    for (int fg = 0; fg < 2; fg++) {
        int n = 0, refused = 0;
        for (bool hit = true; hit && n < 4096; n++) {   // ends with the set-up that makes all of its objects
            fail_in = n;
            int ok = fg ? vk_ffx_fg_check(c) : vk_ffx_up_check(c);
            hit = fail_in < 0;
            fail_in = -1;
            if (hit && !ok) refused++;
            if (!hit && !ok) {
                printf("probe-fsr: the built-in %s could not be set up\n", fg ? "frame generation" : "upscaler");
                bad++;
            }
            if (vk_ffx_live() == before) continue;
            printf("probe-fsr: creation %d of the %s's failed: %d objects left\n", n, fg ? "frame generation" : "upscaler",
                   vk_ffx_live() - before);
            bad++;
        }
        printf("probe-fsr: the built-in %s: %d of %d creations failed in turn stop it\n", fg ? "frame generation" : "upscaler",
               refused, n - 1);
    }
    return bad;
}

int vk_ffx_fg_fits(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh) {
    return G.live && G.w == w && G.h == h && rw <= G.rw && rh <= G.rh;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = {};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb, &di);
}

static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

static FfxResource shared(int i) { return G.shared.fpGetResource(&G.shared, G.res[i]); }

VkImage vk_ffx_fg_record(VkCommandBuffer cb, const VkFsrFrame *fr, VkImage win, VkFormat fmt, int reset) {
    if (!vk_ffx_fg_fits(fr->ow, fr->oh, fr->rw, fr->rh)) return VK_NULL_HANDLE;
    full_barrier(cb);   // what earlier submissions wrote (the frame, the last interpolation)
    if (G.fresh) {
        to_general(cb, G.hudless.img), to_general(cb, G.out.img);
        G.fresh = false;
        reset = 1;
    }
    // The upscaled picture before the HUD as 8 bits a channel. The same blit made the window texture's background out of
    // it (vk_fsr3.c record_upscale), so the two are equal byte for byte wherever the HUD is not, which is how the SDK
    // tells the HUD apart (it compares the two) and keeps it out of the interpolation.
    VkImageBlit b = {};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = b.dstOffsets[1] = {(int32_t)G.w, (int32_t)G.h, 1};
    vkCmdBlitImage(cb, fr->hudless, VK_IMAGE_LAYOUT_GENERAL, G.hudless.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    full_barrier(cb);

    float ms = fr->frame_ms > 0.0f && fr->frame_ms < 100.0f ? fr->frame_ms : 16.7f;
    G.toggle ^= 1;
    FfxCommandList cl = ffxGetCommandListVK(cb);
    FfxFrameInterpolationPrepareDescription pd = {};   // the camera's axes are not known here (vk_fg.c gives none either)
    pd.commandList = cl;
    pd.renderSize = {fr->rw, fr->rh};
    pd.jitterOffset = {fr->jitter[0], fr->jitter[1]};
    pd.motionVectorScale = {1.0f, 1.0f};   // in pixels; the SDK divides by the render size
    pd.frameTimeDelta = ms;
    pd.cameraNear = fr->near_z;
    pd.cameraFar = fr->far_z;
    pd.viewSpaceToMetersFactor = 1.0f;
    pd.cameraFovAngleVertical = fr->fov;
    pd.depth = res(fr->depth, VK_FORMAT_R32_SFLOAT, fr->rw, fr->rh, FFX_RESOURCE_STATE_COMPUTE_READ, L"FG_Depth");
    pd.motionVectors = res(fr->motion, VK_FORMAT_R16G16B16A16_SFLOAT, fr->rw, fr->rh, FFX_RESOURCE_STATE_COMPUTE_READ, L"FG_Motion");
    pd.frameID = ++G.frame_id;
    pd.dilatedDepth = shared(R_DILATED_DEPTH + G.toggle);
    pd.dilatedMotionVectors = shared(R_DILATED_MV + G.toggle);
    pd.reconstructedPrevDepth = shared(R_PREV_DEPTH + G.toggle);
    FfxErrorCode e = ffxFrameInterpolationPrepare(&G.fi, &pd);

    FfxResource hud_less = res(G.hudless.img, VK_FORMAT_R8G8B8A8_UNORM, G.w, G.h, FFX_RESOURCE_STATE_GENERIC_READ, L"FG_Hudless");
    if (e == FFX_OK) {
        FfxOpticalflowDispatchDescription od = {};
        od.commandList = cl;
        od.color = hud_less;
        od.reset = reset != 0;
        od.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        od.opticalFlowVector = shared(R_OF_VECTOR);
        od.opticalFlowSCD = shared(R_OF_SCD);
        e = ffxOpticalflowContextDispatch(&G.of, &od);
    }
    if (e == FFX_OK) {
        FfxFrameInterpolationDispatchDescription dd = {};
        dd.commandList = cl;
        dd.displaySize = {G.w, G.h};
        dd.renderSize = {fr->rw, fr->rh};
        dd.currentBackBuffer = res(win, fmt, G.w, G.h, FFX_RESOURCE_STATE_GENERIC_READ, L"FG_Color");
        dd.currentBackBuffer_HUDLess = hud_less;
        dd.output = res(G.out.img, VK_FORMAT_R8G8B8A8_UNORM, G.w, G.h, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"FG_Output");
        dd.interpolationRect = {0, 0, (int)G.w, (int)G.h};
        dd.opticalFlowVector = shared(R_OF_VECTOR);
        dd.opticalFlowSceneChangeDetection = shared(R_OF_SCD);
        dd.opticalFlowBufferSize = {G.w / 8, G.h / 8};   // the shaders do not read it
        dd.opticalFlowScale = {1.0f / G.w, 1.0f / G.h};
        dd.opticalFlowBlockSize = 8;
        dd.cameraNear = fr->near_z;
        dd.cameraFar = fr->far_z;
        dd.cameraFovAngleVertical = fr->fov;
        dd.viewSpaceToMetersFactor = 1.0f;
        dd.frameTimeDelta = ms;
        dd.reset = reset != 0;
        dd.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dd.minMaxLuminance[0] = 0;
        dd.minMaxLuminance[1] = 1;
        dd.frameID = G.frame_id;
        dd.dilatedDepth = shared(R_DILATED_DEPTH + G.toggle);
        dd.dilatedMotionVectors = shared(R_DILATED_MV + G.toggle);
        dd.reconstructedPrevDepth = shared(R_PREV_DEPTH + G.toggle);
        e = ffxFrameInterpolationDispatch(&G.fi, &dd);
    }
    full_barrier(cb);
    if (e == FFX_OK) return G.out.img;
    rt_log("vk: frame generation: the built-in interpolation failed (%#x)", (unsigned)e);
    return VK_NULL_HANDLE;
}
