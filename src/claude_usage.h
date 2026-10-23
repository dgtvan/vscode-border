#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Claude usage limits per Claude profile (see claude_profile.h) -- the
// session (5-hour) and weekly percentages Claude Code's /usage screen shows
// -- for the hub's usage rings (see project_list_hud.h's
// ProjectListHudUsageRing).
//
// They come from https://api.anthropic.com/api/oauth/usage, called with the
// profile's own Claude Code login token (.credentials.json in its config
// folder). That endpoint isn't a documented API, so every way it can fail is
// reported back as an error instead of a number. The token is never
// refreshed here: that would rotate the refresh token Claude Code itself
// holds and sign it out. When it has expired, the app instead runs Claude
// Code briefly as that profile, in a way that renews the login without
// using any of its quota, and fetches again.
//
// Fetching runs on a worker thread per profile; results reach the main
// thread as `notifyMsg` posted to `notifyWnd`, which calls
// ProcessClaudeUsageResults.

struct ClaudeUsageLimit {
    double percent = -1;    // -1 when the response didn't include this limit
    long long resetsAt = 0; // Unix seconds, 0 if unknown
};

struct ClaudeUsageSnapshot {
    std::wstring profile; // its id (see claude_profile.h), empty for the default
    bool haveData = false; // a fetch has succeeded at least once
    ClaudeUsageLimit session, weekly; // from the last successful fetch
    long long fetchedAt = 0;          // Unix seconds of that fetch
    std::wstring error;               // why the latest attempt failed, empty if it succeeded
    long long errorAt = 0;
};

// Starts, restarts or stops fetching to match g_config (usage_meter_enabled,
// usage_meter_refresh_minutes, the Claude profiles). Call at startup and on
// every config reload; a no-op when nothing relevant changed.
void SyncClaudeUsageMonitor(HWND notifyWnd, UINT notifyMsg);

// Stops every worker. Call on quit.
void StopClaudeUsageMonitor();

// Main thread, on `notifyMsg`: logs what changed since the last call -- a
// failure as a warning, once per distinct error, and its recovery.
void ProcessClaudeUsageResults();

// One snapshot per monitored Claude profile: the default first, then the
// others in slot order. Empty while the monitor is off.
std::vector<ClaudeUsageSnapshot> GetClaudeUsageSnapshots();

// Whether the snapshot's numbers can't be trusted as current: the latest
// fetch failed, or none has succeeded for a while.
bool IsClaudeUsageStale(const ClaudeUsageSnapshot& s);

// Whether the profile's usage is unavailable: a read has been tried and
// there are no current numbers -- the latest one failed, whatever the
// reason, or none has worked for a while. Unlike IsClaudeUsageStale, false
// while the first read is still on its way.
bool IsClaudeUsageUnavailable(const ClaudeUsageSnapshot& s);

// A limit's percent as of now: 0 once its window has reset, even before the
// next fetch says so; -1 when unknown.
double CurrentUsagePercent(const ClaudeUsageLimit& limit);

// Multi-line hover text for one profile's rings.
std::wstring DescribeClaudeUsage(const ClaudeUsageSnapshot& s);
