#include "diagnostic_snapshot.h"

#include "config.h"
#include "file_util.h"
#include "focus_trace.h"
#include "logger.h"
#include "overlay.h"
#include "tracking.h"
#include "window_discovery.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <set>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// DPI APIs, resolved at runtime: this toolchain's headers target Windows 7
// (_WIN32_WINNT 0x0601), which predates all of them, and the app itself
// declares no DPI awareness -- so every rect it works with may be
// virtualized on a scaled monitor. That makes "what the app sees" vs.
// "what's physically on screen" the single most useful thing a snapshot
// can record for a border drawn in the wrong place, which needs these.
// ---------------------------------------------------------------------------

typedef HANDLE DpiContext;
const DpiContext kPerMonitorAwareV2 = (DpiContext)(LONG_PTR)-4; // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

struct DpiApi {
    DpiContext(WINAPI* setThreadContext)(DpiContext) = nullptr;
    DpiContext(WINAPI* getThreadContext)() = nullptr;
    DpiContext(WINAPI* getWindowContext)(HWND) = nullptr;
    int(WINAPI* getAwareness)(DpiContext) = nullptr; // DPI_AWARENESS: -1 invalid, 0 unaware, 1 system, 2 per-monitor
    UINT(WINAPI* getDpiForWindow)(HWND) = nullptr;
    UINT(WINAPI* getDpiForSystem)() = nullptr;
    HRESULT(WINAPI* getDpiForMonitor)(HMONITOR, int, UINT*, UINT*) = nullptr; // shcore
    HRESULT(WINAPI* getProcessAwareness)(HANDLE, int*) = nullptr;            // shcore
};

const DpiApi& Dpi() {
    static DpiApi api;
    static bool loaded = false;
    if (loaded) return api;
    loaded = true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        api.setThreadContext = (decltype(api.setThreadContext))GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        api.getThreadContext = (decltype(api.getThreadContext))GetProcAddress(user32, "GetThreadDpiAwarenessContext");
        api.getWindowContext = (decltype(api.getWindowContext))GetProcAddress(user32, "GetWindowDpiAwarenessContext");
        api.getAwareness = (decltype(api.getAwareness))GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");
        api.getDpiForWindow = (decltype(api.getDpiForWindow))GetProcAddress(user32, "GetDpiForWindow");
        api.getDpiForSystem = (decltype(api.getDpiForSystem))GetProcAddress(user32, "GetDpiForSystem");
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll"); // kept loaded for the process lifetime
    if (shcore) {
        api.getDpiForMonitor = (decltype(api.getDpiForMonitor))GetProcAddress(shcore, "GetDpiForMonitor");
        api.getProcessAwareness = (decltype(api.getProcessAwareness))GetProcAddress(shcore, "GetProcessDpiAwareness");
    }
    return api;
}

const wchar_t* AwarenessName(int awareness) {
    switch (awareness) {
        case 0: return L"unaware";
        case 1: return L"system-aware";
        case 2: return L"per-monitor-aware";
        default: return L"unknown";
    }
}

const wchar_t* WindowAwareness(HWND hwnd) {
    const DpiApi& api = Dpi();
    if (!api.getWindowContext || !api.getAwareness) return L"n/a";
    return AwarenessName(api.getAwareness(api.getWindowContext(hwnd)));
}

UINT WindowDpi(HWND hwnd) { return Dpi().getDpiForWindow ? Dpi().getDpiForWindow(hwnd) : 0; }

// While alive, this thread sees physical pixels (per-monitor-v2), the way
// the screen is actually laid out. Outside one, it sees whatever the app
// normally sees. A no-op (physical == app view) on Windows versions without
// per-thread DPI contexts -- the report says so up front.
class PhysicalPixelsScope {
public:
    PhysicalPixelsScope() {
        if (Dpi().setThreadContext) prev_ = Dpi().setThreadContext(kPerMonitorAwareV2);
    }
    ~PhysicalPixelsScope() {
        if (prev_) Dpi().setThreadContext(prev_);
    }

private:
    DpiContext prev_ = nullptr;
};

// ---------------------------------------------------------------------------
// Report text
// ---------------------------------------------------------------------------

class Report {
public:
    void Line(const wchar_t* fmt, ...) {
        wchar_t buf[4096];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, 4096, _TRUNCATE, fmt, args);
        va_end(args);
        text_ += buf;
        text_ += L"\r\n";
    }
    void Section(const wchar_t* name) {
        text_ += L"\r\n==================================================================\r\n";
        Line(L"%ls", name);
        text_ += L"==================================================================\r\n";
    }
    const std::wstring& Text() const { return text_; }

private:
    std::wstring text_;
};

