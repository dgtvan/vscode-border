#include "project_list_hud.h"

#include "layered_rendering.h"
#include "monitor_scenario.h"

#include <windowsx.h>

#include <algorithm>

static const wchar_t* kProjectListHudClassName = L"VSCodeBorderProjectListHudWndClass";
static const int kProjectListGap = 6;
static const int kProjectListEdgeGrip = 8;
static const int kProjectListMinWidth = 130; // manual (drag-resize) lower bound, both styles -- no upper
                                              // bound: the user can size the HUD as wide as they want
static const int kProjectListAutoMaxWidth = 360; // auto-measured upper bound for the shared item/column
                                                  // width -- narrower so a single long label can't blow up
                                                  // the HUD when the user hasn't sized it by hand
static const int kProjectListMeasurePaddingX = 32;
static const int kProjectListBottomMargin = 24;

static HFONT CreateHudFont(int fontSize, int weight) {
    return CreateFontW(-fontSize, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

struct ProjectListHudState {
    std::vector<ProjectListHudEntry> entries;
    std::vector<RECT> itemRects; // per-entry rect within the HUD's own client area (local coords)
    bool horizontal = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int rowHeight = 0;
    int fontSize = 0;
    bool labelTextColorAuto = false;
    COLORREF labelTextColor = RGB(0, 0, 0);
    int normalOpacity = 255;
    int hoverOpacity = 255;
    bool activateOnHover = false;
    int hoverIndex = -1;
    bool trackingMouseLeave = false;
    bool hoverFocusActive = false;
    HWND previousForeground = nullptr;
    bool manualPosition = false;
    bool manualWidth = false;
    int manualItemWidth = 0; // horizontal style only: the user's chosen per-item width, independent of
                              // how many items are currently shown -- state->width (the whole strip) is
                              // re-derived from this every time entries.size() changes, so item count
                              // never changes how wide each item looks (see RebuildHorizontalItemRects)
    enum DragMode { DragNone, DragMove, DragResizeLeft, DragResizeRight } dragMode = DragNone;
    POINT dragStart = {0, 0};
    RECT dragStartRect = {0, 0, 0, 0};
    std::wstring scenarioKey; // current monitor scenario (see monitor_scenario.h) -- tracked so a
                              // WM_DISPLAYCHANGE can tell whether the active monitor set actually
                              // changed, and so a completed drag knows which scenario to save under
};

static int ClampInt(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

static int ProjectListHitTest(const ProjectListHudState* state, int x, int y) {
    if (!state || x < 0 || x >= state->width || y < 0 || y >= state->height) return -1;
    for (size_t i = 0; i < state->itemRects.size(); i++) {
        const RECT& r = state->itemRects[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)i;
    }
    return -1;
}

static bool IsProjectListResizeEdge(const ProjectListHudState* state, int x) {
    return state && state->width > 0 && (x < kProjectListEdgeGrip || x >= state->width - kProjectListEdgeGrip);
}

static ProjectListHudState::DragMode ProjectListDragModeForPoint(const ProjectListHudState* state, int x) {
    if (!state) return ProjectListHudState::DragNone;
    if (x < kProjectListEdgeGrip) return ProjectListHudState::DragResizeLeft;
    if (x >= state->width - kProjectListEdgeGrip) return ProjectListHudState::DragResizeRight;
    return ProjectListHudState::DragMove;
}

// Recomputes vertical-style item rects from state->width/rowHeight/entries
// -- cheap (no GDI/text-measurement calls), so it's safe to call on every
// mouse-move frame of an interactive resize-drag.
static void RebuildVerticalItemRects(ProjectListHudState* state) {
    state->itemRects.assign(state->entries.size(), RECT{});
    for (size_t i = 0; i < state->entries.size(); i++) {
        int y = (int)i * (state->rowHeight + kProjectListGap);
        state->itemRects[i] = {0, y, state->width, y + state->rowHeight};
    }
}

// Divides state->width evenly across items, same width for every one
// regardless of label length (long labels just ellipsize) -- the horizontal
// analog of RebuildVerticalItemRects's shared column width. Cheap (no
// GDI/text-measurement calls), so it's safe to call on every mouse-move
// frame of an interactive resize-drag.
static void RebuildHorizontalItemRects(ProjectListHudState* state) {
    size_t n = state->entries.size();
    state->itemRects.assign(n, RECT{});
    if (n == 0) return;

    int itemWidth = std::max(1, (state->width - (int)(n - 1) * kProjectListGap) / (int)n);
    int x = 0;
    for (size_t i = 0; i < n; i++) {
        state->itemRects[i] = {x, 0, x + itemWidth, state->rowHeight};
        x += itemWidth + kProjectListGap;
    }
}

static LPCWSTR ProjectListCursorForPoint(const ProjectListHudState* state, int x, int y) {
    if (state && (state->dragMode == ProjectListHudState::DragResizeLeft ||
                  state->dragMode == ProjectListHudState::DragResizeRight)) {
        return IDC_SIZEWE;
    }
    if (IsProjectListResizeEdge(state, x)) return IDC_SIZEWE;
    return ProjectListHitTest(state, x, y) >= 0 ? IDC_HAND : IDC_ARROW;
}

static void RenderProjectListHud(HWND hud, ProjectListHudState* state);

static void PositionProjectListHud(HWND hud, ProjectListHudState* state) {
    if (!state) return;
    SetWindowPos(hud, HWND_TOPMOST, state->x, state->y, state->width, state->height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Re-checks the current monitor scenario against state->scenarioKey and, if
// it changed, applies whatever placement was last saved for the new one --
// or drops back to auto-placement if this scenario has never been seen
// before, letting the next regular UpdateProjectListHud recompute the
// default position/width. Called once at HUD creation (so the very first
// sync already uses a remembered placement, if any) and on every
// WM_DISPLAYCHANGE. Returns true if the scenario actually changed.
//
// The saved "width" always means *per-item* width -- for the vertical
// style that's the same thing as the whole strip's width (one shared
// column), but for horizontal it's deliberately not the whole strip's
// width, so restoring it doesn't depend on how many windows happen to be
// tracked at that moment (see manualItemWidth's comment).
static bool ApplyScenarioForCurrentMonitors(ProjectListHudState* state) {
    std::wstring key = GetMonitorScenarioKey();
    if (!state || key == state->scenarioKey) return false;
    state->scenarioKey = key;

    SavedHudPlacement saved = LoadHudPlacement(key);
    state->manualPosition = saved.found;
    state->manualWidth = saved.found;
    if (saved.found) {
        state->x = saved.x;
        state->y = saved.y;
        if (state->horizontal) {
            state->manualItemWidth = std::max(1, saved.width);
            size_t n = state->entries.size();
            state->width = n > 0 ? (int)n * state->manualItemWidth + (int)(n - 1) * kProjectListGap
                                  : state->manualItemWidth;
        } else {
            state->width = std::max(saved.width, kProjectListMinWidth);
        }
    }
    return true;
}

static void ActivateProjectListItem(ProjectListHudState* state, int index) {
    if (!state || index < 0 || index >= (int)state->entries.size()) return;
    HWND target = state->entries[index].target;
    if (!target || !IsWindow(target)) return;

    if (!state->hoverFocusActive) {
        state->previousForeground = GetForegroundWindow();
        state->hoverFocusActive = true;
    }

    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
}

static void EndProjectListHoverFocus(ProjectListHudState* state, bool restorePrevious) {
    if (!state || !state->hoverFocusActive) return;
    HWND previous = state->previousForeground;
    state->hoverFocusActive = false;
    state->previousForeground = nullptr;

    if (restorePrevious && previous && IsWindow(previous)) {
        if (IsIconic(previous)) ShowWindow(previous, SW_RESTORE);
        SetForegroundWindow(previous);
    }
}

static LRESULT CALLBACK ProjectListHudWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ProjectListHudState* state = (ProjectListHudState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!state) break;
            int mouseX = GET_X_LPARAM(lParam);
            int mouseY = GET_Y_LPARAM(lParam);
            if (state->dragMode != ProjectListHudState::DragNone) {
                POINT pt;
                GetCursorPos(&pt);
                int dx = pt.x - state->dragStart.x;
                int dy = pt.y - state->dragStart.y;
                int left = state->dragStartRect.left;
                int top = state->dragStartRect.top;
                int right = state->dragStartRect.right;
                int bottom = state->dragStartRect.bottom;

                if (state->dragMode == ProjectListHudState::DragMove) {
                    left += dx;
                    top += dy;
                    right += dx;
                    bottom += dy;
                    state->manualPosition = true;
                } else if (state->dragMode == ProjectListHudState::DragResizeLeft) {
                    left = std::min(left + dx, right - kProjectListMinWidth);
                    state->manualPosition = true;
                    state->manualWidth = true;
                } else if (state->dragMode == ProjectListHudState::DragResizeRight) {
                    right = std::max(right + dx, left + kProjectListMinWidth);
                    state->manualPosition = true;
                    state->manualWidth = true;
                }

                int newWidth = right - left;
                bool sizeChanged = newWidth != state->width;
                state->x = left;
                state->y = top;
                state->width = newWidth;
                state->height = bottom - top;
                if (state->horizontal) {
                    // Cache what per-item width this drag corresponds to
                    // *right now* (entries.size() is fixed for the duration
                    // of a single drag) -- this, not state->width, is what
                    // gets remembered on WM_RBUTTONUP.
                    size_t n = state->entries.size();
                    state->manualItemWidth =
                        n > 0 ? std::max(1, (newWidth - (int)(n - 1) * kProjectListGap) / (int)n) : newWidth;
                }
                if (sizeChanged) {
                    if (state->horizontal) RebuildHorizontalItemRects(state);
                    else RebuildVerticalItemRects(state);
                    RenderProjectListHud(hwnd, state);
                }
                PositionProjectListHud(hwnd, state);
                SetCursor(LoadCursorW(nullptr, state->dragMode == ProjectListHudState::DragMove ? IDC_SIZEALL : IDC_SIZEWE));
                return 0;
            }

            int hoverIndex = ProjectListHitTest(state, mouseX, mouseY);
            if (hoverIndex != state->hoverIndex) {
                state->hoverIndex = hoverIndex;
                RenderProjectListHud(hwnd, state);
                if (state->activateOnHover && hoverIndex >= 0) ActivateProjectListItem(state, hoverIndex);
            }
            SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, mouseX, mouseY)));
            if (!state->trackingMouseLeave) {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                state->trackingMouseLeave = TrackMouseEvent(&tme) != 0;
            }
            return 0;
        }
        case WM_RBUTTONDOWN:
            if (state) {
                state->dragMode = ProjectListDragModeForPoint(state, GET_X_LPARAM(lParam));
                GetCursorPos(&state->dragStart);
                GetWindowRect(hwnd, &state->dragStartRect);
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, state->dragMode == ProjectListHudState::DragMove ? IDC_SIZEALL : IDC_SIZEWE));
            }
            return 0;
        case WM_RBUTTONUP:
            if (state && state->dragMode != ProjectListHudState::DragNone) {
                state->dragMode = ProjectListHudState::DragNone;
                if (GetCapture() == hwnd) ReleaseCapture();
                // Remember where the user just put it, scoped to the current
                // monitor scenario -- so switching monitors later restores
                // this placement instead of falling back to auto-anchoring.
                // Saved width is always per-item (see manualItemWidth), so
                // it stays correct however many windows are tracked later.
                if (state->manualPosition) {
                    int savedWidth = state->horizontal ? state->manualItemWidth : state->width;
                    SaveHudPlacement(state->scenarioKey, state->x, state->y, savedWidth);
                }
                SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))));
            }
            return 0;
        case WM_DISPLAYCHANGE:
            if (state && ApplyScenarioForCurrentMonitors(state) && state->manualPosition) {
                // Only need to force an immediate refresh when we just
                // snapped to a remembered placement -- if this scenario has
                // no saved placement, manualPosition/manualWidth are now
                // false and the next regular UpdateProjectListHud call
                // (already running periodically) will recompute the default
                // auto-placement on its own.
                if (state->horizontal) RebuildHorizontalItemRects(state);
                else RebuildVerticalItemRects(state);
                RenderProjectListHud(hwnd, state);
                PositionProjectListHud(hwnd, state);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (state) {
                if (state->dragMode != ProjectListHudState::DragNone) return 0;
                state->trackingMouseLeave = false;
                if (state->hoverIndex != -1) {
                    state->hoverIndex = -1;
                    RenderProjectListHud(hwnd, state);
                }
                EndProjectListHoverFocus(state, true);
            }
            return 0;
        case WM_SETCURSOR:
            if (state && LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, pt.x, pt.y)));
                return TRUE;
            }
            break;
        case WM_CAPTURECHANGED:
            if (state) state->dragMode = ProjectListHudState::DragNone;
            return 0;
        case WM_LBUTTONUP:
            if (state && state->hoverIndex >= 0 && state->hoverIndex < (int)state->entries.size()) {
                ActivateProjectListItem(state, state->hoverIndex);
                EndProjectListHoverFocus(state, false);
            }
            return 0;
        case WM_NCDESTROY:
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void RegisterProjectListHudClass(HINSTANCE hInstance) {
    static bool registered = false;
    if (registered) return;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = ProjectListHudWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kProjectListHudClassName;
    RegisterClassW(&wc);
    registered = true;
}

