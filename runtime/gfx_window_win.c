// The Win32 back end of the window (gfx_wsys.h): the window and its message loop on a thread of their own, F11
// fullscreen, the settings XCX_WINDOW_SIZE and XCX_FULLSCREEN, the monitor's refresh rate, focus, the Vulkan surface.
// The swapchain and the present are gfx_present.c's. The in-game menu's keyboard and mouse come from the window
// procedure (ui/menu_win.cpp), and so do the game's (input_win.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "cfg.h"
#include "flight.h"
#include "fs.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "input.h"
#include "input_win.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "platform/platform.h"
#include <vulkan/vulkan_win32.h>

// ---- window state
static HWND g_hwnd;
static PlatEvent g_wnd_ready;   // manual-reset: the window thread made the window or gave up
static int g_wnd_failed;
static int g_background;                   // XCX_BACKGROUND_WINDOW: the window stays behind the user's windows
static int g_fs_pending;                   // window thread: XCX_FULLSCREEN waits for the first activation

HWND gfx_window_handle(void) { return g_hwnd; }

int gfx_window_vk_extensions(const char **out, int max) {
    if (max < 2) return 0;
    out[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    out[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    return 2;
}

int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family) {
    (void)inst;
    return vkGetPhysicalDeviceWin32PresentationSupportKHR(pd, family) != VK_FALSE;
}

// ---- window thread
#define WND_STYLE WS_OVERLAPPEDWINDOW
#define WM_APP_SETTINGS (WM_APP + 1)
#define WM_APP_FULLSCREEN (WM_APP + 2)   // the fullscreen setting, held back until the window is first activated

static WINDOWPLACEMENT g_saved_place = {sizeof g_saved_place};   // the window behind fullscreen (window thread)

static int is_fullscreen(HWND h) { return !(GetWindowLongW(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW); }

#define cfg_fullscreen gfx_cfg_fullscreen
#define cfg_window_size gfx_cfg_window_size

// F11: borderless window over the whole monitor, and back to the saved window. The swapchain follows through WM_SIZE.
// Not Alt+Enter: Enter is the Plus button (input_win.c). A background window (test runs) keeps its place behind the
// user's windows and takes no focus.
static void toggle_fullscreen(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(h, &g_saved_place) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_background ? SWP_NOZORDER | SWP_NOACTIVATE : 0));
    } else {
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WND_STYLE);
        if (g_background) g_saved_place.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &g_saved_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                         (g_background ? SWP_NOACTIVATE : 0));
    }
}

// The window size and fullscreen state last taken from the settings (window thread). apply_settings acts on a change of
// either value only, so a window resized by hand keeps its size when another setting changes.
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("XCX_WINDOW_SIZE"));
    g_applied_fs = cfg_fullscreen();
}

// Window thread: the window takes the size and fullscreen state of the settings when they changed. In fullscreen the
// size goes into the placement restored when fullscreen ends.
static void apply_settings(HWND h) {
    int size_changed = strcmp(cfg_value("XCX_WINDOW_SIZE"), g_applied_size) != 0, fs = cfg_fullscreen(), fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int cw, ch;
        cfg_window_size(&cw, &ch);
        RECT r = {0, 0, cw, ch};
        AdjustWindowRect(&r, WND_STYLE, FALSE);
        if (is_fullscreen(h)) {
            RECT *n = &g_saved_place.rcNormalPosition;
            n->right = n->left + (r.right - r.left);
            n->bottom = n->top + (r.bottom - r.top);
        } else {
            RECT have;
            if (GetClientRect(h, &have) && (have.right != cw || have.bottom != ch)) {
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                SetWindowPos(h, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    if (fs_changed && fs != is_fullscreen(h)) {
        input_cursor_suspend(1);
        toggle_fullscreen(h);
        input_cursor_suspend(0);
    }
}

void gfx_window_apply_settings(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_SETTINGS, 0, 0);
}


void gfx_window_request_close(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// ---- refresh rate of the window's monitor (XCX_FPS_LIMIT=display), measured when the window opens, lands on another
// monitor or the display mode changes (gfx_window_set_refresh keeps it). QueryDisplayConfig gives the exact rational
// rate (59.94, 143.998); EnumDisplaySettings' whole hertz is the fallback.
static HMONITOR g_rate_mon;

static double monitor_rate_hz(HMONITOR mon) {
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return 0;
    double hz = 0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np && nm) {
        DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
        DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
            for (UINT32 i = 0; i < np && !hz; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                                   .size = sizeof src,
                                                                   .adapterId = paths[i].sourceInfo.adapterId,
                                                                   .id = paths[i].sourceInfo.id}};
                DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && !wcscmp(src.viewGdiDeviceName, mi.szDevice) &&
                    r.Denominator)
                    hz = (double)r.Numerator / r.Denominator;
            }
        free(paths);
        free(modes);
    }
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!hz && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;   // 0 and 1 mean the hardware default
    return hz;
}

// Window thread. force: the display mode changed, measure even on the same monitor.
static void refresh_rate_update(HWND h, int force) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    if (!force && mon == g_rate_mon) return;
    g_rate_mon = mon;
    gfx_window_set_refresh(monitor_rate_hz(mon));
}

