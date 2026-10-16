#include "overlay.h"

#include "focus_trace.h"
#include "layered_rendering.h"
#include "logger.h"
#include "wsl.h"

#include <algorithm>
#include <cstdlib>
#include <map>

const wchar_t* kOverlayClassName = L"VSCodeBorderOverlayWndClass";

// Cursor movement before a press on the label counts as a drag rather than
// a click (which just activates the window).
static const int kLabelDragThreshold = 4;

// Edges a resize grab moves (see OverlayState::resizeEdges).
enum { kEdgeLeft = 1, kEdgeTop = 2, kEdgeRight = 4, kEdgeBottom = 8 };

// Smallest window a resize grab shrinks the target to.
static const int kMinResizeWidth = 400, kMinResizeHeight = 270;

// While resizing, re-checks whether the window has caught up with the last
// size sent (see SendResize).
static const UINT_PTR kResizeTimerId = 1;
static const UINT kResizeTimerMs = 30;
// Gives up waiting for a size to land and sends the next one anyway.
static const ULONGLONG kResizeSettleTimeoutMs = 120;

struct OverlayState {
    HWND target = nullptr;
    RECT label = {}; // label chip, overlay-local -- empty when none is drawn
    int frame = 0;   // border thickness + resize margin as last painted: the overlay rect deflated by
                     // this is the target's visible rect
    int resizeMargin = 0; // invisible grab strip outside the border, 0 if none (see PaintOverlay)
    bool clickable = false; // WS_EX_TRANSPARENT currently lifted (see UpdateOverlayLabelHover)
    int hoverEdges = 0; // while clickable over the resize strip: which kEdge* bits the cursor is on
                        // (0 = over the label chip instead)
    bool pressed = false; // left button went down on the chip (or strip) and hasn't come up yet
    int resizeEdges = 0;  // ...on the strip: the edges being dragged (0 = moving via the chip)
    bool dragging = false; // ...and has moved past kLabelDragThreshold
    RECT wantRect = {};    // resizing: where the cursor says the window should be now
    RECT sentRect = {};    // resizing: the last rect actually sent (empty = none outstanding)
    RECT sentFrom = {};    // resizing: the window's rect when it was sent -- once that changes, WSLg has
                           // replied (see SendResize)
    ULONGLONG sentTick = 0;
    ULONGLONG releaseTick = 0; // resizing: when the button came up -- see WM_TIMER
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

// True if a visible window other than `overlay` sits in front of `target`
// at `pt` -- i.e. the overlay's own pixels there aren't actually on screen.
static bool CoveredInFrontOf(HWND target, HWND overlay, POINT pt) {
    for (HWND w = GetWindow(target, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (w == overlay || !IsWindowVisible(w)) continue;
        RECT r;
        if (GetWindowRect(w, &r) && PtInRect(&r, pt)) return true;
    }
    return false;
}

// The distro a WSLg target runs in (from its title, see wsl.h), or empty.
static std::wstring TargetWslDistro(HWND target) {
    wchar_t title[512] = {};
    GetWindowTextW(target, title, 512);
    std::wstring distro;
    SplitWslgVSCodeTitle(title, nullptr, &distro);
    return distro;
}

static LPCWSTR ResizeCursor(int edges) {
    bool h = (edges & (kEdgeLeft | kEdgeRight)) != 0, v = (edges & (kEdgeTop | kEdgeBottom)) != 0;
    if (h && v) {
        bool nwse = (edges & kEdgeLeft) ? (edges & kEdgeTop) != 0 : (edges & kEdgeBottom) != 0;
        return nwse ? IDC_SIZENWSE : IDC_SIZENESW;
    }
    return h ? IDC_SIZEWE : IDC_SIZENS;
}

// Asks for st.wantRect. With the patched WSLg (see wsl.h), only through its
// control FIFO: Linux VS Code decides the size it actually takes (it may
// round, or hit a limit), and WSLg's reply then sets the Windows frame to
// match -- setting the frame here as well would leave the two disagreeing
// whenever the app picks a slightly different size, and the border (drawn
// from the frame) off the app. One request at a time: the next goes once the
// window has changed since the last one (WSLg replied) or
// kResizeSettleTimeoutMs has passed, and never the same rect twice -- so
// older replies can't land after newer requests. Without the patch, the
// frame is all there is to set. Called on every mouse move and
// kResizeTimerId tick of a resize.
static void SendResize(OverlayState& st) {
    RECT now;
    if (!GetWindowRect(st.target, &now)) return;
    std::wstring distro = TargetWslDistro(st.target);
    if (!distro.empty() && WslgWindowControlAvailable(distro)) {
        if (EqualRect(&st.wantRect, &st.sentRect)) return; // already asked for (the timer stops on release)
        bool outstanding = !IsRectEmpty(&st.sentRect) && EqualRect(&now, &st.sentFrom);
        if (outstanding && GetTickCount64() - st.sentTick < kResizeSettleTimeoutMs) return;
        SendWslgWindowRect(st.target, distro, st.wantRect);
        st.sentRect = st.wantRect;
        st.sentFrom = now;
        st.sentTick = GetTickCount64();
        return;
    }
    if (!IsRectEmpty(&st.sentRect) && !EqualRect(&now, &st.sentRect) &&
        GetTickCount64() - st.sentTick < kResizeSettleTimeoutMs) {
        return; // still on its way
    }
    if (EqualRect(&now, &st.wantRect)) {
        st.sentRect = {};
        return; // already there
    }
    SetWindowPos(st.target, nullptr, st.wantRect.left, st.wantRect.top, st.wantRect.right - st.wantRect.left,
                 st.wantRect.bottom - st.wantRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    st.sentRect = st.wantRect;
    st.sentTick = GetTickCount64();
}

// Which kEdge* bits screen point `pt` is on if it's in the overlay's resize
// strip (or border) -- outside the window, within the overlay -- else 0.
static int ResizeEdgesAt(HWND overlay, const OverlayState& st, POINT pt) {
    RECT o;
    if (st.resizeMargin <= 0 || !GetWindowRect(overlay, &o) || !PtInRect(&o, pt)) return 0;
    RECT visible = o;
    InflateRect(&visible, -st.frame, -st.frame);
    if (PtInRect(&visible, pt)) return 0;
    int edges = 0;
    if (pt.x < visible.left) edges |= kEdgeLeft;
    if (pt.x >= visible.right) edges |= kEdgeRight;
    if (pt.y < visible.top) edges |= kEdgeTop;
    if (pt.y >= visible.bottom) edges |= kEdgeBottom;
    return edges;
}

void UpdateOverlayLabelHover(HWND overlay) {
    auto it = g_overlays.find(overlay);
    if (it == g_overlays.end()) return;
    OverlayState& st = it->second;
    if (st.pressed) return; // mid-press/drag: stays clickable until the button comes up
    bool want = false;
    int edges = 0;
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
        } else if ((edges = ResizeEdgesAt(overlay, st, pt)) != 0 && !CoveredInFrontOf(st.target, overlay, pt)) {
            // Outside the window, within the border or the grab strip around
            // it -- which edge(s), corners being both.
            want = true;
        } else {
            edges = 0;
        }
    }
    bool changed = want && edges && (!st.clickable || edges != st.hoverEdges);
    st.hoverEdges = want ? edges : 0;
    SetOverlayClickable(overlay, st, want);
    // The cursor is only updated on the next mouse move otherwise -- so it
    // would keep showing whatever was under it a moment ago.
    if (changed) SetCursor(LoadCursorW(nullptr, ResizeCursor(edges)));
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto it = g_overlays.find(hwnd);
    if (it == g_overlays.end()) return DefWindowProcW(hwnd, msg, wParam, lParam);
    OverlayState& st = it->second;

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR: {
            // Over the resize strip: the same resize cursor the window's own
            // edge would show, for wherever the cursor is right now. (The
            // label chip keeps the normal arrow.)
            POINT pt;
            GetCursorPos(&pt);
            int edges = st.pressed ? st.resizeEdges : ResizeEdgesAt(hwnd, st, pt);
            if (!edges) break;
            SetCursor(LoadCursorW(nullptr, ResizeCursor(edges)));
            return TRUE;
        }
        case WM_LBUTTONDOWN:
            if (!st.target || !IsWindow(st.target)) return 0;
            st.pressed = true;
            {
                POINT pt;
                GetCursorPos(&pt);
                st.resizeEdges = st.hoverEdges ? ResizeEdgesAt(hwnd, st, pt) : 0;
            }
            st.dragging = false;
            st.wasZoomed = IsZoomed(st.target) != 0;
            GetCursorPos(&st.pressPos);
            GetWindowRect(st.target, &st.pressRect);
            st.wantRect = st.pressRect;
            st.sentRect = {};
            SetCapture(hwnd);
            if (st.resizeEdges) SetTimer(hwnd, kResizeTimerId, kResizeTimerMs, nullptr);
            // Same as clicking the window itself there would have done.
            RequestForeground(st.target, FocusTargetKind::ExternalWindow,
                              st.resizeEdges ? L"resize-press" : L"label-press");
            return 0;
        case WM_MOUSEMOVE: {
            if (!st.pressed || GetCapture() != hwnd) return 0;
            POINT pt;
            GetCursorPos(&pt);
            int dx = pt.x - st.pressPos.x, dy = pt.y - st.pressPos.y;
            if (st.resizeEdges) {
                // Resizing: the grabbed edges follow the cursor, the others
                // stay put. Window-rect deltas are the same as visible-rect
                // ones, so a WSLg window's shadow margin needs no special care.
                RECT r = st.pressRect;
                if (st.resizeEdges & kEdgeLeft) r.left = std::min(r.left + dx, r.right - kMinResizeWidth);
                if (st.resizeEdges & kEdgeRight) r.right = std::max(r.right + dx, r.left + kMinResizeWidth);
                if (st.resizeEdges & kEdgeTop) r.top = std::min(r.top + dy, r.bottom - kMinResizeHeight);
                if (st.resizeEdges & kEdgeBottom) r.bottom = std::max(r.bottom + dy, r.top + kMinResizeHeight);
                st.wantRect = r;
                SendResize(st);
                return 0;
            }
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
        case WM_TIMER:
            if (wParam != kResizeTimerId) break;
            // Keeps sending until the window has reached where the cursor
            // left it -- also after the button is up (see WM_CAPTURECHANGED).
            if (st.target && IsWindow(st.target)) SendResize(st);
            // After release: stops once the last size has been sent (FIFO)
            // or reached (frame), or a second after release regardless, in
            // case the window settles on a size of its own.
            if (!st.pressed && (IsRectEmpty(&st.sentRect) || EqualRect(&st.sentRect, &st.wantRect) ||
                                GetTickCount64() - st.releaseTick > 1000)) {
                st.sentRect = {};
                KillTimer(hwnd, kResizeTimerId);
            }
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            if (st.dragging && st.target && IsWindow(st.target)) {
                // A label drag moved a WSLg window: tell Linux where it is now,
                // or its menus and popups keep opening at the old position.
                RECT now;
                std::wstring distro = TargetWslDistro(st.target);
                if (!distro.empty() && GetWindowRect(st.target, &now)) SendWslgWindowRect(st.target, distro, now);
            }
            st.pressed = false;
            st.resizeEdges = 0;
            st.dragging = false;
            st.releaseTick = GetTickCount64();
            // A resize's last size may still be on its way: the timer keeps
            // going until it has landed (or nothing was outstanding).
            if (IsRectEmpty(&st.sentRect)) KillTimer(hwnd, kResizeTimerId);
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
                   bool labelTextColorAuto, COLORREF labelTextColor, const std::wstring& wslDistro,
                   int resizeMargin) {
    if (width <= 0 || height <= 0) return;
    RECT labelRect = {}; // where the chip ends up, if drawn -- see UpdateOverlayLabelHover
    // Everything below is drawn inside the resize margin: the border and the
    // chip at the same place relative to the window as without one.
    const int m = std::max(0, std::min(resizeMargin, std::min(width, height) / 4));
    const int fullWidth = width;
    width -= 2 * m;
    height -= 2 * m;
    int t = std::min(thickness, std::min(width, height) / 2);
    if (t < 1) t = 1;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = fullWidth;
    bmi.bmiHeader.biHeight = -(height + 2 * m); // top-down
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

    // The resize margin: alpha 1 of 255, so it's invisible but -- unlike the
    // fully transparent rest -- still hit-tests as the overlay's, which is
    // what lets it catch a resize grab (see UpdateOverlayLabelHover).
    UINT32* surface = (UINT32*)bits;
    if (m > 0) {
        const UINT32 kGrabPx = 0x01000000u;
        for (int y = 0; y < height + 2 * m; y++) {
            bool band = y < m || y >= height + m;
            for (int x = 0; x < fullWidth; x++) {
                if (band || x < m || x >= width + m) surface[y * fullWidth + x] = kGrabPx;
            }
        }
    }
    // From here on, (0,0) is the border's own top-left, with rows still
    // fullWidth apart.
    UINT32* pixels = surface + m * fullWidth + m;
    const int stride = fullWidth;

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
            int iconShift = wslDistro.empty() ? 0 : kWslIconSize + kWslIconGap;
            int lw = std::min(width - 2 * t, (int)textSz.cx + paddingX * 2 + iconShift);
            if (lw > 4) {
                int lx = t, ly = t;
                COLORREF textColor = labelTextColorAuto ? ContrastTextColor(color) : labelTextColor;
                BYTE bgR = GetRValue(color), bgG = GetGValue(color), bgB = GetBValue(color);
                UINT32 lpx = (UINT32(255) << 24) | (UINT32(bgR) << 16) | (UINT32(bgG) << 8) | UINT32(bgB);
                for (int y = ly; y < ly + lh; y++)
                    for (int x = lx; x < lx + lw; x++) pixels[y * stride + x] = lpx;
                labelRect = {m + lx, m + ly, m + lx + lw, m + ly + lh};
                // The helpers below take the whole surface, so their
                // coordinates are back in it (offset by the margin).
                if (!wslDistro.empty()) {
                    BlendWslIcon(surface, fullWidth, height + 2 * m, m + lx + paddingX,
                                 m + ly + (lh - kWslIconSize) / 2, labelRect);
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
                BlendTextIntoPixels(screenDC, surface, fullWidth, height + 2 * m, m + lx + paddingX + iconShift,
                                    m + ly, lw - paddingX * 2 - iconShift, lh,
                                    label, font, color, textColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }

            DeleteObject(font);
        }
    }

    POINT ptSrc = {0, 0};
    SIZE sz = {fullWidth, height + 2 * m};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(overlay, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    auto it = g_overlays.find(overlay);
    if (it != g_overlays.end()) {
        it->second.label = labelRect;
        it->second.frame = t + m;
        it->second.resizeMargin = m;
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}