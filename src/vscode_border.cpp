// vscode-border: draws a colored border overlay around each VS Code window.
//
// Each tracked VS Code top-level window gets its own lightweight,
// click-through, layered popup window painted as a hollow rectangle frame
// just outside the target window's bounds. The overlay follows its target's
// position/size/z-order/visibility via SetWinEventHook (mainly
// EVENT_OBJECT_LOCATIONCHANGE) -- no polling loop needed for normal
// tracking; a slow timer only exists as a safety net for missed events.
//
// Thickness / opacity / colors are read from config.ini next to the exe.
// A tray icon is the only UI: right-click -> Reload Config / Exit.
//
// Implementation is split across:
//   logger.*            - file logging
//   config.*            - config.ini loading
//   window_title.*      - VS Code window title -> folder/repo label parsing
//   window_discovery.*  - finding/filtering VS Code top-level windows
//   overlay.*           - layered overlay window creation/painting
//   tracking.*          - tracked-window bookkeeping, WinEvent hooks, sync

#include <windows.h>
#include <shellapi.h>
#include "resource.h"

#include "config.h"
#include "logger.h"
#include "overlay.h"
#include "tracking.h"
#include "worktree_resolver.h"
#include "vscode_settings.h"

#include <algorithm>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HINSTANCE g_hInstance = nullptr;

// ---------------------------------------------------------------------------
// Tray icon / app lifecycle
// ---------------------------------------------------------------------------

static const UINT WM_APP_TRAYICON = WM_APP + 1;
static const UINT ID_TRAY_RELOAD = 1;
static const UINT ID_TRAY_OPEN_CONFIG = 2;
static const UINT ID_TRAY_OPEN_LOG_FOLDER = 3;
static const UINT ID_TRAY_EXIT = 4;
static const UINT_PTR TIMER_RESCAN = 1;
// TIMER id 2 is kForegroundPollTimerId (tracking.cpp), started/stopped there.
static const int kTrayIconSize = 32; // upscaled from 16 so the warning badge/asterisk stays legible

static NOTIFYICONDATAW g_nid = {};
static HICON g_hIconNormal = nullptr;
static HICON g_hIconWarning = nullptr; // lazily built the first time it's needed; cached after that
static bool g_trayShowingWarning = false;
// Global (idProcess=0) hooks: only for the low-frequency events needed to
// *discover* brand-new VS Code windows we don't know the PID of yet, plus
// FOREGROUND (fires once per focus switch, desktop-wide either way, so a
// global hook costs nothing extra). LOCATIONCHANGE/DESTROY for
// already-tracked windows use per-process hooks instead (see tracking.cpp)
// since those events are far higher-frequency.
static HWINEVENTHOOK g_hookCreate = nullptr;
static HWINEVENTHOOK g_hookShow = nullptr;
static HWINEVENTHOOK g_hookNameChange = nullptr;
static HWINEVENTHOOK g_hookForeground = nullptr;

