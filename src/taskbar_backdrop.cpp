#include "taskbar_backdrop.h"

#include "logger.h"

#include <shellapi.h>

#include <algorithm>
#include <vector>

static const wchar_t* kTaskbarBackdropClassName = L"VSCodeBorderTaskbarBackdropWndClass";
static const UINT_PTR kResampleTimerId = 1;    // periodic safety-net resample, while shown
static const UINT kResampleIntervalMs = 30000;
static const UINT_PTR kSettledTimerId = 2;     // one-shot, after a setting change or a hover-deferred
static const UINT kSettledDelayMs = 1500;      // sample -- long enough for the taskbar to finish
                                                // repainting in its new theme first
static const int kSampleInset = 2;  // rows between the taskbar's inner edge and the fill sample: the
static const int kSampleRows = 3;   // edge row itself is Windows 11's 1px border line, sampled
                                    // separately as the band's own top border. Icons, hover highlights
                                    // and running-app underlines never reach this far toward the edge.
static const int kSmoothRadius = 3; // horizontal box blur over the sampled columns, to even out any
                                    // stray pixel the per-column median let through

// One color per band column for the band's fill, and one for its 1px top
// border -- the taskbar's own edge row, so the band gets the same border
// line the taskbar has (or none, where the taskbar's edge row is just its
// fill color).
struct BackdropColors {
    std::vector<COLORREF> fill;
    std::vector<COLORREF> border;
    bool operator==(const BackdropColors& o) const { return fill == o.fill && border == o.border; }
};

struct TaskbarBackdropState {
    RECT band = {};
    BackdropColors colors; // as last painted
};

static int Luminance(COLORREF c) {
    return 299 * GetRValue(c) + 587 * GetGValue(c) + 114 * GetBValue(c);
}

// Used when there's no taskbar to sample (auto-hide). Measured on a dark
// Windows 11 taskbar; the light one is Windows 11's light surface color.
static COLORREF ThemeFallbackColor() {
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light ? RGB(0xF3, 0xF3, 0xF3) : RGB(0x1C, 0x1C, 0x1C);
}

// Copies a screen rect into a top-down 32bpp buffer (0x00RRGGBB per pixel).
static bool CaptureScreenRect(int x, int y, int width, int height, std::vector<UINT32>& out) {
    if (width <= 0 || height <= 0) return false;
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp) {
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        ok = BitBlt(memDC, 0, 0, width, height, screenDC, x, y, SRCCOPY) != 0;
        if (ok) out.assign((UINT32*)bits, (UINT32*)bits + (size_t)width * height);
        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
    }
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return ok;
}

static COLORREF PixelToColor(UINT32 p) {
    return RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
}

// Median by luminance -- picks one real sampled color rather than blending
// channels independently, so an outlier can't tint the result.
static COLORREF MedianColor(std::vector<COLORREF>& samples) {
    std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end(),
                     [](COLORREF a, COLORREF b) { return Luminance(a) < Luminance(b); });
    return samples[samples.size() / 2];
}

enum class SampleResult { Sampled, Deferred, Unavailable };

// Box blur across columns (clamped at the ends), evening out any stray
// pixel the sampling let through.
static void SmoothColumns(std::vector<COLORREF>& columns) {
    int width = (int)columns.size();
    std::vector<COLORREF> smoothed(width);
    for (int x = 0; x < width; x++) {
        int r = 0, g = 0, b = 0, n = 0;
        for (int k = std::max(0, x - kSmoothRadius); k <= std::min(width - 1, x + kSmoothRadius); k++) {
            r += GetRValue(columns[k]);
            g += GetGValue(columns[k]);
            b += GetBValue(columns[k]);
            n++;
        }
        smoothed[x] = RGB(r / n, g / n, b / n);
    }
    columns.swap(smoothed);
}

