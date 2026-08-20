#pragma once

#include <windows.h>

// Returns a copy of `baseIcon` (rendered at `size`x`size`) with a small
// yellow "!" glyph blended onto its bottom-right corner -- used to badge the
// tray icon when HasWarnings() is true (see UpdateTrayIconWarningState in
// vscode_border.cpp). Returns nullptr on failure; caller should keep
// showing whatever icon it already has in that case.
HICON CreateWarningBadgedIcon(HICON baseIcon, int size);