// Composites a small red circle + white "*" onto the bottom-right corner of
// baseIcon, returning a new icon -- used for the tray icon's warning badge
// (see UpdateTrayIconWarningState). baseIcon's own transparency is
// recovered by rendering it once over solid black and once over solid
// white and comparing the two: pixels where both renders match are opaque
// (composited color = either render), pixels that differ are transparent.
// This works regardless of whether the source icon carries a real alpha
// channel or (as assets/app.ico turned out to, confirmed by inspection) a
// classic 1-bit AND mask with an unused/zero alpha byte in its color
// bitmap -- DrawIconEx always composites correctly against a solid
// backdrop either way, so measuring the visible result sidesteps needing
// to know which format is behind it.
static HICON CreateWarningBadgedIcon(HICON baseIcon, int size) {
    HDC screenDC = GetDC(nullptr);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    auto renderOverBg = [&](COLORREF bg, HBITMAP& outBmp, void*& outBits) {
        HDC dc = CreateCompatibleDC(screenDC);
        outBmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &outBits, nullptr, 0);
        HBITMAP old = (HBITMAP)SelectObject(dc, outBmp);
        RECT rc = {0, 0, size, size};
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        DrawIconEx(dc, 0, 0, baseIcon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        DeleteDC(dc);
    };

    HBITMAP blackBmp = nullptr, whiteBmp = nullptr;
    void* blackBits = nullptr;
    void* whiteBits = nullptr;
    renderOverBg(RGB(0, 0, 0), blackBmp, blackBits);
    renderOverBg(RGB(255, 255, 255), whiteBmp, whiteBits);
    if (!blackBmp || !whiteBmp) {
        if (blackBmp) DeleteObject(blackBmp);
        if (whiteBmp) DeleteObject(whiteBmp);
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    HDC memDC = CreateCompatibleDC(screenDC);
    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!colorBmp) {
        DeleteObject(blackBmp);
        DeleteObject(whiteBmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, colorBmp);

    UINT32* blackPx = (UINT32*)blackBits;
    UINT32* whitePx = (UINT32*)whiteBits;
    UINT32* pixels = (UINT32*)bits;
    for (int i = 0; i < size * size; i++) {
        UINT32 bp = blackPx[i] & 0x00FFFFFF;
        UINT32 wp = whitePx[i] & 0x00FFFFFF;
        pixels[i] = (bp == wp) ? (0xFF000000u | bp) : 0u;
    }
    DeleteObject(blackBmp);
    DeleteObject(whiteBmp);

    // Yellow "!" glyph in the bottom-right corner, blended straight onto
    // whatever's already there -- no badge backdrop shape. GDI's
    // antialiased text rendering writes partial coverage into the alpha
    // byte of a 32bpp DIB (same issue as overlay.cpp's label text), so
    // render it into an isolated black-background mask DC first and use
    // its brightness as blend coverage against each destination pixel's
    // *existing* color instead of a fixed background.
    int r = size * 7 / 16;
    int cx = size - r - 1 + r / 3; // nudged right of center-in-corner
    int cy = size - r - 1;
    COLORREF glyphColor = RGB(0xFF, 0xC8, 0x00); // yellow
    BYTE gR = GetRValue(glyphColor), gG = GetGValue(glyphColor), gB = GetBValue(glyphColor);
    int boxSize = r * 2;
    HDC maskDC = CreateCompatibleDC(screenDC);
    BITMAPINFO maskBmi = {};
    maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    maskBmi.bmiHeader.biWidth = boxSize;
    maskBmi.bmiHeader.biHeight = -boxSize;
    maskBmi.bmiHeader.biPlanes = 1;
    maskBmi.bmiHeader.biBitCount = 32;
    maskBmi.bmiHeader.biCompression = BI_RGB;
    void* maskBits = nullptr;
    HBITMAP maskBmp = CreateDIBSection(maskDC, &maskBmi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
    if (maskBmp) {
        HBITMAP oldMaskBmp = (HBITMAP)SelectObject(maskDC, maskBmp);
        HFONT font = CreateFontW(-(int)(r * 2.1), 0, 0, 0, FW_BLACK, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(maskDC, font);
        SetBkMode(maskDC, TRANSPARENT);
        SetTextColor(maskDC, RGB(255, 255, 255));
        RECT textRect = {0, 0, boxSize, boxSize};
        DrawTextW(maskDC, L"!", 1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(maskDC, oldFont);
        DeleteObject(font);

        UINT32* maskPixels = (UINT32*)maskBits;
        int originX = cx - r, originY = cy - r;
        for (int y = 0; y < boxSize; y++) {
            for (int x = 0; x < boxSize; x++) {
                BYTE cov = (BYTE)(maskPixels[y * boxSize + x] & 0xFF); // R=G=B for white-on-black text
                if (cov == 0) continue;
                int px = originX + x, py = originY + y;
                if (px < 0 || py < 0 || px >= size || py >= size) continue;
                UINT32 existing = pixels[py * size + px];
                BYTE exA = (BYTE)((existing >> 24) & 0xFF);
                BYTE exR = (BYTE)((existing >> 16) & 0xFF);
                BYTE exG = (BYTE)((existing >> 8) & 0xFF);
                BYTE exB = (BYTE)(existing & 0xFF);
                BYTE outR = (BYTE)(exR + ((int)gR - exR) * cov / 255);
                BYTE outG = (BYTE)(exG + ((int)gG - exG) * cov / 255);
                BYTE outB = (BYTE)(exB + ((int)gB - exB) * cov / 255);
                BYTE outA = (BYTE)(exA + (255 - exA) * cov / 255); // opaque even over transparent bg
                pixels[py * size + px] = (UINT32(outA) << 24) | (UINT32(outR) << 16) | (UINT32(outG) << 8) |
                                          UINT32(outB);
            }
        }
        SelectObject(maskDC, oldMaskBmp);
        DeleteObject(maskBmp);
    }
    DeleteDC(maskDC);

    // All-zero AND mask -- the color bitmap's own alpha channel (which we
    // just built by hand above) governs transparency for this 32bpp icon.
    int monoStride = ((size + 15) / 16) * 2;
    std::vector<BYTE> monoBits(monoStride * size, 0);
    HBITMAP andMaskBmp = CreateBitmap(size, size, 1, 1, monoBits.data());

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = andMaskBmp;
    ii.hbmColor = colorBmp;
    HICON result = CreateIconIndirect(&ii);

    DeleteObject(andMaskBmp);
    SelectObject(memDC, oldBmp);
    DeleteObject(colorBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return result;
}

static HICON GetOrCreateWarningIcon() {
    if (!g_hIconWarning && g_hIconNormal) g_hIconWarning = CreateWarningBadgedIcon(g_hIconNormal, kTrayIconSize);
    return g_hIconWarning;
}

// Swaps the tray icon to/from the warning-badged variant when HasWarnings()
// changes, and updates the tooltip to match. The badge is the only
// indicator of unexpected-condition warnings (see logger.h's LogWarn) for
// the current run -- since the log starts fresh on every launch, so does
// HasWarnings(), so restarting the app is what clears the badge.
static void UpdateTrayIconWarningState() {
    bool warn = HasWarnings();
    if (warn == g_trayShowingWarning) return;

    HICON icon = warn ? GetOrCreateWarningIcon() : g_hIconNormal;
    if (!icon) return; // badge generation failed -- keep showing whatever's already set

    g_nid.hIcon = icon;
    wcscpy_s(g_nid.szTip, warn ? L"VS Code Window Borders -- warnings logged (tray menu > Open Log Folder)"
                                : L"VS Code Window Borders");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_trayShowingWarning = warn;
}

static void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_APP_TRAYICON;

    g_hIconNormal = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON), IMAGE_ICON, kTrayIconSize,
                                       kTrayIconSize, LR_DEFAULTCOLOR);
    if (!g_hIconNormal) g_hIconNormal = LoadIconW(nullptr, IDI_APPLICATION);
    g_nid.hIcon = g_hIconNormal;

    wcscpy_s(g_nid.szTip, L"VS Code Window Borders");
    g_nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, L"VS Code Window Borders is running");
    wcscpy_s(g_nid.szInfo, L"Right-click this icon for options (Reload Config / Open Log Folder / Exit).");

    BOOL ok = Shell_NotifyIconW(NIM_ADD, &g_nid);
    Log(L"Shell_NotifyIconW(NIM_ADD) -> %d (lastError=%lu)", ok, ok ? 0 : GetLastError());
}

static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_RELOAD, L"Reload Config");
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_CONFIG, L"Open Config");
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_LOG_FOLDER, L"Open Log Folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hwnd); // required so the menu dismisses correctly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static void ReloadConfig() {
    g_config = LoadConfig(GetConfigPath());
    SyncWindowTitleSetting(g_config.showLabel);
    RefreshWorktreeCache();
    RefreshAllLabels();
    ForceRepaintAllTracked();
    KillTimer(g_nid.hWnd, TIMER_RESCAN);
    SetTimer(g_nid.hWnd, TIMER_RESCAN, g_config.rescanIntervalMs, nullptr);
    UpdateTrayIconWarningState();
}