std::wstring RectStr(const RECT& r) {
    wchar_t buf[96];
    _snwprintf_s(buf, 96, _TRUNCATE, L"(%ld,%ld)-(%ld,%ld) %ldx%ld", r.left, r.top, r.right, r.bottom,
                 r.right - r.left, r.bottom - r.top);
    return buf;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring ClassOf(HWND hwnd) {
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    return cls;
}

std::wstring TitleOf(HWND hwnd, int maxChars = 200) {
    std::vector<wchar_t> buf(maxChars + 1, L'\0');
    GetWindowTextW(hwnd, buf.data(), maxChars + 1);
    return buf.data();
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked;
}

bool DwmFrameBounds(HWND hwnd, RECT& out) {
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &out, sizeof(out)));
}

std::wstring MonitorDeviceOf(HMONITOR mon) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return L"?";
    return mi.szDevice;
}

// ---------------------------------------------------------------------------
// Screenshot
// ---------------------------------------------------------------------------

// The whole virtual desktop, captured in physical pixels (taken inside a
// PhysicalPixelsScope). CAPTUREBLT is what makes layered windows -- i.e.
// every border overlay -- show up in the capture at all.
struct ScreenCapture {
    RECT virtualRect = {};
    int width = 0;
    int height = 0;
    HBITMAP bitmap = nullptr;
    UINT32* pixels = nullptr; // top-down BGRA, width*height

    ~ScreenCapture() {
        if (bitmap) DeleteObject(bitmap);
    }

    bool Sample(int screenX, int screenY, COLORREF& out) const {
        int x = screenX - virtualRect.left, y = screenY - virtualRect.top;
        if (!pixels || x < 0 || y < 0 || x >= width || y >= height) return false;
        UINT32 p = pixels[y * width + x];
        out = RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
        return true;
    }
};

BOOL CALLBACK UnionMonitorProc(HMONITOR, HDC, LPRECT rc, LPARAM param) {
    RECT* acc = (RECT*)param;
    if (IsRectEmpty(acc))
        *acc = *rc;
    else
        UnionRect(acc, acc, rc);
    return TRUE;
}

