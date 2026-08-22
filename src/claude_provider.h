#pragma once

#include "ai_provider.h"

// Claude Code's AiProvider implementation. Installs hooks into
// %USERPROFILE%\.claude\settings.json (via claude_status_hook.ps1, invoked
// on several of Claude Code's lifecycle events -- see that script and
// README.md's "AI status indicator" section) and reads back the
// per-session status files those hooks write to bin\claude_status\.
class ClaudeProvider : public AiProvider {
public:
    const char* Id() const override { return "claude"; }
    void SyncInstallation(bool enabled) override;
    std::vector<AiSessionStatus> LoadStatuses() override;
    void ClearStatus() override;
};
