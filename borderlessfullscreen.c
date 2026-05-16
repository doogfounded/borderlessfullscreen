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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ─── Constants ─── */
#define WINDOW_CLASS_NAME L"DesktopSurfaceWindow"
#define WINDOW_TITLE      L"DesktopSurfaceWindow"

/* ─── Globals ─── */
static HWND g_hwnd = NULL;
static HMONITOR g_hMonitor = NULL;
static BOOL g_bPreventFocus = TRUE;  /* set FALSE during creation to avoid recursion */

/**
 * Prevent this window from stealing focus / becoming foreground.
 * Called during creation and on focus-related messages.
 */
static BOOL PreventFocus(HWND hwnd) {
    if (!g_bPreventFocus) return TRUE;
    
    /* Remove any active focus — give it back to the previous window. */
    HWND hPrev = GetForegroundWindow();
    if (hPrev != hwnd) {
        SetForegroundWindow(hPrev);
    }
    /* Force this window to the BOTTOM of the Z-order so normal apps float above it. */
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return TRUE;
}

/* ─── Helpers ─── */

/**
 * Get the primary monitor's full bounds (not work area).
 * For true fullscreen we use the entire monitor rect, not the work area.
 */
static void GetFullscreenRect(RECT *rect) {
    MONITORINFO mi = { sizeof(MONITORINFO) };
    HMONITOR hMonitor = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    g_hMonitor = hMonitor;  /* cache for WM_DISPLAYCHANGE */
    GetMonitorInfo(hMonitor, &mi);
    rect->left   = mi.rcMonitor.left;
    rect->top    = mi.rcMonitor.top;
    rect->right  = mi.rcMonitor.right;
    rect->bottom = mi.rcMonitor.bottom;
}

/**
 * Get the virtual desktop bounds covering all monitors.
 * This is the key to "desktop layer" behavior across multiple monitors.
 */
static void GetVirtualDesktopRect(RECT *rect) {
    rect->left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect->top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect->right  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect->bottom = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/**
 * Get the appropriate fullscreen rect based on multi-monitor mode.
 * If multi-monitor mode is enabled, use the virtual desktop rect.
 * Otherwise, use the primary monitor's bounds.
 */
static void GetAppropriateFullscreenRect(RECT *rect) {
    /* Check if we have multiple monitors */
    if (GetSystemMetrics(SM_CMONITORS) > 1) {
        GetVirtualDesktopRect(rect);
    } else {
        GetFullscreenRect(rect);
    }
}

/* ─── Window Procedure ─── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            /* Size to fullscreen immediately after creation. */
        {
            RECT rect;
            GetAppropriateFullscreenRect(&rect);
            /* HWND_BOTTOM = stay below all normal windows. */
            SetWindowPos(hwnd, HWND_BOTTOM,
                         rect.left, rect.top,
                         rect.right  - rect.left,
                         rect.bottom - rect.top,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
            return 0;
        }

        case WM_DISPLAYCHANGE:
            /* Monitor resolution changed — resize to cover entire virtual desktop. */
        {
            RECT rect;
            GetVirtualDesktopRect(&rect);
            /* HWND_BOTTOM = stay below all normal windows. */
            SetWindowPos(hwnd, HWND_BOTTOM,
                         rect.left, rect.top,
                         rect.right  - rect.left,
                         rect.bottom - rect.top,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
            return 0;
        }

        case WM_ACTIVATE:
            /* Prevent this window from ever stealing focus. */
            if (wParam != WA_INACTIVE) {
                PreventFocus(hwnd);
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);

        case WM_ACTIVATEAPP:
            /* When another app becomes active, ensure we stay inactive. */
            if (!wParam) {
                PreventFocus(hwnd);
            }
            return 0;

        case WM_KILLFOCUS:
            /* We don't want focus — give it back immediately. */
        {
            HWND hwndNewFocus = (HWND)wParam;
            if (hwndNewFocus != NULL) {
                SetFocus(hwndNewFocus);
            }
            return 0;
        }

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
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            /* Fill with a dark background to prove the window exists. */
            HBRUSH hBrush = CreateSolidBrush(RGB(18, 18, 24));
            FillRect(hdc, &ps.rcPaint, hBrush);
            DeleteObject(hBrush);

            /* Draw a small label in the corner. */
            char buf[128];
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            wsprintfA(buf, "DesktopSurfaceWindow  |  %dx%d  |  Borderless Fullscreen",
                      clientRect.right - clientRect.left,
                      clientRect.bottom - clientRect.top);
            SetTextColor(hdc, RGB(100, 100, 120));
            SetBkMode(hdc, TRANSPARENT);
            TextOutA(hdc, 16, 16, buf, lstrlenA(buf));

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

/* ─── Entry Point ─── */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXW wcex = {0};

    /* 1. Register the DesktopSurfaceWindow class. */
    wcex.cbSize          = sizeof(WNDCLASSEXW);
    wcex.style           = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wcex.lpfnWndProc     = WndProc;
    wcex.hInstance       = hInstance;
    wcex.hCursor         = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground   = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName   = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&wcex)) {
        MessageBoxW(NULL, L"Failed to register window class.", L"Error", MB_ICONERROR);
        return 1;
    }

    /* 2. Create the borderless fullscreen window. */
    g_hwnd = CreateWindowExW(
        /* Ex-style: toolwindow removes taskbar entry, NOACTIVATE prevents stealing focus. */
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,

        WINDOW_CLASS_NAME,
        WINDOW_TITLE,

        /* WS_POPUP = no borders, no title bar. */
        WS_POPUP | WS_VISIBLE,

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

    /* 3. Size to the virtual desktop's full bounds (not work area). */
    RECT rect;
    GetVirtualDesktopRect(&rect);
    /* HWND_BOTTOM = stay at the bottom of the Z-order, below all normal windows. */
    SetWindowPos(g_hwnd, HWND_BOTTOM,
                 rect.left, rect.top,
                 rect.right  - rect.left,
                 rect.bottom - rect.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);

    /* 4. Enable focus prevention now that window is created. */
    g_bPreventFocus = TRUE;

    /* 5. Show and enter the message loop. */
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
