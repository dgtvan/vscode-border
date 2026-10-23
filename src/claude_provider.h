#pragma once

#include "ai_provider.h"

// Claude Code's AiProvider implementation. Installs hooks into every Claude
// profile's settings.json (see claude_profile.h) -- claude_status_hook.ps1,
// invoked on several of Claude Code's lifecycle events (see that script and
// README.md's "AI status indicator" section) -- and reads back the
// per-session status files those hooks write to bin\claude_status\, one
// folder for every profile's sessions.
class ClaudeProvider : public AiProvider {
public:
    const char* Id() const override { return "claude"; }
    void SyncInstallation(bool enabled) override;
    std::vector<AiSessionStatus> LoadStatuses() override;
};
