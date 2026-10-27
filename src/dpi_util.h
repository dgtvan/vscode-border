#pragma once

#include <windows.h>

#include <string>

// This app declares no DPI awareness, so on a scaled monitor every rect it
// sees is DPI-virtualized -- except DWM frame bounds, which are always
// physical. These helpers read the physical view, where the border really
// lands on screen, so placement can be checked rather than assumed.

// While alive, this thread sees physical pixels (per-monitor-v2), the way
// the screen is actually laid out. Outside one, it sees whatever the app
// normally sees. A no-op (physical == app view) on Windows versions without
// per-thread DPI contexts -- see PhysicalPixelsAvailable.
class PhysicalPixelsScope {
public:
    PhysicalPixelsScope();
    ~PhysicalPixelsScope();
    PhysicalPixelsScope(const PhysicalPixelsScope&) = delete;
    PhysicalPixelsScope& operator=(const PhysicalPixelsScope&) = delete;

private:
    HANDLE prev_ = nullptr;
};

bool PhysicalPixelsAvailable();

// Effective DPI of a monitor (96 = 100%), or 96 when it can't be read.
UINT MonitorEffectiveDpi(HMONITOR mon);

// "\\.\DISPLAY2 150%", or "?" for a null/unknown monitor.
std::wstring DescribeMonitor(HMONITOR mon);

// One line per monitor, in both views, e.g.
// "\\.\DISPLAY2 150% app=(1920,0)-(3200,720) phys=(1920,0)-(3840,1080)".
// Two layouts compare equal exactly when their strings do.
std::wstring DescribeMonitorLayout();

// Where a border overlay really sits relative to its target's visible
// frame, in physical pixels. The overlay is meant to reach (thickness-2)
// px past the frame on every side (GetVisibleWindowRect pulls the frame in
// by 2 before the border is inflated around it), scaled by the monitor.
struct OverlayPlacement {
    RECT physWindow = {};  // target's GetWindowRect
    RECT physFrame = {};   // target's DWM frame bounds
    RECT physOverlay = {}; // overlay's GetWindowRect
    HMONITOR targetMonitor = nullptr;
    HMONITOR overlayMonitor = nullptr;
    UINT monitorDpi = 96;  // effective DPI of the overlay's monitor
    LONG gaps[4] = {};     // left, top, right, bottom: how far the overlay reaches past the frame
    int expectedGap = 0;
    int tolerance = 0;     // rounding through the virtualized view shifts edges by a px or two
    bool misplaced = false;
};
OverlayPlacement MeasureOverlayPlacement(HWND target, HWND overlay, int thickness);

// "left=1 top=1 right=1 bottom=1 (expect 1+-3 at 150%)".
std::wstring DescribeGaps(const OverlayPlacement& p);

std::wstring RectToString(const RECT& r);
