#include "window_discovery.h"

#include <dwmapi.h>
#include <psapi.h>

#include <algorithm>

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

bool IsCandidateTopLevelWindow(HWND hwnd) {
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

// GetWindowRect includes Windows' invisible resize hit-test margin, which
// is a few pixels larger than the window's actual visible edge -- using it
// would leave a visible gap between the window and the border. The DWM
// extended frame bounds give the true visible bounds instead.
bool GetVisibleWindowRect(HWND hwnd, RECT& out) {
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
