/*
 * borderlessfullscreen.c
 *
 * Borderless fullscreen window using ShellHost → DesktopSurfaceWindow architecture.
 *
 * Properties:
 *   - No title bar
 *   - No borders
 *   - No taskbar entry
 *   - Matches screen resolution exactly
 *
 * Architecture:
 *   ShellHost.exe (parent process concept)
 *   └── DesktopSurfaceWindow (borderless popup, full screen)
 */

#define _WIN32_WINNT 0x0500
#define WINVER       0x0500
#include <windows.h>
#include <wtypes.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <gdiplus.h>
#include <wchar.h>
#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "coremessaging.lib")
#endif

#ifdef __cplusplus
using namespace Gdiplus;
using namespace Gdiplus::DllExports;
#endif

#ifdef __cplusplus
/* Suppress C++17 coroutine deprecation warning */
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
/* Prevent windows.h from redefining GetCurrentTime and breaking WinUI 3 */
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <DispatcherQueue.h>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Hosting;
using namespace winrt::Microsoft::UI::Xaml::Media;
#endif

/* ─── Constants ─── */
#define WINDOW_CLASS_NAME L"DesktopSurfaceWindow"
#define WINDOW_TITLE      L"DesktopSurfaceWindow"

/* ─── Globals ─── */
static HWND g_hwnd = NULL;
static HMONITOR g_hMonitor = NULL;

#define MAX_MENUBARS 10
static HWND g_hMenuBars[MAX_MENUBARS] = { NULL };
static int g_numMenuBars = 0;

/* ─── Wallpaper ─── */
static WCHAR g_wallpaperPath[MAX_PATH] = { 0 };
static GdiplusStartupInput g_gdiplusStartup;
static ULONG_PTR g_gdiplusToken = 0;
static GpImage* g_cachedWallpaperImage = NULL;

#ifdef __cplusplus
static winrt::Microsoft::UI::Dispatching::DispatcherQueueController g_dispatcherQueueController{ nullptr };
static winrt::Windows::System::DispatcherQueueController g_systemDispatcherQueueController{ nullptr };
static WindowsXamlManager g_xamlManager{ nullptr };
static DesktopWindowXamlSource g_desktopSources[MAX_MENUBARS]{ nullptr };
static BOOL g_islandInitialized[MAX_MENUBARS] = { FALSE };  /* Track which islands have been created */
static BOOL g_winui3Initialized = FALSE;
#endif

/* ─── System Tick ─── */
#define TICK_TIMER_ID         1
#define TICK_INTERVAL_MS      1000   /* 1 second between ticks */
#define WALLPAPER_CHECK_ID    2
#define WALLPAPER_CHECK_MS    5000   /* 5 seconds between wallpaper checks */
static UINT g_tickTimer = 0;
static UINT g_wallpaperTimer = 0;
static FILETIME g_lastWallpaperTime = { 0 };
static BOOL g_bTickActive = FALSE;

/* ─── Extension Points for Future Components ─── */
typedef void (*TickCallback)(HWND hwnd, UINT tickType);
static TickCallback g_onTick = NULL;
static TickCallback g_onScreenChange = NULL;
static TickCallback g_onWallpaperChange = NULL;

typedef enum {
    TickType_Heartbeat = 0,
    TickType_ScreenChange,
    TickType_WallpaperChange,
    TickType_InitComplete
} TickType;

typedef enum {
    ScalingMode_Center = 0,
    ScalingMode_Stretch,
    ScalingMode_Fill
} ScalingMode;

static ScalingMode g_scalingMode = ScalingMode_Center;

/**
 * Enumerate all image file extensions we support.
 */
static const WCHAR* IMAGE_EXTENSIONS[] = {
    L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tiff", L".tif", L".webp", L".ico"
};
#define IMAGE_EXT_COUNT (sizeof(IMAGE_EXTENSIONS) / sizeof(IMAGE_EXTENSIONS[0]))

/* ─── Wallpaper ─── */
/**
 * Get the desktop wallpaper path from registry.
 */
static BOOL GetDesktopWallpaperPath(WCHAR* path, size_t cch) {
    HKEY hKey;
    if (cch == 0) return FALSE;

    LONG lResult = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_READ, &hKey);
    if (lResult != ERROR_SUCCESS) return FALSE;

    DWORD type = 0, size = (DWORD)(cch * sizeof(WCHAR));
    lResult = RegQueryValueExW(hKey, L"Wallpaper", NULL, &type, (LPBYTE)path, &size);
    RegCloseKey(hKey);
    path[cch - 1] = L'\0';
    return (lResult == ERROR_SUCCESS && type == REG_SZ && path[0] != L'\0');
}

