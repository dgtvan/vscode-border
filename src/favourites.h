#pragma once

#include <windows.h>

#include <string>
#include <vector>

// One user-saved project/folder, offered from the project list HUD's "+"
// new-window button's menu -- clicking it opens `path` in a
// brand-new VS Code window (see vscode_cli.h's OpenNewVSCodeWindow) as
// `account`. A favourite is the folder *and* the account it was saved from,
// so the same folder can be a favourite once per account.
struct FavouriteProject {
    std::wstring label; // display text shown in the favourites menu -- the item's label (alias, if any)
                        // as of the last time this path was seen open as a hub item (see
                        // RefreshFavouriteLabel), not necessarily its live value if the project isn't
                        // currently open
    std::wstring path;  // absolute folder path passed to the VS Code CLI shim
    std::wstring account; // Windows account to open it as (see account.h), empty for this app's own --
                          // the same convention as ProjectListHudEntry::account
};

// A folder open in a tracked VS Code window, and the account that window
// runs as (empty for this app's own).
struct OpenFolder {
    std::wstring path;
    std::wstring account;
};

// Loads the persisted favourites list, in the order they were added.
std::vector<FavouriteProject> LoadFavourites();

// True if `path` is already saved as a favourite for `account` (both
// case-insensitive; empty `account` is this app's own).
bool IsFavourite(const std::wstring& path, const std::wstring& account);

// Adds `path` (labelled `label`) as a favourite for `account` and persists
// immediately to favourites.ini next to config.ini. No-op if `path` is
// empty or already a favourite for that account.
void AddFavourite(const std::wstring& label, const std::wstring& path, const std::wstring& account);

// Removes `account`'s favourite at `path` (case-insensitive match) and
// persists immediately. No-op if not present.
void RemoveFavourite(const std::wstring& path, const std::wstring& account);

// Updates the stored label for `account`'s favourite at `path` (case-insensitive
// match) to `label` and persists immediately, if it differs from what's
// currently stored. No-op if `path` isn't a favourite or the label already
// matches. Lets a favourite's menu entry track its live hub item's current
// label (title reparse, alias change, ...) instead of staying frozen at
// whatever label was captured when it was added -- see FavouriteProject::
// label's comment.
void RefreshFavouriteLabel(const std::wstring& path, const std::wstring& account, const std::wstring& label);

// True if `favourite` is one of `openFolders` -- the same account, and the
// same path case-insensitively, tolerant of a trailing separator on either
// side.
bool IsFavouriteOpen(const FavouriteProject& favourite, const std::vector<OpenFolder>& openFolders);

// Opens every saved favourite that isn't already open, each in its own new
// VS Code window. Called from the hub "+" button's Favourites >
// Open All (never automatically at startup). `runningWindow` (null if
// no VS Code window is running) picks which VS Code install's CLI shim is used (see vscode_cli.h's
// ResolveVSCodeCliShim) for this account's favourites; logs a warning and
// skips those if no shim can be found. Another account's favourite opens
// through account.h's OpenNewVSCodeWindowAs, whose password prompt (if no
// password is saved) is owned by `owner`. `reason` tags the log lines and
// the focus-trace launch record.
//
// `alreadyOpenFolders` is the resolved folders of the open VS Code windows
// (the hub entries' paths and accounts) -- a favourite counts as open only
// when its folder is open as its own account. Skipping those matters for more than saving a redundant launch:
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
void OpenAllFavourites(HWND runningWindow, HWND owner, const std::vector<OpenFolder>& alreadyOpenFolders,
                       const wchar_t* reason);
