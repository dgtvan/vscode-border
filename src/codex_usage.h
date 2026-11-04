#pragma once

#include "config.h"
#include <windows.h>
#include <string>
#include <vector>

struct CodexUsageLimit {
    double percent = -1;
    long long resetsAt = 0;
    int windowMinutes = 0; // use the server's duration, rather than assuming 5 hours / 1 week
};

struct CodexUsageSnapshot {
    std::wstring profile; // shared provider + folder ID
    std::wstring email;
    bool haveData = false;
    CodexUsageLimit primary, secondary; // inner / outer rings
    long long fetchedAt = 0, errorAt = 0;
    std::wstring error;
    bool transientError = false;
};

// Uses codex app-server's account/read and account/rateLimits/read only.
// No threads/turns are started and no model quota is consumed. Codex owns
// authentication; this app never reads or refreshes its tokens itself.
void SyncCodexUsageMonitor(HWND notifyWnd, UINT notifyMsg);
void StopCodexUsageMonitor();
void ProcessCodexUsageResults();
std::vector<CodexUsageSnapshot> GetCodexUsageSnapshots();
bool IsCodexUsageStale(const CodexUsageSnapshot& snapshot);
bool IsCodexUsageUnavailable(const CodexUsageSnapshot& snapshot);
double CurrentCodexUsagePercent(const CodexUsageLimit& limit);
std::wstring CodexUsageProfileName(const CodexUsageSnapshot& snapshot);
std::wstring DescribeCodexUsage(const CodexUsageSnapshot& snapshot);
// Parses the result object from account/rateLimits/read. Exposed separately
// so protocol changes and malformed responses can be checked without a login.
bool ParseCodexRateLimits(const std::string& result, CodexUsageSnapshot& snapshot, std::wstring& error);
