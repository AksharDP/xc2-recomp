// Vulkan instance, physical device choice, logical device, VMA, timeline semaphore and the validation messenger.
// Ported from LAS Recomp, runtime/nvn/vulkan/device.c:33-330
// (https://github.com/peiikai/LASRecomp @ da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai),
// without SDL. The VMA implementation is runtime/vk_vma.cpp (VMA is C++ only).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_require.h"
#include "platform/platform.h"

static VkCtx g_vk;
static int g_ready;
static PlatMutex g_lock = PLAT_MUTEX_INIT;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    int err = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (err) plat_inc32(&g_vk.validation_errors);
    static volatile int32_t logged;
    if (plat_inc32(&logged) <= 500) rt_log("vk validation %s: %s", err ? "error" : "warning", d->pMessage);
    return VK_FALSE;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int layer_present(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *l = calloc(n ? n : 1, sizeof *l);
    vkEnumerateInstanceLayerProperties(&n, l);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(l[i].layerName, name)) found = 1;
    free(l);
    return found;
}

// Mesa's drivers (RADV on Linux and the Steam Deck) keep a shader cache of their own beside our pipeline cache, by
// default in ~/.cache and up to 1 GB: about 35 MB in a minute of the title screen. A Deck's /home is small and the game
// usually sits on the SD card, so unless MESA_SHADER_CACHE_DIR names a folder, it goes to mesa-cache beside the exe.
static void mesa_cache_beside_exe(void) {
    char dir[PLAT_PATH_MAX * 2];
    if (plat_getenv("MESA_SHADER_CACHE_DIR", dir, sizeof dir)) return;
    int len = plat_exe_path(dir, sizeof dir - 16);
    char *cut = len ? plat_last_sep(dir) : NULL;
    if (!cut) return;
    strcpy(cut + 1, "mesa-cache");
    plat_setenv("MESA_SHADER_CACHE_DIR", dir);
}

static int create_instance(void) {
    mesa_cache_beside_exe();
    const char *ext[10];
    uint32_t n_ext = (uint32_t)gfx_window_vk_extensions(ext, 7);
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    // XC2_VK_VALIDATION=sync adds synchronization validation, which checks the barriers themselves (and is slow).
    const char *vmode = cfg_get("XC2_VK_VALIDATION");
    int sync_val = vmode && !strcmp(vmode, "sync");
    int validate = cfg_bool("XC2_VK_VALIDATION") || sync_val;
    if (validate && !layer_present(layers[0])) {
        rt_log("vk: XC2_VK_VALIDATION set but %s is not installed; continuing without", layers[0]);
        validate = 0;
        sync_val = 0;
    }
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (sync_val) ext[n_ext++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
    const VkValidationFeatureEnableEXT sync_enable[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    const VkValidationFeaturesEXT features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .enabledValidationFeatureCount = 1,
                                              .pEnabledValidationFeatures = sync_enable};
    if (sync_val) rt_log("vk: synchronization validation on (XC2_VK_VALIDATION=sync)");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "XC2 Recomp",
                             .pEngineName = "XC2 Recomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers,
                               .pNext = sync_val ? &features : NULL};
    rdoc_init();
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.instance);
    if (r != VK_SUCCESS) { rt_log("vk: vkCreateInstance failed (%d); is a Vulkan 1.3 driver installed?", (int)r); return 0; }
    g_vk.validation = validate;
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.instance, &mi, NULL, &g_vk.messenger);
    }
    return 1;
}

// XC2_VK_HIDE_EXT=<name>[,<name>...]: the device's extension list without those names, in the device choice and in
// device creation, so a GPU that has an extension runs the path of a driver without it. XC2_VK_HIDE_EXT=
// VK_EXT_external_memory_host runs the shadow storage such a driver gets (vk_memory.c mirror_mode).
static int ext_hidden(const char *name) {
    const char *s = cfg_get("XC2_VK_HIDE_EXT");
    size_t n = strlen(name);
    while (s && *s) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        if (len == n && !strncmp(s, name, n)) return 1;
        s = comma ? comma + 1 : NULL;
    }
    return 0;
}

