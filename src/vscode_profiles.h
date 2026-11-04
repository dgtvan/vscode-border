#pragma once

#include <string>
#include <vector>

// VS Code's own window profiles (`code --profile <name>`), which are what
// actually keep one assistant per window: a profile carries its own enabled
// extensions and its own settings (so its own
// `terminal.integrated.env.windows`), and -- unlike `--disable-extension`,
// which VS Code's help calls "not persisted and ... effective only when the
// command opens a new window" -- it survives a window reload and is
// inherited by windows VS Code opens by itself.
//
// VS Code records, in `<user data>\globalStorage\storage.json`, which
// profile each folder was last opened with (`profileAssociations.workspaces`
// maps a folder URI to a profile's location id) and which profiles exist
// (`userDataProfiles`, location id -> name). That association is the only
// evidence about a window VS Code opened itself: it has no launch record
// here, and its environment is the VS Code process's, not the one this app
// passed for the window it was opened from.

// The name of the VS Code profile `folder` is associated with -- empty when
// it has none, is on the default profile, or VS Code's state can't be read.
// Reparsed only when storage.json is rewritten; stable and Insiders are both
// consulted, most recently written first.
std::wstring VSCodeProfileForFolder(const std::wstring& folder);

// Whether VS Code already has a profile called `name`. Launching with
// `--profile` for a name VS Code doesn't know would silently create an
// empty profile (no extensions, no settings), so a misspelt
// profile_<n>_vscode_profile must not be passed through.
bool VSCodeProfileExists(const std::wstring& name);

// The parse behind both, exposed for tests: `storageJson` is one
// storage.json's contents.
std::wstring ParseVSCodeProfileForFolder(const std::string& storageJson, const std::wstring& folder);
std::vector<std::wstring> ParseVSCodeProfileNames(const std::string& storageJson);
