#pragma once

#include <windows.h>

#include <string>

// Resolves hwnd's owning process's image file name (e.g. "Code.exe").
bool GetProcessImageName(HWND hwnd, std::wstring& outName);

bool IsVSCodeExe(const std::wstring& name);

// Whether exeName (hwnd's process image name) is the right process for a
// VS Code window of hwnd's kind: Code.exe for a Windows VS Code window,
// WSL's msrdc.exe for a WSLg one (Linux VS Code inside a distro -- see
// wsl.h).
bool IsVSCodeWindowProcess(HWND hwnd, const std::wstring& exeName);

// Visible, unowned, non-tool top-level window with a Chrome_WidgetWin_1
// class and a non-empty title (or a WSLg RAIL_WINDOW whose title marks it
// as VS Code -- see wsl.h) -- the cheap, in-process filters applied
// before paying for a process query.
bool IsCandidateTopLevelWindow(HWND hwnd);

// True visible bounds of hwnd (DWM extended frame bounds, falling back to
// GetWindowRect; for a WSLg window, its window rect minus Linux VS Code's
// shadow margin -- see wsl.h's MeasureWslgShadowInsets).
bool GetVisibleWindowRect(HWND hwnd, RECT& out);

// A WSLg window's shadow margin as last measured while it wasn't maximized
// (by GetVisibleWindowRect), or 0 if not known yet.
int GetWslgShadowMargin(HWND hwnd);

// Drops what GetVisibleWindowRect remembers about hwnd -- call once it's
// no longer tracked.
void ForgetVisibleWindowRect(HWND hwnd);
