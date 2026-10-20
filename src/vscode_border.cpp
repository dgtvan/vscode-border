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
// A tray icon is the only UI: right-click -> Close All Windows / Hide
// Borders and Hub / Reload Config / Exit.
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
//   focus_trace.*       - traces every focus-moving call + detects activation storms
//   diagnostic_snapshot.* - tray > Capture Diagnostic Snapshot: screenshot + full geometry/DPI report
//   ai_provider.*        - abstract interface an AI coding assistant integration implements
//   claude_provider.*   - Claude Code's AiProvider implementation (hooks + status file reading)
//   tracking.*          - tracked-window bookkeeping, WinEvent hooks, sync
//   vscode_cli.*        - VS Code CLI shim lookup, opening/closing VS Code windows
//   favourites.*        - persisted favourite projects, open-all
//   claude_usage.*      - Claude session/weekly usage per account, for the hub's usage rings

#include <windows.h>
#include <shellapi.h>
#include "resource.h"

#include "account.h"
#include "ai_provider.h"
#include "claude_usage.h"
#include "config.h"
#include "diagnostic_snapshot.h"
#include "focus_trace.h"
#include "label_alias.h"
#include "logger.h"
#include "overlay.h"
#include "tracking.h"
#include "tray_icon.h"
#include "worktree_resolver.h"
#include "vscode_settings.h"
#include "vscode_cli.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HINSTANCE g_hInstance = nullptr;

// ---------------------------------------------------------------------------
// Tray icon / app lifecycle
// ---------------------------------------------------------------------------

static const UINT WM_APP_TRAYICON = WM_APP + 1;
static const UINT WM_APP_CLAUDE_USAGE = WM_APP + 2; // a usage worker has a new result (see claude_usage.h)
static const UINT ID_TRAY_RELOAD = 1;
static const UINT ID_TRAY_OPEN_CONFIG = 2;
static const UINT ID_TRAY_OPEN_LOG_FOLDER = 3;
static const UINT ID_TRAY_EXIT = 4;
static const UINT ID_TRAY_TOGGLE_HIDDEN = 5;
static const UINT ID_TRAY_CLOSE_ALL = 6;
static const UINT ID_TRAY_CAPTURE_SNAPSHOT = 7;
// + user profile index (see account.h): forget that profile's saved password.
static const UINT ID_TRAY_FORGET_PASSWORD_FIRST = 100;
static const UINT_PTR TIMER_RESCAN = 1;
// TIMER id 2 is kForegroundPollTimerId (tracking.cpp), started/stopped there.
static const UINT_PTR TIMER_CAPTURE_SNAPSHOT = 3;
// Long enough for the tray menu's fade-out to finish, so the menu itself
// isn't baked into the snapshot's screenshot over whatever it was covering.
static const UINT kSnapshotDelayMs = 400;
static const int kTrayIconSize = 32; // upscaled from 16 so the warning badge/asterisk stays legible

static NOTIFYICONDATAW g_nid = {};
static HICON g_hIconNormal = nullptr;
static HICON g_hIconWarning = nullptr; // lazily built the first time it's needed; cached after that
static bool g_trayShowingWarning = false;
// Folder of the last diagnostic snapshot while its "saved" balloon is up --
// clicking that balloon opens it. Cleared when the balloon goes away, so a
// click on any other balloon (e.g. the startup one) doesn't.
static std::wstring g_snapshotBalloonDir;
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
    wcscpy_s(g_nid.szInfo, L"Right-click this icon for options (Hide Borders and Hub / Reload Config / Exit).");

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
    AppendMenuW(menu, MF_STRING | (TrackedWindowCount() == 0 ? MF_GRAYED : 0), ID_TRAY_CLOSE_ALL,
                L"Close All Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (AreOverlaysHidden() ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_TOGGLE_HIDDEN,
                L"Hide Borders and Hub");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // One item per user profile, greyed out while it has no saved password.
    std::vector<std::wstring> profiles = ConfiguredUserProfiles();
    if (!profiles.empty()) {
        HMENU forgetMenu = CreatePopupMenu();
        for (size_t i = 0; i < profiles.size(); i++) {
            UINT flags = MF_STRING | (HasSavedAccountPassword(profiles[i]) ? 0 : MF_GRAYED);
            AppendMenuW(forgetMenu, flags, ID_TRAY_FORGET_PASSWORD_FIRST + (UINT)i,
                        AccountMenuText(profiles[i]).c_str());
            MENUITEMINFOW mii = {sizeof(mii)};
            mii.fMask = MIIM_BITMAP;
            mii.hbmpItem = GetAccountIconMenuBitmap(profiles[i]);
            if (mii.hbmpItem) SetMenuItemInfoW(forgetMenu, (UINT)i, TRUE, &mii);
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)forgetMenu, L"Forget Saved Password");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, ID_TRAY_RELOAD, L"Reload Config");
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_CONFIG, L"Open Config");
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_LOG_FOLDER, L"Open Log Folder");
    AppendMenuW(menu, MF_STRING, ID_TRAY_CAPTURE_SNAPSHOT, L"Capture Diagnostic Snapshot");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    RequestForeground(hwnd, FocusTargetKind::OwnUi,
                      L"tray-menu"); // required so the menu dismisses correctly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static void ReloadConfig() {
    g_config = LoadConfig(GetConfigPath());
    SyncWindowTitleSetting(g_config.showLabel);
    SyncAllAiProviders(g_config);
    SyncClaudeUsageMonitor(g_nid.hWnd, WM_APP_CLAUDE_USAGE);
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

