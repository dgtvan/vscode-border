#pragma once

#include <string>

// Applies any user-set alias for `label` (see SetAlias), or returns `label`
// unchanged if none is set. Matches by exact label text -- if two tracked
// windows ever produce the identical label, aliasing one aliases both.
std::wstring ResolveAlias(const std::wstring& label);

// Sets (or, if `alias` is empty after trimming, removes) the alias for
// `label`, persists immediately to label_aliases.ini next to config.ini,
// and updates the in-memory cache so ResolveAlias reflects it right away.
void SetAlias(const std::wstring& label, const std::wstring& alias);

// Re-reads label_aliases.ini from disk, discarding the in-memory cache --
// mirrors config.ini's "Reload Config" for manual edits to the alias file.
void ReloadAliases();