// vkEnumerateDeviceExtensionProperties without the XC2_VK_HIDE_EXT names (same calling convention).
static VKAPI_ATTR VkResult VKAPI_CALL dev_exts(VkPhysicalDevice pd, const char *layer, uint32_t *count,
                                               VkExtensionProperties *out) {
    uint32_t n = 0;
    VkResult r = vkEnumerateDeviceExtensionProperties(pd, layer, &n, NULL);
    if (r < 0) return r;
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof *all);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((r = vkEnumerateDeviceExtensionProperties(pd, layer, &n, all)) < 0) return free(all), r;
    uint32_t k = 0, cap = out ? *count : UINT32_MAX;
    r = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        if (ext_hidden(all[i].extensionName)) continue;
        if (k == cap) { r = VK_INCOMPLETE; break; }
        if (out) out[k] = all[i];
        k++;
    }
    free(all);
    *count = k;
    return r;
}

static const VkRequireProcs k_require_procs = {vkGetPhysicalDeviceProperties, dev_exts, vkGetPhysicalDeviceFeatures2,
                                               vkGetPhysicalDeviceQueueFamilyProperties};

// Graphics+compute family that can present to the window.
static uint32_t find_gfx_family(VkPhysicalDevice pd, const VkQueueFamilyProperties *qp, uint32_t nq) {
    for (uint32_t i = 0; i < nq; i++)
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            gfx_window_vk_presentable(g_vk.instance, pd, i))
            return i;
    return UINT32_MAX;
}