HWND CreateProjectListHud(HINSTANCE hInstance, bool horizontal) {
    RegisterProjectListHudClass(hInstance);
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kProjectListHudClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return nullptr;
    ProjectListHudState* state = new ProjectListHudState();
    state->horizontal = horizontal; // must be right before ApplyScenarioForCurrentMonitors below, since
                                     // it decides whether a saved width means "per-item" or "whole strip"
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    // Establishes the initial scenarioKey and, if this monitor scenario has
    // a remembered placement, pre-sets manualPosition/manualWidth from it
    // so the very first UpdateProjectListHud call (once real entries show
    // up) uses it instead of auto-anchoring.
    ApplyScenarioForCurrentMonitors(state);
    return hwnd;
}

static void RenderProjectListHud(HWND hud, ProjectListHudState* state) {
    if (!state || state->width <= 0 || state->height <= 0 || state->entries.empty()) return;

    int width = state->width;
    int height = state->height;
    int rowHeight = state->rowHeight;

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
    if (!bmp) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

    UINT32* pixels = (UINT32*)bits;
    for (int i = 0; i < width * height; i++) pixels[i] = 0x01000000u;

    HFONT font = CreateHudFont(state->fontSize, FW_SEMIBOLD);
    HFONT hoverFont = CreateHudFont(state->fontSize, FW_BLACK);

    const int paddingX = 12;
    for (size_t i = 0; i < state->entries.size() && i < state->itemRects.size(); i++) {
        const ProjectListHudEntry& entry = state->entries[i];
        const RECT& item = state->itemRects[i];
        int itemWidth = item.right - item.left;
        COLORREF color = entry.color;
        BYTE alpha = (BYTE)((int)i == state->hoverIndex ? state->hoverOpacity : state->normalOpacity);
        BYTE r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
        if ((int)i == state->hoverIndex) {
            r = (BYTE)std::min(255, (int)r + 32);
            g = (BYTE)std::min(255, (int)g + 32);
            b = (BYTE)std::min(255, (int)b + 32);
        }
        COLORREF chipColor = RGB(r, g, b);
        UINT32 chipPx = (UINT32(255) << 24) | (UINT32(r) << 16) | (UINT32(g) << 8) | UINT32(b);
        for (int row = item.top; row < item.bottom && row < height; row++) {
            for (int col = item.left; col < item.right && col < width; col++) {
                pixels[row * width + col] = chipPx;
            }
        }
        COLORREF tx = state->labelTextColorAuto ? ContrastTextColor(color) : state->labelTextColor;
        BlendTextIntoPixels(screenDC, pixels, width, height, item.left + paddingX, item.top,
                            itemWidth - paddingX * 2, rowHeight, entry.label,
                            (int)i == state->hoverIndex ? hoverFont : font, chipColor, tx,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        for (int row = item.top; row < item.bottom && row < height; row++) {
            for (int col = item.left; col < item.right && col < width; col++) {
                UINT32 px = pixels[row * width + col];
                BYTE pxR = (BYTE)((px >> 16) & 0xFF);
                BYTE pxG = (BYTE)((px >> 8) & 0xFF);
                BYTE pxB = (BYTE)(px & 0xFF);
                pixels[row * width + col] = (UINT32(alpha) << 24) | (UINT32((pxR * alpha) / 255) << 16) |
                                            (UINT32((pxG * alpha) / 255) << 8) | UINT32((pxB * alpha) / 255);
            }
        }
    }

    DeleteObject(font);
    DeleteObject(hoverFont);

    POINT ptSrc = {0, 0};
    SIZE sz = {width, height};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hud, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

void HideProjectListHud(HWND hud) {
    if (hud) ShowWindow(hud, SW_HIDE);
}

// Widest label (padded, clamped) across `entries`, using the same font
// RenderProjectListHud paints normal (non-hover) rows with -- so the HUD is
// sized to fit what it's about to draw.
static int MeasureRequiredWidth(const std::vector<ProjectListHudEntry>& entries, int fontSize) {
    HDC screenDC = GetDC(nullptr);
    HFONT font = CreateHudFont(fontSize, FW_SEMIBOLD);
    HFONT oldFont = (HFONT)SelectObject(screenDC, font);
    int width = 0;
    for (const ProjectListHudEntry& entry : entries) {
        SIZE textSz = {0, 0};
        GetTextExtentPoint32W(screenDC, entry.label.c_str(), (int)entry.label.size(), &textSz);
        width = std::max(width, (int)textSz.cx + kProjectListMeasurePaddingX);
    }
    SelectObject(screenDC, oldFont);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDC);
    return ClampInt(width, kProjectListMinWidth, kProjectListAutoMaxWidth);
}

void UpdateProjectListHud(HWND hud, const std::vector<ProjectListHudEntry>& entries,
                          const ProjectListHudStyle& style) {
    if (entries.empty()) return;

    ProjectListHudState* state = (ProjectListHudState*)GetWindowLongPtrW(hud, GWLP_USERDATA);
    if (!state) return;

    std::vector<ProjectListHudEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const ProjectListHudEntry& a, const ProjectListHudEntry& b) {
        if (a.windowRect.left != b.windowRect.left) return a.windowRect.left < b.windowRect.left;
        if (a.windowRect.top != b.windowRect.top) return a.windowRect.top < b.windowRect.top;
        return a.target < b.target;
    });

    int rowHeight = std::max(18, style.rowHeight);
    state->rowHeight = rowHeight;
    state->entries = sorted;
    if (state->horizontal != style.horizontal) {
        // A manually-set size from one style doesn't translate to the
        // other -- horizontal's per-item width and vertical's shared
        // column width aren't the same concept, so reusing either one
        // as-is after a style switch (e.g. via config reload) would size
        // things nonsensically. Position stays valid either way, just the
        // width falls back to auto-fit until the user resizes again.
        state->manualWidth = false;
        state->manualItemWidth = 0;
    }
    state->horizontal = style.horizontal;
    int totalHeight;

    if (style.horizontal) {
        // Every item shares one width (like MeasureRequiredWidth's shared
        // column width for the vertical style), sized to fit the widest
        // label when not manually resized. When manually resized, reuse the
        // remembered per-item width (manualItemWidth) rather than whatever
        // state->width happened to be -- state->width is a function of both
        // that and the current entry count, so re-deriving it here is what
        // keeps every item the same width no matter how many windows are
        // currently tracked.
        int itemWidth = !state->manualWidth ? MeasureRequiredWidth(sorted, style.fontSize)
                                             : std::max(1, state->manualItemWidth);
        state->width = (int)sorted.size() * itemWidth + (int)(sorted.size() - 1) * kProjectListGap;
        RebuildHorizontalItemRects(state);
        totalHeight = rowHeight;
    } else {
        if (!state->manualWidth) {
            state->width = MeasureRequiredWidth(sorted, style.fontSize);
        } else {
            state->width = std::max(state->width, kProjectListMinWidth);
        }
        RebuildVerticalItemRects(state);
        totalHeight = (int)sorted.size() * rowHeight + ((int)sorted.size() - 1) * kProjectListGap;
    }
    state->height = totalHeight;

    if (!state->manualPosition) {
        RECT workArea = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        state->x = workArea.right - state->width;
        state->y = workArea.bottom - state->height - kProjectListBottomMargin;
    }

    state->fontSize = style.fontSize;
    state->labelTextColorAuto = style.labelTextColorAuto;
    state->labelTextColor = style.labelTextColor;
    state->normalOpacity = ClampInt(style.normalOpacity, 0, 255);
    state->hoverOpacity = ClampInt(style.hoverOpacity, 0, 255);
    state->activateOnHover = style.activateOnHover;
    if (!state->activateOnHover) EndProjectListHoverFocus(state, true);
    if (state->hoverIndex >= (int)sorted.size()) state->hoverIndex = -1;
    RenderProjectListHud(hud, state);
    PositionProjectListHud(hud, state);
}