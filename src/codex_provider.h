#pragma once

#include "ai_provider.h"

#include <string>

// Codex's AiProvider implementation. Read-only, unlike Claude Code's
// (claude_provider.h): nothing is installed into Codex's own configuration,
// so SyncInstallation does nothing at all. Status comes from the session
// rollout files Codex appends under <CODEX_HOME>\sessions\, and liveness
// from the per-thread lock files under <CODEX_HOME>\thread-writer-locks\ --
// see docs/CODEX-AI-STATUS.md for the evidence behind both and for what
// Codex does not report this way (a permission prompt).
class CodexProvider : public AiProvider {
public:
    const char* Id() const override { return "codex"; }
    void SyncInstallation(bool enabled) override;
    std::vector<AiSessionStatus> LoadStatuses() override;
};

// --- The rollout parsing, exposed for tests (tests/codex_usage_tests.cpp) ---

// A rollout's `session_meta` line: the folder the session runs in, its own
// id, and whether it belongs to a subagent rather than to the user (see
// CollectSession for why those are left out).
struct CodexRolloutMeta {
    bool found = false;
    std::wstring sessionId;
    std::wstring cwd;
    bool subagent = false;
};
CodexRolloutMeta ParseCodexRolloutMeta(const std::string& head);

// The newest turn-boundary event in `region` -- "working", "attention",
// "waiting", or "" when it holds none. `startsMidLine` is for a region read
// from the middle of a file: its first (partial) line is then skipped,
// since the nested objects inside a half-line can otherwise read as a
// complete event of their own.
std::wstring ParseCodexRolloutStatus(const std::string& region, bool startsMidLine);
