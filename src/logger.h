#pragma once

// Writes a timestamped line to %TEMP%\vscode_border.log (truncated at the
// start of each run). printf-style formatting, wide strings.
void Log(const wchar_t* fmt, ...);

// Same formatting, but appends to an in-memory ring buffer instead of
// touching disk -- for use in latency-sensitive hot paths (e.g. per-event
// tracing) where even a single fflush()/syscall can be enough to perturb a
// race you're trying to observe. Call FlushLogBuffer() periodically (or on
// exit) to actually write the buffered lines out.
void LogFast(const wchar_t* fmt, ...);
void FlushLogBuffer();

