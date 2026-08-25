#pragma once

#include <string>
#include <vector>

// One user-saved project/folder, offered from the project list HUD's "+"
// new-window button's right-click menu -- clicking it opens `path` in a
// brand-new VS Code window (see project_list_hud.cpp's OpenFavouriteProject).
struct FavouriteProject {
    std::wstring label; // display text shown in the favourites menu -- a snapshot of the item's label
                        // (alias, if any) at the time it was added, not re-resolved against
                        // label_alias.h afterwards
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
