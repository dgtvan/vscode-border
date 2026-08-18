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

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <psapi.h>
#include "resource.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static void Log(const wchar_t* fmt, ...) {
    static wchar_t path[MAX_PATH] = {};
    static bool truncatedThisRun = false;
    if (!path[0]) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        swprintf_s(path, L"%s\\vscode_border.log", tmp);
    }
    // Start a fresh log each run instead of appending forever -- this app
    // is meant to run continuously via autostart, so a log that grows
    // across every single launch over weeks/months isn't acceptable.
    FILE* f = nullptr;
    const wchar_t* mode = truncatedThisRun ? L"a, ccs=UTF-8" : L"w, ccs=UTF-8";
    if (_wfopen_s(&f, path, mode) != 0 || !f) return;
    truncatedThisRun = true;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);
    fwprintf(f, L"\n");
    fclose(f);
}

// ---------------------------------------------------------------------------
// Config (config.ini next to the exe)
// ---------------------------------------------------------------------------

struct Config {
    int thickness = 4;
    int opacity = 255;
    int rescanIntervalMs = 1500;
    std::vector<COLORREF> palette;
};

static std::vector<COLORREF> DefaultPalette() {
    return {
        RGB(0xE6, 0x4A, 0x4A), RGB(0x4A, 0x9C, 0xE6), RGB(0x4A, 0xE6, 0x7A), RGB(0xE6, 0xB8, 0x4A),
        RGB(0xB0, 0x4A, 0xE6), RGB(0x4A, 0xE6, 0xDC), RGB(0xE6, 0x4A, 0xA8), RGB(0x9C, 0xE6, 0x4A),
        RGB(0xE6, 0x7A, 0x4A), RGB(0x4A, 0x5C, 0xE6),
    };
}

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}

static std::wstring GetConfigPath() {
    return GetExeDir() + L"\\config.ini";
}

static std::string ReadFileBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();

    DWORD size = GetFileSize(h, nullptr);
    std::string buf;
    if (size != INVALID_FILE_SIZE && size > 0) {
        buf.resize(size);
        DWORD bytesRead = 0;
        ReadFile(h, &buf[0], size, &bytesRead, nullptr);
        buf.resize(bytesRead);
    }
    CloseHandle(h);
    return buf;
}

static std::vector<std::string> SplitChar(const std::string& s, char delim) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == delim) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static COLORREF ParseHexColor(const std::string& hex) {
    unsigned long v = strtoul(hex.c_str(), nullptr, 16);
    BYTE r = (BYTE)((v >> 16) & 0xFF), g = (BYTE)((v >> 8) & 0xFF), b = (BYTE)(v & 0xFF);
    return RGB(r, g, b);
}

static Config LoadConfig(const std::wstring& path) {
    Config cfg;
    cfg.palette = DefaultPalette();

    std::string content = ReadFileBytes(path);
    if (content.empty()) {
        Log(L"config: not found at %ls, using defaults", path.c_str());
        return cfg;
    }

    for (const std::string& rawLine : SplitChar(content, '\n')) {
        std::string line = Trim(rawLine);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "thickness") {
            cfg.thickness = std::max(1, atoi(val.c_str()));
        } else if (key == "opacity") {
            cfg.opacity = std::min(255, std::max(0, atoi(val.c_str())));
        } else if (key == "rescan_interval_ms") {
            cfg.rescanIntervalMs = std::max(500, atoi(val.c_str()));
        } else if (key == "colors") {
            std::vector<COLORREF> palette;
            for (const std::string& item : SplitChar(val, ',')) {
                std::string c = Trim(item);
                if (!c.empty()) palette.push_back(ParseHexColor(c));
            }
            if (!palette.empty()) cfg.palette = palette;
        }
    }

    Log(L"config: loaded thickness=%d opacity=%d rescan_ms=%d colors=%zu",
        cfg.thickness, cfg.opacity, cfg.rescanIntervalMs, cfg.palette.size());
    return cfg;
}

static Config g_config;
static HINSTANCE g_hInstance = nullptr;

// ---------------------------------------------------------------------------
// Window discovery / filtering
// ---------------------------------------------------------------------------

static bool GetProcessImageName(HWND hwnd, std::wstring& outName) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return false;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(hProcess, 0, path, &size) != 0;
    CloseHandle(hProcess);
    if (!ok) return false;

    std::wstring full(path);
    size_t slash = full.find_last_of(L"\\/");
    outName = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    return true;
}

