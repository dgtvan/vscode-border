#include "claude_usage.h"

#include "account.h"
#include "config.h"
#include "file_util.h"
#include "json_scan.h"
#include "logger.h"
#include "text_util.h"

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>

const wchar_t* const kClaudeUsageHelperArg = L"--claude-usage-helper";

namespace {

long long UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return (long long)((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

SYSTEMTIME UnixToLocalTime(long long unix) {
    ULARGE_INTEGER t;
    t.QuadPart = (ULONGLONG)unix * 10000000ULL + 116444736000000000ULL;
    FILETIME utc = {t.LowPart, t.HighPart};
    SYSTEMTIME st = {}, local = {};
    FileTimeToSystemTime(&utc, &st);
    SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
    return local;
}

// "21:20", with the weekday in front ("Sat 07:00") unless it's today.
std::wstring FormatLocalTime(long long unix) {
    static const wchar_t* const kDays[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    SYSTEMTIME t = UnixToLocalTime(unix), today = UnixToLocalTime(UnixNow());
    bool sameDay = t.wYear == today.wYear && t.wMonth == today.wMonth && t.wDay == today.wDay;
    wchar_t buf[32];
    if (sameDay) swprintf(buf, 32, L"%02u:%02u", t.wHour, t.wMinute);
    else swprintf(buf, 32, L"%ls %02u:%02u", kDays[t.wDayOfWeek % 7], t.wHour, t.wMinute);
    return buf;
}

// "4h 51m", "4d 14h", "12m".
std::wstring FormatDuration(long long seconds) {
    long long minutes = std::max(0LL, seconds) / 60;
    wchar_t buf[32];
    if (minutes >= 24 * 60) swprintf(buf, 32, L"%lldd %lldh", minutes / (24 * 60), minutes / 60 % 24);
    else if (minutes >= 60) swprintf(buf, 32, L"%lldh %lldm", minutes / 60, minutes % 60);
    else swprintf(buf, 32, L"%lldm", minutes);
    return buf;
}

// "2026-10-20T21:19:59.593933+00:00" (or with "Z") as Unix seconds; 0 if it
// isn't one.
long long ParseIso8601(const std::string& s) {
    SYSTEMTIME st = {};
    int year, month, day, hour, minute, second, consumed = 0;
    if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute, &second, &consumed) !=
        6) {
        return 0;
    }
    st.wYear = (WORD)year;
    st.wMonth = (WORD)month;
    st.wDay = (WORD)day;
    st.wHour = (WORD)hour;
    st.wMinute = (WORD)minute;
    st.wSecond = (WORD)second;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    long long unix = (long long)((t.QuadPart - 116444736000000000ULL) / 10000000ULL);

    size_t i = (size_t)consumed;
    if (i < s.size() && s[i] == '.') {
        i++;
        while (i < s.size() && isdigit((unsigned char)s[i])) i++;
    }
    int offH = 0, offM = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-') && sscanf(s.c_str() + i + 1, "%2d:%2d", &offH, &offM) == 2) {
        long long offset = (offH * 60LL + offM) * 60LL;
        unix += s[i] == '+' ? -offset : offset;
    }
    return unix;
}

// The value of `key` directly inside the object whose content span is
// [objStart, objEnd), as raw JSON text; empty if absent.
std::string RawValue(const std::string& content, size_t objStart, size_t objEnd, const char* key) {
    JsonScan::KeyEntry e = JsonScan::FindKeyInObject(content, objStart, objEnd, key);
    if (!e.found || e.valueEnd <= e.valueStart) return "";
    return content.substr(e.valueStart, e.valueEnd - e.valueStart);
}

// The content span of the object `key` holds, or not found if it's absent,
// null, or not an object.
JsonScan::Span ObjectValue(const std::string& content, size_t objStart, size_t objEnd, const char* key) {
    JsonScan::KeyEntry e = JsonScan::FindKeyInObject(content, objStart, objEnd, key);
    if (!e.found || e.valueStart >= content.size() || content[e.valueStart] != '{') return {};
    return {true, e.valueStart + 1, e.valueEnd - 1};
}

// A JSON string's text between its quotes. Only used on values that never
// hold escapes (tokens, timestamps).
std::string Unquote(const std::string& raw) {
    return raw.size() >= 2 && raw.front() == '"' && raw.back() == '"' ? raw.substr(1, raw.size() - 2) : "";
}

struct FetchResult {
    bool ok = false;
    ClaudeUsageLimit session, weekly;
    std::wstring error;
    bool needsYou = false;      // see ClaudeUsageSnapshot::needsYou
    bool wrongPassword = false; // set by a user profile's worker -- the saved password is to be dropped
};

ClaudeUsageLimit ParseLimit(const std::string& content, const JsonScan::Span& root, const char* key) {
    ClaudeUsageLimit limit;
    JsonScan::Span obj = ObjectValue(content, root.start, root.end, key);
    if (!obj.found) return limit;
    std::string pct = RawValue(content, obj.start, obj.end, "utilization");
    if (!pct.empty() && pct != "null") limit.percent = strtod(pct.c_str(), nullptr);
    limit.resetsAt = ParseIso8601(Unquote(RawValue(content, obj.start, obj.end, "resets_at")));
    return limit;
}

// Claude Code's config folder for the account this runs as: CLAUDE_CONFIG_DIR
// if set (Claude Code honours it too), else %USERPROFILE%\.claude.
std::wstring ClaudeConfigDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(buf) + L"\\.claude";
    return L"";
}

