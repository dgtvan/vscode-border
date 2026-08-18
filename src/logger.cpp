#include "logger.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <share.h>
#include <string>

static FILE* GetLogFile() {
    static FILE* f = nullptr;
    if (!f) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\vscode_border.log", tmp);
        // Start a fresh log each run instead of appending forever -- this
        // app is meant to run continuously via autostart, so a log that
        // grows across every single launch over weeks/months isn't
        // acceptable. Opened with deny-write sharing (not the CRT default
        // of deny-all) so the log can still be tailed by another process
        // while this one keeps writing to it.
        f = _wfsopen(path, L"w, ccs=UTF-8", _SH_DENYWR);
    }
    return f;
}

void Log(const wchar_t* fmt, ...) {
    FILE* f = GetLogFile();
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);
    fwprintf(f, L"\n");
    fflush(f);
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

void FlushLogBuffer() {
    if (g_ringBuffer.empty()) return;
    FILE* f = GetLogFile();
    if (!f) return;
    for (const std::wstring& line : g_ringBuffer) fwprintf(f, L"%ls\n", line.c_str());
    fflush(f);
    g_ringBuffer.clear();
}
