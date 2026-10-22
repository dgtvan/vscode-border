#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Claude usage limits per Windows account -- the session (5-hour) and weekly
// percentages Claude Code's /usage screen shows -- for the hub's usage rings
// (see project_list_hud.h's ProjectListHudUsageRing).
//
// They come from https://api.anthropic.com/api/oauth/usage, called with the
// account's own Claude Code login token (.credentials.json in its Claude
// config folder). That endpoint isn't a documented API, so every way it can
// fail is reported back as an error instead of a number. The token is never
// refreshed here: that would rotate the refresh token Claude Code itself
// holds and sign it out. When it has expired, the app instead runs that
// account's Claude Code briefly, in a way that renews the login without
// using any of its quota, and fetches again.
//
// This app's own account is fetched in-process. Another account's token
// file is inside that account's profile, which this account can't read, so
// each user profile (config.ini's user_profile_<N>_account, see account.h)
// gets a hidden helper -- this same exe, run with kClaudeUsageHelperArg as
// that account through its password saved in Credential Manager -- which
// fetches on the same schedule and writes each result to a pipe.
//
// Fetching runs on worker threads; results reach the main thread as
// `notifyMsg` posted to `notifyWnd`, which calls ProcessClaudeUsageResults.

// Command-line switch that runs the exe as the helper (see RunClaudeUsageHelper).
extern const wchar_t* const kClaudeUsageHelperArg;

struct ClaudeUsageLimit {
    double percent = -1;    // -1 when the response didn't include this limit
    long long resetsAt = 0; // Unix seconds, 0 if unknown
};

struct ClaudeUsageSnapshot {
    std::wstring account; // empty for this app's own account
    bool haveData = false; // a fetch has succeeded at least once
    ClaudeUsageLimit session, weekly; // from the last successful fetch
    long long fetchedAt = 0;          // Unix seconds of that fetch
    std::wstring error;               // why the latest attempt failed, empty if it succeeded
    long long errorAt = 0;
};

// Starts, restarts or stops fetching to match g_config (usage_meter_enabled,
// usage_meter_refresh_minutes, the user profiles). Call at startup and on
// every config reload; a no-op when nothing relevant changed.
void SyncClaudeUsageMonitor(HWND notifyWnd, UINT notifyMsg);

// Stops every worker and helper. Call on quit.
void StopClaudeUsageMonitor();

// Main thread, on `notifyMsg`: logs what changed since the last call -- a
// failure as a warning, once per distinct error, and its recovery.
void ProcessClaudeUsageResults();

// One snapshot per monitored account: this app's own first, then the user
// profiles in slot order. Empty while the monitor is off.
std::vector<ClaudeUsageSnapshot> GetClaudeUsageSnapshots();

// Whether the snapshot's numbers can't be trusted as current: the latest
// fetch failed, or none has succeeded for a while.
bool IsClaudeUsageStale(const ClaudeUsageSnapshot& s);

// Whether the account's usage is unavailable: a read has been tried and
// there are no current numbers -- the latest one failed, whatever the
// reason, or none has worked for a while. Unlike IsClaudeUsageStale, false
// while the first read is still on its way.
bool IsClaudeUsageUnavailable(const ClaudeUsageSnapshot& s);

// A limit's percent as of now: 0 once its window has reset, even before the
// next fetch says so; -1 when unknown.
double CurrentUsagePercent(const ClaudeUsageLimit& limit);

// Multi-line hover text for one account's rings.
std::wstring DescribeClaudeUsage(const ClaudeUsageSnapshot& s);

// Entry point of the helper process: fetches every `intervalMinutes`,
// writing one line per result to stdout, until stdin closes (the app exited
// or stopped it). Never logs -- it runs as another account, whose log files
// would be the wrong ones.
int RunClaudeUsageHelper(int intervalMinutes);