// The account's login file's last write time, 0 if it can't be read.
ULONGLONG CredentialsWriteTime() {
    std::wstring dir = ClaudeConfigDir();
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (dir.empty() || !GetFileAttributesExW((dir + L"\\.credentials.json").c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// How often, after a failed fetch, the login file is checked for a change.
const DWORD kCredentialsPollMs = 3000;

// Waits `ms` for `stop`; true if it fired. After a failed fetch
// (`watchCredentials`) it also ends early, returning false, once the
// account's login file changes: Claude Code rewrites it when it renews an
// expired token or signs in, so the next fetch can work within seconds
// rather than a whole refresh interval later.
bool WaitForNextFetch(HANDLE stop, DWORD ms, bool watchCredentials) {
    if (!watchCredentials) return WaitForSingleObject(stop, ms) == WAIT_OBJECT_0;
    ULONGLONG start = GetTickCount64(), seen = CredentialsWriteTime();
    for (;;) {
        ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= ms) return false;
        DWORD wait = (DWORD)std::min<ULONGLONG>(kCredentialsPollMs, ms - elapsed);
        if (WaitForSingleObject(stop, wait) == WAIT_OBJECT_0) return true;
        if (CredentialsWriteTime() != seen) {
            // A moment for Claude Code to finish writing it.
            return WaitForSingleObject(stop, 1000) == WAIT_OBJECT_0;
        }
    }
}

// Reads `path`, sharing it with Claude Code rewriting it; false with
// `error` set (the Win32 error) if it can't be opened.
bool ReadSharedFile(const std::wstring& path, std::string& out, DWORD& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    DWORD size = GetFileSize(h, nullptr);
    out.clear();
    if (size != INVALID_FILE_SIZE && size > 0) {
        out.resize(size);
        DWORD read = 0;
        if (!ReadFile(h, &out[0], size, &read, nullptr)) read = 0;
        out.resize(read);
    }
    CloseHandle(h);
    return true;
}

std::wstring FormatError(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 1024, fmt, args);
    va_end(args);
    return buf;
}

// GET https://api.anthropic.com/api/oauth/usage with `token`; true with
// the body in `body` on HTTP 200.
bool HttpGetUsage(const std::string& token, std::string& body, std::wstring& error, DWORD& status) {
    status = 0;
    HINTERNET session = WinHttpOpen(L"vscode-border", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = FormatError(L"couldn't start WinHTTP (error %lu)", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000);
    HINTERNET connect = WinHttpConnect(session, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/api/oauth/usage", nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     WINHTTP_FLAG_SECURE)
                                : nullptr;
    std::wstring headers = L"Authorization: Bearer " + Utf8ToWide(token) +
                           L"\r\nanthropic-beta: oauth-2025-04-20\r\nAccept: application/json\r\n";
    bool sent = request &&
                WinHttpSendRequest(request, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr);
    SecureZeroMemory(&headers[0], headers.size() * sizeof(wchar_t));
    bool ok = false;
    if (!sent) {
        error = FormatError(L"couldn't reach api.anthropic.com (WinHTTP error %lu)", GetLastError());
    } else {
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        body.clear();
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0) break;
            body.append(chunk, 0, read);
        }
        if (status == 200) {
            ok = true;
        } else {
            std::wstring snippet = Utf8ToWide(body.substr(0, 200));
            for (wchar_t& c : snippet) {
                if (c == L'\r' || c == L'\n') c = L' ';
            }
            const wchar_t* hint = status == 401   ? L" -- the login token was rejected; Claude Code renews it the next "
                                                    L"time it runs in this account"
                                  : status == 429 ? L" -- rate limited; try a longer usage_meter_refresh_minutes"
                                                  : L"";
            error = FormatError(L"api.anthropic.com answered HTTP %lu%ls: %ls", status, hint, snippet.c_str());
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// One fetch for the account this runs as. Doesn't log (see
// RunClaudeUsageHelper).
FetchResult FetchClaudeUsage() {
    FetchResult r;
    std::wstring dir = ClaudeConfigDir();
    if (dir.empty()) {
        r.error = L"neither CLAUDE_CONFIG_DIR nor USERPROFILE is set";
        return r;
    }
    std::wstring path = dir + L"\\.credentials.json";
    std::string content;
    DWORD err = 0;
    if (!ReadSharedFile(path, content, err)) {
        r.needsYou = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        r.error = r.needsYou
                      ? FormatError(L"no Claude Code login at %ls -- sign in with /login in Claude Code as this account",
                                    path.c_str())
                      : FormatError(L"can't read %ls (error %lu)", path.c_str(), err);
        return r;
    }
    if (JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) {
        r.error = FormatError(L"%ls isn't JSON", path.c_str());
        return r;
    }
    JsonScan::Span root = JsonScan::EnsureRootObject(content);
    JsonScan::Span oauth = ObjectValue(content, root.start, root.end, "claudeAiOauth");
    std::string token = oauth.found ? Unquote(RawValue(content, oauth.start, oauth.end, "accessToken")) : "";
    if (token.empty()) {
        r.error = FormatError(L"%ls has no Claude.ai login token (claudeAiOauth.accessToken)", path.c_str());
        r.needsYou = true;
        return r;
    }
    long long expiresAtMs = _atoi64(RawValue(content, oauth.start, oauth.end, "expiresAt").c_str());
    SecureZeroMemory(&content[0], content.size());
    if (expiresAtMs > 0 && expiresAtMs / 1000 <= UnixNow()) {
        r.error = FormatError(L"the login token expired at %ls; Claude Code renews it the next time it runs in this "
                              L"account",
                              FormatLocalTime(expiresAtMs / 1000).c_str());
        r.needsYou = true;
        SecureZeroMemory(&token[0], token.size());
        return r;
    }

    std::string body;
    DWORD status = 0;
    bool ok = HttpGetUsage(token, body, r.error, status);
    SecureZeroMemory(&token[0], token.size());
    if (!ok) {
        r.needsYou = status == 401;
        return r;
    }

    if (JsonScan::FindRootObjectOpenBrace(body) == std::string::npos) {
        r.error = L"api.anthropic.com's answer isn't JSON";
        return r;
    }
    JsonScan::Span bodyRoot = JsonScan::EnsureRootObject(body);
    r.session = ParseLimit(body, bodyRoot, "five_hour");
    r.weekly = ParseLimit(body, bodyRoot, "seven_day");
    if (r.session.percent < 0 && r.weekly.percent < 0) {
        r.error = L"api.anthropic.com's answer has neither five_hour nor seven_day utilization -- its format may have "
                  L"changed";
        return r;
    }
    r.ok = true;
    return r;
}

// The helper's stdout protocol, one UTF-8 line per result:
//   ok <session %> <session resets_at> <weekly %> <weekly resets_at>
//   err <message>    -- a failure that may pass on its own
//   need <message>   -- one that lasts until someone acts (see ClaudeUsageSnapshot::needsYou)
std::string FormatHelperLine(const FetchResult& r) {
    char buf[160];
    if (r.ok) {
        snprintf(buf, sizeof(buf), "ok %.4f %lld %.4f %lld\n", r.session.percent, r.session.resetsAt,
                 r.weekly.percent, r.weekly.resetsAt);
        return buf;
    }
    std::string text = WideToUtf8(r.error);
    for (char& c : text) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    return (r.needsYou ? "need " : "err ") + text + "\n";
}

FetchResult ParseHelperLine(const std::string& line) {
    FetchResult r;
    if (line.rfind("ok ", 0) == 0 &&
        sscanf(line.c_str() + 3, "%lf %lld %lf %lld", &r.session.percent, &r.session.resetsAt, &r.weekly.percent,
               &r.weekly.resetsAt) == 4) {
        r.ok = true;
    } else if (line.rfind("err ", 0) == 0) {
        r.error = Utf8ToWide(line.substr(4));
    } else if (line.rfind("need ", 0) == 0) {
        r.error = Utf8ToWide(line.substr(5));
        r.needsYou = true;
    } else {
        r.error = L"the usage helper wrote an unexpected line: " + Utf8ToWide(line.substr(0, 200));
    }
    return r;
}

// One monitored account. Shared between its worker thread and the main
// thread, under g_lock, except where marked.
struct Worker {
    std::wstring account; // empty for this app's own
    int intervalMinutes = 5;
    HWND notifyWnd = nullptr;
    UINT notifyMsg = 0;
    HANDLE stopEvent = nullptr;
    HANDLE thread = nullptr;
    HANDLE child = nullptr; // the running helper, for StopClaudeUsageMonitor to end

    ClaudeUsageSnapshot snapshot;
    unsigned seq = 0; // bumped per result
    bool forgetPassword = false;

    // Main thread only: what ProcessClaudeUsageResults last logged.
    unsigned loggedSeq = 0;
    std::wstring loggedError;
    bool loggedOk = false;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<std::shared_ptr<Worker>> g_workers; // main thread only
int g_intervalMinutes = 5;

void Report(Worker& w, const FetchResult& r) {
    AcquireSRWLockExclusive(&g_lock);
    long long now = UnixNow();
    if (r.ok) {
        w.snapshot.haveData = true;
        w.snapshot.session = r.session;
        w.snapshot.weekly = r.weekly;
        w.snapshot.fetchedAt = now;
        w.snapshot.error.clear();
        w.snapshot.needsYou = false;
    } else {
        w.snapshot.error = r.error;
        w.snapshot.errorAt = now;
        w.snapshot.needsYou = r.needsYou;
    }
    if (r.wrongPassword) w.forgetPassword = true;
    w.seq++;
    ReleaseSRWLockExclusive(&g_lock);
    PostMessageW(w.notifyWnd, w.notifyMsg, 0, 0);
}

bool WaitForStop(Worker& w, DWORD ms) {
    return WaitForSingleObject(w.stopEvent, ms) == WAIT_OBJECT_0;
}

// Runs one helper as w.account until it exits, reporting every line it
// writes.
void RunHelperOnce(Worker& w) {
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE childIn = nullptr, ourIn = nullptr, ourOut = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &ourIn, &inherit, 0) || !CreatePipe(&ourOut, &childOut, &inherit, 0)) {
        FetchResult r;
        r.error = FormatError(L"couldn't create the pipes for the usage helper (error %lu)", GetLastError());
        if (childIn) CloseHandle(childIn);
        if (ourIn) CloseHandle(ourIn);
        Report(w, r);
        return;
    }
    // Only the child's ends may be inherited: a copy of ours in the child
    // would keep its stdin from ever reaching end-of-file.
    SetHandleInformation(ourIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ourOut, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" " + kClaudeUsageHelperArg + L" " +
                               std::to_wstring(w.intervalMinutes);
    DWORD err = 0;
    HANDLE child = StartHiddenProcessAsAccount(w.account, exe, commandLine, GetExeDir(), childIn, childOut, err);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (!child) {
        FetchResult r;
        if (err == ERROR_NOT_FOUND) {
            r.error = L"no password saved for this account -- open a VS Code window as it from the hub's + button "
                      L"and tick Remember";
            r.needsYou = true;
        } else if (err == ERROR_LOGON_FAILURE) {
            r.error = L"its saved password was rejected, so it's been dropped -- open a VS Code window as it from the "
                      L"hub's + button to save the right one";
            r.wrongPassword = true;
            r.needsYou = true;
        } else {
            r.error = FormatError(L"couldn't start the usage helper as this account (error %lu)", err);
        }
        CloseHandle(ourIn);
        CloseHandle(ourOut);
        Report(w, r);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    w.child = child;
    ReleaseSRWLockExclusive(&g_lock);
    if (WaitForStop(w, 0)) TerminateProcess(child, 0);

    std::string pending;
    char buf[512];
    DWORD read = 0;
    while (ReadFile(ourOut, buf, sizeof(buf), &read, nullptr) && read > 0) {
        pending.append(buf, read);
        size_t eol;
        while ((eol = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, eol);
            pending.erase(0, eol + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) Report(w, ParseHelperLine(line));
        }
    }

    AcquireSRWLockExclusive(&g_lock);
    w.child = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
    if (!WaitForStop(w, 0)) {
        WaitForSingleObject(child, 2000);
        DWORD code = 0;
        GetExitCodeProcess(child, &code);
        FetchResult r;
        r.error = FormatError(L"the usage helper running as this account exited (code %lu)", code);
        Report(w, r);
    }
    CloseHandle(child);
    CloseHandle(ourIn);
    CloseHandle(ourOut);
}

DWORD WINAPI WorkerThread(LPVOID param) {
    std::shared_ptr<Worker> owned = *(std::shared_ptr<Worker>*)param;
    delete (std::shared_ptr<Worker>*)param;
    Worker& w = *owned;
    DWORD intervalMs = (DWORD)w.intervalMinutes * 60 * 1000;
    bool wrongPassword = false, failed = false;
    do {
        if (w.account.empty()) {
            FetchResult r = FetchClaudeUsage();
            failed = !r.ok;
            Report(w, r);
            continue;
        }
        // Never retry a password that was rejected (that could lock the
        // account out): wait until the main thread has dropped it and a new
        // one has been saved.
        if (wrongPassword && !HasSavedAccountPassword(w.account)) wrongPassword = false;
        if (wrongPassword) continue;
        RunHelperOnce(w);
        AcquireSRWLockShared(&g_lock);
        wrongPassword = w.forgetPassword;
        ReleaseSRWLockShared(&g_lock);
        // The helper watches that account's login file itself.
        failed = false;
    } while (!WaitForNextFetch(w.stopEvent, intervalMs, failed));
    return 0;
}

std::wstring AccountForLog(const std::wstring& account) {
    return account.empty() ? L"this account (" + CurrentAccountName() + L")" : L"[" + account + L"]";
}

void StopWorkers() {
    for (const std::shared_ptr<Worker>& w : g_workers) {
        SetEvent(w->stopEvent);
        AcquireSRWLockExclusive(&g_lock);
        if (w->child) TerminateProcess(w->child, 0);
        ReleaseSRWLockExclusive(&g_lock);
    }
    for (const std::shared_ptr<Worker>& w : g_workers) {
        // A fetch can sit in WinHTTP for up to its timeouts; past this the
        // thread is left to finish on its own -- it holds its own reference
        // to the Worker, so nothing it touches goes away under it.
        if (WaitForSingleObject(w->thread, 5000) != WAIT_OBJECT_0) {
            LogWarn(L"claude_usage: the worker for %ls didn't stop within 5s -- leaving it to finish",
                    AccountForLog(w->account).c_str());
        }
        CloseHandle(w->thread);
    }
    g_workers.clear();
}

} // namespace

void SyncClaudeUsageMonitor(HWND notifyWnd, UINT notifyMsg) {
    std::vector<std::wstring> accounts;
    if (g_config.usageMeterEnabled) {
        accounts.push_back(L"");
        for (const std::wstring& account : ConfiguredUserProfiles()) accounts.push_back(account);
    }
    bool same = accounts.size() == g_workers.size() && g_config.usageMeterRefreshMinutes == g_intervalMinutes;
    for (size_t i = 0; same && i < accounts.size(); i++) same = accounts[i] == g_workers[i]->account;
    if (same) return;

    // Kept across the restart, so the rings don't blank out on a reload.
    std::vector<std::shared_ptr<Worker>> previous = g_workers;
    StopWorkers();
    g_intervalMinutes = g_config.usageMeterRefreshMinutes;
    Log(L"claude_usage: monitoring %zu account(s), every %d minute(s)", accounts.size(), g_intervalMinutes);

    for (const std::wstring& account : accounts) {
        auto w = std::make_shared<Worker>();
        w->account = account;
        w->intervalMinutes = g_intervalMinutes;
        w->notifyWnd = notifyWnd;
        w->notifyMsg = notifyMsg;
        w->snapshot.account = account;
        for (const std::shared_ptr<Worker>& old : previous) {
            if (old->account != account) continue;
            w->snapshot = old->snapshot;
            w->loggedError = old->loggedError;
            w->loggedOk = old->loggedOk;
        }
        w->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w->thread = CreateThread(nullptr, 0, WorkerThread, new std::shared_ptr<Worker>(w), 0, nullptr);
        if (!w->thread) {
            LogWarn(L"claude_usage: couldn't start the worker for %ls (error %lu)", AccountForLog(account).c_str(),
                    GetLastError());
            continue;
        }
        g_workers.push_back(w);
    }
}

void StopClaudeUsageMonitor() {
    StopWorkers();
}

void ProcessClaudeUsageResults() {
    for (const std::shared_ptr<Worker>& w : g_workers) {
        AcquireSRWLockExclusive(&g_lock);
        ClaudeUsageSnapshot s = w->snapshot;
        unsigned seq = w->seq;
        bool forget = w->forgetPassword;
        w->forgetPassword = false;
        ReleaseSRWLockExclusive(&g_lock);

        if (forget) ForgetSavedAccountPassword(w->account);
        if (seq == w->loggedSeq) continue;
        w->loggedSeq = seq;
        std::wstring who = AccountForLog(w->account);
        if (!s.error.empty()) {
            if (s.error != w->loggedError) {
                LogWarn(L"claude_usage: can't read Claude usage for %ls: %ls", who.c_str(), s.error.c_str());
            } else {
                LogDiag(L"claude_usage: still can't read Claude usage for %ls: %ls", who.c_str(), s.error.c_str());
            }
            w->loggedError = s.error;
            w->loggedOk = false;
            continue;
        }
        bool first = !w->loggedOk;
        (first ? Log : LogDiag)(L"claude_usage: %ls%ls: session %.0f%% (resets %ls), weekly %.0f%% (resets %ls)",
                                who.c_str(), w->loggedError.empty() ? L"" : L" readable again", s.session.percent,
                                s.session.resetsAt ? FormatLocalTime(s.session.resetsAt).c_str() : L"?",
                                s.weekly.percent, s.weekly.resetsAt ? FormatLocalTime(s.weekly.resetsAt).c_str() : L"?");
        w->loggedError.clear();
        w->loggedOk = true;
    }
}

std::vector<ClaudeUsageSnapshot> GetClaudeUsageSnapshots() {
    std::vector<ClaudeUsageSnapshot> out;
    AcquireSRWLockShared(&g_lock);
    for (const std::shared_ptr<Worker>& w : g_workers) out.push_back(w->snapshot);
    ReleaseSRWLockShared(&g_lock);
    return out;
}

bool IsClaudeUsageStale(const ClaudeUsageSnapshot& s) {
    // Three missed refreshes in a row, without any of them reporting why.
    return !s.haveData || !s.error.empty() || UnixNow() - s.fetchedAt > 3LL * g_intervalMinutes * 60;
}

double CurrentUsagePercent(const ClaudeUsageLimit& limit) {
    if (limit.percent < 0) return -1;
    if (limit.resetsAt > 0 && limit.resetsAt <= UnixNow()) return 0;
    return limit.percent;
}

std::wstring DescribeClaudeUsage(const ClaudeUsageSnapshot& s) {
    std::wstring text = L"Claude usage - " + (s.account.empty() ? AccountForLog(s.account) : s.account);
    long long now = UnixNow();
    auto describeLimit = [&](const wchar_t* name, const ClaudeUsageLimit& limit) {
        wchar_t line[200];
        double pct = CurrentUsagePercent(limit);
        if (pct < 0) {
            swprintf(line, 200, L"\n%ls: not reported", name);
        } else if (limit.resetsAt > 0 && limit.resetsAt <= now) {
            swprintf(line, 200, L"\n%ls: 0%% - reset at %ls", name, FormatLocalTime(limit.resetsAt).c_str());
        } else if (limit.resetsAt > 0) {
            swprintf(line, 200, L"\n%ls: %.0f%% - resets %ls (in %ls)", name, pct,
                     FormatLocalTime(limit.resetsAt).c_str(), FormatDuration(limit.resetsAt - now).c_str());
        } else {
            swprintf(line, 200, L"\n%ls: %.0f%%", name, pct);
        }
        text += line;
    };
    if (s.haveData) {
        describeLimit(L"Session", s.session);
        describeLimit(L"Weekly", s.weekly);
        text += L"\nUpdated " + FormatLocalTime(s.fetchedAt);
    } else if (s.error.empty()) {
        text += L"\nNot read yet";
    }
    if (!s.error.empty()) {
        text += L"\n\nLast update failed at " + FormatLocalTime(s.errorAt) + L":\n" + s.error;
    }
    return text;
}

namespace {

struct StdinWatch {
    HANDLE in, closed;
};

// Blocks reading stdin, which ends when the app closes its end or exits.
DWORD WINAPI StdinWatchThread(LPVOID param) {
    StdinWatch* watch = (StdinWatch*)param;
    char buf[64];
    DWORD read = 0;
    while (ReadFile(watch->in, buf, sizeof(buf), &read, nullptr) && read > 0) {
    }
    SetEvent(watch->closed);
    return 0;
}

} // namespace

int RunClaudeUsageHelper(int intervalMinutes) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE stdinClosed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, StdinWatchThread, new StdinWatch{GetStdHandle(STD_INPUT_HANDLE), stdinClosed}, 0,
                 nullptr);

    DWORD intervalMs = (DWORD)std::min(60, std::max(1, intervalMinutes)) * 60 * 1000;
    bool failed = false;
    do {
        FetchResult r = FetchClaudeUsage();
        failed = !r.ok;
        std::string line = FormatHelperLine(r);
        DWORD written = 0;
        if (!WriteFile(out, line.data(), (DWORD)line.size(), &written, nullptr)) return 1;
    } while (!WaitForNextFetch(stdinClosed, intervalMs, failed));
    return 0;
}
