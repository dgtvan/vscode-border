#pragma once

#include <string>

// Aliases (and the project list HUD's manual order -- see
// project_list_order.h) are keyed by a window's *alias key*: normally its
// raw label text, so an alias follows the folder/repo across window
// restarts and app restarts. If two tracked windows ever produce the
// identical label, aliasing one aliases both.
//
// A window with no folder or repo open at all (raw label "(no folder)")
// has nothing stable to key on -- every such window shares that same
// label -- so it gets a session key from SessionAliasKey instead, unique
// to that one window for as long as it's tracked. Session-keyed aliases
// live in memory only: never written to label_aliases.ini (or
// project_list_order.ini), dropped by ForgetSessionAlias when the window
// closes, and untouched by ReloadAliases.

// The session key for the tracked window with this trackSeq (see
// tracking.cpp's TrackedWindow::trackSeq -- never reused within a run,
// unlike an HWND). Contains a ':', which can't appear in a Windows folder
// name or a git branch name, so it never collides with a real label.
std::wstring SessionAliasKey(long long trackSeq);

// Whether `key` came from SessionAliasKey (i.e. must not be persisted).
bool IsSessionAliasKey(const std::wstring& key);

// Applies any user-set alias for `key` (see SetAlias), or returns
// `fallback` (the raw label) unchanged if none is set.
std::wstring ResolveAlias(const std::wstring& key, const std::wstring& fallback);

// Sets (or, if `alias` is empty after trimming, removes) the alias for
// `key`, and updates the in-memory cache so ResolveAlias reflects it right
// away. A non-session key is also persisted immediately to
// label_aliases.ini next to config.ini.
void SetAlias(const std::wstring& key, const std::wstring& alias);

// Drops any session alias for `key` -- called when its window is untracked.
void ForgetSessionAlias(const std::wstring& key);

// Re-reads label_aliases.ini from disk, discarding the in-memory cache of
// persisted aliases -- mirrors config.ini's "Reload Config" for manual
// edits to the alias file. Session aliases are kept.
void ReloadAliases();
