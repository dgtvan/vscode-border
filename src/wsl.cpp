#include "wsl.h"
#include "logger.h"
#include "resource.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

const wchar_t* const kWslgWindowClass = L"RAIL_WINDOW";

bool IsWslgHostExe(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return lower == L"msrdc.exe";
}

static bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    size_t len = wcslen(suffix);
    return s.size() >= len && s.compare(s.size() - len, len, suffix) == 0;
}

bool SplitWslgVSCodeTitle(const std::wstring& title, std::wstring* vscodeTitle, std::wstring* distro) {
    if (title.empty() || title.back() != L')') return false;
    size_t open = title.rfind(L" (");
    if (open == std::wstring::npos || open + 3 > title.size()) return false;
    std::wstring name = title.substr(open + 2, title.size() - open - 3);
    if (name.empty()) return false;
    std::wstring inner = title.substr(0, open);
    // "Visual Studio Code" alone is a window with nothing open yet.
    if (!EndsWith(inner, L"Visual Studio Code") && !EndsWith(inner, L"Visual Studio Code - Insiders")) return false;
    if (vscodeTitle) *vscodeTitle = inner;
    if (distro) *distro = name;
    return true;
}

bool MeasureWslgShadowInsets(HWND hwnd, RECT& insets) {
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return false;
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return false;
    // PrintWindow is a synchronous round-trip into msrdc -- don't let a hung
    // one stall this app's whole UI thread with it.
    if (!SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, nullptr)) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    const UINT kPwRenderFullContent = 2; // PW_RENDERFULLCONTENT
    bool printed = PrintWindow(hwnd, dc, kPwRenderFullContent) != 0;
    GdiFlush();
    const UINT32* px = (const UINT32*)bits;
    auto lit = [&](int x, int y) { return (px[y * w + x] & 0x00FFFFFFu) != 0; };

    // Several sample lines per side, taking the smallest inset any of them
    // finds, so one line that happens to cross dark content doesn't
    // overshoot. Capped well above the real margin: no hit within the cap
    // means this isn't a margin (or nothing's painted yet).
    const int kMaxInset = 160;
    const double kSamples[] = {0.25, 0.5, 0.75};
    auto scan = [&](bool horizontal, bool fromStart) {
        int best = -1;
        for (double f : kSamples) {
            int span = horizontal ? w : h;
            int fixed = (int)((horizontal ? h : w) * f);
            for (int i = 0; i < std::min(kMaxInset, span); i++) {
                int pos = fromStart ? i : span - 1 - i;
                if (horizontal ? lit(pos, fixed) : lit(fixed, pos)) {
                    if (best < 0 || i < best) best = i;
                    break;
                }
            }
        }
        return best;
    };
    int left = -1, top = -1, right = -1, bottom = -1;
    if (printed) {
        left = scan(true, true);
        right = scan(true, false);
        top = scan(false, true);
        bottom = scan(false, false);
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);

    if (left < 0 || top < 0 || right < 0 || bottom < 0 || left + right >= w || top + bottom >= h) return false;
    insets = {left, top, right, bottom};
    return true;
}