static int name_has(const char *s, const char *sub) {
    for (size_t n = strlen(sub); *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return 0;
}

// First discrete GPU that meets vk_require.h (Vulkan 1.3, the required extensions and features) and has a
// graphics+present queue; XC2_GPU_INDEX (index) or XC2_GPU_NAME (case-insensitive part of the name) overrides. The
// features are checked here too, not only at device creation, so a laptop whose discrete GPU lacks one plays on its
// integrated GPU instead of stopping.
static int pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &n, NULL);
    if (!n) { rt_log("vk: no Vulkan device"); return 0; }
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.instance, &n, pd);
    const char *e = cfg_get("XC2_GPU_INDEX");   // no fixed default: the best device when unset
    int want = e && *e ? atoi(e) : -1;
    const char *name = cfg_get("XC2_GPU_NAME");
    int best = -1, best_score = -1, best_import = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        rt_log("vk: gpu %u: %s (type %d, api %u.%u.%u)", i, p.deviceName, (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (want >= 0 && (int)i != want) continue;
        if (name && *name && !name_has(p.deviceName, name)) continue;
        VkRequireResult req;
        vk_require_check(&k_require_procs, pd[i], &req);
        if (!req.usable) {
            char why[512];
            size_t k = 0;
            for (int m = 0; m < req.n_missing && m < VK_REQUIRE_MISSING_MAX; m++)
                k += (size_t)snprintf(why + k, k < sizeof why ? sizeof why - k : 0, "%s%s", m ? ", " : "", req.missing[m]);
            rt_log("vk: gpu %u skipped: lacks %s", i, why);
            continue;
        }
        if (!req.host_import)
            rt_log("vk: gpu %u: no %s; CPU-visible pools will be shadow copies", i, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, NULL);
        VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, qp);
        uint32_t fam = find_gfx_family(pd[i], qp, nq);
        free(qp);
        if (fam == UINT32_MAX) { rt_log("vk: gpu %u skipped: no graphics+compute+present queue family", i); continue; }
        int score = (int)i == want ? 100 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) { best_score = score; best = (int)i; best_import = req.host_import; }
    }
    if (best < 0) {
        if (want >= 0) rt_log("vk: XC2_GPU_INDEX=%d is not usable (see above)", want);
        else if (name && *name) rt_log("vk: no usable gpu matches XC2_GPU_NAME=%s (see above)", name);
        else rt_log("vk: no device with Vulkan 1.3 and the required extensions and features (see above)");
        free(pd);
        return 0;
    }
    g_vk.phys = pd[best];
    g_vk.device_index = best;
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.mem_props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.gfx_family = find_gfx_family(g_vk.phys, qp, nq);
    g_vk.gfx_queue_count = g_vk.gfx_family < nq ? qp[g_vk.gfx_family].queueCount : 1;
    g_vk.comp_family = UINT32_MAX;
    g_vk.xfer_family = g_vk.gfx_family;
    for (uint32_t i = 0; i < nq; i++)   // a family that does transfer only (async DMA engine)
        if ((qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            g_vk.xfer_family = i;
            break;
        }
    for (uint32_t i = 0; i < nq; i++)   // a compute-only family: frame generation needs four distinct queues in all
        if (i != g_vk.gfx_family && i != g_vk.xfer_family && qp[i].queueCount &&
            (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            g_vk.comp_family = i;
            g_vk.comp_queue_count = qp[i].queueCount < 3 ? qp[i].queueCount : 3;
            break;
        }
    free(qp);
    rt_log("vk: using gpu %d: %s, graphics family %u with %u queues", best, g_vk.props.deviceName, g_vk.gfx_family, g_vk.gfx_queue_count);
    g_vk.has_host_import = best_import;   // create_device enables the extension by the same list
    vk_mem_class_log(&g_vk.props, &g_vk.mem_props, best_import);
    return 1;
}

typedef struct {
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids;
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb;
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceRobustness2FeaturesEXT rob2;
    VkPhysicalDeviceFaultFeaturesEXT fault;
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop;
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR deriv;
#ifdef VK_EXT_SHADER_FLOAT8_EXTENSION_NAME
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8;
#endif
} FeatureChain;

// XC2_VRAM_LIMIT=<MiB>: a test of small cards on a large one. Right after the allocator is made, device-local memory
// is taken in 256 MiB blocks and held until shutdown, until the largest device-local heap has about that many MiB
// left, so the driver's paging sees a card of that size. vk_mem_vram takes the blocks out of both the usage and the
// budget it reports, so the mirror budget check (vk_memory.c, mirror_create) also measures its margin against the
// small card's budget. The blocks are never bound or touched.
#define BALLAST_MAX 256
static VkDeviceMemory ballast[BALLAST_MAX];
static int n_ballast;

static void vram_limit(void) {
    const char *e = cfg_get("XC2_VRAM_LIMIT");
    if (!e || !*e) return;
    uint64_t want = strtoull(e, NULL, 10) << 20;
    const VkPhysicalDeviceMemoryProperties *mp = &g_vk.mem_props;
    uint32_t heap = UINT32_MAX;
    for (uint32_t h = 0; h < mp->memoryHeapCount; h++)
        if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (heap == UINT32_MAX || mp->memoryHeaps[h].size > mp->memoryHeaps[heap].size))
            heap = h;
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mp->memoryTypeCount && type == UINT32_MAX; t++)
        if (mp->memoryTypes[t].heapIndex == heap && mp->memoryTypes[t].propertyFlags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            type = t;
    if (heap == UINT32_MAX || type == UINT32_MAX || !want || want >= mp->memoryHeaps[heap].size) {
        rt_log("vk: XC2_VRAM_LIMIT=%s: no device-local heap larger than that, nothing held", e);
        return;
    }
    uint64_t take = mp->memoryHeaps[heap].size - want, held = 0;
    while (held < take && n_ballast < BALLAST_MAX) {
        uint64_t n = take - held < (256ull << 20) ? take - held : 256ull << 20;
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = n, .memoryTypeIndex = type};
        if (vkAllocateMemory(g_vk.device, &ai, NULL, &ballast[n_ballast]) != VK_SUCCESS) break;
        n_ballast++, held += n;
    }
    g_vk.vram_held = held;
    rt_log("vk: XC2_VRAM_LIMIT=%s: holding %.0f MiB of heap %u (%.0f MiB) in %d blocks, %.0f MiB left", e,
           held / 1048576.0, heap, mp->memoryHeaps[heap].size / 1048576.0, n_ballast,
           (mp->memoryHeaps[heap].size - held) / 1048576.0);
}