bool CaptureScreen(ScreenCapture& cap) {
    RECT virt = {};
    EnumDisplayMonitors(nullptr, nullptr, UnionMonitorProc, (LPARAM)&virt);
    if (IsRectEmpty(&virt)) return false;
    cap.virtualRect = virt;
    cap.width = virt.right - virt.left;
    cap.height = virt.bottom - virt.top;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cap.width;
    bmi.bmiHeader.biHeight = -cap.height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    cap.bitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (cap.bitmap) {
        HBITMAP old = (HBITMAP)SelectObject(memDC, cap.bitmap);
        ok = BitBlt(memDC, 0, 0, cap.width, cap.height, screenDC, virt.left, virt.top, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(memDC, old);
        GdiFlush();
        cap.pixels = (UINT32*)bits;
    }
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return ok;
}

bool SavePng(HBITMAP bitmap, const std::wstring& path) {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        // Image/png encoder -- fixed, documented GDI+ CLSID.
        const CLSID pngEncoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        ok = image.Save(path.c_str(), &pngEncoder, nullptr) == Gdiplus::Ok;
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

void WriteHeader(Report& rep, const std::wstring& snapshotDir, bool screenshotSaved) {
    SYSTEMTIME local, utc;
    GetLocalTime(&local);
    GetSystemTime(&utc);
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    RTL_OSVERSIONINFOW os = {};
    os.dwOSVersionInfoSize = sizeof(os);
    typedef LONG(WINAPI * RtlGetVersionFn)(RTL_OSVERSIONINFOW*);
    RtlGetVersionFn rtlGetVersion = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtlGetVersion) rtlGetVersion(&os);

    rep.Line(L"vscode-border diagnostic snapshot");
    rep.Line(L"captured (local): %04d-%02d-%02d %02d:%02d:%02d.%03d", local.wYear, local.wMonth, local.wDay, local.wHour,
             local.wMinute, local.wSecond, local.wMilliseconds);
    rep.Line(L"captured (UTC):   %04d-%02d-%02d %02d:%02d:%02d", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
             utc.wMinute, utc.wSecond);
    rep.Line(L"exe: %ls (pid %lu)", exePath, GetCurrentProcessId());
    rep.Line(L"windows: %lu.%lu build %lu", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    rep.Line(L"uptime: %llu s", (unsigned long long)(GetTickCount64() / 1000));
    rep.Line(L"snapshot folder: %ls", snapshotDir.c_str());
    rep.Line(L"screenshot: %ls", screenshotSaved ? L"screen.png (virtual desktop, physical pixels)" : L"FAILED");
    rep.Line(L"borders/hub hidden via tray toggle: %ls%ls", AreOverlaysHidden() ? L"YES" : L"no",
             AreOverlaysHidden() ? L"  <-- borders are not on screen, so the screenshot and pixel samples "
                                   L"can't show them; geometry below is still what the app would draw"
                                 : L"");
    rep.Line(L"tracked VS Code windows: %zu", TrackedWindowCount());
    rep.Line(L"warnings logged this run: %ls", HasWarnings() ? L"yes" : L"no");
    rep.Line(L"session log: %ls", GetLogFilePath().c_str());
    rep.Line(L"");
    rep.Line(L"Rects are listed twice where it matters: [app] = as this process sees them (it declares no DPI");
    rep.Line(L"awareness, so on a scaled monitor these may be DPI-virtualized), [phys] = physical pixels, the");
    rep.Line(L"coordinate space of screen.png. DWM frame bounds are physical in both views by design.");
}

void WriteConfigSummary(Report& rep) {
    rep.Section(L"CONFIG (key values -- full file copied as config.ini)");
    rep.Line(L"config path: %ls", GetConfigPath().c_str());
    rep.Line(L"thickness=%d opacity=%d rescan_interval_ms=%d", g_config.thickness, g_config.opacity,
             g_config.rescanIntervalMs);
    rep.Line(L"show_label=%d label_height=%d label_font_size=%d", g_config.showLabel, g_config.labelHeight,
             g_config.labelFontSize);
    rep.Line(L"show_project_list=%d fixed_match_taskbar=%d", g_config.showProjectList,
             g_config.projectListFixedMatchTaskbar);
    rep.Line(L"verbose_logging=%d palette entries=%zu", g_config.verboseLogging, g_config.palette.size());
}

void WriteDpiInfo(Report& rep) {
    const DpiApi& api = Dpi();
    rep.Section(L"DPI");
    int processAwareness = -1;
    if (api.getProcessAwareness && SUCCEEDED(api.getProcessAwareness(nullptr, &processAwareness)))
        rep.Line(L"process DPI awareness: %ls (%d)", AwarenessName(processAwareness), processAwareness);
    else
        rep.Line(L"process DPI awareness: n/a");
    if (api.getThreadContext && api.getAwareness)
        rep.Line(L"thread DPI awareness (app view): %ls", AwarenessName(api.getAwareness(api.getThreadContext())));
    rep.Line(L"system DPI: %u", api.getDpiForSystem ? api.getDpiForSystem() : 0);
    HDC dc = GetDC(nullptr);
    rep.Line(L"screen DC LOGPIXELSX=%d HORZRES=%d VERTRES=%d DESKTOPHORZRES=%d DESKTOPVERTRES=%d",
             GetDeviceCaps(dc, LOGPIXELSX), GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES),
             GetDeviceCaps(dc, 118 /* DESKTOPHORZRES */), GetDeviceCaps(dc, 117 /* DESKTOPVERTRES */));
    ReleaseDC(nullptr, dc);
    rep.Line(L"physical-pixel view available: %ls",
             api.setThreadContext ? L"yes" : L"NO (pre-1607 Windows: [phys] rows below equal [app])");
}

struct MonitorEntry {
    HMONITOR handle;
    MONITORINFOEXW info;
};

BOOL CALLBACK CollectMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    MonitorEntry e = {};
    e.handle = mon;
    e.info.cbSize = sizeof(e.info);
    GetMonitorInfoW(mon, &e.info);
    ((std::vector<MonitorEntry>*)param)->push_back(e);
    return TRUE;
}

void WriteMonitorList(Report& rep, const wchar_t* view) {
    std::vector<MonitorEntry> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc, (LPARAM)&monitors);
    rep.Line(L"[%ls] %zu monitor(s); virtual screen (%d,%d) %dx%d", view, monitors.size(),
             GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
             GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    for (const MonitorEntry& m : monitors) {
        UINT effX = 0, effY = 0, rawX = 0, rawY = 0;
        if (Dpi().getDpiForMonitor) {
            Dpi().getDpiForMonitor(m.handle, 0 /* MDT_EFFECTIVE_DPI */, &effX, &effY);
            Dpi().getDpiForMonitor(m.handle, 2 /* MDT_RAW_DPI */, &rawX, &rawY);
        }
        rep.Line(L"[%ls]   hmon=%p %ls%ls rect=%ls work=%ls effectiveDpi=%u (%u%%) rawDpi=%u", view, m.handle,
                 m.info.szDevice, (m.info.dwFlags & MONITORINFOF_PRIMARY) ? L" PRIMARY" : L"",
                 RectStr(m.info.rcMonitor).c_str(), RectStr(m.info.rcWork).c_str(), effX, effX * 100 / 96, rawX);
    }
}

