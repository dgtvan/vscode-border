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

// Forces the next call to ResolveMainRepoName to rescan from disk. Wired up
// to the tray's "Reload Config" action so newly created worktrees can be
// picked up without restarting the app.
void RefreshWorktreeCache();
