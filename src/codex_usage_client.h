#pragma once

#include "codex_usage.h"

// Main thread: resolve the native CLI and capture a profile's environment.
// Workers use these copies, never the reloadable global config.
std::wstring FindCodexUsageExecutable();
std::wstring CodexUsageEnvironment(const std::wstring& configDir);

// Worker thread: start a hidden, short-lived app-server over private stdio,
// read account metadata and limits, then shut it down. Stop cancels promptly.
bool FetchCodexUsage(HANDLE stop, const std::wstring& exe, const std::wstring& configDir,
                     std::wstring environment, CodexUsageSnapshot& snapshot, std::wstring& error,
                     bool& transientError);