void WriteMonitors(Report& rep) {
    rep.Section(L"MONITORS");
    WriteMonitorList(rep, L"app");
    {
        PhysicalPixelsScope phys;
        WriteMonitorList(rep, L"phys");
    }

    rep.Line(L"");
    rep.Line(L"display devices (adapter -> monitors) and current modes:");
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD ai = 0; EnumDisplayDevicesW(nullptr, ai, &adapter, 0); ai++) {
        bool active = (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;
        rep.Line(L"  adapter %ls [%ls] flags=0x%lx%ls%ls", adapter.DeviceName, adapter.DeviceString,
                 adapter.StateFlags, active ? L" ACTIVE" : L"",
                 (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? L" PRIMARY" : L"");
        if (active) {
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
                rep.Line(L"    mode: pos=(%ld,%ld) %lux%lu @%luHz %lubpp orientation=%lu fixedOutput=%lu",
                         dm.dmPosition.x, dm.dmPosition.y, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency,
                         dm.dmBitsPerPel, dm.dmDisplayOrientation, dm.dmDisplayFixedOutput);
            }
            DISPLAY_DEVICEW mon = {};
            mon.cb = sizeof(mon);
            for (DWORD mi = 0; EnumDisplayDevicesW(adapter.DeviceName, mi, &mon, 0); mi++) {
                rep.Line(L"    monitor %ls [%ls] id=%ls flags=0x%lx%ls", mon.DeviceName, mon.DeviceString,
                         mon.DeviceID, mon.StateFlags, (mon.StateFlags & DISPLAY_DEVICE_ACTIVE) ? L" ACTIVE" : L"");
                mon.cb = sizeof(mon);
            }
        }
        adapter.cb = sizeof(adapter);
    }

    rep.Line(L"");
    rep.Line(L"display config (active paths):");
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
        rep.Line(L"  GetDisplayConfigBufferSizes failed");
        return;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        rep.Line(L"  QueryDisplayConfig failed");
        return;
    }
    for (UINT32 i = 0; i < pathCount; i++) {
        const DISPLAYCONFIG_PATH_INFO& p = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        DisplayConfigGetDeviceInfo(&src.header);
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = p.targetInfo.adapterId;
        tgt.header.id = p.targetInfo.id;
        DisplayConfigGetDeviceInfo(&tgt.header);
        double hz = p.targetInfo.refreshRate.Denominator
                        ? (double)p.targetInfo.refreshRate.Numerator / p.targetInfo.refreshRate.Denominator
                        : 0.0;
        rep.Line(L"  path %u: source=%ls target=[%ls] tech=%d rotation=%d scaling=%d refresh=%.2fHz", i,
                 src.viewGdiDeviceName, tgt.monitorFriendlyDeviceName, (int)p.targetInfo.outputTechnology,
                 (int)p.targetInfo.rotation, (int)p.targetInfo.scaling, hz);
        UINT32 srcIdx = p.sourceInfo.modeInfoIdx;
        if (srcIdx < modeCount && modes[srcIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const DISPLAYCONFIG_SOURCE_MODE& sm = modes[srcIdx].sourceMode;
            rep.Line(L"           source mode %ux%u at (%ld,%ld)", sm.width, sm.height, sm.position.x, sm.position.y);
        }
        UINT32 tgtIdx = p.targetInfo.modeInfoIdx;
        if (tgtIdx < modeCount && modes[tgtIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& vs = modes[tgtIdx].targetMode.targetVideoSignalInfo;
            rep.Line(L"           target signal active=%ux%u total=%ux%u", vs.activeSize.cx, vs.activeSize.cy,
                     vs.totalSize.cx, vs.totalSize.cy);
        }
        rep.Line(L"           monitor path=%ls", tgt.monitorDevicePath);
    }
}

BOOL CALLBACK CollectTaskbarsProc(HWND hwnd, LPARAM param) {
    std::wstring cls = ClassOf(hwnd);
    if (cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd") ((std::vector<HWND>*)param)->push_back(hwnd);
    return TRUE;
}

void WriteTaskbars(Report& rep) {
    rep.Section(L"TASKBARS");
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    UINT_PTR state = SHAppBarMessage(ABM_GETSTATE, &abd);
    rep.Line(L"ABM_GETSTATE=0x%Ix (autohide=%d alwaysOnTop=%d)", state, (state & ABS_AUTOHIDE) ? 1 : 0,
             (state & ABS_ALWAYSONTOP) ? 1 : 0);
    abd = {};
    abd.cbSize = sizeof(abd);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd))
        rep.Line(L"ABM_GETTASKBARPOS edge=%u rc=%ls", abd.uEdge, RectStr(abd.rc).c_str());

    std::vector<HWND> taskbars;
    EnumWindows(CollectTaskbarsProc, (LPARAM)&taskbars);
    for (HWND tb : taskbars) {
        RECT appRc = {}, physRc = {};
        GetWindowRect(tb, &appRc);
        {
            PhysicalPixelsScope phys;
            GetWindowRect(tb, &physRc);
        }
        rep.Line(L"  %p %ls visible=%d monitor=%ls [app]%ls [phys]%ls", tb, ClassOf(tb).c_str(),
                 IsWindowVisible(tb) ? 1 : 0, MonitorDeviceOf(MonitorFromWindow(tb, MONITOR_DEFAULTTONULL)).c_str(),
                 RectStr(appRc).c_str(), RectStr(physRc).c_str());
    }
}