static void OpenConfigInDefaultEditor() {
    std::wstring configPath = GetConfigPath();
    HINSTANCE result = ShellExecuteW(nullptr, L"open", configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        Log(L"ShellExecuteW(open config) failed for %ls, code=%Id", configPath.c_str(), (INT_PTR)result);
    }
}

static void OpenLogFolderInExplorer() {
    std::wstring logPath = GetLogFilePath();
    // /select opens Explorer at the containing folder (same as
    // config.ini's) with vscode_border.log pre-highlighted, rather than
    // dropping the user into it with nothing pointed out.
    std::wstring params = L"/select,\"" + logPath + L"\"";
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        Log(L"ShellExecuteW(open log folder) failed for %ls, code=%Id", logPath.c_str(), (INT_PTR)result);
    }
}

static void CleanupAndQuit(HWND hwnd) {
    CleanupAllTracked();

    if (g_hookCreate) UnhookWinEvent(g_hookCreate);
    if (g_hookShow) UnhookWinEvent(g_hookShow);
    if (g_hookNameChange) UnhookWinEvent(g_hookNameChange);
    if (g_hookForeground) UnhookWinEvent(g_hookForeground);

    FlushLogBuffer();
    RemoveTrayIcon();
    PostQuitMessage(0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_EXIT) {
                CleanupAndQuit(hwnd);
            } else if (LOWORD(wParam) == ID_TRAY_RELOAD) {
                ReloadConfig();
            } else if (LOWORD(wParam) == ID_TRAY_OPEN_CONFIG) {
                OpenConfigInDefaultEditor();
            } else if (LOWORD(wParam) == ID_TRAY_OPEN_LOG_FOLDER) {
                OpenLogFolderInExplorer();
            }
            return 0;
        case WM_TIMER:
            if (wParam == TIMER_RESCAN) {
                RescanAllWindows();
                FlushLogBuffer(); // periodic, off the hot event path -- see LogFast
            } else if (wParam == kForegroundPollTimerId) {
                PollForegroundChange();
            }
            UpdateTrayIconWarningState(); // cheap no-op unless HasWarnings() just changed
            return 0;
        case WM_DESTROY:
            CleanupAndQuit(hwnd);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VSCodeBorderApp_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"another instance already running, exiting");
        return 0;
    }

    g_hInstance = hInstance;
    g_config = LoadConfig(GetConfigPath());
    SyncWindowTitleSetting(g_config.showLabel);

    const wchar_t* kClassName = L"VSCodeBorderAppWndClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    WNDCLASSW wcOverlay = {};
    wcOverlay.lpfnWndProc = DefWindowProcW;
    wcOverlay.hInstance = hInstance;
    wcOverlay.lpszClassName = kOverlayClassName;
    RegisterClassW(&wcOverlay);

    HWND hwnd = CreateWindowExW(0, kClassName, L"VSCodeBorderApp", 0,
                                 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!hwnd) {
        Log(L"CreateWindowExW failed, lastError=%lu", GetLastError());
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    TrackingInit(hInstance, hwnd);

    Log(L"=== startup ===");
    AddTrayIcon(hwnd);
    RescanAllWindows();
    Log(L"initial rescan complete, tracked=%zu", TrackedWindowCount());
    UpdateTrayIconWarningState(); // picks up any warnings from config load / rescan above

    g_hookCreate = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
                                    nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
                                  nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookNameChange = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
                                        nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                        nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    SetTimer(hwnd, TIMER_RESCAN, g_config.rescanIntervalMs, nullptr);
    // Foreground-poll timer (id kForegroundPollTimerId) is started by
    // TrackingInit/RescanAllWindows above, the moment a window is tracked.

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
