#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "config.h"

// Terminal CLI launches -- the counterpart to vscode_cli.h's window
// launches. Same assistant accounts (see assistant_profile.h) and the same
// per-account environment, but started as a command-line session in a
// terminal instead of as a VS Code window. Nothing here is tracked: a
// terminal window gets no border, no hub item and no AI status; these
// functions only start it.
//
// Provider-neutral by design: everything specific to one assistant's CLI
// lives in one table (AiClis() in ai_cli.cpp) keyed by ProfileType, and the
// account environment comes from assistant_profile.h, which already knows
// each provider's variable. Onboarding another provider's CLI is a row in
// that table with `supported` set -- the menus, account handling and
// terminal hosting below are shared. Claude Code's CLI is supported today;
// Codex's row is there, switched off.

// Whether provider `type` has a CLI this app offers at all -- a supported
// row in the table, regardless of whether it is installed.
bool AiCliSupported(ProfileType type);

// Provider `type`'s CLI command name for logs (e.g. L"claude"), or L"" when
// it has no supported row.
const wchar_t* AiCliCommand(ProfileType type);

// Resolves provider `type`'s CLI executable: on PATH first, then the
// per-user locations its installers use. False when the provider is
// unsupported or nothing is installed. Doesn't log.
bool FindAiCli(ProfileType type, std::wstring& outExePath);

// The configured accounts -- ConfiguredProfileIds() order -- whose provider
// has a CLI that is both supported and installed. This is what the "Start AI
// CLI" / "Open in AI CLI" menus list; empty means they are left out.
std::vector<std::wstring> AiCliProfileIds();

// Whether any supported CLI is installed. Used to decide whether to offer an
// account-less launch when no accounts are configured at all, mirroring how
// "Start VS Code" still appears with no accounts.
bool AnyAiCliInstalled();

// Starts account `profile`'s CLI in a terminal, with `folder` as its working
// directory -- empty (or gone) falls back to the user's profile folder,
// mirroring an empty VS Code window. An empty `profile` is None: the CLI
// keeps whatever account the environment already selects, rather than being
// pointed at an account folder. `reason` tags the log line. Returns whether
// the terminal was launched; logs on failure.
bool OpenAiCliSession(const std::wstring& folder, const wchar_t* reason, const std::wstring& profile = L"");