// A top-level window's on-screen footprint in physical pixels, in z-order
// (topmost first) -- for telling "the border isn't there" apart from "the
// border is there, but another window is in front of it".
struct ZWindow {
    HWND hwnd;
    RECT physFrame; // DWM frame bounds: GetWindowRect would include the invisible resize margin
    bool paints;    // visible, not minimized, not cloaked, and not a click-through layered overlay
};

std::vector<ZWindow> PhysicalZOrder(const std::vector<HWND>& zOrder) {
    std::vector<ZWindow> result;
    PhysicalPixelsScope phys;
    for (HWND hwnd : zOrder) {
        ZWindow z = {hwnd, {}, false};
        if (!DwmFrameBounds(hwnd, z.physFrame)) GetWindowRect(hwnd, &z.physFrame);
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        bool clickThroughLayered = (ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT);
        z.paints = IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsCloaked(hwnd) && !clickThroughLayered;
        result.push_back(z);
    }
    return result;
}

// First window above `overlay` that covers `pt`, or null if nothing does.
HWND CoveringWindow(const std::vector<ZWindow>& zOrder, HWND overlay, POINT pt) {
    for (const ZWindow& z : zOrder) {
        if (z.hwnd == overlay) return nullptr;
        if (z.paints && PtInRect(&z.physFrame, pt)) return z.hwnd;
    }
    return nullptr;
}

// One pixel from the middle of each side of the overlay's border band,
// read out of the screenshot, with the border's color expected there. The
// overlay is DPI-unaware, so on a scaled monitor DWM stretches its
// `thickness`-px band by the monitor's scale -- the sample is taken at the
// middle of the band as stretched.
void WritePixelSamples(Report& rep, const ScreenCapture& cap, const std::vector<ZWindow>& zOrder, HWND overlay,
                       const RECT& physOverlay, int bandPx, COLORREF expected) {
    int in = bandPx / 2;
    int midX = (physOverlay.left + physOverlay.right) / 2;
    int midY = (physOverlay.top + physOverlay.bottom) / 2;
    struct Side {
        const wchar_t* name;
        int x, y;
    } sides[] = {
        {L"top", midX, physOverlay.top + in},
        {L"bottom", midX, physOverlay.bottom - 1 - in},
        {L"left", physOverlay.left + in, midY},
        {L"right", physOverlay.right - 1 - in, midY},
    };
    bool exact = g_config.opacity >= 255;
    for (const Side& s : sides) {
        COLORREF got = 0;
        if (!cap.Sample(s.x, s.y, got)) {
            rep.Line(L"      pixel %-6ls at (%d,%d): off-screen / not captured", s.name, s.x, s.y);
            continue;
        }
        int diff = std::max({std::abs((int)GetRValue(got) - (int)GetRValue(expected)),
                             std::abs((int)GetGValue(got) - (int)GetGValue(expected)),
                             std::abs((int)GetBValue(got) - (int)GetBValue(expected))});
        HWND cover = CoveringWindow(zOrder, overlay, {s.x, s.y});
        std::wstring verdict;
        if (cover) {
            std::wstring exe;
            GetProcessImageName(cover, exe);
            wchar_t buf[200];
            _snwprintf_s(buf, 200, _TRUNCATE, L"covered by %p %ls [%ls] (not a fault)", cover, exe.c_str(),
                         ClassOf(cover).c_str());
            verdict = buf;
        } else if (!exact) {
            verdict = L"(opacity<255, blended -- compare by eye)";
        } else {
            verdict = diff <= 8 ? L"MATCH" : L"MISMATCH";
        }
        rep.Line(L"      pixel %-6ls at (%d,%d): got #%02X%02X%02X expected #%02X%02X%02X %ls", s.name, s.x, s.y,
                 GetRValue(got), GetGValue(got), GetBValue(got), GetRValue(expected), GetGValue(expected),
                 GetBValue(expected), verdict.c_str());
    }
}

