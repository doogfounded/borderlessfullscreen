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
 *   ShellHost
├── MenuBar
├── DesktopSurface
└── WindowTracker
 */

#define _WIN32_WINNT 0x0500
#define WINVER       0x0500
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ─── Constants ─── */
#define WINDOW_CLASS_NAME L"DesktopSurfaceWindow"
#define WINDOW_TITLE      L"DesktopSurfaceWindow"
#define IDC_TOP_BUTTON    1001

/* ─── Globals ─── */
static HWND g_hwnd = NULL;
static HMONITOR g_hMonitor = NULL;
static BOOL g_bPreventFocus = FALSE;  /* set FALSE during creation to avoid recursion */

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

/* ─── Window Procedure ─── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            /* Size to fullscreen immediately after creation. */
        {
            RECT rect;
            GetFullscreenRect(&rect);
            /* HWND_BOTTOM = stay below all normal windows. */
            SetWindowPos(hwnd, HWND_BOTTOM,
                         rect.left, rect.top,
                         rect.right  - rect.left,
                         rect.bottom - rect.top,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);

            /* Create a button in the top white band. */
            CreateWindowW(L"BUTTON", L"🍎",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          5, 4, 100, 24, /* X=10, Y=4, Width=100, Height=24 */
                          hwnd, (HMENU)IDC_TOP_BUTTON,
                          ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            CreateWindowW(L"BUTTON", L"File",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          105, 4, 100, 24, /* X=10, Y=4, Width=100, Height=24 */
                          hwnd, (HMENU)IDC_TOP_BUTTON,
                          ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            CreateWindowW(L"BUTTON", L"Edit",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          205, 4, 100, 24, /* X=10, Y=4, Width=100, Height=24 */
                          hwnd, (HMENU)IDC_TOP_BUTTON,
                          ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            CreateWindowW(L"BUTTON", L"View",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          305, 4, 100, 24, /* X=10, Y=4, Width=100, Height=24 */
                          hwnd, (HMENU)IDC_TOP_BUTTON,
                          ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            CreateWindowW(L"BUTTON", L"Special",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          405, 4, 100, 24, /* X=10, Y=4, Width=100, Height=24 */
                          hwnd, (HMENU)IDC_TOP_BUTTON,
                          ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            CreateWindowW(L"BUTTON", L"Help",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          505, 4, 100, 24, /* X=10, Y=4, Width=100, Height=24 */
                          hwnd, (HMENU)IDC_TOP_BUTTON,
                          ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            return 0;
        }

        case WM_DISPLAYCHANGE:
            /* Monitor resolution changed — resize to primary monitor. */
        {
            RECT rect;
            GetFullscreenRect(&rect);
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

        case WM_COMMAND:
            /* Handle button clicks. */
            if (LOWORD(wParam) == IDC_TOP_BUTTON) {
                MessageBoxW(hwnd, L"The button works!", L"Notice", MB_OK | MB_ICONINFORMATION);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);

            /* Fill the entire client area with a dark background to ensure everything aligns perfectly. */
            HBRUSH hBrush = CreateSolidBrush(RGB(138, 138, 138));
            FillRect(hdc, &clientRect, hBrush);
            DeleteObject(hBrush);

            /* Draw a narrow white band at the top. */
            RECT topBandRect = clientRect;
            topBandRect.bottom = 32; /* whatever pixels height */

            /* Create a white brush for the inside and a thick black pen for the border */
            HBRUSH hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            HPEN hBlackPen = CreatePen(PS_INSIDEFRAME, 2, RGB(0, 0, 0)); /* 2 pixels thick */
            
            HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hWhiteBrush);
            HPEN hOldPen = (HPEN)SelectObject(hdc, hBlackPen);
            
            /* Rectangle() draws the border using the selected pen and fills with the selected brush */
            Rectangle(hdc, topBandRect.left, topBandRect.top, topBandRect.right, topBandRect.bottom);
            
            SelectObject(hdc, hOldBrush);
            SelectObject(hdc, hOldPen);
            DeleteObject(hWhiteBrush);
            DeleteObject(hBlackPen);

            /* Draw a small label in the corner. */
            char buf[256];

            SYSTEMTIME st;
            GetSystemTime(&st);

            /* Get UTC time. */

            wsprintfA(buf, "Hello",
                      clientRect.right - clientRect.left,
                      clientRect.bottom - clientRect.top,
                      st.wHour,
                      st.wMinute,
                      st.wSecond);

            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(138, 138, 138));
            SetBkMode(hdc, TRANSPARENT);

            /* Create a custom font */
            HFONT hFont = CreateFontA(
                48,                        /* cHeight (Text size) */
                0,                         /* cWidth (0 = automatically match height) */
                0,                         /* cEscapement */
                0,                         /* cOrientation */
                FW_THIN,                   /* cWeight (e.g., FW_NORMAL, FW_BOLD, FW_HEAVY) */
                FALSE,                     /* bItalic */
                FALSE,                     /* bUnderline */
                FALSE,                     /* bStrikeOut */
                ANSI_CHARSET,              /* iCharSet */
                OUT_DEFAULT_PRECIS,        /* iOutPrecision */
                CLIP_DEFAULT_PRECIS,       /* iClipPrecision */
                CLEARTYPE_QUALITY,         /* iQuality (Antialiasing) */
                DEFAULT_PITCH | FF_SWISS,  /* iPitchAndFamily */
                "Fixedsys"                 /* pszFaceName (Font style/family) */
            );
            
            /* Select the new font into the device context and save the old one */
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

            /* Measure the exact width and height of the text */
            SIZE textSize;
            GetTextExtentPoint32A(hdc, buf, lstrlenA(buf), &textSize);

            /* Calculate coordinates to place the text exactly in the center */
            int x = (clientRect.right - textSize.cx) / 2;
            int y = (clientRect.bottom - textSize.cy) / 2;

            TextOutA(hdc, x, y, buf, lstrlenA(buf));

            /* Clean up: restore the old font and delete the custom font to prevent memory leaks */
            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);

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

    /* 3. Size to the primary monitor's full bounds (not work area). */
    RECT rect;
    GetFullscreenRect(&rect);
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