static bool IsVSCodeExe(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return lower == L"code.exe" || lower == L"code - insiders.exe";
}

static bool IsCandidateTopLevelWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0) return false;

    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    if (title[0] == L'\0') return false;

    return true;
}

// ---------------------------------------------------------------------------
// Overlay windows
// ---------------------------------------------------------------------------

static const wchar_t* kOverlayClassName = L"VSCodeBorderOverlayWndClass";

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                   LONG idObject, LONG idChild, DWORD, DWORD);

struct TrackedWindow {
    HWND overlay = nullptr;
    int colorIndex = 0;
    int lastWidth = -1;
    int lastHeight = -1;
    DWORD pid = 0;
};

static std::unordered_map<HWND, TrackedWindow> g_tracked; // target hwnd -> info
static std::vector<bool> g_colorInUse;

// EVENT_OBJECT_LOCATIONCHANGE fires for every move/resize/visibility/Z-order
// change of every top-level window on the whole desktop -- registering it
// globally (idProcess=0) means our callback gets invoked constantly on a
// busy multi-app desktop even though we only care about a handful of VS
// Code windows. Instead we register LOCATIONCHANGE/DESTROY per-process,
// scoped to the PIDs we're actually tracking, so the OS itself filters out
// everything irrelevant before it ever reaches us. Ref-counted because a
// PID can own more than one tracked window.
struct PidHooks {
    HWINEVENTHOOK location = nullptr;
    HWINEVENTHOOK destroy = nullptr;
    int refCount = 0;
};
static std::unordered_map<DWORD, PidHooks> g_pidHooks;

static void AddPidHooks(DWORD pid) {
    PidHooks& h = g_pidHooks[pid];
    h.refCount++;
    if (h.refCount == 1) {
        h.location = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                                      nullptr, WinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
        h.destroy = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
                                     nullptr, WinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
        Log(L"per-process hooks added pid=%lu", pid);
    }
}

static void ReleasePidHooks(DWORD pid) {
    auto it = g_pidHooks.find(pid);
    if (it == g_pidHooks.end()) return;
    it->second.refCount--;
    if (it->second.refCount <= 0) {
        if (it->second.location) UnhookWinEvent(it->second.location);
        if (it->second.destroy) UnhookWinEvent(it->second.destroy);
        g_pidHooks.erase(it);
        Log(L"per-process hooks removed pid=%lu", pid);
    }
}

static void ReleaseAllPidHooks() {
    for (auto& kv : g_pidHooks) {
        if (kv.second.location) UnhookWinEvent(kv.second.location);
        if (kv.second.destroy) UnhookWinEvent(kv.second.destroy);
    }
    g_pidHooks.clear();
}

static int AllocateColorIndex() {
    size_t n = g_config.palette.size();
    if (g_colorInUse.size() != n) g_colorInUse.assign(n, false);

    for (size_t i = 0; i < n; i++) {
        if (!g_colorInUse[i]) {
            g_colorInUse[i] = true;
            return (int)i;
        }
    }
    static int roundRobin = 0;
    int idx = roundRobin % (int)n;
    roundRobin++;
    return idx;
}

// GetWindowRect includes Windows' invisible resize hit-test margin, which
// is a few pixels larger than the window's actual visible edge -- using it
// would leave a visible gap between the window and the border. The DWM
// extended frame bounds give the true visible bounds instead.
static bool GetVisibleWindowRect(HWND hwnd, RECT& out) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &out, sizeof(RECT)))) {
        return true;
    }
    return GetWindowRect(hwnd, &out) != 0;
}

static void FreeColorIndex(int idx) {
    if (idx >= 0 && idx < (int)g_colorInUse.size()) g_colorInUse[idx] = false;
}

static HWND CreateOverlay() {
    return CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, g_hInstance, nullptr);
}

