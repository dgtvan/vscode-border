#pragma once

#include <string>

// Given a git repo name as shown by VS Code's ${activeRepositoryName} (which,
// inside a git worktree checkout, is actually the *worktree's* folder name,
// not the main repo's), tries to resolve the real main repo name.
//
// Relies on the common ".worktrees" convention: worktrees living at
// "<mainRepoName>.worktrees\<worktreeName>". The mapping is discovered by
// scanning VS Code's own recently-opened-folder records
// (workspaceStorage\*\workspace.json) once and caching the result in
// memory, since scanning is comparatively slow and the result is the same
// on every repaint.
//
// Returns an empty string if no worktree mapping is found for `repoName`
// (caller should keep showing `repoName` unchanged in that case).
std::wstring ResolveMainRepoName(const std::wstring& repoName);

// Given the leaf name of the folder a window has open (its title's
// ${folderName}), looks up the real absolute path VS Code has it open at,
// via the same workspaceStorage scan ResolveMainRepoName uses -- for every
// entry, not just worktree ones. When several recorded folders share that
// leaf name, `repoName` (the title's raw ${activeRepositoryName}, empty
// outside a git repo) and how recently VS Code used each one pick between
// them -- see the ranking in the definition. Returns an empty string if no
// matching entry is found (e.g. a plain non-git folder VS Code has never
// recorded, or a multi-root workspace, which workspace.json doesn't expose
// as a single folder path). `wslDistro` (a WSLg window -- see wsl.h) looks
// in that distro's Linux VS Code records instead, via wsl.exe, and returns
// the folder's \\wsl.localhost\<distro>\... form (see wsl.h's WslUncPath).
// `account` (a window running as another Windows account, see account.h)
// looks in that account's own VS Code records -- read from its profile
// folder where this account may, else by a helper run as that account
// (with its password saved in Credential Manager), whose answer a later
// call picks up -- falling back to this account's while those have no
// folder of that name (or can't be read).
std::wstring ResolveFolderPath(const std::wstring& name, const std::wstring& repoName,
                               const std::wstring& wslDistro = L"", const std::wstring& account = L"");

// Rescans from disk right away. Wired up to the tray's "Reload Config"
// action so newly created worktrees can be picked up without restarting
// the app.
void RefreshWorktreeCache();

// Marks the cache stale so the next ResolveMainRepoName/ResolveFolderPath
// call rescans -- for when a window opens a different folder, which may be
// one VS Code recorded after the last scan under a leaf name the cache
// already holds for some other folder (so no lookup would ever miss and
// trigger a rescan by itself).
void InvalidateWorktreeCache();

// Command-line switch that runs the exe as the one-shot helper listing the
// folders VS Code has recorded for the account it runs as -- run as another
// account by ResolveFolderPath (see RunListVSCodeFolders).
extern const wchar_t* const kListVSCodeFoldersArg;

// Entry point of that helper: writes one "<last used>\t<path>" line (UTF-8,
// last used as a FILETIME) per recorded folder to stdout. Never logs -- it
// runs as another account.
int RunListVSCodeFolders();