void WriteTrackedWindows(Report& rep, const std::vector<TrackedWindowDiag>& tracked, const ScreenCapture* cap,
                         const std::vector<ZWindow>& zOrder) {
    rep.Section(L"TRACKED VS CODE WINDOWS (target + its border overlay)");
    HWND fg = GetForegroundWindow();
    int t = g_config.thickness;
    for (const TrackedWindowDiag& d : tracked) {
        HWND w = d.target;
        rep.Line(L"");
        rep.Line(L"target %p pid=%lu%ls", w, d.pid, w == fg ? L"  <-- FOREGROUND" : L"");
        rep.Line(L"  title=[%ls]", TitleOf(w, 400).c_str());
        rep.Line(L"  label=[%ls] rawLabel=[%ls] borderLabel=[%ls] branch=[%ls] folderName=[%ls] repoName=[%ls] "
                 L"loading=%d",
                 d.label.c_str(), d.rawLabel.c_str(), d.borderLabel.c_str(), d.branch.c_str(), d.folderName.c_str(),
                 d.repoName.c_str(), d.loading ? 1 : 0);
        rep.Line(L"  colorIndex=%d color=#%02X%02X%02X lastPainted=%dx%d lastKnownRect=%ls", d.colorIndex,
                 GetRValue(d.color), GetGValue(d.color), GetBValue(d.color), d.lastPaintedWidth, d.lastPaintedHeight,
                 RectStr(d.lastKnownRect).c_str());
        if (!IsWindow(w)) {
            rep.Line(L"  TARGET IS DEAD (IsWindow=0)");
            continue;
        }
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        GetWindowPlacement(w, &wp);
        rep.Line(L"  state: visible=%d iconic=%d zoomed=%d cloaked=%d showCmd=%u normalPos=%ls",
                 IsWindowVisible(w) ? 1 : 0, IsIconic(w) ? 1 : 0, IsZoomed(w) ? 1 : 0, IsCloaked(w) ? 1 : 0,
                 wp.showCmd, RectStr(wp.rcNormalPosition).c_str());
        HMONITOR overlayMonitor = d.overlay ? MonitorFromWindow(d.overlay, MONITOR_DEFAULTTONEAREST) : nullptr;
        rep.Line(L"  target dpi=%u awareness=%ls monitor=%ls | overlay dpi=%u awareness=%ls monitor=%ls", WindowDpi(w),
                 WindowAwareness(w), MonitorDeviceOf(MonitorFromWindow(w, MONITOR_DEFAULTTONULL)).c_str(),
                 d.overlay ? WindowDpi(d.overlay) : 0, d.overlay ? WindowAwareness(d.overlay) : L"-",
                 MonitorDeviceOf(overlayMonitor).c_str());

        // [app] view: exactly the calls SyncOverlay makes.
        RECT appWin = {}, appVisible = {}, appOverlay = {}, appClient = {};
        GetWindowRect(w, &appWin);
        bool haveVisible = GetVisibleWindowRect(w, appVisible);
        GetClientRect(w, &appClient);
        MapWindowPoints(w, nullptr, (POINT*)&appClient, 2);
        if (d.overlay) GetWindowRect(d.overlay, &appOverlay);
        RECT expected = appVisible;
        InflateRect(&expected, t + d.resizeMargin, t + d.resizeMargin);

        // [phys] view: where things really are on screen / in screen.png.
        RECT physWin = {}, physOverlay = {}, physFrame = {};
        {
            PhysicalPixelsScope phys;
            GetWindowRect(w, &physWin);
            if (d.overlay) GetWindowRect(d.overlay, &physOverlay);
            if (!DwmFrameBounds(w, physFrame)) physFrame = physWin;
        }

        rep.Line(L"  [app]  window=%ls client=%ls", RectStr(appWin).c_str(), RectStr(appClient).c_str());
        rep.Line(L"  [app]  visibleRect(used for border)=%ls%ls", RectStr(appVisible).c_str(),
                 haveVisible ? L"" : L" (GetVisibleWindowRect FAILED)");
        rep.Line(L"  [phys] window=%ls dwmFrame=%ls", RectStr(physWin).c_str(), RectStr(physFrame).c_str());
        rep.Line(L"  [app]  overlay=%ls visible=%d", RectStr(appOverlay).c_str(),
                 d.overlay && IsWindowVisible(d.overlay) ? 1 : 0);
        rep.Line(L"  [phys] overlay=%ls", RectStr(physOverlay).c_str());

        // SyncOverlay deliberately hides the overlay for these -- nothing
        // on screen to compare against, and their rects are parked
        // off-screen (minimized windows sit at -32000).
        if (AreOverlaysHidden() || IsIconic(w) || !IsWindowVisible(w)) {
            rep.Line(L"  (overlay hidden by design: %ls -- comparisons skipped)",
                     AreOverlaysHidden() ? L"borders hidden via tray" : IsIconic(w) ? L"minimized" : L"not visible");
            continue;
        }

        UINT monitorDpi = 96, unused = 0;
        if (overlayMonitor && Dpi().getDpiForMonitor)
            Dpi().getDpiForMonitor(overlayMonitor, 0 /* MDT_EFFECTIVE_DPI */, &monitorDpi, &unused);
        // GetVisibleWindowRect pulls the DWM frame in by 2px before the
        // border is inflated around it, so only thickness-2 sits outside.
        int expectedGap = MulDiv(t - 2 + d.resizeMargin, monitorDpi, 96);
        rep.Line(L"  [app]  overlay vs expected (visibleRect inflated by thickness=%d + resize margin=%d) %ls -> %ls",
                 t, d.resizeMargin,
                 RectStr(expected).c_str(), EqualRect(&expected, &appOverlay) ? L"matches" : L"DIFFERS");
        rep.Line(L"  [phys] overlay edge outside dwmFrame per side: left=%ld top=%ld right=%ld bottom=%ld "
                 L"(expect %d = (thickness-2+resize margin) x %u%% scale)",
                 physFrame.left - physOverlay.left, physFrame.top - physOverlay.top,
                 physOverlay.right - physFrame.right, physOverlay.bottom - physFrame.bottom, expectedGap,
                 monitorDpi * 100 / 96);

        HWND above = GetWindow(w, GW_HWNDPREV);
        if (above == d.overlay) {
            rep.Line(L"  z-order: overlay sits directly above target (correct)");
        } else {
            wchar_t aboveDesc[400] = {};
            DescribeWindowForLog(above, aboveDesc, 400);
            rep.Line(L"  z-order: window directly above target is NOT its overlay: %ls", aboveDesc);
        }
        if (d.overlay) {
            LONG_PTR ex = GetWindowLongPtrW(d.overlay, GWL_EXSTYLE);
            rep.Line(L"  overlay %p exstyle=0x%Ix topmost=%d", d.overlay, (ULONG_PTR)ex, (ex & WS_EX_TOPMOST) ? 1 : 0);
        }

        if (cap && d.overlay)
            WritePixelSamples(rep, *cap, zOrder, d.overlay, physOverlay, std::max(1, MulDiv(t, monitorDpi, 96)),
                              d.color);
    }
}

