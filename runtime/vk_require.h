// What the Vulkan back end needs of a device, in one place: the game's device choice and device creation
// (runtime/vk_device.c) and the installer's host check (installer/hostcheck.c) both judge a GPU by these lists, so the
// installer refuses exactly the GPUs the game would skip and names what each one lacks.
//
// Required: Vulkan 1.3; the extensions in vk_require_ext; the features vk_require_features_missing lists; a queue family
// with graphics and compute. Presenting to the game's window is checked by the game alone (it needs the window).
// VK_EXT_external_memory_host is optional: with it the CPU-visible pools are imported guest memory, without it they are
// host-visible copies kept in step with guest memory (shadow storage, runtime/vk_memory.c), as on Linux.
//
// Header-only and free of the rest of the runtime: hostcheck.c opens the Vulkan loader at run time and passes the
// entry points it looked up (VkRequireProcs); the game passes its own.
#ifndef VK_REQUIRE_H
#define VK_REQUIRE_H

#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define VK_REQUIRE_API VK_API_VERSION_1_3

static const char *const vk_require_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                                             VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME};
#define VK_REQUIRE_EXT_COUNT ((int)(sizeof vk_require_ext / sizeof *vk_require_ext))

#define VK_REQUIRE_MISSING_MAX 24

// Names of the required features the structures lack, in a fixed order, into names (at most max); returns how many
// there are. The structures are those of a vkGetPhysicalDeviceFeatures2 chain, or of the chain a device is created with.
static inline int vk_require_features_missing(const VkPhysicalDeviceFeatures *h, const VkPhysicalDeviceVulkan12Features *f12,
                                              const VkPhysicalDeviceVulkan13Features *f13,
                                              const VkPhysicalDeviceDepthClipControlFeaturesEXT *clip,
                                              const char **names, int max) {
    const struct { VkBool32 ok; const char *name; } t[] = {
        {f12->timelineSemaphore, "timelineSemaphore"},
        {f13->dynamicRendering, "dynamicRendering"},
        {f13->synchronization2, "synchronization2"},
        {h->shaderStorageImageWriteWithoutFormat, "shaderStorageImageWriteWithoutFormat"},
        {clip->depthClipControl, "depthClipControl"},
        {h->shaderInt64, "shaderInt64"},
        {h->samplerAnisotropy, "samplerAnisotropy"},
        {h->independentBlend, "independentBlend"},
        {h->geometryShader, "geometryShader"},
        {h->textureCompressionBC, "textureCompressionBC"},
    };
    int n = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (!t[i].ok) {
            if (n < max) names[n] = t[i].name;
            n++;
        }
    return n;
}

typedef struct {
    PFN_vkGetPhysicalDeviceProperties props;
    PFN_vkEnumerateDeviceExtensionProperties exts;
    PFN_vkGetPhysicalDeviceFeatures2 features2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queues;
} VkRequireProcs;

typedef struct {
    int usable;        // meets every requirement above
    int host_import;   // has VK_EXT_external_memory_host (else shadow storage)
    int n_missing;     // entries of missing, which holds at most VK_REQUIRE_MISSING_MAX
    const char *missing[VK_REQUIRE_MISSING_MAX];   // "Vulkan 1.3", extension and feature names, static strings
} VkRequireResult;

static inline void vk_require_note(VkRequireResult *r, const char *name) {
    if (r->n_missing < VK_REQUIRE_MISSING_MAX) r->missing[r->n_missing] = name;
    r->n_missing++;
}

static inline int vk_require_has(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

// Fills r for one device. The features of a device below Vulkan 1.3 are not queried: their 1.2 and 1.3 structures
// need that version, and such a device is refused for its version already.
static inline void vk_require_check(const VkRequireProcs *p, VkPhysicalDevice pd, VkRequireResult *r) {
    memset(r, 0, sizeof *r);
    VkPhysicalDeviceProperties props;
    p->props(pd, &props);
    int api_ok = props.apiVersion >= VK_REQUIRE_API;
    if (!api_ok) vk_require_note(r, "Vulkan 1.3");
    uint32_t ne = 0;
    p->exts(pd, NULL, &ne, NULL);
    VkExtensionProperties *e = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *e);
    if (e) p->exts(pd, NULL, &ne, e);
    else ne = 0;
    for (int i = 0; i < VK_REQUIRE_EXT_COUNT; i++)
        if (!vk_require_has(e, ne, vk_require_ext[i])) vk_require_note(r, vk_require_ext[i]);
    r->host_import = vk_require_has(e, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    int clip_ext = vk_require_has(e, ne, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    free(e);
    if (api_ok && p->features2) {
        VkPhysicalDeviceDepthClipControlFeaturesEXT clip = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
        VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                .pNext = clip_ext ? &clip : NULL};
        VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
        p->features2(pd, &f2);
        const char *names[VK_REQUIRE_MISSING_MAX];
        int n = vk_require_features_missing(&f2.features, &f12, &f13, &clip, names, VK_REQUIRE_MISSING_MAX);
        for (int i = 0; i < n && i < VK_REQUIRE_MISSING_MAX; i++)
            if (strcmp(names[i], "depthClipControl") || clip_ext)   // the missing extension is named already
                vk_require_note(r, names[i]);
    }
    uint32_t nq = 0;
    p->queues(pd, &nq, NULL);
    VkQueueFamilyProperties *q = (VkQueueFamilyProperties *)calloc(nq ? nq : 1, sizeof *q);
    int gc = 0;
    if (q) {
        p->queues(pd, &nq, q);
        for (uint32_t i = 0; i < nq && !gc; i++)
            gc = (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT);
        free(q);
    }
    if (!gc) vk_require_note(r, "a graphics and compute queue family");
    r->usable = r->n_missing == 0;
}

#endif