static void OpenFolderInExplorer(const std::wstring& dir) {
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + dir + L"\"").c_str(), nullptr,
                                     SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        Log(L"ShellExecuteW(open folder) failed for %ls, code=%Id", dir.c_str(), (INT_PTR)result);
    }
}

// See diagnostic_snapshot.h. Reports back with a balloon rather than by
// opening Explorer: the point is to grab the evidence and get back to
// work, not to be pulled into it -- the balloon is clickable for whoever
// does want to look right away.
static void CaptureSnapshotAndNotify() {
    std::wstring dir = CaptureDiagnosticSnapshot();
    UpdateTrayIconWarningState(); // a failed capture warns

    NOTIFYICONDATAW balloon = g_nid;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = dir.empty() ? NIIF_WARNING : NIIF_INFO;
    if (dir.empty()) {
        wcscpy_s(balloon.szInfoTitle, L"Diagnostic snapshot failed");
        wcscpy_s(balloon.szInfo, L"See the log (tray menu > Open Log Folder) for details.");
    } else {
        size_t slash = dir.find_last_of(L'\\');
        std::wstring name = slash == std::wstring::npos ? dir : dir.substr(slash + 1);
        wcscpy_s(balloon.szInfoTitle, L"Diagnostic snapshot saved");
        _snwprintf_s(balloon.szInfo, ARRAYSIZE(balloon.szInfo), _TRUNCATE,
                     L"logs\\snapshots\\%ls\nClick to open the folder.", name.c_str());
    }
    g_snapshotBalloonDir = dir;
    Shell_NotifyIconW(NIM_MODIFY, &balloon);
}

static void CleanupAndQuit(HWND hwnd) {
    StopClaudeUsageMonitor();
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
            } else if (lParam == NIN_BALLOONUSERCLICK) {
                if (!g_snapshotBalloonDir.empty()) OpenFolderInExplorer(g_snapshotBalloonDir);
                g_snapshotBalloonDir.clear();
            } else if (lParam == NIN_BALLOONTIMEOUT || lParam == NIN_BALLOONHIDE) {
                g_snapshotBalloonDir.clear();
            }
            return 0;
        case WM_APP_CLAUDE_USAGE:
            ProcessClaudeUsageResults();
            RefreshProjectListHud();
            UpdateTrayIconWarningState();
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
            } else if (LOWORD(wParam) == ID_TRAY_TOGGLE_HIDDEN) {
                SetOverlaysHidden(!AreOverlaysHidden());
            } else if (LOWORD(wParam) == ID_TRAY_CAPTURE_SNAPSHOT) {
                SetTimer(hwnd, TIMER_CAPTURE_SNAPSHOT, kSnapshotDelayMs, nullptr);
            } else if (LOWORD(wParam) == ID_TRAY_CLOSE_ALL) {
                ConfirmAndCloseVSCodeWindows(hwnd, GetTrackedWindows(), L"tray");
            } else if (LOWORD(wParam) >= ID_TRAY_FORGET_PASSWORD_FIRST &&
                       LOWORD(wParam) < ID_TRAY_FORGET_PASSWORD_FIRST + kMaxUserProfiles) {
                std::vector<std::wstring> profiles = ConfiguredUserProfiles();
                size_t i = LOWORD(wParam) - ID_TRAY_FORGET_PASSWORD_FIRST;
                if (i < profiles.size()) ForgetSavedAccountPassword(profiles[i]);
            }
            return 0;
        case WM_TIMER:
            if (wParam == TIMER_RESCAN) {
                RescanAllWindows();
                FlushLogBuffer(); // periodic, off the hot event path -- see LogFast
            } else if (wParam == kForegroundPollTimerId) {
                PollForegroundChange();
            } else if (wParam == TIMER_CAPTURE_SNAPSHOT) {
                KillTimer(hwnd, TIMER_CAPTURE_SNAPSHOT); // one-shot
                CaptureSnapshotAndNotify();
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
    // Before the single-instance check: the helper runs alongside the app,
    // as another account in the same session.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && wcscmp(argv[1], kClaudeUsageHelperArg) == 0) {
        int minutes = argc >= 3 ? _wtoi(argv[2]) : 5;
        LocalFree(argv);
        return RunClaudeUsageHelper(minutes);
    }
    if (argv) LocalFree(argv);

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
    wcOverlay.lpfnWndProc = OverlayWndProc;
    wcOverlay.hCursor = LoadCursorW(nullptr, IDC_ARROW); // over the label chip, while it's clickable
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
    SyncClaudeUsageMonitor(hwnd, WM_APP_CLAUDE_USAGE);

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
