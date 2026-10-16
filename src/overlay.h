#pragma once

#include <windows.h>

#include <string>

extern const wchar_t* kOverlayClassName;

// Creates the (hidden, 1x1) layered click-through border overlay popup
// window for `target`.
HWND CreateOverlay(HINSTANCE hInstance, HWND target);

// The overlay window class's window procedure -- registered for
// kOverlayClassName.
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Left-click-and-hold on the label chip drags the target window around (a
// maximized one is restored first and put back under the cursor, like a
// title-bar drag). With a resize strip (see PaintOverlay's resizeMargin),
// left-click-and-drag on the border or the strip resizes it from that
// edge/corner, showing the matching resize cursor. The overlay is
// click-through everywhere else, though -- it must never get in the way of
// the window it frames -- so it's made clickable only while the cursor is
// actually over the chip or the strip, with that part on screen (not covered
// by another window) and no mouse button already held for something else. Call often (tracking.cpp's 50ms
// foreground poll) for every overlay to keep that up to date.
void UpdateOverlayLabelHover(HWND overlay);

// Paints a hollow rectangular frame of `thickness` px into the overlay's
// layered surface, plus an optional folder-name label chip just inside the
// top-left corner (overlapping the target's own top-left corner -- see
// docs/ARCHITECTURE.md's "Known issue: label can occasionally blink" for
// why, and why it's accepted rather than fixed).
//
// Label text color is either fixed (`labelTextColor`, used when
// `labelTextColorAuto` is false) or auto-picked for contrast against `color`
// (when `labelTextColorAuto` is true). A non-empty `wslDistro` (a WSLg
// window -- see wsl.h) puts the WSL icon in front of the label text. A
// non-zero `resizeMargin` adds an invisible strip that wide all around,
// outside the border (so `width`/`height` include it), which works as the
// window's resize edges -- see UpdateOverlayLabelHover. Also records where
// the chip and the strip are, for UpdateOverlayLabelHover.
void PaintOverlay(HWND overlay, int width, int height, COLORREF color, int thickness, int opacity,
                   const std::wstring& label, bool showLabel, int labelHeight, int labelFontSize,
                   bool labelTextColorAuto, COLORREF labelTextColor, const std::wstring& wslDistro,
                   int resizeMargin);