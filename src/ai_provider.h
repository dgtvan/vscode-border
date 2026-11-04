#pragma once

#include "config.h"

#include <string>
#include <vector>

// One reported session's status from an AI coding assistant, regardless of
// which provider produced it.
struct AiSessionStatus {
    std::wstring status; // "working" | "attention" | "waiting"
    std::wstring cwd;
    std::wstring home; // the home directory, where a session started in a window with no folder open
                       // runs (see tracking.cpp's ComputeAiStatus)
    std::string provider; // which provider reported it, stamped by the caller (tracking.cpp) so a
                          // window running one assistant never shows the other's status
};

// One AI coding assistant integration (Claude Code and Codex today,
// Copilot planned -- see config.ini's ai_indicator_provider). Each
// provider owns both sides of reporting its own status:
// installing/removing whatever hook/config it needs in that tool's own
// settings, and reading back whatever it reported. The *mechanism* for
// both is entirely up to the provider -- Claude Code uses hooks plus
// per-session status files, Codex needs nothing installed at all and is
// read from the session files it already writes (codex_provider.h) -- but
// every caller (tracking.cpp, the tray menu) sees the same shape
// regardless of which provider it is talking to.
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
    // Responsible for its own stale/abandoned-state cleanup (e.g. a
    // session whose terminal was killed abruptly) -- there's no separate
    // manual recovery path, so a provider can't leave that to the user.
    virtual std::vector<AiSessionStatus> LoadStatuses() = 0;
};

// Returns the provider for `id` (e.g. "claude", "codex"), or nullptr if
// `id` isn't implemented yet (e.g. "copilot" today -- accepted in config
// but not backed by a real provider until that integration exists).
AiProvider* GetAiProvider(const std::string& id);

// Every implemented provider, regardless of whether it's currently enabled
// in config. Used internally by SyncAllAiProviders/GetAiProvider.
std::vector<AiProvider*> GetAllAiProviders();

// Syncs every provider's installation state to match `cfg`: installs (or
// leaves installed) whichever providers are both enabled and listed in
// cfg.aiIndicatorProviders, uninstalls everything else. Call at startup
// and on every config reload.
void SyncAllAiProviders(const Config& cfg);