// Fills `out` with one fill and one border color per band column, sampled
// from the taskbar.
static SampleResult SampleTaskbar(const RECT& band, BackdropColors& out) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    APPBARDATA abd = {};
    abd.cbSize = sizeof(abd);
    RECT tb = {};
    if (!taskbar || !IsWindowVisible(taskbar) || (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) ||
        !GetWindowRect(taskbar, &tb)) {
        return SampleResult::Unavailable;
    }

    POINT cursor;
    GetCursorPos(&cursor);
    if (PtInRect(&tb, cursor)) return SampleResult::Deferred;

    int bandWidth = band.right - band.left;
    std::vector<COLORREF>& columns = out.fill;
    columns.assign(bandWidth, RGB(0, 0, 0));
    out.border.assign(bandWidth, RGB(0, 0, 0));
    std::vector<UINT32> pixels;
    const int stripDepth = kSampleInset + kSampleRows; // edge row, skipped rows, fill rows

    if (tb.right - tb.left >= tb.bottom - tb.top) {
        // Horizontal taskbar: rows just inside the edge facing the rest of
        // the screen (its top edge when it's at the bottom), over the
        // band's own x range, so each band column gets the taskbar column
        // directly below it.
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTOPRIMARY), &mi);
        bool atBottom = tb.top > (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
        // Strip rows are indexed from the edge inward, whichever way up the
        // taskbar is.
        int y = atBottom ? tb.top : tb.bottom - stripDepth;
        if (!CaptureScreenRect(band.left, y, bandWidth, stripDepth, pixels)) return SampleResult::Unavailable;
        auto rowFromEdge = [&](int i) { return atBottom ? i : stripDepth - 1 - i; };
        std::vector<COLORREF> samples(kSampleRows);
        for (int x = 0; x < bandWidth; x++) {
            for (int r = 0; r < kSampleRows; r++) {
                samples[r] = PixelToColor(pixels[(size_t)rowFromEdge(kSampleInset + r) * bandWidth + x]);
            }
            columns[x] = MedianColor(samples);
            out.border[x] = PixelToColor(pixels[(size_t)rowFromEdge(0) * bandWidth + x]);
        }
    } else {
        // Vertical taskbar (left/right, Windows 10): no column lines up with
        // the band, so one color for the whole band, from a strip just
        // inside the taskbar's inner edge.
        bool atLeft = tb.left <= band.left;
        int x = atLeft ? tb.right - stripDepth : tb.left;
        int height = tb.bottom - tb.top;
        if (!CaptureScreenRect(x, tb.top, stripDepth, height, pixels)) return SampleResult::Unavailable;
        auto colFromEdge = [&](int i) { return atLeft ? stripDepth - 1 - i : i; };
        std::vector<COLORREF> fill, edge;
        for (int row = 0; row < height; row++) {
            for (int c = 0; c < kSampleRows; c++) {
                fill.push_back(PixelToColor(pixels[(size_t)row * stripDepth + colFromEdge(kSampleInset + c)]));
            }
            edge.push_back(PixelToColor(pixels[(size_t)row * stripDepth + colFromEdge(0)]));
        }
        std::fill(columns.begin(), columns.end(), MedianColor(fill));
        std::fill(out.border.begin(), out.border.end(), MedianColor(edge));
        return SampleResult::Sampled;
    }

    SmoothColumns(columns);
    SmoothColumns(out.border);
    return SampleResult::Sampled;
}