static int create_device(void) {
    uint32_t ne = 0;
    dev_exts(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    dev_exts(g_vk.phys, NULL, &ne, ep);
    const char *ext[40];   // the required ones and every optional one below
    uint32_t n_ext = 0;
    for (int i = 0; i < VK_REQUIRE_EXT_COUNT; i++) ext[n_ext++] = vk_require_ext[i];
    // Exercise the command aliases needed for a future Vulkan 1.2 path on a current driver. API/feature
    // negotiation still requires 1.3; this switch alone does not make an older driver usable.
    int extension_dispatch = cfg_bool("XC2_VK_KHR_DISPATCH");
    if (extension_dispatch) {
        const char *needed[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME};
        for (int i = 0; i < 3; i++) {
            if (!has_ext(ep, ne, needed[i])) {
                rt_log("vk: XC2_VK_KHR_DISPATCH needs %s", needed[i]);
                free(ep);
                return 0;
            }
            if (i < 2) ext[n_ext++] = needed[i];   // extended dynamic state is enabled by OPTIONAL_EXT below
        }
    }
    // Optional (vk_require.h): without it the CPU-visible pools get shadow storage (vk_memory.c mirror_mode)
    g_vk.has_host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (g_vk.has_host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    {
        const char *h = cfg_get("XC2_VK_HIDE_EXT");
        if (h && *h) rt_log("vk: extensions hidden from the device choice and creation (XC2_VK_HIDE_EXT): %s", h);
    }

    FeatureChain *c = calloc(1, sizeof *c);
    void **tail = &c->f2.pNext;
#define CHAIN(member, stype) do { c->member.sType = stype; *tail = &c->member; tail = &c->member.pNext; } while (0)
    c->f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    CHAIN(f11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    CHAIN(f12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    CHAIN(f13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    CHAIN(clip, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT);
#define OPTIONAL_EXT(flag, name, member, stype) do { \
        if (has_ext(ep, ne, name)) { g_vk.flag = 1; ext[n_ext++] = name; CHAIN(member, stype); } } while (0)
    OPTIONAL_EXT(has_custom_border_color, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, cbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
    OPTIONAL_EXT(has_eds, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, eds, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
    OPTIONAL_EXT(has_eds2, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, eds2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT);
    OPTIONAL_EXT(has_eds3, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    OPTIONAL_EXT(has_vertex_input_dynamic_state, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, vids, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT);
    OPTIONAL_EXT(has_conditional_rendering, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, cond, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT);
    OPTIONAL_EXT(has_transform_feedback, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, xfb, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT);
    OPTIONAL_EXT(has_provoking_vertex, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, pv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT);
    OPTIONAL_EXT(has_robustness2, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, rob2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
    OPTIONAL_EXT(has_device_fault, VK_EXT_DEVICE_FAULT_EXTENSION_NAME, fault, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT);
    // Ryujinx's SPIR-V declares fragment shader interlock whenever the host reports it (vk_pipeline.c spirv_fixup).
    OPTIONAL_EXT(has_fragment_shader_interlock, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, fsi,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT);
    // Feedback loops (vk.h VkCtx.feedback): the layout, and barriers inside dynamic rendering
    int has_fbl = 0, has_lr = 0;
    if (has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
        has_ext(ep, ne, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME)) {
        has_fbl = has_lr = 1;
        ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
        CHAIN(fbl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT);
        CHAIN(lr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR);
    }
    if (has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) { g_vk.has_memory_budget = 1; ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME; }
    // Shaders of AMD's library (vk_fsr3.c) declare compute derivatives, also those of FSR 3.1.6 that every vendor's GPU
    // runs.
    int derivatives = vk_fsr3_libraries() & 1 && has_ext(ep, ne, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    if (derivatives) {
        ext[n_ext++] = VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;
        CHAIN(deriv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR);
    }
    // The networks of FSR Upscaling 4 run on the features of the game's device: cooperative matrices and, on Radeon RX
    // 9000, 8-bit floats. Without them the library has only FSR 3 left to offer. No other vendor's GPU gets FSR 4, so
    // theirs stay as they were.
    if (g_vk.props.vendorID == 0x1002 && vk_fsr3_libraries() & 1) {
        int got[2] = {has_ext(ep, ne, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME), 0};
        if (got[0]) {
            ext[n_ext++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
            CHAIN(coop, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR);
        }
#ifdef VK_EXT_SHADER_FLOAT8_EXTENSION_NAME
        if ((got[1] = got[0] && has_ext(ep, ne, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME))) {
            ext[n_ext++] = VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;
            CHAIN(f8, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT);
        }
#endif
        rt_log("vk: for FSR 4: cooperative matrix %d, 8-bit floats %d, compute derivatives %d", got[0], got[1],
               derivatives);
    }
    // GPU breadcrumbs for a lost device (vk_crumbs.c): AMD's drivers and RADV report it.
    if (has_ext(ep, ne, VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) { g_vk.has_buffer_marker = 1; ext[n_ext++] = VK_AMD_BUFFER_MARKER_EXTENSION_NAME; }
    // Graphics pipeline library: chained last so it can be dropped again when vk_caps_gpl declines it.
    void **gpl_link = NULL;
    if (has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        gpl_link = tail;
        CHAIN(gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
    }
    free(ep);

    // The driver fills the chain with what it supports; the same chain then enables all of it (robustBufferAccess
    // stays on: guest shaders index constant buffers freely and out-of-range reads must stay harmless).
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &c->f2);
    vk_caps_driver(g_vk.phys);
    vk_caps_features(&c->f2.features);
    if (gpl_link) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        if (!vk_caps_gpl(&c->gpl, &gp)) { *gpl_link = NULL; n_ext -= 2; }
    }
    const VkPhysicalDeviceFeatures *h = &c->f2.features;
    const char *miss = NULL;   // pick_device checked the same list; vk_caps_features may have cleared one since
    if (vk_require_features_missing(h, &c->f12, &c->f13, &c->clip, &miss, 1)) {
        rt_log("vk: required feature missing on %s: %s", g_vk.props.deviceName, miss);
        free(c);
        return 0;
    }
    if (!c->fault.deviceFault) g_vk.has_device_fault = 0;   // vkGetDeviceFaultInfoEXT needs the feature, not just the extension
    g_vk.has_buffer_device_address = c->f12.bufferDeviceAddress;
    g_vk.has_astc_ldr = h->textureCompressionASTC_LDR;
    {
        const char *e = cfg_get("XC2_FEEDBACK");
        int layout_ok = has_fbl && has_lr && c->fbl.attachmentFeedbackLoopLayout && c->lr.dynamicRenderingLocalRead;
        g_vk.feedback = e && !strcmp(e, "0") ? VK_FEEDBACK_OFF
                      : layout_ok && !(e && !strcmp(e, "restart")) ? VK_FEEDBACK_LAYOUT : VK_FEEDBACK_RESTART;
        rt_log("vk: feedback loops: %s (attachmentFeedbackLoopLayout %d, dynamicRenderingLocalRead %d)",
               g_vk.feedback == VK_FEEDBACK_LAYOUT ? "feedback layout + by-region barrier"
               : g_vk.feedback == VK_FEEDBACK_RESTART ? "pass restart + full barrier" : "off (XC2_FEEDBACK=0)",
               has_fbl && c->fbl.attachmentFeedbackLoopLayout, has_lr && c->lr.dynamicRenderingLocalRead);
    }

    const float prio[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // one per queue of a family
    // Up to four queues from the graphics family, and up to three from a compute-only one for GPUs with a single graphics
    // queue: frame generation refuses less than four distinct queues, and its threads then never submit on the queue the
    // renderer uses (vk_fg.c pick_queues). gfx_queue_count becomes what exists, as vkGetDeviceQueue takes no other index.
    if (g_vk.gfx_queue_count > 4) g_vk.gfx_queue_count = 4;
    VkDeviceQueueCreateInfo qi[3] = {
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.gfx_family,
         .queueCount = g_vk.gfx_queue_count, .pQueuePriorities = prio}};
    uint32_t nqi = 1;
    if (g_vk.xfer_family != g_vk.gfx_family) {
        qi[1] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = g_vk.xfer_family, .queueCount = 1, .pQueuePriorities = prio};
        nqi = 2;
    }
    if (g_vk.comp_family != UINT32_MAX) {   // a compute-only family's queues, for frame generation
        qi[nqi++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                              .queueFamilyIndex = g_vk.comp_family, .queueCount = g_vk.comp_queue_count,
                                              .pQueuePriorities = prio};
    }
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &c->f2, .queueCreateInfoCount = nqi,
                             .pQueueCreateInfos = qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VkResult r = vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.device);
    free(c);
    if (r != VK_SUCCESS) { rt_log("vk: vkCreateDevice failed (%d)", (int)r); return 0; }
    if (!vk_api_load(&vk_api, g_vk.device, vkGetDeviceProcAddr, extension_dispatch)) {
        rt_log("vk: required %s command entry point missing", extension_dispatch ? "KHR/EXT" : "core");
        return 0;
    }
    rt_log("vk: renderer command dispatch: %s", extension_dispatch ? "KHR/EXT aliases" : "Vulkan 1.3 core");
    vkGetDeviceQueue(g_vk.device, g_vk.gfx_family, 0, &g_vk.gfx_queue);
    if (g_vk.xfer_family != g_vk.gfx_family) vkGetDeviceQueue(g_vk.device, g_vk.xfer_family, 0, &g_vk.xfer_queue);
    else { g_vk.xfer_queue = g_vk.gfx_queue; g_vk.xfer_shared = 1; }

    if (g_vk.has_host_import) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &emh};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        g_vk.min_imported_host_alignment = (uint32_t)emh.minImportedHostPointerAlignment;
    }
    vk_caps_init(&g_vk);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.device, .instance = g_vk.instance,
                                 .vulkanApiVersion = VK_API_VERSION_1_3,
                                 .flags = (g_vk.has_buffer_device_address ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0) |
                                          (g_vk.has_memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0),
                                 .pDeviceMemoryCallbacks = vk_headroom_callbacks()};
    if ((r = vmaCreateAllocator(&ai, &g_vk.vma)) != VK_SUCCESS) { rt_log("vk: vmaCreateAllocator failed (%d)", (int)r); return 0; }
    vram_limit();

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    if ((r = vkCreateSemaphore(g_vk.device, &si, NULL, &g_vk.timeline)) != VK_SUCCESS) {
        rt_log("vk: timeline semaphore creation failed (%d)", (int)r);
        return 0;
    }
    vk_crumbs_init(&g_vk);
    return 1;
}

static long g_errors_total;     // outlives the device so the probe can print it after vk_shutdown

static void destroy_all(void) {
    gfx_window_release_vk();
    if (g_vk.device) vkDeviceWaitIdle(g_vk.device);
    if (g_vk.device) vk_astc_release();
    if (g_vk.device) vk_crumbs_release();
    if (g_vk.timeline) vkDestroySemaphore(g_vk.device, g_vk.timeline, NULL);
    vk_headroom_stop();
    if (g_vk.vma) vmaDestroyAllocator(g_vk.vma);
    for (int i = 0; i < n_ballast; i++) vkFreeMemory(g_vk.device, ballast[i], NULL);
    n_ballast = 0;
    if (g_vk.device) vkDestroyDevice(g_vk.device, NULL);
    if (g_vk.messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (f) f(g_vk.instance, g_vk.messenger, NULL);
    }
    if (g_vk.instance) vkDestroyInstance(g_vk.instance, NULL);
    g_errors_total += g_vk.validation_errors;
    memset(&g_vk, 0, sizeof g_vk);
}

long vk_validation_errors_total(void) { return g_errors_total + (g_ready ? g_vk.validation_errors : 0); }

int vk_init(void) {
    plat_mutex_lock(&g_lock);
    if (!g_ready) {
        memset(&g_vk, 0, sizeof g_vk);
        if (create_instance() && pick_device() && create_device()) g_ready = 1, vk_headroom_start();
        else destroy_all();
    }
    int ok = g_ready;
    plat_mutex_unlock(&g_lock);
    return ok;
}

void vk_shutdown(void) {
    plat_mutex_lock(&g_lock);
    if (g_ready) { destroy_all(); g_ready = 0; }
    plat_mutex_unlock(&g_lock);
}

VkCtx *vk_ctx(void) { return g_ready ? &g_vk : NULL; }

static const char *fault_address_type(VkDeviceFaultAddressTypeEXT t) {
    switch (t) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "instruction pointer (fault)";
    default: return "unknown type";
    }
}

// The query runs once per process. Threads that hit the loss at the same time wait on the lock until it is logged, so
// none of them reaches its rt_fatal and ends the process before the report is written.
void vk_device_fault_report(VkResult r) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static int done;
    if (r != VK_ERROR_DEVICE_LOST) return;
    plat_mutex_lock(&lock);
    if (done) { plat_mutex_unlock(&lock); return; }
    done = 1;
    vk_crumbs_report();
    VkCtx *c = vk_ctx();
    PFN_vkGetDeviceFaultInfoEXT get =
        c && c->has_device_fault ? (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(c->device, "vkGetDeviceFaultInfoEXT")
                                 : NULL;
    if (!get) {
        rt_log("vk: device lost; no fault report (VK_EXT_device_fault %s)", c && c->has_device_fault ? "has no entry point"
                                                                                                    : "not enabled");
        plat_mutex_unlock(&lock);
        return;
    }
    VkDeviceFaultCountsEXT n = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult q = get(c->device, &n, NULL);
    VkDeviceFaultAddressInfoEXT *ai = calloc(n.addressInfoCount + 1, sizeof *ai);
    VkDeviceFaultVendorInfoEXT *vi = calloc(n.vendorInfoCount + 1, sizeof *vi);
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = ai, .pVendorInfos = vi};
    uint64_t binary = n.vendorBinarySize;
    n.vendorBinarySize = 0;   // the vendor binary is not fetched; its size alone is logged
    if (q == VK_SUCCESS && ai && vi) q = get(c->device, &n, &info);
    if (q != VK_SUCCESS && q != VK_INCOMPLETE) {
        rt_log("vk: device lost; vkGetDeviceFaultInfoEXT failed (%d)", (int)q);
    } else {
        rt_log("vk: device fault: \"%s\" (%u address infos, %u vendor infos, vendor binary %llu bytes)", info.description,
               n.addressInfoCount, n.vendorInfoCount, (unsigned long long)binary);
        // The faulting access lies in the precision-aligned range around the reported address.
        for (uint32_t i = 0; i < n.addressInfoCount; i++)
            rt_log("vk: device fault address %u: %s at %#llx, precision %#llx", i, fault_address_type(ai[i].addressType),
                   (unsigned long long)ai[i].reportedAddress, (unsigned long long)ai[i].addressPrecision);
        for (uint32_t i = 0; i < n.vendorInfoCount; i++)
            rt_log("vk: device fault vendor info %u: \"%s\" code %#llx data %#llx", i, vi[i].description,
                   (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
    }
    free(ai);
    free(vi);
    plat_mutex_unlock(&lock);
}

int vk_selftest(void) {
    if (!vk_init()) return 1;
    VkCtx *c = &g_vk;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 << 10,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    VkBuffer buf;
    VmaAllocation alloc;
    VkResult r = vmaCreateBuffer(c->vma, &bi, &aci, &buf, &alloc, NULL);
    if (r != VK_SUCCESS) { rt_log("vk selftest: vmaCreateBuffer failed (%d)", (int)r); return 2; }
    vmaDestroyBuffer(c->vma, buf, alloc);

    VkSemaphoreSignalInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline, .value = 1};
    if ((r = vkSignalSemaphore(c->device, &sig)) != VK_SUCCESS) { rt_log("vk selftest: vkSignalSemaphore failed (%d)", (int)r); return 3; }
    uint64_t value = 1;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &c->timeline,
                              .pValues = &value};
    if ((r = vkWaitSemaphores(c->device, &wi, 1000000000ull)) != VK_SUCCESS) { rt_log("vk selftest: vkWaitSemaphores failed (%d)", (int)r); return 4; }
    uint64_t now = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
    if (now != 1) { rt_log("vk selftest: timeline value %llu, expected 1", (unsigned long long)now); return 5; }
    return 0;
}

// --probe-vkdevice (host.h)
int probe_vkdevice(void) {
    int rc = vk_selftest();
    VkCtx *c = vk_ctx();
    if (rc || !c) { printf("probe-vkdevice: FAILED (selftest %d)\n", rc ? rc : 1); vk_shutdown(); return 1; }
    uint32_t v = c->props.apiVersion;
    printf("probe-vkdevice: device %d \"%s\" api %u.%u.%u, gfx queue family %u, transfer family %u%s, host import alignment %#x\n",
           c->device_index, c->props.deviceName, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v),
           c->gfx_family, c->xfer_family, c->xfer_shared ? " (shared queue)" : "", c->min_imported_host_alignment);
    printf("probe-vkdevice: optional: custom_border=%d eds=%d eds2=%d eds3=%d vertex_input=%d cond_render=%d xfb=%d provoking=%d "
           "robustness2=%d fault=%d budget=%d bda=%d interlock=%d validation=%d\n",
           c->has_custom_border_color, c->has_eds, c->has_eds2, c->has_eds3, c->has_vertex_input_dynamic_state,
           c->has_conditional_rendering, c->has_transform_feedback, c->has_provoking_vertex, c->has_robustness2,
           c->has_device_fault, c->has_memory_budget, c->has_buffer_device_address, c->has_fragment_shader_interlock,
           c->validation);
    vk_shutdown();
    printf("probe-vkdevice: %s (validation errors %ld)\n", g_errors_total ? "FAILED" : "passed", g_errors_total);
    return g_errors_total ? 1 : 0;
}