static BOOL HasImageExtension(const WCHAR* path) {
    int pathLen = lstrlenW(path);

    for (size_t i = 0; i < IMAGE_EXT_COUNT; i++) {
        int extLen = lstrlenW(IMAGE_EXTENSIONS[i]);
        if (pathLen >= extLen &&
            lstrcmpiW(path + pathLen - extLen, IMAGE_EXTENSIONS[i]) == 0) {
            return TRUE;
        }
    }

    return FALSE;
}

static void EnsureTrailingBackslash(WCHAR* path, size_t cch) {
    int len = lstrlenW(path);
    if (len > 0 && path[len - 1] != L'\\' && (size_t)(len + 1) < cch) {
        lstrcatW(path, L"\\");
    }
}

/**
 * Find a wallpaper image file near the registry wallpaper path.
 * If the registry path is an image, use it directly.
 * Otherwise, scan the wallpaper directory for common image formats.
 */
static BOOL FindWallpaperImage(WCHAR* outPath, size_t cch) {
    WCHAR wallpaperDir[MAX_PATH] = { 0 };
    WCHAR* lastSlash = NULL;

    /* Try registry wallpaper path first */
    if (GetDesktopWallpaperPath(outPath, cch)) {
        /* Check if it's an image file */
        if (HasImageExtension(outPath) && PathFileExistsW(outPath)) {
            return TRUE;
        }
        /* It's a directory — scan it */
        lastSlash = wcsrchr(outPath, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
            lstrcpyW(wallpaperDir, outPath);
            EnsureTrailingBackslash(wallpaperDir, MAX_PATH);
        }
    }

    /* Default wallpaper directory */
    if (wallpaperDir[0] == L'\0') {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, wallpaperDir)) && wallpaperDir[0]) {
            /* Append backslash if needed */
            EnsureTrailingBackslash(wallpaperDir, MAX_PATH);
        }
        else {
            /* Fallback: Windows folder */
            GetWindowsDirectoryW(wallpaperDir, MAX_PATH);
            if (lstrlenW(wallpaperDir) + 16 < MAX_PATH) {
                lstrcatW(wallpaperDir, L"\\Web\\Wallpaper\\");
            }
        }
    }

    /* Scan wallpaper directory for image files */
    WIN32_FIND_DATAW findData;
    HANDLE hFind;
    WCHAR searchPath[MAX_PATH];
    if (lstrlenW(wallpaperDir) + 4 >= MAX_PATH) return FALSE;
    lstrcpynW(searchPath, wallpaperDir, MAX_PATH);
    lstrcatW(searchPath, L"*.*");

    hFind = FindFirstFileW(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) return FALSE;

    BOOL found = FALSE;
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        /* Check extension */
        if (HasImageExtension(findData.cFileName)) {
            /* Use StringCchPrintfW to prevent buffer overflow */
            if (lstrlenW(wallpaperDir) + lstrlenW(findData.cFileName) + 1 < cch) {
                if (SUCCEEDED(StringCchPrintfW(outPath, cch, L"%s%s", wallpaperDir, findData.cFileName))) {
                    found = TRUE;
                }
            }
        }
        if (found) break;
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
    return found;
}

/**
 * Initialize GDI+ for image loading.
 */
static BOOL InitGdiplus() {
    if (g_gdiplusToken) return TRUE;
    g_gdiplusStartup.GdiplusVersion = 1;
    return GdiplusStartup(&g_gdiplusToken, &g_gdiplusStartup, NULL) == Ok;
}

static void ClearCachedWallpaper(void) {
    if (g_cachedWallpaperImage) {
        GdipDisposeImage(g_cachedWallpaperImage);
        g_cachedWallpaperImage = NULL;
    }
}

/**
 * Cleanup GDI+.
 */
