#include "project_list_hud.h"

#include "layered_rendering.h"

#include <windowsx.h>

#include <algorithm>

static const wchar_t* kProjectListHudClassName = L"VSCodeBorderProjectListHudWndClass";
static const int kProjectListGap = 6;
static const int kProjectListEdgeGrip = 8;
static const int kProjectListMinWidth = 130;
static const int kProjectListMaxWidth = 900;

struct ProjectListHudState {
    std::vector<ProjectListHudEntry> entries;
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
    enum DragMode { DragNone, DragMove, DragResizeLeft, DragResizeRight } dragMode = DragNone;
    POINT dragStart = {0, 0};
    RECT dragStartRect = {0, 0, 0, 0};
};

static int ClampInt(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

static int ProjectListHitTest(const ProjectListHudState* state, int x, int y) {
    if (!state || x < 0 || x >= state->width || y < 0 || y >= state->height) return -1;
    int stride = state->rowHeight + kProjectListGap;
    if (stride <= 0) return -1;
    int index = y / stride;
    if (index < 0 || index >= (int)state->entries.size()) return -1;
    int rowTop = index * stride;
    return y < rowTop + state->rowHeight ? index : -1;
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
                    left = ClampInt(left + dx, right - kProjectListMaxWidth, right - kProjectListMinWidth);
                    state->manualPosition = true;
                    state->manualWidth = true;
                } else if (state->dragMode == ProjectListHudState::DragResizeRight) {
                    right = ClampInt(right + dx, left + kProjectListMinWidth, left + kProjectListMaxWidth);
                    state->manualPosition = true;
                    state->manualWidth = true;
                }

                int newWidth = right - left;
                bool sizeChanged = newWidth != state->width;
                state->x = left;
                state->y = top;
                state->width = newWidth;
                state->height = bottom - top;
                if (sizeChanged) RenderProjectListHud(hwnd, state);
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
                SetCursor(LoadCursorW(nullptr, ProjectListCursorForPoint(state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))));
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

HWND CreateProjectListHud(HINSTANCE hInstance) {
    RegisterProjectListHudClass(hInstance);
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kProjectListHudClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)new ProjectListHudState());
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

    HFONT font = CreateFontW(-state->fontSize, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hoverFont = CreateFontW(-state->fontSize, 0, 0, 0, FW_BLACK, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    const int paddingX = 12;
    for (size_t i = 0; i < state->entries.size(); i++) {
        const ProjectListHudEntry& entry = state->entries[i];
        int y = (int)i * (rowHeight + kProjectListGap);
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
        for (int row = y; row < y + rowHeight && row < height; row++) {
            for (int col = 0; col < width; col++) {
                pixels[row * width + col] = chipPx;
            }
        }
        COLORREF tx = state->labelTextColorAuto ? ContrastTextColor(color) : state->labelTextColor;
        BlendTextIntoPixels(screenDC, pixels, width, height, paddingX, y, width - paddingX * 2, rowHeight,
                            entry.label, (int)i == state->hoverIndex ? hoverFont : font, chipColor, tx,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        for (int row = y; row < y + rowHeight && row < height; row++) {
            for (int col = 0; col < width; col++) {
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

void UpdateProjectListHud(HWND hud, int x, int y, int width, int height,
                          const std::vector<ProjectListHudEntry>& entries,
                          int rowHeight, int fontSize,
                          bool labelTextColorAuto, COLORREF labelTextColor,
                          int normalOpacity, int hoverOpacity, bool activateOnHover) {
    if (width <= 0 || height <= 0 || entries.empty()) return;

    ProjectListHudState* state = (ProjectListHudState*)GetWindowLongPtrW(hud, GWLP_USERDATA);
    if (!state) return;
    if (!state->manualWidth) {
        state->width = width;
    } else {
        state->width = ClampInt(state->width, kProjectListMinWidth, kProjectListMaxWidth);
    }
    if (!state->manualPosition) {
        state->x = x + width - state->width;
        state->y = y;
    }
    state->entries = entries;
    state->height = height;
    state->rowHeight = rowHeight;
    state->fontSize = fontSize;
    state->labelTextColorAuto = labelTextColorAuto;
    state->labelTextColor = labelTextColor;
    state->normalOpacity = ClampInt(normalOpacity, 0, 255);
    state->hoverOpacity = ClampInt(hoverOpacity, 0, 255);
    state->activateOnHover = activateOnHover;
    if (!state->activateOnHover) EndProjectListHoverFocus(state, true);
    if (state->hoverIndex >= (int)entries.size()) state->hoverIndex = -1;
    RenderProjectListHud(hud, state);
    PositionProjectListHud(hud, state);
}