static void Paint(HWND backdrop, TaskbarBackdropState* state) {
    int width = state->band.right - state->band.left;
    int height = state->band.bottom - state->band.top;
    const BackdropColors& colors = state->colors;
    if (width <= 0 || height <= 0 || (int)colors.fill.size() != width || (int)colors.border.size() != width) return;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        UINT32* pixels = (UINT32*)bits;
        auto opaque = [](COLORREF c) {
            return (UINT32(255) << 24) | (UINT32(GetRValue(c)) << 16) | (UINT32(GetGValue(c)) << 8) |
                   UINT32(GetBValue(c));
        };
        // Row 0 is the border; the rest is the fill, row 1 copied down.
        for (int x = 0; x < width; x++) {
            pixels[x] = opaque(colors.border[x]);
            if (height > 1) pixels[width + x] = opaque(colors.fill[x]);
        }
        for (int y = 2; y < height; y++) std::copy(pixels + width, pixels + 2 * width, pixels + (size_t)y * width);

        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
        POINT dst = {state->band.left, state->band.top};
        SIZE size = {width, height};
        POINT src = {0, 0};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(backdrop, screenDC, &dst, &size, memDC, &src, 0, &blend, ULW_ALPHA);
        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
    }
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// Resamples and repaints if anything changed. `force` repaints regardless
// (the band itself moved or resized).
static void Resample(HWND backdrop, TaskbarBackdropState* state, bool force) {
    BackdropColors colors;
    SampleResult result = SampleTaskbar(state->band, colors);
    if (result == SampleResult::Deferred) {
        // Cursor on the taskbar: try again shortly. Keep whatever's painted,
        // unless this is a forced repaint (first show, or the band moved)
        // -- then paint the theme fallback for now rather than nothing, and
        // let the retry correct it.
        SetTimer(backdrop, kSettledTimerId, kSettledDelayMs, nullptr);
        if (!force) return;
        result = SampleResult::Unavailable;
    }
    if (result == SampleResult::Unavailable) {
        // No border in the fallback: without a taskbar to copy it from,
        // there's no telling what it should look like.
        colors.fill.assign(std::max(0L, state->band.right - state->band.left), ThemeFallbackColor());
        colors.border = colors.fill;
    }
    if (!force && colors == state->colors) return;

    const std::vector<COLORREF>& fill = colors.fill;
    bool uniform = std::all_of(fill.begin(), fill.end(), [&](COLORREF c) { return c == fill.front(); });
    COLORREF first = fill.empty() ? 0 : fill.front();
    COLORREF border = colors.border.empty() ? 0 : colors.border.front();
    Log(L"backdrop: %ls color=%02X%02X%02X border=%02X%02X%02X%ls",
        result == SampleResult::Sampled ? L"sampled taskbar" : L"theme fallback", GetRValue(first),
        GetGValue(first), GetBValue(first), GetRValue(border), GetGValue(border), GetBValue(border),
        uniform ? L"" : L" (varies across the band)");
    state->colors = std::move(colors);
    Paint(backdrop, state);
}

static LRESULT CALLBACK TaskbarBackdropWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TaskbarBackdropState* state = (TaskbarBackdropState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_TIMER:
            if (state && IsWindowVisible(hwnd)) {
                if (wParam == kSettledTimerId) KillTimer(hwnd, kSettledTimerId);
                Resample(hwnd, state, false);
            }
            return 0;
        case WM_SETTINGCHANGE:
        case WM_DISPLAYCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            // Theme/accent/transparency/wallpaper changes all arrive here,
            // before the taskbar has repainted -- so wait for it to settle.
            if (state && IsWindowVisible(hwnd)) SetTimer(hwnd, kSettledTimerId, kSettledDelayMs, nullptr);
            break;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCDESTROY:
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateTaskbarBackdrop(HINSTANCE hInstance) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = TaskbarBackdropWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kTaskbarBackdropClassName;
        RegisterClassW(&wc);
        registered = true;
    }
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                kTaskbarBackdropClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                hInstance, nullptr);
    if (hwnd) SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)new TaskbarBackdropState());
    return hwnd;
}

void ShowTaskbarBackdrop(HWND backdrop, const RECT& band) {
    TaskbarBackdropState* state = backdrop ? (TaskbarBackdropState*)GetWindowLongPtrW(backdrop, GWLP_USERDATA) : nullptr;
    if (!state) return;
    state->band = band;
    Resample(backdrop, state, true);
    SetWindowPos(backdrop, nullptr, band.left, band.top, band.right - band.left, band.bottom - band.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    SetTimer(backdrop, kResampleTimerId, kResampleIntervalMs, nullptr);
}

void HideTaskbarBackdrop(HWND backdrop) {
    if (!backdrop || !IsWindowVisible(backdrop)) return;
    KillTimer(backdrop, kResampleTimerId);
    KillTimer(backdrop, kSettledTimerId);
    ShowWindow(backdrop, SW_HIDE);
}