static void ShutdownGdiplus() {
    ClearCachedWallpaper();
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

/**
 * Get last modification time of a file.
 */
static BOOL GetFileLastWriteTime(const WCHAR* path, FILETIME* ft) {
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(path, &findData);
    if (hFind == INVALID_HANDLE_VALUE) return FALSE;
    FindClose(hFind);
    ft->dwLowDateTime = findData.ftLastWriteTime.dwLowDateTime;
    ft->dwHighDateTime = findData.ftLastWriteTime.dwHighDateTime;
    return TRUE;
}

/**
 * Check if wallpaper file has changed since last check.
 */
static BOOL IsWallpaperChanged(void) {
    if (g_wallpaperPath[0] == L'\0') return FALSE;

    FILETIME currentTime = { 0 };
    if (!GetFileLastWriteTime(g_wallpaperPath, &currentTime)) return FALSE;
    if (g_lastWallpaperTime.dwLowDateTime == 0 && g_lastWallpaperTime.dwHighDateTime == 0) {
        g_lastWallpaperTime = currentTime;
        return FALSE;
    }
    LONG cmp = CompareFileTime(&currentTime, &g_lastWallpaperTime);
    if (cmp != 0) {
        g_lastWallpaperTime = currentTime;
        return TRUE;
    }
    return FALSE;
}

/**
 * Invalidate the window to force a repaint (wallpaper refresh).
 */
static void RefreshWallpaper(void) {
    if (g_hwnd == NULL) return;
    RECT clientRect;
    if (GetClientRect(g_hwnd, &clientRect)) {
        InvalidateRect(g_hwnd, &clientRect, TRUE);
    }
}

/**
 * Timer callback for system tick.
 */
 /**
  * Start the system tick timer.
  */
static BOOL StartTickTimer(void) {
    if (g_bTickActive) return TRUE;
    g_tickTimer = SetTimer(g_hwnd, TICK_TIMER_ID, TICK_INTERVAL_MS, NULL);
    g_wallpaperTimer = SetTimer(g_hwnd, WALLPAPER_CHECK_ID, WALLPAPER_CHECK_MS, NULL);
    g_bTickActive = (g_tickTimer != 0);
    return g_bTickActive;
}

/**
 * Stop the system tick timer.
 */
static void StopTickTimer(void) {
    if (g_tickTimer) {
        KillTimer(NULL, TICK_TIMER_ID);
        g_tickTimer = 0;
    }
    if (g_wallpaperTimer) {
        KillTimer(NULL, WALLPAPER_CHECK_ID);
        g_wallpaperTimer = 0;
    }
    g_bTickActive = FALSE;
}

/**
 * Register a tick callback for future component integration.
 */
static void RegisterTickCallback(TickCallback callback) {
    g_onTick = callback;
}

/**
 * Register a screen change callback for future component integration.
 */
static void RegisterScreenChangeCallback(TickCallback callback) {
    g_onScreenChange = callback;
}

/**
 * Register a wallpaper change callback for future component integration.
 */
static void RegisterWallpaperChangeCallback(TickCallback callback) {
    g_onWallpaperChange = callback;
}

/**
 * Render the wallpaper image onto the DC using the selected scaling mode.
 */
static BOOL RenderWallpaper(HDC hdc, RECT* clientRect) {
    if (g_wallpaperPath[0] == L'\0') return FALSE;

    GpGraphics* graphics = NULL;
    UINT imageWidth = 0;
    UINT imageHeight = 0;
    GpStatus status = GdipCreateFromHDC(hdc, &graphics);
    if (status != Ok) return FALSE;

    if (g_cachedWallpaperImage == NULL) {
        status = GdipLoadImageFromFile(g_wallpaperPath, &g_cachedWallpaperImage);
        if (status != Ok) {
            GdipDeleteGraphics(graphics);
            return FALSE;
        }
    }

    if (GdipGetImageWidth(g_cachedWallpaperImage, &imageWidth) != Ok ||
        GdipGetImageHeight(g_cachedWallpaperImage, &imageHeight) != Ok ||
        imageWidth == 0 || imageHeight == 0) {
        ClearCachedWallpaper();
        GdipDeleteGraphics(graphics);
        return FALSE;
    }

    REAL imgWidth = (REAL)imageWidth;
    REAL imgHeight = (REAL)imageHeight;
    REAL dstWidth = (REAL)(clientRect->right - clientRect->left);
    REAL dstHeight = (REAL)(clientRect->bottom - clientRect->top);

    /* Prevent division by zero */
    if (dstWidth <= 0.0f || dstHeight <= 0.0f || imgWidth <= 0.0f || imgHeight <= 0.0f) {
        GdipDeleteGraphics(graphics);
        return FALSE;
    }

    switch (g_scalingMode) {
    case ScalingMode_Stretch:
        /* Stretch to fill entire client area */
        GdipDrawImageRectI(graphics, g_cachedWallpaperImage, clientRect->left, clientRect->top,
            (INT)dstWidth, (INT)dstHeight);
        break;

    case ScalingMode_Fill:
        /* Maintain aspect ratio, fill entire area (may crop edges) */
    {
        REAL scale = dstWidth / dstHeight < imgWidth / imgHeight
            ? dstHeight / imgHeight
            : dstWidth / imgWidth;
        REAL drawWidth = imgWidth * scale;
        REAL drawHeight = imgHeight * scale;
        REAL x = clientRect->left + (dstWidth - drawWidth) / 2.0f;
        REAL y = clientRect->top + (dstHeight - drawHeight) / 2.0f;
        GdipDrawImageRectI(graphics, g_cachedWallpaperImage, (INT)x, (INT)y, (INT)drawWidth, (INT)drawHeight);
    }
    break;

    case ScalingMode_Center:
    default:
        /* Maintain aspect ratio, center in area (may have letterboxing) */
    {
        REAL scale = dstWidth / dstHeight < imgWidth / imgHeight
            ? dstWidth / imgWidth
            : dstHeight / imgHeight;
        REAL drawWidth = imgWidth * scale;
        REAL drawHeight = imgHeight * scale;
        REAL x = clientRect->left + (dstWidth - drawWidth) / 2.0f;
        REAL y = clientRect->top + (dstHeight - drawHeight) / 2.0f;
        GdipDrawImageRectI(graphics, g_cachedWallpaperImage, (INT)x, (INT)y, (INT)drawWidth, (INT)drawHeight);
    }
    break;
    }

    GdipDeleteGraphics(graphics);
    return TRUE;
}

/* ─── Helpers ─── */

/**
 * Get the primary monitor's full bounds (not work area).
 * For true fullscreen we use the entire monitor rect, not the work area.
 */
static void GetFullscreenRect(HWND hwnd, RECT* rect) {
    MONITORINFO mi = { 0 };
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    mi.cbSize = sizeof(mi);
    g_hMonitor = hMonitor;  /* cache for WM_DISPLAYCHANGE */
    GetMonitorInfo(hMonitor, &mi);
    rect->left = mi.rcMonitor.left;
    rect->top = mi.rcMonitor.top;
    rect->right = mi.rcMonitor.right;
    rect->bottom = mi.rcMonitor.bottom;
}

/**
 * Get the virtual desktop bounds covering all monitors.
 * This is the key to "desktop layer" behavior across multiple monitors.
 */
static void GetVirtualDesktopRect(RECT* rect) {
    rect->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect->right = rect->left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect->bottom = rect->top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/**
 * Get the appropriate fullscreen rect based on multi-monitor mode.
 * If multi-monitor mode is enabled, use the virtual desktop rect.
 * Otherwise, use the primary monitor's bounds.
 */
static void GetAppropriateFullscreenRect(HWND hwnd, RECT* rect) {
    /* Check if we have multiple monitors */
    if (GetSystemMetrics(SM_CMONITORS) > 1) {
        GetVirtualDesktopRect(rect);
    }
    else {
        GetFullscreenRect(hwnd, rect);
    }
}

#ifdef __cplusplus
static void InitWinUI3(BOOL requireXaml = FALSE) {
    if (g_winui3Initialized) return;

    OutputDebugStringW(L"[InitWinUI3] Starting WinUI3 initialization...\n");

    try {
        /* 1. Safely initialize COM as STA */
        OutputDebugStringW(L"[InitWinUI3] Initializing COM apartment as STA...\n");
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        OutputDebugStringW(L"[InitWinUI3] COM apartment initialized successfully.\n");
    } catch (const winrt::hresult_error& ex) {
        wchar_t buffer[512];
        swprintf_s(buffer, 512, L"[InitWinUI3] COM initialization hresult_error (may be already initialized): 0x%X - %s\n", ex.code().value, ex.message().c_str());
        OutputDebugStringW(buffer);
    } catch (const std::exception& ex) {
        char buffer[512];
        sprintf_s(buffer, 512, "[InitWinUI3] COM initialization std::exception: %s\n", ex.what());
        OutputDebugStringA(buffer);
        /* Ignore if COM is already initialized by GDI+ */
    } catch (...) {
        OutputDebugStringW(L"[InitWinUI3] Unexpected exception during COM initialization.\n");
    }

    try {
        /* 2. Create the WinUI 3 DispatcherQueueController */
        OutputDebugStringW(L"[InitWinUI3] Creating DispatcherQueueController on current thread...\n");
        if (!g_dispatcherQueueController) {
            try {
                /* Prefer WinRT helper if available */
                g_dispatcherQueueController = winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
                OutputDebugStringW(L"[InitWinUI3] DispatcherQueueController created via WinRT helper.\n");
            } catch (const winrt::hresult_error& ex) {
                /* If the helper fails, log and continue without XAML islands. */
                wchar_t buffer[256];
                swprintf_s(buffer, 256, L"[InitWinUI3] DispatcherQueueController CreateOnCurrentThread failed: 0x%X\n", ex.code().value);
                OutputDebugStringW(buffer);
                g_dispatcherQueueController = nullptr;
            } catch (...) {
                OutputDebugStringW(L"[InitWinUI3] DispatcherQueueController CreateOnCurrentThread threw unknown exception.\n");
                g_dispatcherQueueController = nullptr;
            }
        } else {
            OutputDebugStringW(L"[InitWinUI3] DispatcherQueueController already exists.\n");
        }

        /* 3. Initialize WindowsXamlManager */
        OutputDebugStringW(L"[InitWinUI3] Initializing WindowsXamlManager for current thread...\n");
        if (!g_xamlManager) {
            g_xamlManager = WindowsXamlManager::InitializeForCurrentThread();
            OutputDebugStringW(L"[InitWinUI3] WindowsXamlManager initialized successfully.\n");
        } else {
            OutputDebugStringW(L"[InitWinUI3] WindowsXamlManager already exists.\n");
        }

        /* Mark as successfully initialized */
        OutputDebugStringW(L"[InitWinUI3] WinUI3 initialization completed successfully.\n");
        g_winui3Initialized = TRUE;
    } catch (const winrt::hresult_error& ex) {
        wchar_t buffer[512];
        swprintf_s(buffer, 512, L"[InitWinUI3] WinRT hresult_error: 0x%X - %s\n", ex.code().value, ex.message().c_str());
        OutputDebugStringW(buffer);
        g_xamlManager = nullptr;
        g_dispatcherQueueController = nullptr;
        g_systemDispatcherQueueController = nullptr;
        g_winui3Initialized = FALSE;
    } catch (const std::exception& ex) {
        char buffer[512];
        sprintf_s(buffer, 512, "[InitWinUI3] std::exception: %s\n", ex.what());
        OutputDebugStringA(buffer);
        g_xamlManager = nullptr;
        g_dispatcherQueueController = nullptr;
        g_systemDispatcherQueueController = nullptr;
        g_winui3Initialized = FALSE;
    } catch (...) {
        OutputDebugStringW(L"[InitWinUI3] Unknown exception during WinUI3 initialization.\n");
        g_xamlManager = nullptr;
        g_dispatcherQueueController = nullptr;
        g_systemDispatcherQueueController = nullptr;
        g_winui3Initialized = FALSE;
    }

    if (requireXaml && !g_winui3Initialized) {
        MessageBoxW(NULL, L"Failed to initialize WinUI 3 XAML Islands. The application requires XAML features to run and will now abort.", L"Fatal Error", MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }
}

static void ShutdownWinUI3() {
    if (g_xamlManager) {
        g_xamlManager.Close();
        g_xamlManager = nullptr;
    }
    if (g_dispatcherQueueController) {
        /* Deliberately skip ShutdownQueueAsync to avoid hanging or aborting the CRT on exit */
        g_dispatcherQueueController = nullptr;
    }
    if (g_systemDispatcherQueueController) {
        g_systemDispatcherQueueController = nullptr;
    }
    g_winui3Initialized = FALSE;
}

static void InitWinUI3Island(HWND hMenuBar, int index) {
    wchar_t buffer[512];

    swprintf_s(buffer, 512, L"[InitWinUI3Island] Starting island creation for index %d, HWND=0x%p\n", index, hMenuBar);
    OutputDebugStringW(buffer);

    if (!hMenuBar || !IsWindow(hMenuBar)) {
        OutputDebugStringW(L"[InitWinUI3Island] ERROR: Invalid or destroyed menu bar HWND\n");
        return;
    }

    if (!g_xamlManager) {
        OutputDebugStringW(L"[InitWinUI3Island] ERROR: g_xamlManager is null\n");
        return;
    }

    if (!g_dispatcherQueueController && !g_systemDispatcherQueueController) {
        OutputDebugStringW(L"[InitWinUI3Island] ERROR: No DispatcherQueueController exists\n");
        return;
    }

    if (index < 0 || index >= MAX_MENUBARS) {
        swprintf_s(buffer, 512, L"[InitWinUI3Island] ERROR: Index %d out of range [0, %d)\n", index, MAX_MENUBARS);
        OutputDebugStringW(buffer);
        return;
    }

    try {
        OutputDebugStringW(L"[InitWinUI3Island] Creating DesktopWindowXamlSource...\n");
        /* 1. Create the DesktopWindowXamlSource (the Island) */
        g_desktopSources[index] = DesktopWindowXamlSource();
        OutputDebugStringW(L"[InitWinUI3Island] DesktopWindowXamlSource created successfully\n");

        /* 2. Attach the Island to the MenuBar HWND using modern WinAppSDK interop */
        OutputDebugStringW(L"[InitWinUI3Island] Initializing island with HWND...\n");
        winrt::Microsoft::UI::WindowId windowId;
        windowId.Value = (uint64_t)(UINT_PTR)hMenuBar;
        g_desktopSources[index].Initialize(windowId);
        OutputDebugStringW(L"[InitWinUI3Island] Island initialized with HWND\n");

        /* 3. Build the WinUI 3 XAML Tree Programmatically */
        OutputDebugStringW(L"[InitWinUI3Island] Building XAML tree...\n");
        Grid rootGrid;
        rootGrid.Background(SolidColorBrush(winrt::Microsoft::UI::Colors::WhiteSmoke()));

        ColumnDefinition colLeft, colRight;
        colLeft.Width(GridLengthHelper::Auto());
        colRight.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        rootGrid.ColumnDefinitions().Append(colLeft);
        rootGrid.ColumnDefinitions().Append(colRight);

        /* Left Section: Apple, AppName, MenuItems */
        StackPanel leftPanel;
        leftPanel.Orientation(Orientation::Horizontal);
        leftPanel.Padding(ThicknessHelper::FromLengths(16, 0, 0, 0));

        TextBlock appleBtn;
        appleBtn.Text(L"\xD83C\xDF4E"); /* Unicode Apple Emoji */
        appleBtn.VerticalAlignment(VerticalAlignment::Center);
        appleBtn.Margin(ThicknessHelper::FromLengths(0, 0, 16, 0));

        TextBlock appName;
        appName.Text(L"Finder");
        appName.VerticalAlignment(VerticalAlignment::Center);
        appName.Margin(ThicknessHelper::FromLengths(0, 0, 16, 0));
        appName.Foreground(SolidColorBrush(winrt::Microsoft::UI::Colors::Black()));

        TextBlock menuItems;
        menuItems.Text(L"File   Edit   View");
        menuItems.VerticalAlignment(VerticalAlignment::Center);
        menuItems.Foreground(SolidColorBrush(winrt::Microsoft::UI::Colors::Black()));

        leftPanel.Children().Append(appleBtn);
        leftPanel.Children().Append(appName);
        leftPanel.Children().Append(menuItems);
        Grid::SetColumn(leftPanel, 0);

        /* Right Section: Clock, StatusIcons, UserMenu */
        StackPanel rightPanel;
        rightPanel.Orientation(Orientation::Horizontal);
        rightPanel.HorizontalAlignment(HorizontalAlignment::Right);
        rightPanel.Padding(ThicknessHelper::FromLengths(0, 0, 16, 0));

        TextBlock clockTxt;
        clockTxt.Text(L"9:41PM");
        clockTxt.VerticalAlignment(VerticalAlignment::Center);
        clockTxt.Margin(ThicknessHelper::FromLengths(0, 0, 16, 0));
        clockTxt.Foreground(SolidColorBrush(winrt::Microsoft::UI::Colors::Black()));

        TextBlock statusIcons;
        statusIcons.Text(L"\xD83D\xDD0B \xD83D\xDCF6"); /* Battery, WiFi */
        statusIcons.VerticalAlignment(VerticalAlignment::Center);
        statusIcons.Margin(ThicknessHelper::FromLengths(0, 0, 16, 0));
        statusIcons.Foreground(SolidColorBrush(winrt::Microsoft::UI::Colors::Black()));

        TextBlock userMenu;
        userMenu.Text(L"\xD83D\xDC64"); /* User icon */
        userMenu.VerticalAlignment(VerticalAlignment::Center);
        userMenu.Foreground(SolidColorBrush(winrt::Microsoft::UI::Colors::Black()));

        rightPanel.Children().Append(clockTxt);
        rightPanel.Children().Append(statusIcons);
        rightPanel.Children().Append(userMenu);
        Grid::SetColumn(rightPanel, 1);

        rootGrid.Children().Append(leftPanel);
        rootGrid.Children().Append(rightPanel);

        OutputDebugStringW(L"[InitWinUI3Island] XAML tree built, setting content...\n");
        /* 4. Set the content of the Island */
        g_desktopSources[index].Content(rootGrid);
        OutputDebugStringW(L"[InitWinUI3Island] Island content set successfully\n");
    } catch (const winrt::hresult_error& ex) {
        swprintf_s(buffer, 512, L"[InitWinUI3Island] WinRT hresult_error: 0x%X - %s\n", ex.code().value, ex.message().c_str());
        OutputDebugStringW(buffer);
        g_desktopSources[index] = nullptr;
    } catch (const std::exception& ex) {
        char abuffer[512];
        sprintf_s(abuffer, 512, "[InitWinUI3Island] std::exception: %s\n", ex.what());
        OutputDebugStringA(abuffer);
        g_desktopSources[index] = nullptr;
    } catch (...) {
        OutputDebugStringW(L"[InitWinUI3Island] Unknown exception during island creation\n");
        g_desktopSources[index] = nullptr;
    }
}
#endif

/* ─── Menu Bar Management ─── */
struct MonitorBounds {
    RECT rect;
};
static MonitorBounds g_monitors[MAX_MENUBARS];
static int g_numMonitors = 0;

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    if (g_numMonitors >= MAX_MENUBARS) return TRUE;
    g_monitors[g_numMonitors].rect = *lprcMonitor;
    g_numMonitors++;
    return TRUE;
}

static void CreateMenuBars() {
    g_numMonitors = 0;
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);

    HINSTANCE hInstance = GetModuleHandleW(NULL);

    for (int i = 0; i < g_numMonitors; i++) {
        if (g_numMenuBars >= MAX_MENUBARS) break;

        HWND hMenuBar = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"MenuBarWindow", L"MenuBar", WS_POPUP,
            g_monitors[i].rect.left, g_monitors[i].rect.top, 
            g_monitors[i].rect.right - g_monitors[i].rect.left, 32,
            NULL, NULL, hInstance, NULL
        );

        SetWindowPos(hMenuBar, HWND_TOPMOST,
            g_monitors[i].rect.left, g_monitors[i].rect.top, 
            g_monitors[i].rect.right - g_monitors[i].rect.left, 32,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);

        int currentIndex = g_numMenuBars;
        g_hMenuBars[currentIndex] = hMenuBar;
        g_numMenuBars++; /* Increment early so WM_SIZE loop finds it */

#ifdef __cplusplus
        /* Mark island for deferred initialization (will be created on first WM_CREATE/WM_SIZE) */
        g_islandInitialized[currentIndex] = FALSE;
#endif

        ShowWindow(hMenuBar, SW_SHOWNOACTIVATE);
        UpdateWindow(hMenuBar);
    }
}

