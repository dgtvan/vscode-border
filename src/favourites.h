#pragma once

#include <windows.h>

#include <string>
#include <vector>

// One user-saved project/folder, offered from the project list HUD's "+"
// new-window button's right-click menu -- clicking it opens `path` in a
// brand-new VS Code window (see vscode_cli.h's OpenNewVSCodeWindow). Also
// listed under the tray icon's Favourites submenu.
struct FavouriteProject {
    std::wstring label; // display text shown in the favourites menu -- the item's label (alias, if any)
                        // as of the last time this path was seen open as a hub item (see
                        // RefreshFavouriteLabel), not necessarily its live value if the project isn't
                        // currently open
    std::wstring path;  // absolute folder path passed to the VS Code CLI shim
};

// Loads the persisted favourites list, in the order they were added.
std::vector<FavouriteProject> LoadFavourites();

// True if `path` is already saved as a favourite (case-insensitive match).
bool IsFavourite(const std::wstring& path);

// Adds `path` (labelled `label`) to the favourites list and persists
// immediately to favourites.ini next to config.ini. No-op if `path` is
// empty or already present.
void AddFavourite(const std::wstring& label, const std::wstring& path);

// Removes the favourite at `path` (case-insensitive match) and persists
// immediately. No-op if not present.
void RemoveFavourite(const std::wstring& path);

// Updates the stored label for the favourite at `path` (case-insensitive
// match) to `label` and persists immediately, if it differs from what's
// currently stored. No-op if `path` isn't a favourite or the label already
// matches. Lets a favourite's menu entry track its live hub item's current
// label (title reparse, alias change, ...) instead of staying frozen at
// whatever label was captured when it was added -- see FavouriteProject::
// label's comment.
void RefreshFavouriteLabel(const std::wstring& path, const std::wstring& label);

// True if `path` is one of `openFolderPaths` (what tracking.h's
// GetTrackedFolderPaths returns) -- case-insensitive, tolerant of a
// trailing separator on either side.
bool IsFavouriteOpen(const std::wstring& path, const std::vector<std::wstring>& openFolderPaths);

// Opens every saved favourite that isn't already open, each in its own new
// VS Code window. Called once from vscode_border.cpp's wWinMain at startup
// and from the tray icon's Favourites > Open All. `runningWindow` (null if
// no VS Code window is running) picks which VS Code install's CLI shim is used (see vscode_cli.h's
// ResolveVSCodeCliShim); logs a warning and no-ops if no shim can be found.
// `reason` tags the log lines and the focus-trace launch record.
//
// `alreadyOpenFolderPaths` is what tracking.h's GetTrackedFolderPaths
// returns. Skipping those matters for more than saving a redundant launch:
// `code -n <path>` on a folder VS Code already has open does NOT create a
// second window, it *activates the existing one*. Firing that at several
// already-open favourites in a row makes them fight over the foreground,
// and every window that loses the race is left showing the highlighted
// wants-attention state on its taskbar button -- i.e. the whole row of VS
// Code windows lights up. That was a real, reproducible bug
// (see docs/ARCHITECTURE.md's "Focus tracing" section); this parameter is
// the fix, not an optimisation.
//
// The list is best-effort: a window whose folder path could not be resolved
// is not in it, so its favourite is still launched, which is exactly the
// pre-existing behaviour. Erring that way keeps the feature working (the
// project does open) instead of silently skipping something the user
// expects to see.
void OpenAllFavourites(HWND runningWindow, const std::vector<std::wstring>& alreadyOpenFolderPaths,
                       const wchar_t* reason);
