#pragma once

#include <windows.h>

#include <string>

extern const wchar_t* kOverlayClassName;

// Creates the (hidden, 1x1) layered click-through border overlay popup window.
HWND CreateOverlay(HINSTANCE hInstance);

// Paints a hollow rectangular frame of `thickness` px into the overlay's
// layered surface, plus an optional folder-name label chip just inside the
// top-left corner (overlapping the target's own top-left corner -- see
// docs/ARCHITECTURE.md's "Known issue: label can occasionally blink" for
// why, and why it's accepted rather than fixed).
//
// Label text color is either fixed (`labelTextColor`, used when
// `labelTextColorAuto` is false) or auto-picked for contrast against `color`
// (when `labelTextColorAuto` is true).
void PaintOverlay(HWND overlay, int width, int height, COLORREF color, int thickness, int opacity,
                   const std::wstring& label, bool showLabel, int labelHeight, int labelFontSize,
                   bool labelTextColorAuto, COLORREF labelTextColor);