static void DestroyMenuBars() {
    for (int i = 0; i < g_numMenuBars; i++) {
        if (g_hMenuBars[i]) {
            DestroyWindow(g_hMenuBars[i]);
            g_hMenuBars[i] = NULL;
        }
    }
    g_numMenuBars = 0;
}

/* ─── Menu Bar Window Procedure ─── */
static LRESULT CALLBACK MenuBarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
#ifdef __cplusplus
        /* On WM_CREATE, the message loop is now active, so we can safely create the island */
        for (int i = 0; i < g_numMenuBars; i++) {
            if (g_hMenuBars[i] == hwnd && !g_islandInitialized[i]) {
                OutputDebugStringW(L"[MenuBarWndProc] WM_CREATE: Initializing island...\n");
                if (g_winui3Initialized) {
                    try {
                        InitWinUI3Island(hwnd, i);
                        g_islandInitialized[i] = TRUE;
                    } catch (...) {
                        OutputDebugStringW(L"[MenuBarWndProc] WM_CREATE: Island initialization failed\n");
                    }
                }
                break;
            }
        }
#endif
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) return 0;
        break;
    case WM_SIZE:
#ifdef __cplusplus
        /* Resize the internal XAML Island HWND to fill the Win32 window */
        for (int i = 0; i < g_numMenuBars; i++) {
            if (g_hMenuBars[i] == hwnd) {
                /* Initialize island on first WM_SIZE if not already done */
                if (!g_islandInitialized[i] && g_winui3Initialized) {
                    OutputDebugStringW(L"[MenuBarWndProc] WM_SIZE: Deferred island initialization\n");
                    try {
                        InitWinUI3Island(hwnd, i);
                        g_islandInitialized[i] = TRUE;
                    } catch (...) {
                        OutputDebugStringW(L"[MenuBarWndProc] WM_SIZE: Island initialization failed\n");
                    }
                }

                /* Resize the island if it exists */
                if (g_desktopSources[i]) {
                    HWND islandHwnd = FindWindowExW(hwnd, NULL, NULL, NULL);
                    if (islandHwnd) {
                        RECT rc;
                        GetClientRect(hwnd, &rc);
                        SetWindowPos(islandHwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_SHOWWINDOW);
                    }
                }
                break;
            }
        }
