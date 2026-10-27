#include "dpi_util.h"

#include <dwmapi.h>

#include <cstdlib>
#include <cwchar>
#include <vector>

namespace {

typedef HANDLE DpiContext;
const DpiContext kPerMonitorAwareV2 = (DpiContext)(LONG_PTR)-4; // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

// Resolved at runtime: this toolchain's headers target Windows 7
// (_WIN32_WINNT 0x0601), which predates all of these.
struct Api {
    DpiContext(WINAPI* setThreadContext)(DpiContext) = nullptr;
    HRESULT(WINAPI* getDpiForMonitor)(HMONITOR, int, UINT*, UINT*) = nullptr; // shcore
};

const Api& GetApi() {
    static Api api;
    static bool loaded = false;
    if (loaded) return api;
    loaded = true;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        api.setThreadContext = (decltype(api.setThreadContext))GetProcAddress(user32, "SetThreadDpiAwarenessContext");
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) // kept loaded for the process lifetime
        api.getDpiForMonitor = (decltype(api.getDpiForMonitor))GetProcAddress(shcore, "GetDpiForMonitor");
    return api;
}

BOOL CALLBACK CollectMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    ((std::vector<HMONITOR>*)param)->push_back(mon);
    return TRUE;
}

RECT MonitorRect(HMONITOR mon) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    return mi.rcMonitor;
}

} // namespace

PhysicalPixelsScope::PhysicalPixelsScope() {
    if (GetApi().setThreadContext) prev_ = GetApi().setThreadContext(kPerMonitorAwareV2);
}

PhysicalPixelsScope::~PhysicalPixelsScope() {
    if (prev_) GetApi().setThreadContext(prev_);
}

bool PhysicalPixelsAvailable() { return GetApi().setThreadContext != nullptr; }

UINT MonitorEffectiveDpi(HMONITOR mon) {
    UINT x = 0, y = 0;
    if (!mon || !GetApi().getDpiForMonitor || FAILED(GetApi().getDpiForMonitor(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) ||
        x == 0)
        return 96;
    return x;
}

std::wstring DescribeMonitor(HMONITOR mon) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return L"?";
    wchar_t buf[80];
    _snwprintf_s(buf, 80, _TRUNCATE, L"%ls %u%%", mi.szDevice, MonitorEffectiveDpi(mon) * 100 / 96);
    return buf;
}

std::wstring DescribeMonitorLayout() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc, (LPARAM)&monitors);
    std::vector<RECT> physRects;
    {
        PhysicalPixelsScope phys;
        for (HMONITOR mon : monitors) physRects.push_back(MonitorRect(mon));
    }
    std::wstring out;
    for (size_t i = 0; i < monitors.size(); i++) {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitors[i], &mi);
        if (!out.empty()) out += L"\n";
        out += DescribeMonitor(monitors[i]) + ((mi.dwFlags & MONITORINFOF_PRIMARY) ? L" PRIMARY" : L"") +
               L" app=" + RectToString(mi.rcMonitor) + L" phys=" + RectToString(physRects[i]);
    }
    return out;
}

OverlayPlacement MeasureOverlayPlacement(HWND target, HWND overlay, int thickness) {
    OverlayPlacement p;
    {
        PhysicalPixelsScope phys;
        GetWindowRect(target, &p.physWindow);
        if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &p.physFrame, sizeof(RECT))))
            p.physFrame = p.physWindow;
        if (overlay) GetWindowRect(overlay, &p.physOverlay);
    }
    p.targetMonitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    p.overlayMonitor = overlay ? MonitorFromWindow(overlay, MONITOR_DEFAULTTONEAREST) : nullptr;
    p.monitorDpi = MonitorEffectiveDpi(p.overlayMonitor ? p.overlayMonitor : p.targetMonitor);
    p.gaps[0] = p.physFrame.left - p.physOverlay.left;
    p.gaps[1] = p.physFrame.top - p.physOverlay.top;
    p.gaps[2] = p.physOverlay.right - p.physFrame.right;
    p.gaps[3] = p.physOverlay.bottom - p.physFrame.bottom;
    p.expectedGap = MulDiv(thickness - 2, p.monitorDpi, 96);
    p.tolerance = 1 + MulDiv(2, p.monitorDpi, 96);
    for (LONG g : p.gaps)
        if (labs(g - p.expectedGap) > p.tolerance) p.misplaced = true;
    return p;
}

std::wstring DescribeGaps(const OverlayPlacement& p) {
    wchar_t buf[128];
    _snwprintf_s(buf, 128, _TRUNCATE, L"left=%ld top=%ld right=%ld bottom=%ld (expect %d+-%d at %u%%)", p.gaps[0],
                 p.gaps[1], p.gaps[2], p.gaps[3], p.expectedGap, p.tolerance, p.monitorDpi * 100 / 96);
    return buf;
}

std::wstring RectToString(const RECT& r) {
    wchar_t buf[96];
    _snwprintf_s(buf, 96, _TRUNCATE, L"(%ld,%ld)-(%ld,%ld) %ldx%ld", r.left, r.top, r.right, r.bottom,
                 r.right - r.left, r.bottom - r.top);
    return buf;
}
