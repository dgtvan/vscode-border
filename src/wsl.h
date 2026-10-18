#pragma once

#include <windows.h>

#include <set>
#include <string>
#include <vector>

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

// Size (px, square) and trailing gap of the WSL icon (Tux, assets/wsl.ico)
// drawn in front of a WSLg window's label -- on its border's label chip and
// its hub item alike, whichever distro the window runs in.
const int kWslIconSize = 14, kWslIconGap = 4;

// Runs wsl.exe with `args` (a command-line tail, e.g. L"--list --running"),
// feeding it `input` and collecting its stdout (stderr is dropped). True if it ran and
// exited 0 within 10s.
bool RunWslExe(const std::wstring& args, const std::string& input, std::string* output);

// Runs `argv` inside the distro as its default user (wsl.exe -d <distro> -e
// ...) and waits for it, as RunWslExe. Used instead of the \\wsl.localhost
// share, which a distro can have switched off (e.g. with interop disabled),
// while wsl.exe from Windows always works. Only call for a distro that's
// running -- this would start a stopped one.
bool RunInWsl(const std::wstring& distro, const std::vector<std::wstring>& argv, const std::string& input,
              std::string* output);

// Starts `argv` inside the distro and returns without waiting -- for things
// that keep running, like `code -n <folder>`.
bool LaunchInWsl(const std::wstring& distro, const std::vector<std::wstring>& argv);

// The command (for LaunchInWsl) that opens `linuxPath` in a new Linux VS
// Code window. Linux `code` run under WSL otherwise stops at a "use Windows
// VS Code instead -- continue anyway? [y/N]" prompt nobody can answer.
// Adds the distro's `wsl_vscode_extra_args.<distro>` arguments from
// config.ini, if any, right after `code`.
std::vector<std::wstring> LinuxVSCodeOpenCommand(const std::wstring& distro, const std::wstring& linuxPath);

// Distros running right now. Doesn't start anything.
std::set<std::wstring> RunningWslDistros();

// WSLg doesn't pass a Windows-side resize of its window on to the Linux app
// (msrdc only reports the end of its own move/size loop, and Weston ignores
// the size even then -- microsoft/wslg#22), so moving/resizing one from here
// leaves the app drawing at its old size inside a bigger or smaller frame.
// With patched WSLg modules installed (weston-mirror PR #176 plus a control
// FIFO, from Mihir-Null/wslg-resize-fix), Weston listens on
// /mnt/wslg/runtime-dir/wslg-window-ctl for "move <id> <l> <t> <r> <b>" lines
// and handles each exactly like a real move from Windows.
//
// True if `distro`'s WSLg has that control FIFO -- checked through wsl.exe,
// cached, and re-checked at most once a minute while it isn't there (e.g.
// until WSL is restarted with the patched modules).
bool WslgWindowControlAvailable(const std::wstring& distro);

// Tells `distro`'s Weston that WSLg window `hwnd` now covers `windowRect`
// (its window rect, shadow margin included), so the Linux app is moved and
// resized to match. Through one long-lived relay process per distro (a
// wsl.exe copying stdin into the FIFO), so it's cheap enough to call on
// every step of a drag. No-op if the control FIFO isn't available.
void SendWslgWindowRect(HWND hwnd, const std::wstring& distro, const RECT& windowRect);

// A Linux path inside `distro` as the Windows path to it,
// \\wsl.localhost\<distro>\..., and back. The Windows form is what a WSL
// window's folder is known by on the Windows side (hub item paths,
// favourites); it may not actually be reachable (the share can be off).
std::wstring WslUncPath(const std::wstring& distro, const std::wstring& linuxPath);
bool ParseWslUncPath(const std::wstring& path, std::wstring* distro, std::wstring* linuxPath);

// The WSL icon as a 32bpp premultiplied-alpha bitmap at the system's small
// icon size -- for a menu item's MENUITEMINFO::hbmpItem. Created once and
// kept for the life of the process (a menu only borrows it); null if the
// icon resource can't be loaded.
HBITMAP GetWslIconMenuBitmap();

// Composites the WSL icon (kWslIconSize square) onto an opaque 32bpp surface
// with its top-left at (left, top), clipped to `clip` and to the surface.
void BlendWslIcon(UINT32* pixels, int surfaceWidth, int surfaceHeight, int left, int top, const RECT& clip);
