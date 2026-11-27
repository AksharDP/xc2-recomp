// Host check of the installer's first step, compiled with the bundled clang at install time (-I <runtime headers>):
// the processor's x86-64 level, the Windows build and native architecture, and each Vulkan device judged by the game's
// own requirements. Prints one JSON object on stdout.
//
// The rules are the game's: runtime/cpu_level.h decides the level (3 builds the game for x86-64-v3, 2 for x86-64-v2,
// 1 is too old) and runtime/vk_require.h decides whether a device is usable and names what it lacks, so the installer
// refuses exactly the PCs the game would refuse. The Vulkan loader is opened at run time, so a machine without one
// gets "loader": false instead of a failed start.
#include <stdio.h>
#include <string.h>

#include "cpu_level.h"
#include "vk_require.h"

#ifdef _WIN32
#include <windows.h>
#define open_lib() LoadLibraryA("vulkan-1.dll")
#define lib_sym(l, n) ((void *)GetProcAddress((l), (n)))
#else
#include <dlfcn.h>
#define open_lib() dlopen("libvulkan.so.1", RTLD_NOW)
#define lib_sym(l, n) dlsym((l), (n))
#endif

static void json_string(const char *s) {
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20) printf("\\u%04x", *s);
        else putchar(*s);
    }
    putchar('"');
}

#ifdef _WIN32
// The Windows version as the kernel reports it (RtlGetVersion is not subject to the manifest-based version lie of
// GetVersionEx), and the native machine: an x64 process on Windows on Arm runs under emulation, which IsWow64Process2
// reveals (it reports the process machine as unknown and the native machine as ARM64).
static void windows_info(void) {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v = {sizeof v};
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    RtlGetVersionFn get = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (get && get(&v) == 0)
        printf("\"windows\": [%lu, %lu, %lu], ", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    typedef BOOL(WINAPI * IsWow64Process2Fn)(HANDLE, USHORT *, USHORT *);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    IsWow64Process2Fn wow = k32 ? (IsWow64Process2Fn)(void *)GetProcAddress(k32, "IsWow64Process2") : NULL;
    USHORT process = 0, native = 0;
    const char *arch = "x64";
    if (wow && wow(GetCurrentProcess(), &process, &native) && native == 0xAA64 /* IMAGE_FILE_MACHINE_ARM64 */)
        arch = "arm64";
    printf("\"native_arch\": \"%s\", ", arch);
}
#endif

int main(void) {
    int level = cpu_level_host();
    printf("{\"cpu_level\": %d, \"x86_64_v3\": %s, ", level, level >= 3 ? "true" : "false");
#ifdef _WIN32
    windows_info();
#endif
    void *lib = open_lib();
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)lib_sym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!gipa) {
        printf("\"loader\": false, \"devices\": []}\n");
        return 0;
    }
    printf("\"loader\": true, ");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "xc2-install",
                             .apiVersion = VK_REQUIRE_API};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = create ? create(&ci, NULL, &inst) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS) {
        printf("\"instance\": %d, \"devices\": []}\n", (int)r);
        return 0;
    }
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    VkRequireProcs procs = {
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties"),
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties"),
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2"),
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties")};
    VkPhysicalDevice devs[16];
    uint32_t n = 16;
    if (!enumerate || !procs.props || !procs.exts || !procs.queues || enumerate(inst, &n, devs) < 0) n = 0;
    printf("\"instance\": 0, \"devices\": [");
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        procs.props(devs[i], &p);
        VkRequireResult req;
        vk_require_check(&procs, devs[i], &req);
        printf("%s{\"name\": ", i ? ", " : "");
        json_string(p.deviceName);
        printf(", \"api\": [%u, %u, %u], \"type\": %d, \"vendor\": %u, \"usable\": %s, \"host_import\": %s, \"missing\": [",
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               (int)p.deviceType, p.vendorID, req.usable ? "true" : "false", req.host_import ? "true" : "false");
        for (int m = 0; m < req.n_missing && m < VK_REQUIRE_MISSING_MAX; m++) {
            if (m) printf(", ");
            json_string(req.missing[m]);
        }
        printf("]}");
    }
    printf("]}\n");
    if (destroy) destroy(inst, NULL);
    return 0;
}