#endif
        return 0;
    case WM_DESTROY:
#ifdef __cplusplus
        for (int i = 0; i < g_numMenuBars; i++) {
            if (g_hMenuBars[i] == hwnd && g_desktopSources[i]) {
                g_desktopSources[i].Close();
                g_desktopSources[i] = nullptr;
                g_islandInitialized[i] = FALSE;
                break;
            }
        }
#endif
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ─── Window Procedure ─── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        /* Size to fullscreen immediately after creation. */
    {
        RECT rect;
        GetAppropriateFullscreenRect(hwnd, &rect);
        /* HWND_BOTTOM = stay below all normal windows. */
        SetWindowPos(hwnd, HWND_BOTTOM,
            rect.left, rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);
        return 0;
    }

    case WM_TIMER:
        if (wParam == TICK_TIMER_ID) {
            /* Heartbeat tick — runs on UI thread, no data races */
            if (g_onTick != NULL) {
                g_onTick(hwnd, TickType_Heartbeat);
            }
            return 0;
        }
        else if (wParam == WALLPAPER_CHECK_ID) {
            /* Check for wallpaper changes separately to avoid UI thread stalling */
            if (IsWallpaperChanged()) {
                ClearCachedWallpaper();
                RefreshWallpaper();
                if (g_onWallpaperChange != NULL) {
                    g_onWallpaperChange(hwnd, TickType_WallpaperChange);
                }
            }
            return 0;
        }
        break;

    case WM_DISPLAYCHANGE:
        /* Monitor resolution changed — resize to cover appropriate area. */
    {
        RECT rect;
        GetAppropriateFullscreenRect(hwnd, &rect);
        /* HWND_BOTTOM = stay below all normal windows. */
        SetWindowPos(hwnd, HWND_BOTTOM,
            rect.left, rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE);

        /* Recreate Menu Bars for new monitor layout */
        DestroyMenuBars();
        CreateMenuBars();

        /* Notify screen change callback */
        if (g_onScreenChange != NULL) {
            g_onScreenChange(hwnd, TickType_ScreenChange);
        }
        return 0;
    }

    case WM_MOUSEACTIVATE:
        /* Prevent focus stealing when clicked. */
        return MA_NOACTIVATE;

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            /* Refuse activation to stay in the background. */
            return 0;
        }
        break;

    case WM_ACTIVATEAPP:
        /* Do NOT call PreventFocus here — it interferes with the activation transition
         * and can prevent the activating app from gaining foreground. */
        break;

    case WM_SIZE:
        /* Keep client area synced to window size. */
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_KEYDOWN:
        /* Ctrl+Q to quit */
        if (wParam == 'Q' && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
            DestroyWindow(hwnd);
            return 0;
        }
        /* Escape to quit */
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break; /* Pass unhandled keys to DefWindowProc */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* Fill with dark background first */
        FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));

        /* Render wallpaper if available */
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        RenderWallpaper(hdc, &clientRect);

        /* Draw a small label in the corner. */
        char buf[512];
        wsprintfA(buf, "DesktopSurfaceWindow  |  %dx%d  |  Borderless Fullscreen",
            clientRect.right - clientRect.left,
            clientRect.bottom - clientRect.top);
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        TextOutA(hdc, 16, 16, buf, lstrlenA(buf));

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_RBUTTONDOWN:
        /* Right-click to quit — safe for a standalone app. */
        DestroyWindow(hwnd);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ─── Entry Point ─── */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXW wcex = { 0 };
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

