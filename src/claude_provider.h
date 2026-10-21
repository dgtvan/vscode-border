#pragma once

#include "ai_provider.h"

// Claude Code's AiProvider implementation. Installs hooks into
// %USERPROFILE%\.claude\settings.json (via claude_status_hook.ps1, invoked
// on several of Claude Code's lifecycle events -- see that script and
// README.md's "AI status indicator" section) and reads back the
// per-session status files those hooks write to bin\claude_status\.
//
// Every other account in config.ini's user profiles (see account.h) gets
// the same, through a hidden status helper -- this exe run with
// kClaudeStatusHelperArg as that account, with its password saved in
// Credential Manager -- since this account can't read or write that
// account's profile. The helper copies the hook script into the account's
// own %LOCALAPPDATA%\vscode-border\ and installs the hooks pointing at that
// copy, so its status files land there, then reads them on the same rules
// as this account's and writes each change to a pipe.
class ClaudeProvider : public AiProvider {
public:
    const char* Id() const override { return "claude"; }
    void SyncInstallation(bool enabled) override;
    std::vector<AiSessionStatus> LoadStatuses() override;
};

// Command-line switch that runs the exe as a status helper (see
// RunClaudeStatusHelper), followed by "install" or "uninstall", and
// "verbose" for config.ini's verbose_logging.
extern const wchar_t* const kClaudeStatusHelperArg;

// Entry point of the status helper. `install`: installs the hooks for the
// account this runs as, then reports its sessions on stdout until stdin
// closes (the app exited or stopped it). Otherwise removes them and exits.
// Its log lines go to stdout too, for the app to log: its own log files
// would be the wrong ones.
int RunClaudeStatusHelper(bool install, bool verbose);
