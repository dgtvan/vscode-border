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

// The `timestamp` of the newest complete line in `region` ("" when it holds
// none), as Codex writes it: `2026-11-04T08:02:45.989Z`. This, not the
// rollout's last-write time, is how fresh the file is: Windows leaves the
// directory entry's timestamp behind while a handle keeps appending, and
// Codex holds its rollout open, so a live rollout's recorded time can be
// over an hour stale while its size and contents are current (measured --
// see docs/CODEX-AI-STATUS.md).
std::string ParseCodexRolloutActivity(const std::string& region, bool startsMidLine);

// Whether `timestamp` (as above) is more than `minutes` old by the system
// clock. False for a timestamp that can't be read, so an unreadable one
// never passes for silence.
bool IsCodexTimestampStale(const std::string& timestamp, unsigned minutes);