#ifdef __cplusplus
    /* WinUI 3 strictly requires an STA thread. Initialize COM and XAML before ANY other Win32/Shell APIs are called */
    try {
        InitWinUI3(TRUE); /* Pass TRUE to abort if XAML is required */
    } catch (...) {
        OutputDebugStringW(L"[WinMain] Uncaught exception from InitWinUI3().\n");
        MessageBoxW(NULL, L"Uncaught exception during WinUI 3 initialization. The application will now abort.", L"Fatal Error", MB_ICONERROR | MB_OK);
        ExitProcess(1);
    }
#endif

    /* 1. Register the DesktopSurfaceWindow class. */
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&wcex)) {
        MessageBoxW(NULL, L"Failed to register window class.", L"Error", MB_ICONERROR);
        return 1;
    }

    /* Register the MenuBarWindow class. */
    WNDCLASSEXW wcexMenu = { 0 };
    wcexMenu.cbSize = sizeof(WNDCLASSEXW);
    wcexMenu.style = CS_HREDRAW | CS_VREDRAW;
    wcexMenu.lpfnWndProc = MenuBarWndProc;
    wcexMenu.hInstance = hInstance;
    wcexMenu.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcexMenu.lpszClassName = L"MenuBarWindow";
    RegisterClassExW(&wcexMenu);

    /* 2. Create the borderless fullscreen window. */
    g_hwnd = CreateWindowExW(
        /* Ex-style: toolwindow removes taskbar entry, NOACTIVATE prevents stealing focus, LAYERED for DWM compatibility. */
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,

        WINDOW_CLASS_NAME,
        WINDOW_TITLE,

        /* WS_POPUP = no borders, no title bar. Visibility deferred to ShowWindow. */
        WS_POPUP,

        /* 0,0,0,0 — we resize immediately below. */
        0, 0, 0, 0,

        NULL,   /* no parent */
        NULL,   /* no menu */
        hInstance,
        NULL
    );

    if (!g_hwnd) {
        MessageBoxW(NULL, L"Failed to create window.", L"Error", MB_ICONERROR);
        return 1;
    }

    // [FIX 2] Initialize the layered window attributes so the window actually renders!
    // This sets it to fully opaque (255) so your wallpaper draws normally.
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    /* Initialize GDI+ and load wallpaper */
    if (InitGdiplus()) {
        if (FindWallpaperImage(g_wallpaperPath, MAX_PATH)) {
            /* Wallpaper found — will be rendered in WM_PAINT */
        }
    }

    /* 3. Size appropriately for the initial display configuration. */
    RECT rect;
    GetAppropriateFullscreenRect(g_hwnd, &rect);
    /* HWND_BOTTOM = stay at the bottom of the Z-order, below all normal windows. */
    SetWindowPos(g_hwnd, HWND_BOTTOM,
        rect.left, rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_FRAMECHANGED | SWP_NOACTIVATE);

    /* 4. Create Menu Bars for each monitor */
    CreateMenuBars();

    /* 5. Show and enter the message loop. */
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);

    /* 6. Start the system tick timer for wallpaper refresh and future hooks. */
    if (!StartTickTimer()) {
        /* Timer failed — continue without tick functionality */
    }

    /* 7. Register init-complete callback for future components. */
    if (g_onTick != NULL) {
        g_onTick(g_hwnd, TickType_InitComplete);
    }

    /* 8. Message loop with periodic tick processing. */
    MSG msg;
    BOOL bGotMsg = TRUE;
    while ((bGotMsg = GetMessage(&msg, NULL, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* Cleanup */
    StopTickTimer();
    ShutdownGdiplus();
    DestroyMenuBars();
#ifdef __cplusplus
    ShutdownWinUI3();
#endif

    return (int)msg.wParam;
}
