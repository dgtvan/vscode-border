#pragma once

#include <string>
#include <vector>

// One user-saved project/folder, offered from the project list HUD's "+"
// new-window button's menu, which opens `path` in a brand-new VS Code window
// (see vscode_cli.h's OpenNewVSCodeWindow) as whichever Claude profile is
// picked there. A favourite is just the folder: it stores no profile, and
// it's a favourite whatever profile its window runs.
struct FavouriteProject {
    std::wstring label; // display text shown in the favourites menu -- the item's label (alias, if any)
                        // as of the last time this path was seen open as a hub item (see
                        // RefreshFavouriteLabel), not necessarily its live value if the project isn't
                        // currently open
    std::wstring path;  // absolute folder path passed to the VS Code CLI shim
    std::wstring aliasKey; // the hub item's alias key (see label_alias.h) as of the same moment as `label`,
                           // so its alias can be set from the favourites menu while the folder isn't open;
                           // empty for a favourite saved before this was recorded and not seen open since
};

// Loads the persisted favourites list, in the order they were added.
std::vector<FavouriteProject> LoadFavourites();

// True if `path` is saved as a favourite (case-insensitive).
bool IsFavourite(const std::wstring& path);

// Adds `path` (labelled `label`, alias key `aliasKey`) as a favourite and
// persists immediately to favourites.ini next to config.ini. No-op if `path`
// is empty or already a favourite.
void AddFavourite(const std::wstring& label, const std::wstring& path, const std::wstring& aliasKey);

// Removes the favourite at `path` (case-insensitive match) and persists
// immediately. No-op if not present.
void RemoveFavourite(const std::wstring& path);

// Updates the stored label and alias key of the favourite at `path`
// (case-insensitive match), persisting immediately if either differs. No-op
// if `path` isn't a favourite. Lets a favourite's menu entry track its live
// hub item's current label (title reparse, alias change, ...) instead of
// staying frozen at whatever label was captured when it was added -- see
// FavouriteProject::label's comment.
void RefreshFavouriteLabel(const std::wstring& path, const std::wstring& label, const std::wstring& aliasKey);