bool FixWslgClampedMaximize(HWND hwnd, int knownMargin) {
    RECT wr;
    MONITORINFO mi = {sizeof(mi)};
    if (IsIconic(hwnd) || !GetWindowRect(hwnd, &wr) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        return false;
    }
    // Per window: its last real restored rect, and the grown maximized rect
    // last applied. A window regrown here and then restored can come back
    // still at the grown rect, just no longer maximized (Windows took it as
    // the window's restored position) -- put it back where it was instead.
    struct State {
        RECT restored = {};
        RECT grown = {};
    };
    static std::map<HWND, State> states;
    State& st = states[hwnd];
    if (!IsZoomed(hwnd)) {
        if (!IsRectEmpty(&st.grown) && EqualRect(&wr, &st.grown) && !IsRectEmpty(&st.restored)) {
            RECT to = st.restored;
            st.grown = {};
            SetWindowPos(hwnd, nullptr, to.left, to.top, to.right - to.left, to.bottom - to.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Log(L"wsl: hwnd=%p restored but still at its regrown maximized rect -- moved back to (%ld,%ld)-(%ld,%ld)",
                hwnd, to.left, to.top, to.right, to.bottom);
            return true;
        }
        if (!EqualRect(&wr, &st.grown)) st.restored = wr;
        return false;
    }
    // Already hanging off the work area's top-left: either fixed, or a
    // window with no margin to hang (e.g. native title bar).
    if (wr.left < mi.rcWork.left || wr.top < mi.rcWork.top) return false;
    // Don't keep fighting (or re-measuring, a whole-window render each time)
    // if a fix somehow doesn't stick.
    static std::map<HWND, ULONGLONG> lastAttempt;
    ULONGLONG now = GetTickCount64();
    auto it = lastAttempt.find(hwnd);
    if (it != lastAttempt.end() && now - it->second < 2000) return false;

    int m = knownMargin;
    if (m <= 0) {
        // Clamped, the content is offset by exactly the margin at the
        // top-left (the right/bottom margin is what fell off-screen). No
        // offset: nothing to fix.
        RECT insets;
        if (!MeasureWslgShadowInsets(hwnd, insets) || insets.left <= 0 || insets.left != insets.top) return false;
        m = insets.left;
    }
    lastAttempt[hwnd] = now;
    RECT target = mi.rcWork;
    InflateRect(&target, m, m);
    // SWP_NOSENDCHANGING: msrdc caps a window's size at the screen's, which
    // the grown rect exceeds.
    SetWindowPos(hwnd, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    st.grown = target;
    Log(L"wsl: hwnd=%p maximized but clamped to (%ld,%ld)-(%ld,%ld) -- regrown by its %dpx shadow margin", hwnd,
        wr.left, wr.top, wr.right, wr.bottom, m);
    return true;
}

// The WSL icon rendered to premultiplied ARGB (kWslIconSize^2 pixels),
// once; empty if the resource can't be loaded.
static const std::vector<UINT32>& WslIconPixels() {
    static std::vector<UINT32> out;
    static bool loaded = false;
    if (loaded) return out;
    loaded = true;
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_WSL), IMAGE_ICON, kWslIconSize,
                                   kWslIconSize, 0);
    if (!icon) {
        LogWarn(L"wsl: couldn't load the WSL icon resource, lastError=%lu", GetLastError());
        return out;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kWslIconSize;
    bmi.bmiHeader.biHeight = -kWslIconSize; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0, kWslIconSize * kWslIconSize * 4);
        DrawIconEx(dc, 0, 0, icon, kWslIconSize, kWslIconSize, 0, nullptr, DI_NORMAL);
        GdiFlush();
        const UINT32* src = (const UINT32*)bits;
        out.assign(src, src + kWslIconSize * kWslIconSize);
        SelectObject(dc, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    DestroyIcon(icon);
    return out;
}

void BlendWslIcon(UINT32* pixels, int surfaceWidth, int surfaceHeight, int left, int top, const RECT& clip) {
    const std::vector<UINT32>& icon = WslIconPixels();
    if (icon.empty()) return;
    for (int y = 0; y < kWslIconSize; y++) {
        int row = top + y;
        if (row < 0 || row >= surfaceHeight || row < clip.top || row >= clip.bottom) continue;
        for (int x = 0; x < kWslIconSize; x++) {
            int col = left + x;
            if (col < 0 || col >= surfaceWidth || col < clip.left || col >= clip.right) continue;
            UINT32 s = icon[y * kWslIconSize + x];
            UINT32 a = s >> 24;
            if (!a) continue;
            UINT32 d = pixels[row * surfaceWidth + col];
            auto blend = [&](int shift) {
                UINT32 sc = (s >> shift) & 0xFF, dc = (d >> shift) & 0xFF;
                return std::min<UINT32>(255, sc + dc * (255 - a) / 255) << shift; // src is premultiplied
            };
            pixels[row * surfaceWidth + col] = 0xFF000000u | blend(16) | blend(8) | blend(0);
        }
    }
}
