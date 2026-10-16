#pragma once

#include <windows.h>

#include <string>

// Linux VS Code installed inside a WSL distro and shown on the Windows
// desktop through WSLg is a different kind of window than Windows VS Code:
// it belongs to WSL's msrdc.exe (one process hosting *every* WSLg app, e.g.
// a Linux browser too), has the RAIL_WINDOW class, and WSLg appends the
// distro name to its title: "<VS Code's own title> (<distro>)". So unlike
// Code.exe windows, which VS Code app owns a WSLg window can only be told
// from its title.

// The window class WSLg gives every Linux app window it shows.
extern const wchar_t* const kWslgWindowClass;

// True for WSL's msrdc.exe (the WSLg host process).
bool IsWslgHostExe(const std::wstring& name);

// Splits a WSLg title into VS Code's own title and the distro name WSLg
// appended, e.g. "file - folder - Visual Studio Code (Ubuntu)" ->
// "file - folder - Visual Studio Code" + "Ubuntu". False (outputs
// untouched) unless the title has that suffix *and* what's left still
// ends with VS Code's app name -- the only thing that marks a WSLg window
// as VS Code rather than some other Linux app. Either output may be null.
bool SplitWslgVSCodeTitle(const std::wstring& title, std::wstring* vscodeTitle, std::wstring* distro);

// Linux VS Code draws its own drop shadow when it isn't maximized, inside
// a transparent margin that WSLg includes in the Windows window rect -- so
// the window rect is that much bigger than what's actually visible (32px a
// side on a 96 DPI monitor). Nothing on the Windows side reports that
// margin, so this measures it from the window's own pixels (PrintWindow
// renders it even while covered; the margin comes back pure black): the
// first non-black pixel in from each edge, along a few rows/columns.
// Returns the inset per side in `insets` (left/top/right/bottom), or false
// if it couldn't tell -- e.g. msrdc not responding, or a window whose
// content isn't painted yet.
bool MeasureWslgShadowInsets(HWND hwnd, RECT& insets);

// Maximized, Linux VS Code (custom title bar) asks for the work area grown
// by its shadow margin on every side, so the margin hangs off-screen. But
// while an AppBar shrinks the work area (this app's own fixed hub, for
// one), Windows clamps a maximized WSLg window inside the screen instead,
// and Linux VS Code ends up drawn shifted by its margin: the right/bottom
// edge, with the window buttons, pushed off-screen -- and WSLg's own
// maximize/restore state goes wrong from there. Spots that clamped state
// (maximized, with no margin hanging off the work area's top-left) and puts
// the window back at the work area grown by the margin. Cheap when there's
// nothing to fix; call on every sync of a WSLg window. `knownMargin` is the
// margin already measured while restored (see window_discovery.h's
// GetWslgShadowMargin), or 0 to measure it here -- less reliable, since the
// moment a window was just clamped is also when WSLg may not have repainted
// it yet. True if it moved the window.
bool FixWslgClampedMaximize(HWND hwnd, int knownMargin);

// The distro's icon at `size` x `size` px -- the shortcut.ico WSL keeps in
// the distro's install folder (found via its Lxss registry entry), falling
// back to wsl.exe's own icon. Cached per distro+size for the life of the
// process, so it's cheap to call on every paint; the caller must not
// destroy it. Null only if neither icon can be loaded.
HICON GetWslDistroIcon(const std::wstring& distro, int size);
