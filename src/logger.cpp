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
static std::wstring g_logDir;
static bool g_hasWarnings = false;

// Identifies this process's run -- fixed the first time any log line is
// written and never changed again, even across day-rollover/size-rotation
// (see OpenNewLogFile below). Every file name that shares this prefix
// belongs to the same app session; g_seq (1-based, incremented on each
// rotation) orders those files within it. That's the whole naming
// contract: counting distinct session ids across logs\ tells you how many
// sessions there are, and grouping by session id + sorting by seq tells
// you which files belong to which session and in what order, even when a
// session spans multiple calendar days.
static std::wstring g_sessionId;
static int g_seq = 0;
static int g_openedDateKey = -1; // YYYYMMDD of the currently open file

// Old sessions' log files are never deleted individually as they age --
// only pruned in bulk, once per process, so a copy left running for months
// via autostart doesn't accumulate logs\ forever. Long enough to still be
// useful ("the user noticed something was off last week"), short enough to
// stay bounded.
static const int kLogRetentionDays = 14;

// Each file is capped so a single very active day can't grow one file
// without bound; crossing this rotates to a new part of the *same*
// session (g_seq++), it does not start a new session or lose anything.
static const __int64 kMaxLogFileSizeBytes = 5LL * 1024 * 1024; // 5 MB

static int DateKeyOf(const SYSTEMTIME& st) { return st.wYear * 10000 + st.wMonth * 100 + st.wDay; }

static bool IsOlderThanDays(const FILETIME& ft, int days) {
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now, then;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    then.LowPart = ft.dwLowDateTime;
    then.HighPart = ft.dwHighDateTime;
    if (now.QuadPart < then.QuadPart) return false;
    const unsigned long long ticksPerDay = 10000000ULL * 60 * 60 * 24;
    return (now.QuadPart - then.QuadPart) > (unsigned long long)days * ticksPerDay;
}

// Deletes this app's own log files once they're older than
// kLogRetentionDays. Only matches this app's own naming pattern, so it
// never touches anything else that might live in logs\. Called once, right
// before the very first file of a new session is created.
static void PruneOldLogFiles() {
    std::wstring pattern = g_logDir + L"\\vscode_border_*.log";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!IsOlderThanDays(fd.ftLastWriteTime, kLogRetentionDays)) continue;
        DeleteFileW((g_logDir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Opens the next file in the current session -- the very first file of a
// fresh process (assigning a new session id), or the next part when the
// caller is rotating for a new day or an oversized file (bumping g_seq
// within the existing session id). Each resulting path is unique and never
// reused, so this is always a fresh create, never a truncation of
// something already on disk -- an accidental restart can't wipe out a
// previous session's log.
static void OpenNewLogFile() {
    SYSTEMTIME st;
    GetLocalTime(&st);

    if (g_logDir.empty()) {
        g_logDir = GetExeDir() + L"\\logs";
        CreateDirectoryW(g_logDir.c_str(), nullptr);
    }

    if (g_sessionId.empty()) {
        wchar_t idBuf[32];
        swprintf_s(idBuf, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                   st.wSecond);
        g_sessionId = idBuf;
        g_seq = 1;
        PruneOldLogFiles();
    } else {
        g_seq++;
    }
    g_openedDateKey = DateKeyOf(st);

    wchar_t nameBuf[80];
    swprintf_s(nameBuf, L"vscode_border_%ls_%02d.log", g_sessionId.c_str(), g_seq);
    g_logPath = g_logDir + L"\\" + nameBuf;

    // Opened with deny-write sharing (not the CRT default of deny-all) so
    // the log can still be tailed by another process while this one keeps
    // writing to it.
    g_logFile = _wfsopen(g_logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
}

static FILE* GetLogFile() {
    if (!g_logFile) {
        OpenNewLogFile();
        return g_logFile;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (DateKeyOf(st) == g_openedDateKey) return g_logFile;

    int prevSeq = g_seq;
    fclose(g_logFile);
    g_logFile = nullptr;
    OpenNewLogFile();
    if (g_logFile) fwprintf(g_logFile, L"=== new day, continuing session %ls (part %d -> %d) ===\n",
                             g_sessionId.c_str(), prevSeq, g_seq);
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

static void RotateLogIfTooLarge() {
    if (!g_logFile) return;
    __int64 size = _filelengthi64(_fileno(g_logFile));
    if (size < kMaxLogFileSizeBytes) return;
    int prevSeq = g_seq;
    fclose(g_logFile);
    g_logFile = nullptr;
    OpenNewLogFile();
    if (g_logFile) fwprintf(g_logFile, L"=== log rotated (previous part reached %lld bytes; part %d -> %d) ===\n",
                             size, prevSeq, g_seq);
}

void FlushLogBuffer() {
    RotateLogIfTooLarge();
    if (g_ringBuffer.empty()) return;
    FILE* f = GetLogFile();
    if (!f) return;
    for (const std::wstring& line : g_ringBuffer) fwprintf(f, L"%ls\n", line.c_str());
    fflush(f);
    g_ringBuffer.clear();
}

std::wstring GetLogFilePath() {
    if (!g_logFile) OpenNewLogFile(); // lazily resolves g_logPath as a side effect
    return g_logPath;
}