BOOL CALLBACK CollectAllTopLevelProc(HWND hwnd, LPARAM param) {
    ((std::vector<HWND>*)param)->push_back(hwnd);
    return TRUE;
}

void WriteOwnWindows(Report& rep, const std::vector<HWND>& zOrder, const std::vector<TrackedWindowDiag>& tracked) {
    rep.Section(L"THIS APP'S TOP-LEVEL WINDOWS");
    std::set<HWND> knownOverlays;
    for (const TrackedWindowDiag& d : tracked) knownOverlays.insert(d.overlay);
    DWORD self = GetCurrentProcessId();
    for (HWND hwnd : zOrder) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != self) continue;
        std::wstring cls = ClassOf(hwnd);
        const wchar_t* note = L"";
        if (cls == kOverlayClassName) {
            note = knownOverlays.count(hwnd) ? L" (overlay of a tracked window)"
                                             : L" (overlay NOT owned by any tracked window -- ORPHAN)";
        }
        RECT appRc = {}, physRc = {};
        GetWindowRect(hwnd, &appRc);
        {
            PhysicalPixelsScope phys;
            GetWindowRect(hwnd, &physRc);
        }
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        rep.Line(L"  %p %ls%ls visible=%d topmost=%d dpi=%u monitor=%ls [app]%ls [phys]%ls", hwnd, cls.c_str(), note,
                 IsWindowVisible(hwnd) ? 1 : 0, (ex & WS_EX_TOPMOST) ? 1 : 0, WindowDpi(hwnd),
                 MonitorDeviceOf(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL)).c_str(), RectStr(appRc).c_str(),
                 RectStr(physRc).c_str());
    }
}

void WriteUntrackedVSCodeWindows(Report& rep, const std::vector<HWND>& zOrder,
                                 const std::vector<TrackedWindowDiag>& tracked) {
    rep.Section(L"VISIBLE VS CODE WINDOWS NOT TRACKED");
    std::set<HWND> trackedSet;
    for (const TrackedWindowDiag& d : tracked) trackedSet.insert(d.target);
    int shown = 0;
    for (HWND hwnd : zOrder) {
        if (trackedSet.count(hwnd) || !IsWindowVisible(hwnd)) continue;
        std::wstring exe;
        if (!GetProcessImageName(hwnd, exe) || !IsVSCodeWindowProcess(hwnd, exe)) continue;
        RECT physRc = {};
        {
            PhysicalPixelsScope phys;
            GetWindowRect(hwnd, &physRc);
        }
        rep.Line(L"  %p %ls class=%ls candidate=%d cloaked=%d [phys]%ls title=[%ls]", hwnd, exe.c_str(),
                 ClassOf(hwnd).c_str(), IsCandidateTopLevelWindow(hwnd) ? 1 : 0, IsCloaked(hwnd) ? 1 : 0,
                 RectStr(physRc).c_str(), TitleOf(hwnd).c_str());
        shown++;
    }
    if (!shown) rep.Line(L"  (none)");
}

