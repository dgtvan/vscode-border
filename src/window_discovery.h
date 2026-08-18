#pragma once

#include <windows.h>

#include <string>

// Resolves hwnd's owning process's image file name (e.g. "Code.exe").
bool GetProcessImageName(HWND hwnd, std::wstring& outName);

bool IsVSCodeExe(const std::wstring& name);

// Visible, unowned, non-tool top-level window with a Chrome_WidgetWin_1
// class and a non-empty title -- the cheap, in-process filters applied
// before paying for a process query.
bool IsCandidateTopLevelWindow(HWND hwnd);

// True visible bounds of hwnd (DWM extended frame bounds, falling back to
// GetWindowRect).
bool GetVisibleWindowRect(HWND hwnd, RECT& out);
