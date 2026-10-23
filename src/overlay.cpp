#include "overlay.h"

#include "claude_profile.h"
#include "focus_trace.h"
#include "layered_rendering.h"
#include "logger.h"

#include <algorithm>
#include <cstdlib>
#include <map>

const wchar_t* kOverlayClassName = L"VSCodeBorderOverlayWndClass";

// Cursor movement before a press on the label counts as a drag rather than
// a click (which just activates the window).
static const int kLabelDragThreshold = 4;

struct OverlayState {
    HWND target = nullptr;
    RECT label = {}; // label chip, overlay-local -- empty when none is drawn
    bool clickable = false; // WS_EX_TRANSPARENT currently lifted (see UpdateOverlayLabelHover)
    bool pressed = false; // left button went down on the chip and hasn't come up yet
    bool dragging = false; // ...and has moved past kLabelDragThreshold
    bool wasZoomed = false; // target was maximized when the press began
    POINT pressPos = {};    // cursor at the press (or at the restore, see WM_MOUSEMOVE)
    RECT pressRect = {};    // target's window rect at the same moment
};
static std::map<HWND, OverlayState> g_overlays;

HWND CreateOverlay(HINSTANCE hInstance, HWND target) {
    HWND overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (overlay) g_overlays[overlay].target = target;
    return overlay;
}

static void SetOverlayClickable(HWND overlay, OverlayState& st, bool clickable) {
    if (st.clickable == clickable) return;
    LONG_PTR ex = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
    SetWindowLongPtrW(overlay, GWL_EXSTYLE, clickable ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT));
    st.clickable = clickable;
}