void WriteZOrder(Report& rep, const std::vector<HWND>& zOrder, const std::vector<TrackedWindowDiag>& tracked) {
    rep.Section(L"DESKTOP Z-ORDER (visible top-level windows, topmost first)");
    std::set<HWND> targets, overlays;
    for (const TrackedWindowDiag& d : tracked) {
        targets.insert(d.target);
        overlays.insert(d.overlay);
    }
    int index = 0;
    for (HWND hwnd : zOrder) {
        index++;
        if (!IsWindowVisible(hwnd)) continue;
        std::wstring exe;
        GetProcessImageName(hwnd, exe);
        RECT physRc = {};
        {
            PhysicalPixelsScope phys;
            GetWindowRect(hwnd, &physRc);
        }
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        const wchar_t* role = targets.count(hwnd) ? L" [TRACKED]" : overlays.count(hwnd) ? L" [OVERLAY]" : L"";
        rep.Line(L"  #%-4d %p%ls %ls class=%ls topmost=%d cloaked=%d [phys]%ls title=[%ls]", index, hwnd, role,
                 exe.c_str(), ClassOf(hwnd).c_str(), (ex & WS_EX_TOPMOST) ? 1 : 0, IsCloaked(hwnd) ? 1 : 0,
                 RectStr(physRc).c_str(), TitleOf(hwnd, 80).c_str());
    }
}

// ---------------------------------------------------------------------------
// File copies
// ---------------------------------------------------------------------------

// The log is held open for writing by this same process (logger.cpp), so
// it has to be opened with write sharing -- CopyFileW / ReadFileBytes
// would hit a sharing violation.
std::string ReadSharedFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::string buf;
    char chunk[65536];
    DWORD read = 0;
    while (ReadFile(h, chunk, sizeof(chunk), &read, nullptr) && read > 0) buf.append(chunk, read);
    CloseHandle(h);
    return buf;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DirOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

// Every part of the current session's log (vscode_border_<session>_NN.log
// -- see logger.cpp's naming contract), not just the part being written
// now, so a session that rotated still comes along whole.
int CopySessionLogs(const std::wstring& destDir) {
    std::wstring current = GetLogFilePath();
    std::wstring name = FileNameOf(current);
    size_t lastUnderscore = name.find_last_of(L'_');
    std::wstring pattern = DirOf(current) + L"\\" +
                           (lastUnderscore == std::wstring::npos ? name : name.substr(0, lastUnderscore) + L"_*.log");
    int copied = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        std::string content = ReadSharedFile(DirOf(current) + L"\\" + fd.cFileName);
        if (WriteFileBytes(destDir + L"\\" + fd.cFileName, content)) copied++;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return copied;
}

std::wstring CreateSnapshotDir() {
    std::wstring root = DirOf(GetLogFilePath()) + L"\\snapshots";
    CreateDirectoryW(root.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    _snwprintf_s(name, 64, _TRUNCATE, L"snapshot_%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
    std::wstring dir = root + L"\\" + name;
    for (int suffix = 2; !CreateDirectoryW(dir.c_str(), nullptr); suffix++) {
        if (GetLastError() != ERROR_ALREADY_EXISTS || suffix > 50) return std::wstring();
        dir = root + L"\\" + name + L"_" + std::to_wstring(suffix);
    }
    return dir;
}

} // namespace

std::wstring CaptureDiagnosticSnapshot() {
    std::wstring dir = CreateSnapshotDir();
    if (dir.empty()) {
        LogWarn(L"diagnostic snapshot: couldn't create a snapshot folder, lastError=%lu", GetLastError());
        return std::wstring();
    }

    // Screenshot first, so it's as close as possible to the moment the user
    // saw the problem -- everything after this only reads window state.
    ScreenCapture cap;
    bool captured = false, screenshotSaved = false;
    {
        PhysicalPixelsScope phys;
        captured = CaptureScreen(cap);
    }
    if (captured) screenshotSaved = SavePng(cap.bitmap, dir + L"\\screen.png");

    std::vector<TrackedWindowDiag> tracked = GetTrackedWindowDiagnostics();
    std::vector<HWND> zOrder;
    EnumWindows(CollectAllTopLevelProc, (LPARAM)&zOrder);

    Report rep;
    WriteHeader(rep, dir, screenshotSaved);
    WriteConfigSummary(rep);
    WriteDpiInfo(rep);
    WriteMonitors(rep);
    WriteTaskbars(rep);
    WriteTrackedWindows(rep, tracked, captured ? &cap : nullptr, PhysicalZOrder(zOrder));
    WriteOwnWindows(rep, zOrder, tracked);
    WriteUntrackedVSCodeWindows(rep, zOrder, tracked);
    WriteZOrder(rep, zOrder, tracked);
    bool reportSaved = WriteFileBytes(dir + L"\\report.txt", WideToUtf8(rep.Text()));

    WriteFileBytes(dir + L"\\config.ini", ReadFileBytes(GetConfigPath()));

    Log(L"diagnostic snapshot saved to %ls (report=%d screenshot=%d tracked=%zu)", dir.c_str(), reportSaved ? 1 : 0,
        screenshotSaved ? 1 : 0, tracked.size());
    FlushLogBuffer(); // pull in any buffered LogFast lines before copying
    CopySessionLogs(dir);
    return dir;
}