int gfx_window_monitor_size(int *w, int *h) {
    MONITORINFO mi = {sizeof mi};
    if (!g_hwnd || !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return 0;
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return 1;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT input_res;
    if (input_window_msg(h, m, w, l, &input_res)) return input_res;
    if (menu_window_msg(h, m, w, l, &input_res)) return input_res;
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) {   // bit 30: auto-repeat
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
            cfg_set("XCX_FULLSCREEN", is_fullscreen(h) ? "1" : "0");   // the menu shows it; the next start keeps it
            cfg_save();
            note_applied();   // what cfg_get reads now (the environment may still override it)
        }
        break;
    case WM_APP_SETTINGS:
        apply_settings(h);
        return 0;
    case WM_APP_FULLSCREEN:
        if (cfg_fullscreen() && !is_fullscreen(h)) {
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
        }
        return 0;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) gfx_window_mark_dirty();
        refresh_rate_update(h, 0);
        return 0;
    case WM_EXITSIZEMOVE:   // dragged onto another monitor
        refresh_rate_update(h, 0);
        break;
    case WM_DISPLAYCHANGE:
        refresh_rate_update(h, 1);
        break;
    case WM_ACTIVATEAPP: {
        int in = w != 0;
        gfx_window_focus_changed(in);
        if (in && g_fs_pending) {
            g_fs_pending = 0;
            PostMessageW(h, WM_APP_FULLSCREEN, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE:
        gfx_window_closed();
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static void window_main(void *arg) {
    (void)arg;
    // Sizes in physical pixels: without this, Windows display scaling (150 % on many 4K monitors) reports the monitor
    // as 2560x1440, so the window size list stops there, and stretches the swapchain to the screen.
    // Looked up: the headers declare it only for _WIN32_WINNT 0x0605 and up (Windows 10 1703).
    BOOL(WINAPI * set_dpi)(HANDLE) =
        (BOOL(WINAPI *)(HANDLE))(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (set_dpi) set_dpi((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    HINSTANCE inst = GetModuleHandleW(NULL);
    // Icon resource 1 of xcx.rc (runtime/xcx.ico): the title bar, the taskbar button and Alt+Tab.
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = wnd_proc, .hInstance = inst,
                      .hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_SHARED),
                      .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_SHARED),
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"XCXRecompWindow"};
    RegisterClassExW(&wc);
    int cw, ch;
    cfg_window_size(&cw, &ch);
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, WND_STYLE, FALSE);
    // The window opens behind the user's windows without taking focus: it shows the game's blank loading frames for a
    // while, and must not cover what the player works on meanwhile. The game still counts itself in focus until the
    // first WM_ACTIVATEAPP, so loading goes on, and keyboard input waits for the foreground (input_win.c). Its taskbar
    // button flashes until it is brought up, and XCX_FULLSCREEN waits for that first activation.
    // XCX_BACKGROUND_WINDOW=1 (test runs): no flashing, never fullscreen at start, and F11 keeps the window's place.
    int background = g_background = cfg_get("XCX_BACKGROUND_WINDOW") && atoi(cfg_get("XCX_BACKGROUND_WINDOW"));
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"XCX Recomp", WND_STYLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!g_hwnd) {
        rt_log("gfx: CreateWindowEx failed (%lu)", GetLastError());
        g_wnd_failed = 1;
    } else {
        SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!background) {
            FLASHWINFO fw = {.cbSize = sizeof fw, .hwnd = g_hwnd, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
            FlashWindowEx(&fw);
            g_fs_pending = cfg_fullscreen();
        }
        note_applied();
        refresh_rate_update(g_hwnd, 1);
    }
    plat_event_set(&g_wnd_ready);
    if (g_wnd_failed) return;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return;
}

// ---- gfx_wsys.h
// Makes the window on its thread and waits until it is there.
int wsys_open(void) {
    g_wnd_failed = 0;
    plat_event_init(&g_wnd_ready, 1, 0);
    if (!plat_thread_spawn(window_main, NULL, "window", 0)) rt_fatal("gfx: cannot start the window thread");
    plat_event_wait(&g_wnd_ready);
    return !g_wnd_failed;
}

VkSurfaceKHR wsys_surface(VkInstance inst) {
    VkWin32SurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                      .hinstance = GetModuleHandleW(NULL), .hwnd = g_hwnd};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult r = vkCreateWin32SurfaceKHR(inst, &si, NULL, &surface);
    if (r != VK_SUCCESS) rt_log("gfx: vkCreateWin32SurfaceKHR failed (%d)", (int)r);
    return r == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}

void wsys_client_size(int *w, int *h) {
    RECT rc = {0};
    GetClientRect(g_hwnd, &rc);
    *w = rc.right, *h = rc.bottom;
}

int wsys_minimized(void) { return IsIconic(g_hwnd) != 0; }

void wsys_resize(int cw, int ch) {
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, WND_STYLE, FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void wsys_probe_focus(int in) { SendMessageW(g_hwnd, WM_ACTIVATEAPP, in ? TRUE : FALSE, 0); }

void wsys_probe_escape(void) { SendMessageW(g_hwnd, WM_KEYDOWN, VK_ESCAPE, 0); }

int wsys_probe_click(int where) {
    if (where == 0) SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, 0);
    else SendMessageW(g_hwnd, WM_MOUSEACTIVATE, (WPARAM)g_hwnd, MAKELPARAM(where == 1 ? HTCLIENT : HTCAPTION, WM_LBUTTONDOWN));
    return 1;
}
