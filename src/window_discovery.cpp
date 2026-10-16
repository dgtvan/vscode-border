#include "window_discovery.h"
#include "logger.h"
#include "wsl.h"

#include <dwmapi.h>
#include <psapi.h>

#include <algorithm>
#include <map>

bool GetProcessImageName(HWND hwnd, std::wstring& outName) {
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

bool IsVSCodeExe(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return lower == L"code.exe" || lower == L"code - insiders.exe";
}

static bool IsWslgWindow(HWND hwnd) {
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    return wcscmp(cls, kWslgWindowClass) == 0;
}

bool IsVSCodeWindowProcess(HWND hwnd, const std::wstring& exeName) {
    return IsWslgWindow(hwnd) ? IsWslgHostExe(exeName) : IsVSCodeExe(exeName);
}

bool IsCandidateTopLevelWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;

    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    bool wslg = wcscmp(cls, kWslgWindowClass) == 0;
    if (!wslg && wcscmp(cls, L"Chrome_WidgetWin_1") != 0) return false;

    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, 512);
    if (title[0] == L'\0') return false;
    // Every Linux app WSLg shows shares this class (and process), so only
    // the title says which ones are VS Code -- see wsl.h.
    if (wslg && !SplitWslgVSCodeTitle(title, nullptr, nullptr)) return false;

    return true;
}

// A WSLg window's shadow margin while not maximized (see wsl.h's
// MeasureWslgShadowInsets), remembered per window: measuring means
// rendering the whole window, so it's only redone when moving to another
// monitor (which can change its DPI scale) -- never on a plain move or
// resize, which keep it the same.
struct WslgInsetCache {
    HMONITOR monitor = nullptr;
    RECT insets = {};
};
static std::map<HWND, WslgInsetCache> g_wslgInsets;

static void ApplyWslgShadowInsets(HWND hwnd, RECT& rect) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (IsZoomed(hwnd)) {
        // Maximized, Linux VS Code fills the work area: either the window
        // rect is exactly that, or (custom title bar, no AppBar shrinking the
        // work area) its shadow margin hangs off-screen around it. Pulled in
        // by the same 2px a maximized Windows VS Code window gets (see
        // GetVisibleWindowRect), so the border shows the same strip along the
        // screen edges instead of landing entirely off-screen.
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(monitor, &mi)) {
            IntersectRect(&rect, &rect, &mi.rcWork);
            InflateRect(&rect, -2, -2);
        }
        return;
    }
    auto it = g_wslgInsets.find(hwnd);
    if (it == g_wslgInsets.end() || it->second.monitor != monitor) {
        RECT insets;
        // Not cached on failure (e.g. content not painted yet), so the next
        // sync tries again; until then the last good value, if any, stands.
        if (!MeasureWslgShadowInsets(hwnd, insets)) {
            if (it == g_wslgInsets.end()) return;
        } else {
            Log(L"wsl: hwnd=%p shadow insets l=%ld t=%ld r=%ld b=%ld (monitor=%p)", hwnd, insets.left, insets.top,
                insets.right, insets.bottom, monitor);
            WslgInsetCache& c = g_wslgInsets[hwnd];
            c.monitor = monitor;
            c.insets = insets;
            it = g_wslgInsets.find(hwnd);
        }
    }
    rect.left += it->second.insets.left;
    rect.top += it->second.insets.top;
    rect.right -= it->second.insets.right;
    rect.bottom -= it->second.insets.bottom;
}

int GetWslgShadowMargin(HWND hwnd) {
    auto it = g_wslgInsets.find(hwnd);
    if (it == g_wslgInsets.end()) return 0;
    const RECT& in = it->second.insets;
    // Only a uniform margin is trusted as "the" margin.
    return (in.left == in.top && in.left == in.right && in.left == in.bottom) ? in.left : 0;
}

void ForgetVisibleWindowRect(HWND hwnd) {
    g_wslgInsets.erase(hwnd);
}

// GetWindowRect includes Windows' invisible resize hit-test margin, which
// is a few pixels larger than the window's actual visible edge -- using it
// would leave a visible gap between the window and the border. The DWM
// extended frame bounds give the true visible bounds instead.
bool GetVisibleWindowRect(HWND hwnd, RECT& out) {
    if (IsWslgWindow(hwnd)) {
        // WSLg windows have square corners and no Windows-side resize margin,
        // so the window rect is exact apart from Linux VS Code's own shadow.
        if (!GetWindowRect(hwnd, &out)) return false;
        ApplyWslgShadowInsets(hwnd, out);
        return true;
    }
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &out, sizeof(RECT)))) {
        // Even the extended frame bounds still leave a 1-2px gap on Windows
        // 11: its rounded window corners and drop-shadow anti-aliasing eat
        // into the reported edge, so the window's actually-opaque pixels
        // start a couple px further in than DWMWA_EXTENDED_FRAME_BOUNDS
        // claims. Pull the rect in slightly so the border overlaps that
        // sliver instead of leaving a visible seam.
        const int kEdgeOverlap = 2;
        out.left += kEdgeOverlap;
        out.top += kEdgeOverlap;
        out.right -= kEdgeOverlap;
        out.bottom -= kEdgeOverlap;
        return true;
    }
    return GetWindowRect(hwnd, &out) != 0;
}
