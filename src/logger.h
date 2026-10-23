#pragma once

#include <string>

// Writes a timestamped line to the current session's log file, under
// logs\ next to the exe (same folder as config.ini). Each app startup
// begins a new file rather than truncating -- see logger.cpp for the
// naming scheme and rotation/retention rules. printf-style formatting,
// wide strings.
void Log(const wchar_t* fmt, ...);

// Same formatting, but appends to an in-memory ring buffer instead of
// touching disk -- for use in latency-sensitive hot paths (e.g. per-event
// tracing) where even a single fflush()/syscall can be enough to perturb a
// race you're trying to observe. Call FlushLogBuffer() periodically (or on
// exit) to actually write the buffered lines out.
void LogFast(const wchar_t* fmt, ...);
void FlushLogBuffer();

// Two log tiers built on top of the above, distinguished by whether a user
// needs to opt in to see them:
//
// - LogDiag/LogFastDiag: diagnostic detail, off by default -- only written
//   when config.ini's verbose_logging is true. Use for anything
//   high-volume or only meaningful while actively debugging (per-event
//   tracing, cache hit/miss detail). Equivalent to
//   `if (g_config.verboseLogging) Log(...)` / `LogFast(...)`, just without
//   repeating the guard at every call site.
// - LogWarn: always on, prefixed "[WARN]" so it's easy to grep for. Use
//   for conditions a user might want to know about even without turning on
//   verbose logging -- e.g. a VS Code window's title didn't match any
//   recognized shape (see window_title.cpp), which usually means a
//   workspace-level window.title override is fighting this app's label
//   feature.
void LogDiag(const wchar_t* fmt, ...);
void LogFastDiag(const wchar_t* fmt, ...);
void LogWarn(const wchar_t* fmt, ...);

// True once LogWarn has fired during the current run (always false right
// after a fresh launch -- this flag itself isn't persisted, only what it
// gates: the current run's warning badge). Drives the tray icon's warning
// badge in vscode_border.cpp; restarting the app is what clears it.
bool HasWarnings();

// Full path to the on-disk log file currently being written to. Wired to
// the tray menu's "Open Log Folder".
std::wstring GetLogFilePath();
