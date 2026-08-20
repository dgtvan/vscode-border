#include "logger.h"
#include "config.h"
#include "file_util.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <io.h>
#include <share.h>
#include <string>

static FILE* g_logFile = nullptr;
static std::wstring g_logPath;
static bool g_hasWarnings = false;

static void OpenLogFile() {
    if (g_logPath.empty()) {
        g_logPath = GetExeDir() + L"\\vscode_border.log";
    }
    // Start a fresh log each run instead of appending forever -- this app
    // is meant to run continuously via autostart, so a log that grows
    // across every single launch over weeks/months isn't acceptable. Since
    // every run starts with an empty log anyway, HasWarnings() (below)
    // intentionally only reflects warnings from the current run -- nothing
    // is carried over from whatever this truncates away, since there'd be
    // nothing left to look at. Opened with deny-write sharing (not the CRT
    // default of deny-all) so the log can still be tailed by another
    // process while this one keeps writing to it.
    g_logFile = _wfsopen(g_logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
}

static FILE* GetLogFile() {
    if (!g_logFile) OpenLogFile();
    return g_logFile;
}

static void LogVPrefixed(const wchar_t* prefix, const wchar_t* fmt, va_list args) {
    FILE* f = GetLogFile();
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d] %ls", st.wHour, st.wMinute, st.wSecond, prefix);
    vfwprintf(f, fmt, args);
    fwprintf(f, L"\n");
    fflush(f);
}

void Log(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogVPrefixed(L"", fmt, args);
    va_end(args);
}

void LogWarn(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogVPrefixed(L"[WARN] ", fmt, args);
    va_end(args);
    g_hasWarnings = true;
}

bool HasWarnings() { return g_hasWarnings; }

void LogDiag(const wchar_t* fmt, ...) {
    if (!g_config.verboseLogging) return;
    va_list args;
    va_start(args, fmt);
    LogVPrefixed(L"", fmt, args);
    va_end(args);
}

static std::deque<std::wstring> g_ringBuffer;
static const size_t kRingBufferCap = 2000;

void LogFast(const wchar_t* fmt, ...) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[600];
    int prefixLen = _snwprintf_s(line, 600, _TRUNCATE, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
                                  st.wMilliseconds);
    if (prefixLen < 0) prefixLen = 0;

    // Explicit-size form, not the array-size-deducing template overload --
    // that one produced garbled/truncated-to-nothing output under this
    // MinGW toolchain.
    va_list args;
    va_start(args, fmt);
    vswprintf_s(line + prefixLen, 600 - prefixLen, fmt, args);
    va_end(args);

    if (g_ringBuffer.size() >= kRingBufferCap) g_ringBuffer.pop_front();
    g_ringBuffer.emplace_back(line);
}

void LogFastDiag(const wchar_t* fmt, ...) {
    if (!g_config.verboseLogging) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t line[600];
    int prefixLen = _snwprintf_s(line, 600, _TRUNCATE, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
                                  st.wMilliseconds);
    if (prefixLen < 0) prefixLen = 0;

    va_list args;
    va_start(args, fmt);
    vswprintf_s(line + prefixLen, 600 - prefixLen, fmt, args);
    va_end(args);

    if (g_ringBuffer.size() >= kRingBufferCap) g_ringBuffer.pop_front();
    g_ringBuffer.emplace_back(line);
}

// Truncating only happens once, at process startup (see OpenLogFile) --
// with no further cap, a copy left running for weeks via autostart (see
// docs/AUTOSTART.md) would grow the log unbounded between restarts. This
// is the safety net for that: checked on every FlushLogBuffer() call
// (already on the rescan timer, so every rescan_interval_ms), it re-opens
// (truncating) once the file crosses kMaxLogSizeBytes.
static const __int64 kMaxLogSizeBytes = 5LL * 1024 * 1024; // 5 MB

static void TruncateLogIfTooLarge() {
    if (!g_logFile) return;
    __int64 size = _filelengthi64(_fileno(g_logFile));
    if (size < kMaxLogSizeBytes) return;
    fclose(g_logFile);
    g_logFile = nullptr;
    OpenLogFile();
    Log(L"=== log auto-truncated (was %lld bytes) ===", size);
}

void FlushLogBuffer() {
    TruncateLogIfTooLarge();
    if (g_ringBuffer.empty()) return;
    FILE* f = GetLogFile();
    if (!f) return;
    for (const std::wstring& line : g_ringBuffer) fwprintf(f, L"%ls\n", line.c_str());
    fflush(f);
    g_ringBuffer.clear();
}

std::wstring GetLogFilePath() {
    if (!g_logFile) OpenLogFile(); // lazily resolves g_logPath as a side effect
    return g_logPath;
}
