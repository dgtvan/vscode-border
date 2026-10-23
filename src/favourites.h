#pragma once

#include <windows.h>

#include <string>
#include <vector>

// One user-saved project/folder, offered from the project list HUD's "+"
// new-window button's menu -- clicking it opens `path` in a
// brand-new VS Code window (see vscode_cli.h's OpenNewVSCodeWindow) as
// Claude profile `profile`. A favourite is the folder, whatever profile its
// window runs -- VS Code keeps one window per folder -- and `profile` is
// just what it opens as: the one it was last seen open as (see
// RefreshFavourite).
struct FavouriteProject {
    std::wstring label; // display text shown in the favourites menu -- the item's label (alias, if any)
                        // as of the last time this path was seen open as a hub item (see
                        // RefreshFavouriteLabel), not necessarily its live value if the project isn't
                        // currently open
    std::wstring path;  // absolute folder path passed to the VS Code CLI shim
    std::wstring profile; // Claude profile id to open it as (see claude_profile.h), empty for the default --
                          // the same convention as ProjectListHudEntry::profile
};

// Loads the persisted favourites list, in the order they were added.
std::vector<FavouriteProject> LoadFavourites();

// True if `path` is saved as a favourite (case-insensitive).
bool IsFavourite(const std::wstring& path);

// Adds `path` (labelled `label`, opening as Claude profile `profile`) as a
// favourite and persists immediately to favourites.ini next to config.ini.
// No-op if `path` is empty or already a favourite.
void AddFavourite(const std::wstring& label, const std::wstring& path, const std::wstring& profile);

// Removes the favourite at `path` (case-insensitive match) and persists
// immediately. No-op if not present.
void RemoveFavourite(const std::wstring& path);

// Updates the stored label and profile of the favourite at `path`
// (case-insensitive match) to those of its open window, persisting
// immediately if either differs. No-op if `path` isn't a favourite. Lets a
// favourite's menu entry track its live hub item's current label (title
// reparse, alias change, ...) instead of staying frozen at whatever label
// was captured when it was added -- see FavouriteProject::label's comment
// -- and open as the profile the folder was last used with.
void RefreshFavourite(const std::wstring& path, const std::wstring& profile, const std::wstring& label);

// Opens every saved favourite whose folder isn't already open, each in its
// own new VS Code window as its own profile. Called from the hub "+"
// button's Favourites > Open All (never automatically at startup).
// `runningWindow` (null if no VS Code window is running) picks which VS Code
// install's CLI shim is used (see vscode_cli.h's ResolveVSCodeCliShim) for
// the Windows folders; logs a warning and skips those if no shim can be
// found. `reason` tags the log lines and the focus-trace launch record.
//
// `alreadyOpenFolders` is the resolved folders of the open VS Code windows
// (the hub entries' paths) -- a favourite counts as open when its folder is,
// whichever profile that window runs: VS Code keeps one window per folder.
// Skipping those matters for more than saving a redundant launch:
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
void OpenAllFavourites(HWND runningWindow, const std::vector<std::wstring>& alreadyOpenFolders, const wchar_t* reason);