// Paints a hollow rectangular frame of `thickness` px into the overlay's
// layered surface. Only the border bands are touched (not the whole
// width*height area) so resizing a large window stays cheap.
static void PaintOverlay(HWND overlay, int width, int height, COLORREF color, int thickness, int opacity) {
    if (width <= 0 || height <= 0) return;
    int t = std::min(thickness, std::min(width, height) / 2);
    if (t < 1) t = 1;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    // No ZeroMemory here: CreateDIBSection always hands back a freshly
    // committed buffer, and Windows guarantees freshly committed pages are
    // zero-filled (never hands user-mode code another process's leftover
    // memory) -- an explicit clear would just be a second full-buffer pass
    // for no benefit, on what's already the most size-sensitive call here.

    BYTE a = (BYTE)opacity;
    BYTE r = (BYTE)((GetRValue(color) * a) / 255);
    BYTE g = (BYTE)((GetGValue(color) * a) / 255);
    BYTE b = (BYTE)((GetBValue(color) * a) / 255);
    UINT32 px = (UINT32(a) << 24) | (UINT32(r) << 16) | (UINT32(g) << 8) | UINT32(b);

    UINT32* pixels = (UINT32*)bits;
    for (int y = 0; y < t; y++)
        for (int x = 0; x < width; x++) pixels[y * width + x] = px;
    for (int y = height - t; y < height; y++)
        for (int x = 0; x < width; x++) pixels[y * width + x] = px;
    for (int y = t; y < height - t; y++) {
        for (int x = 0; x < t; x++) pixels[y * width + x] = px;
        for (int x = width - t; x < width; x++) pixels[y * width + x] = px;
    }

    POINT ptSrc = {0, 0};
    SIZE sz = {width, height};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(overlay, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// Repositions/resizes/repaints/hides the overlay to match its target's
// current state. Cheap when only position changed (no repaint needed).
static void SyncOverlay(HWND target, TrackedWindow& tw) {
    if (!IsWindow(target) || !tw.overlay) return;

    if (IsIconic(target) || !IsWindowVisible(target)) {
        ShowWindow(tw.overlay, SW_HIDE);
        return;
    }

    RECT r;
    if (!GetVisibleWindowRect(target, r)) return;

    int t = g_config.thickness;
    int ox = r.left - t;
    int oy = r.top - t;
    int ow = (r.right - r.left) + 2 * t;
    int oh = (r.bottom - r.top) + 2 * t;
    if (ow <= 0 || oh <= 0) return;

    if (ow != tw.lastWidth || oh != tw.lastHeight) {
        COLORREF color = g_config.palette[tw.colorIndex % g_config.palette.size()];
        PaintOverlay(tw.overlay, ow, oh, color, t, g_config.opacity);
        tw.lastWidth = ow;
        tw.lastHeight = oh;
    }

    // Move/resize without touching Z-order. This has to be a separate call
    // from the Z-order fixup below: once the overlay is correctly stacked,
    // "the window in front of target" IS the overlay itself, and a combined
    // SetWindowPos(overlay, overlay, x, y, ...) is self-referential and
    // silently stops applying the position update -- the overlay would
    // freeze in place on every sync after the first.
    SetWindowPos(tw.overlay, nullptr, ox, oy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // SetWindowPos(w, insertAfter, ...) places w immediately BEHIND
    // insertAfter, not in front of it. To put the overlay directly in front
    // of its target, insert it behind whatever currently sits in front of
    // the target (or at HWND_TOP -- which is literally NULL, same as this
    // GetWindow call returns when target is already the frontmost window).
    // Only do this when the stacking actually needs fixing (e.g. right
    // after creation, or another window got inserted between them) --
    // otherwise this degenerates into the self-referential case above.
    HWND aboveTarget = GetWindow(target, GW_HWNDPREV);
    if (aboveTarget != tw.overlay) {
        SetWindowPos(tw.overlay, aboveTarget, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    }
}

static void TrackWindow(HWND hwnd) {
    if (g_tracked.find(hwnd) != g_tracked.end()) return;

    // Cheap, in-process checks (class name / visibility / owner / title)
    // first -- this rejects nearly every window on the desktop instantly.
    // Only genuine candidates pay for OpenProcess + QueryFullProcessImageName,
    // which is the one call in this path that's an actual kernel round-trip.
    if (!IsCandidateTopLevelWindow(hwnd)) return;

    std::wstring exeName;
    if (!GetProcessImageName(hwnd, exeName) || !IsVSCodeExe(exeName)) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    TrackedWindow tw;
    tw.overlay = CreateOverlay();
    tw.colorIndex = AllocateColorIndex();
    tw.pid = pid;
    g_tracked[hwnd] = tw;
    SyncOverlay(hwnd, g_tracked[hwnd]);
    AddPidHooks(pid);

    Log(L"tracked hwnd=%p colorIndex=%d overlay=%p pid=%lu", hwnd, tw.colorIndex, tw.overlay, pid);
}

static void UntrackWindow(HWND hwnd) {
    auto it = g_tracked.find(hwnd);
    if (it == g_tracked.end()) return;
    FreeColorIndex(it->second.colorIndex);
    ReleasePidHooks(it->second.pid);
    DestroyWindow(it->second.overlay);
    g_tracked.erase(it);
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    TrackWindow(hwnd);
    return TRUE;
}

static void RescanAllWindows() {
    EnumWindows(EnumWindowsProc, 0);

    std::vector<HWND> stale;
    for (auto& kv : g_tracked) {
        if (!IsWindow(kv.first) || !IsCandidateTopLevelWindow(kv.first)) {
            stale.push_back(kv.first);
        }
    }
    for (HWND hwnd : stale) UntrackWindow(hwnd);

    for (auto& kv : g_tracked) SyncOverlay(kv.first, kv.second);
}

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                   LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;

    if (event == EVENT_OBJECT_DESTROY) {
        UntrackWindow(hwnd);
    } else if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_NAMECHANGE) {
        // NAMECHANGE matters because a new window is often shown before its
        // title is set -- IsCandidateTopLevelWindow requires a non-empty
        // title, so without this the window would sit untracked until the
        // next safety-net rescan (up to rescan_interval_ms later).
        TrackWindow(hwnd);
    } else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        auto it = g_tracked.find(hwnd);
        if (it != g_tracked.end()) SyncOverlay(hwnd, it->second);
    }
}

// ---------------------------------------------------------------------------
// Tray icon / app lifecycle
// ---------------------------------------------------------------------------

static const UINT WM_APP_TRAYICON = WM_APP + 1;
static const UINT ID_TRAY_RELOAD = 1;
static const UINT ID_TRAY_OPEN_CONFIG = 2;
static const UINT ID_TRAY_EXIT = 3;
static const UINT_PTR TIMER_RESCAN = 1;

static NOTIFYICONDATAW g_nid = {};
// Global (idProcess=0) hooks: only for the low-frequency events needed to
// *discover* brand-new VS Code windows we don't know the PID of yet.
// LOCATIONCHANGE/DESTROY for already-tracked windows use per-process hooks
// instead (see PidHooks above) since those events are far higher-frequency.
static HWINEVENTHOOK g_hookCreate = nullptr;
static HWINEVENTHOOK g_hookShow = nullptr;
static HWINEVENTHOOK g_hookNameChange = nullptr;

static void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g_nid.uCallbackMessage = WM_APP_TRAYICON;

    g_nid.hIcon = (HICON)LoadImageW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    wcscpy_s(g_nid.szTip, L"VS Code Window Borders");
    g_nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, L"VS Code Window Borders is running");
    wcscpy_s(g_nid.szInfo, L"Right-click this icon for options (Reload Config / Exit).");

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
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hwnd); // required so the menu dismisses correctly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

