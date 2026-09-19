#pragma once

#include <windows.h>

// A strip that fills the project list HUD's reserved band in fixed mode
// (see SetProjectListHudDocked) and is painted to look like a continuation
// of the taskbar -- so the band reads as part of the taskbar rather than a
// gap showing the wallpaper. Instead of reconstructing the taskbar's look
// from theme settings (dark/light, accent color, transparency, Windows 10
// acrylic vs. Windows 11 Mica, third-party restylers...), it copies what
// the taskbar actually shows on screen: a thin row of pixels just inside
// the taskbar's edge, reduced to one color per column. A solid taskbar
// gives a solid band; a translucent one gives the blurred wallpaper's
// colors column by column, stretched up the band -- a snapshot, not a live
// blur.
//
// Resampled when the backdrop is (re)shown, on any WM_SETTINGCHANGE (theme,
// accent, wallpaper, ...) or display change, and every 30s as a safety
// net -- but never while the cursor is over the taskbar, so a hover
// highlight can't get baked in. When there's nothing to sample (auto-hide
// taskbar), it falls back to a plain dark or light color matching the
// Windows theme.
//
// Absorbs clicks without taking focus. Created topmost, like the taskbar.

HWND CreateTaskbarBackdrop(HINSTANCE hInstance);

// Shows the backdrop over `band` (screen coords), resampling first. Leaves
// its z-order alone -- the HUD owns that (see project_list_hud.cpp's
// RaiseBandWindows), since the two have to move together.
void ShowTaskbarBackdrop(HWND backdrop, const RECT& band);

void HideTaskbarBackdrop(HWND backdrop);