void UpdateOverlayLabelHover(HWND overlay) {
    auto it = g_overlays.find(overlay);
    if (it == g_overlays.end()) return;
    OverlayState& st = it->second;
    if (st.pressed) return; // mid-press/drag: stays clickable until the button comes up
    bool want = false;
    POINT pt;
    RECT o;
    // A button already held (e.g. a text selection being dragged across the
    // corner) is someone else's gesture -- don't start catching it.
    bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (IsWindowVisible(overlay) && GetCursorPos(&pt) && GetWindowRect(overlay, &o) && (st.clickable || !buttonDown)) {
        RECT chip = st.label;
        OffsetRect(&chip, o.left, o.top);
        if (!IsRectEmpty(&st.label) && PtInRect(&chip, pt)) {
            // Only when it's the chip that's actually under the cursor: the
            // overlay itself once clickable, the target underneath it before
            // -- not some other window covering this corner.
            HWND under = GetAncestor(WindowFromPoint(pt), GA_ROOT);
            want = under == overlay || under == st.target;
        }
    }
    SetOverlayClickable(overlay, st, want);
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto it = g_overlays.find(hwnd);
    if (it == g_overlays.end()) return DefWindowProcW(hwnd, msg, wParam, lParam);
    OverlayState& st = it->second;

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            if (!st.target || !IsWindow(st.target)) return 0;
            st.pressed = true;
            st.dragging = false;
            st.wasZoomed = IsZoomed(st.target) != 0;
            GetCursorPos(&st.pressPos);
            GetWindowRect(st.target, &st.pressRect);
            SetCapture(hwnd);
            // Same as clicking the window itself there would have done.
            RequestForeground(st.target, FocusTargetKind::ExternalWindow, L"label-press");
            return 0;
        case WM_MOUSEMOVE: {
            if (!st.pressed || GetCapture() != hwnd) return 0;
            POINT pt;
            GetCursorPos(&pt);
            int dx = pt.x - st.pressPos.x, dy = pt.y - st.pressPos.y;
            if (!st.dragging) {
                if (abs(dx) < kLabelDragThreshold && abs(dy) < kLabelDragThreshold) return 0;
                st.dragging = true;
                Log(L"label drag: moving hwnd=%p%ls", st.target, st.wasZoomed ? L" (restoring from maximized)" : L"");
            }
            if (st.wasZoomed) {
                // Like dragging a maximized window's title bar: restore it,
                // keeping the cursor at the same fraction of its width and
                // the same height from its top, then carry on from there.
                st.wasZoomed = false;
                WINDOWPLACEMENT wp = {sizeof(wp)};
                GetWindowPlacement(st.target, &wp);
                int w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
                int h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
                int pressW = std::max(1, (int)(st.pressRect.right - st.pressRect.left));
                double fx = (double)(st.pressPos.x - st.pressRect.left) / pressW;
                int left = pt.x - (int)(fx * w);
                int top = pt.y - (st.pressPos.y - st.pressRect.top);
                ShowWindow(st.target, SW_RESTORE);
                SetWindowPos(st.target, nullptr, left, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                st.pressPos = pt;
                st.pressRect = {left, top, left + w, top + h};
                return 0;
            }
            SetWindowPos(st.target, nullptr, st.pressRect.left + dx, st.pressRect.top + dy, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            st.pressed = false;
            st.dragging = false;
            return 0;
        case WM_NCDESTROY:
            g_overlays.erase(it);
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Only the border bands (and label rect) are touched (not the whole
// width*height area) so resizing a large window stays cheap.
void PaintOverlay(HWND overlay, int width, int height, COLORREF color, int thickness, int opacity,
                   const std::wstring& label, bool showLabel, int labelHeight, int labelFontSize,
                   bool labelTextColorAuto, COLORREF labelTextColor, const std::wstring* profileIcon) {
    if (width <= 0 || height <= 0) return;
    RECT labelRect = {}; // where the chip ends up, if drawn -- see UpdateOverlayLabelHover
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
    const int stride = width;

    // Border bands.
    for (int y = 0; y < t; y++)
        for (int x = 0; x < width; x++) pixels[y * stride + x] = px;
    for (int y = height - t; y < height; y++)
        for (int x = 0; x < width; x++) pixels[y * stride + x] = px;
    for (int y = t; y < height - t; y++) {
        for (int x = 0; x < t; x++) pixels[y * stride + x] = px;
        for (int x = width - t; x < width; x++) pixels[y * stride + x] = px;
    }

    // Label chip: sits just inside the border at the top-left corner,
    // overlapping the target window's own top-left corner content, drawn
    // fully opaque (independent of the border's `opacity`) so the folder
    // name stays legible even when the border itself is faint. Known
    // trade-off: this can occasionally blink -- see docs/ARCHITECTURE.md's
    // "Known issue: label can occasionally blink".
    if (showLabel && !label.empty() && labelHeight > 0) {
        int lh = std::min(labelHeight, height - 2 * t);
        if (lh > 4) {
            HFONT font = CreateFontW(-labelFontSize, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(memDC, font);

            SIZE textSz = {0, 0};
            GetTextExtentPoint32W(memDC, label.c_str(), (int)label.size(), &textSz);
            SelectObject(memDC, oldFont);
            const int paddingX = 10;
            int iconShift = profileIcon ? kProfileIconSize + kProfileIconGap : 0;
            int lw = std::min(width - 2 * t, (int)textSz.cx + paddingX * 2 + iconShift);
            if (lw > 4) {
                int lx = t, ly = t;
                COLORREF textColor = labelTextColorAuto ? ContrastTextColor(color) : labelTextColor;
                BYTE bgR = GetRValue(color), bgG = GetGValue(color), bgB = GetBValue(color);
                UINT32 lpx = (UINT32(255) << 24) | (UINT32(bgR) << 16) | (UINT32(bgG) << 8) | UINT32(bgB);
                for (int y = ly; y < ly + lh; y++)
                    for (int x = lx; x < lx + lw; x++) pixels[y * stride + x] = lpx;
                labelRect = {lx, ly, lx + lw, ly + lh};
                if (profileIcon) {
                    BlendClaudeProfileIcon(*profileIcon, pixels, width, height, lx + paddingX,
                                           ly + (lh - kProfileIconSize) / 2, labelRect);
                }

                // GDI's ClearType/antialiased text rendering writes partial
                // coverage into the alpha byte of a 32bpp DIB, which would
                // make partially-covered glyph-edge pixels blend with
                // whatever is *behind* the overlay instead of the label
                // background -- looking transparent/washed-out on some
                // pixels while fully-covered ones stay solid. Render the
                // glyphs into an isolated black-background mask instead and
                // use its per-channel brightness as blend coverage, so the
                // label rect written above always stays fully opaque.
                BlendTextIntoPixels(screenDC, pixels, width, height, lx + paddingX + iconShift,
                                    ly, lw - paddingX * 2 - iconShift, lh,
                                    label, font, color, textColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }

            DeleteObject(font);
        }
    }

    POINT ptSrc = {0, 0};
    SIZE sz = {width, height};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(overlay, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    auto it = g_overlays.find(overlay);
    if (it != g_overlays.end()) it->second.label = labelRect;

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}