static void ReloadConfig() {
    g_config = LoadConfig(GetConfigPath());
    for (auto& kv : g_tracked) {
        kv.second.lastWidth = -1;
        kv.second.lastHeight = -1;
        SyncOverlay(kv.first, kv.second);
    }
    KillTimer(g_nid.hWnd, TIMER_RESCAN);
    SetTimer(g_nid.hWnd, TIMER_RESCAN, g_config.rescanIntervalMs, nullptr);
}

static void OpenConfigInDefaultEditor() {
    std::wstring configPath = GetConfigPath();
    HINSTANCE result = ShellExecuteW(nullptr, L"open", configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        Log(L"ShellExecuteW(open config) failed for %ls, code=%Id", configPath.c_str(), (INT_PTR)result);
    }
}

static void CleanupAndQuit(HWND hwnd) {
    for (auto& kv : g_tracked) DestroyWindow(kv.second.overlay);
    g_tracked.clear();
    ReleaseAllPidHooks();

    if (g_hookCreate) UnhookWinEvent(g_hookCreate);
    if (g_hookShow) UnhookWinEvent(g_hookShow);
    if (g_hookNameChange) UnhookWinEvent(g_hookNameChange);

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
            }
            return 0;
        case WM_TIMER:
            if (wParam == TIMER_RESCAN) {
                RescanAllWindows();
            }
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

    Log(L"=== startup ===");
    AddTrayIcon(hwnd);
    RescanAllWindows();
    Log(L"initial rescan complete, tracked=%zu", g_tracked.size());

    g_hookCreate = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
                                    nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
                                  nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    g_hookNameChange = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
                                        nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    SetTimer(hwnd, TIMER_RESCAN, g_config.rescanIntervalMs, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
