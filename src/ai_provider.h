#pragma once

#include "config.h"

#include <string>
#include <vector>

// One reported session's status from an AI coding assistant, regardless of
// which provider produced it.
struct AiSessionStatus {
    std::wstring status; // "working" | "attention" | "waiting"
    std::wstring cwd;
};

// One AI coding assistant integration (Claude Code today, Copilot planned
// -- see config.ini's ai_indicator_provider). Each provider owns both
// sides of reporting its own status: installing/removing whatever
// hook/config it needs in that tool's own settings, and reading back
// whatever it reported. The *mechanism* for both is entirely up to the
// provider (Claude Code uses hooks + per-session status files; a future
// Copilot integration might need something else entirely), but every
// caller (tracking.cpp, the tray menu) sees the same shape regardless of
// which provider it's talking to.
class AiProvider {
public:
    virtual ~AiProvider() = default;

    // Matches a token in config.ini's ai_indicator_provider list, e.g. "claude".
    virtual const char* Id() const = 0;

    // Installs (enabled=true) or removes (enabled=false) whatever this
    // provider needs to report status. Called (via SyncAllAiProviders)
    // at startup and on every config reload; expected to be a cheap no-op
    // when already in the desired state.
    virtual void SyncInstallation(bool enabled) = 0;

    // Every currently-reported session, for the caller to correlate to
    // tracked windows by folder (see tracking.cpp's ComputeAiStatus).
    virtual std::vector<AiSessionStatus> LoadStatuses() = 0;

    // Manual recovery for stuck/stale state (wired to the tray's "Clear AI
    // Status" action) -- e.g. deleting leftover status files left behind
    // by a session whose terminal was killed abruptly.
    virtual void ClearStatus() = 0;
};

// Returns the provider for `id` (e.g. "claude"), or nullptr if `id` isn't
// implemented yet (e.g. "copilot" today -- accepted in config but not
// backed by a real provider until that integration exists).
AiProvider* GetAiProvider(const std::string& id);

// Every implemented provider, regardless of whether it's currently enabled
// in config -- for callers (the tray's "Clear AI Status") that need to act
// on all of them.
std::vector<AiProvider*> GetAllAiProviders();

// Syncs every provider's installation state to match `cfg`: installs (or
// leaves installed) whichever providers are both enabled and listed in
// cfg.aiIndicatorProviders, uninstalls everything else. Call at startup
// and on every config reload.
void SyncAllAiProviders(const Config& cfg);
