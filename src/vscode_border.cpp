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
//   file_util.*         - shared exe-relative path / file-read / file-write helpers
//   text_util.*         - shared UTF-8-safe line-escaping helpers for small *.ini state files
//   logger.*            - file logging
//   config.*            - config.ini loading
//   window_title.*      - VS Code window title -> folder/repo label parsing
//   window_discovery.*  - finding/filtering VS Code top-level windows
//   worktree_resolver.* - main-repo name lookup for git worktree checkouts
//   vscode_settings.*   - syncs VS Code's window.title setting
//   layered_rendering.* - shared per-pixel text-blend helpers for layered windows
//   overlay.*           - layered overlay window creation/painting
//   monitor_scenario.*  - identifies the active-monitor set, persists/recalls HUD placement per one
//   label_alias.*       - persists/recalls user-set label aliases (HUD right-click > Set Alias)
//   project_list_order.* - persists/recalls the manually-dragged project-list item order
//   project_list_hud.*  - the draggable/resizable project-list HUD window
//   tray_icon.*         - tray icon warning-badge compositing
//   ai_provider.*        - abstract interface an AI coding assistant integration implements
//   claude_provider.*   - Claude Code's AiProvider implementation (hooks + status file reading)
//   tracking.*          - tracked-window bookkeeping, WinEvent hooks, sync

#include <windows.h>
#include <shellapi.h>
#include "resource.h"

#include "ai_provider.h"
#include "config.h"
#include "label_alias.h"
#include "logger.h"
#include "overlay.h"
#include "tracking.h"
#include "tray_icon.h"
#include "worktree_resolver.h"
#include "vscode_settings.h"

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
    SyncAllAiProviders(g_config);
    RefreshWorktreeCache();
    ReloadAliases();
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
    SyncAllAiProviders(g_config);

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
