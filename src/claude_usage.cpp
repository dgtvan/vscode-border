#include "claude_usage.h"

#include "claude_profile.h"
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
    std::wstring note; // something worth logging that isn't a failure (a renewed login)
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

// The login file in Claude Code config folder `dir`.
std::wstring CredentialsPath(const std::wstring& dir) {
    return dir + L"\\.credentials.json";
}

// The profile's login file's last write time, 0 if it can't be read.
ULONGLONG CredentialsWriteTime(const std::wstring& dir) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(CredentialsPath(dir).c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// How often, after a failed fetch, the login file is checked for a change.
const DWORD kCredentialsPollMs = 3000;

// Waits `ms` for `stop`; true if it fired. After a failed fetch
// (`watchCredentials`) it also ends early, returning false, once the login
// file in config folder `dir` changes: Claude Code rewrites it when it
// renews an expired token or signs in, so the next fetch can work within
// seconds rather than a whole refresh interval later.
bool WaitForNextFetch(HANDLE stop, DWORD ms, bool watchCredentials, const std::wstring& dir) {
    if (!watchCredentials) return WaitForSingleObject(stop, ms) == WAIT_OBJECT_0;
    ULONGLONG start = GetTickCount64(), seen = CredentialsWriteTime(dir);
    for (;;) {
        ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= ms) return false;
        DWORD wait = (DWORD)std::min<ULONGLONG>(kCredentialsPollMs, ms - elapsed);
        if (WaitForSingleObject(stop, wait) == WAIT_OBJECT_0) return true;
        if (CredentialsWriteTime(dir) != seen) {
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
bool HttpGetUsage(const std::string& token, std::string& body, std::wstring& error) {
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
        DWORD status = 0, size = sizeof(status);
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
                                                    L"time it runs as this profile"
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

// The Claude Code executable: on PATH (the native installer's
// ~/.local/bin), its native install, else the newest one bundled with the VS
// Code extension. Empty if none.
std::wstring FindClaudeExe() {
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"claude.exe", nullptr, MAX_PATH, found, nullptr) > 0) return found;
    wchar_t home[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring native = std::wstring(home) + L"\\.local\\bin\\claude.exe";
    if (GetFileAttributesW(native.c_str()) != INVALID_FILE_ATTRIBUTES) return native;

    std::wstring best;
    ULONGLONG bestTime = 0;
    for (const wchar_t* vscodeDir : {L"\\.vscode\\extensions\\", L"\\.vscode-insiders\\extensions\\"}) {
        std::wstring extensions = std::wstring(home) + vscodeDir;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((extensions + L"anthropic.claude-code-*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring exe = extensions + fd.cFileName + L"\\resources\\native-binary\\claude.exe";
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &data)) continue;
            ULONGLONG t = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
            if (t > bestTime) {
                bestTime = t;
                best = exe;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}

const DWORD kRenewTimeoutMs = 60000;

// Has Claude Code renew a profile's expired login -- run with `environment`
// (the profile's, see claude_profile.h's ClaudeProfileEnvironment), so with
// its CLAUDE_CONFIG_DIR: `claude -p` with
// nothing on stdin renews the token as it starts, then stops with "Input
// must be provided" before calling the model -- so it costs no usage. (Seen
// with Claude Code 2.1.304: `claude auth status` doesn't renew.) Claude Code
// does the renewing itself, so its own login is never at risk the way it
// would be if this app refreshed the token. True once it has run; false with
// `error` set if it couldn't be. Doesn't log: it runs on a worker thread.
bool RunClaudeCodeToRenewLogin(std::wstring environment, std::wstring& exeOut, std::wstring& error) {
    exeOut = FindClaudeExe();
    if (exeOut.empty()) {
        error = L"no Claude Code found to renew it with (not on PATH, in ~\\.local\\bin, or in the VS Code "
                L"extensions)";
        return false;
    }
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE stdinRead = nullptr, stdinWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite, &inherit, 0)) {
        error = FormatError(L"couldn't create a pipe for Claude Code (error %lu)", GetLastError());
        return false;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    // Output goes to a file rather than a pipe, so a hung Claude Code can't
    // block the read past the timeout.
    wchar_t outPath[MAX_PATH] = {};
    GetTempFileNameW(tempDir, L"vbr", 0, outPath);
    HANDLE outFile = CreateFileW(outPath, GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_TEMPORARY, nullptr);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = stdinRead;
    si.hStdOutput = outFile;
    si.hStdError = outFile;
    PROCESS_INFORMATION pi = {};
    // Hooks off: it isn't a real session, and the app's own status hook
    // would otherwise report one starting and ending.
    std::wstring commandLine = L"\"" + exeOut + L"\" -p --settings \"{\\\"disableAllHooks\\\":true}\"";
    bool started = outFile != INVALID_HANDLE_VALUE &&
                   CreateProcessW(exeOut.c_str(), &commandLine[0], nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, &environment[0], tempDir, &si, &pi);
    DWORD startError = GetLastError();
    CloseHandle(stdinRead);
    CloseHandle(stdinWrite); // nothing on stdin
    if (outFile != INVALID_HANDLE_VALUE) CloseHandle(outFile);
    if (!started) {
        DeleteFileW(outPath);
        error = FormatError(L"couldn't run Claude Code (%ls) to renew it (error %lu)", exeOut.c_str(), startError);
        return false;
    }
    CloseHandle(pi.hThread);
    bool finished = WaitForSingleObject(pi.hProcess, kRenewTimeoutMs) == WAIT_OBJECT_0;
    if (!finished) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    std::string output;
    DWORD readErr = 0;
    ReadSharedFile(outPath, output, readErr);
    DeleteFileW(outPath);
    if (!finished) {
        error = FormatError(L"Claude Code (%ls), run to renew it, didn't finish within %lus", exeOut.c_str(),
                            kRenewTimeoutMs / 1000);
        return false;
    }
    // For the caller's message if the token still isn't renewed.
    std::wstring said = Utf8ToWide(output.substr(0, 200));
    for (wchar_t& c : said) {
        if (c == L'\r' || c == L'\n') c = L' ';
    }
    error = said;
    return true;
}

// The login file's expiresAt (Unix ms) in config folder `dir`, 0 if it
// can't be read.
long long ReadLoginExpiry(const std::wstring& dir) {
    std::string content;
    DWORD err = 0;
    if (!ReadSharedFile(CredentialsPath(dir), content, err) ||
        JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) {
        return 0;
    }
    JsonScan::Span root = JsonScan::EnsureRootObject(content);
    JsonScan::Span oauth = ObjectValue(content, root.start, root.end, "claudeAiOauth");
    long long expiresAtMs = oauth.found ? _atoi64(RawValue(content, oauth.start, oauth.end, "expiresAt").c_str()) : 0;
    SecureZeroMemory(&content[0], content.size());
    return expiresAtMs;
}

FetchResult FetchClaudeUsageOnce(const std::wstring& dir);

// One fetch for the profile with config folder `dir` and environment
// `environment`. When the login has expired, has Claude Code renew it (see
// RunClaudeCodeToRenewLogin) and fetches again -- once per expired token
// (`renewTriedFor`, the expiresAt last tried), so a renewal that doesn't
// work isn't retried every fetch. Doesn't log: it runs on a worker thread.
FetchResult FetchClaudeUsage(const std::wstring& dir, const std::wstring& environment, long long& renewTriedFor) {
    FetchResult r = FetchClaudeUsageOnce(dir);
    long long expiredAt = r.ok ? 0 : ReadLoginExpiry(dir);
    if (r.ok || expiredAt <= 0 || expiredAt / 1000 > UnixNow() || expiredAt == renewTriedFor) return r;
    renewTriedFor = expiredAt;

    std::wstring exe, said;
    if (!RunClaudeCodeToRenewLogin(environment, exe, said)) {
        r.error += L" -- and the app couldn't renew it: " + said;
        return r;
    }
    long long now = ReadLoginExpiry(dir);
    if (now <= expiredAt || now / 1000 <= UnixNow()) {
        r.error += L" -- running Claude Code (" + exe + L") to renew it didn't" + (said.empty() ? L"" : L"; it said: " + said);
        return r;
    }
    FetchResult renewed = FetchClaudeUsageOnce(dir);
    renewed.note = L"renewed the expired login (it had expired at " + FormatLocalTime(expiredAt / 1000) +
                   L") by running Claude Code: " + exe;
    return renewed;
}

FetchResult FetchClaudeUsageOnce(const std::wstring& dir) {
    FetchResult r;
    std::wstring path = CredentialsPath(dir);
    std::string content;
    DWORD err = 0;
    if (!ReadSharedFile(path, content, err)) {
        r.error = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND
                      ? FormatError(L"no Claude Code login at %ls -- sign in with /login in Claude Code as this profile",
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
        return r;
    }
    long long expiresAtMs = _atoi64(RawValue(content, oauth.start, oauth.end, "expiresAt").c_str());
    SecureZeroMemory(&content[0], content.size());
    if (expiresAtMs > 0 && expiresAtMs / 1000 <= UnixNow()) {
        r.error = FormatError(L"the login token expired at %ls", FormatLocalTime(expiresAtMs / 1000).c_str());
        SecureZeroMemory(&token[0], token.size());
        return r;
    }

    std::string body;
    bool ok = HttpGetUsage(token, body, r.error);
    SecureZeroMemory(&token[0], token.size());
    if (!ok) return r;

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

// One monitored profile. Shared between its worker thread and the main
// thread, under g_lock, except where marked.
struct Worker {
    std::wstring profile;     // its id (see claude_profile.h)
    std::wstring configDir;   // fixed at start, like environment: g_config can be reloaded under the thread
    std::wstring environment; // see ClaudeProfileEnvironment
    int intervalMinutes = 5;
    HWND notifyWnd = nullptr;
    UINT notifyMsg = 0;
    HANDLE stopEvent = nullptr;
    HANDLE thread = nullptr;

    ClaudeUsageSnapshot snapshot;
    unsigned seq = 0; // bumped per result
    std::vector<std::wstring> notes; // not yet logged

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
    if (!r.note.empty()) w.notes.push_back(r.note);
    if (r.ok) {
        w.snapshot.haveData = true;
        w.snapshot.session = r.session;
        w.snapshot.weekly = r.weekly;
        w.snapshot.fetchedAt = now;
        w.snapshot.error.clear();
    } else {
        w.snapshot.error = r.error;
        w.snapshot.errorAt = now;
    }
    w.seq++;
    ReleaseSRWLockExclusive(&g_lock);
    PostMessageW(w.notifyWnd, w.notifyMsg, 0, 0);
}

DWORD WINAPI WorkerThread(LPVOID param) {
    std::shared_ptr<Worker> owned = *(std::shared_ptr<Worker>*)param;
    delete (std::shared_ptr<Worker>*)param;
    Worker& w = *owned;
    DWORD intervalMs = (DWORD)w.intervalMinutes * 60 * 1000;
    long long renewTriedFor = 0;
    bool failed = false;
    do {
        FetchResult r = FetchClaudeUsage(w.configDir, w.environment, renewTriedFor);
        failed = !r.ok;
        Report(w, r);
    } while (!WaitForNextFetch(w.stopEvent, intervalMs, failed, w.configDir));
    return 0;
}

std::wstring ProfileForLog(const std::wstring& profile) {
    return L"[" + ClaudeProfileName(profile) + L"]";
}

void StopWorkers() {
    for (const std::shared_ptr<Worker>& w : g_workers) SetEvent(w->stopEvent);
    for (const std::shared_ptr<Worker>& w : g_workers) {
        // A fetch can sit in WinHTTP for up to its timeouts; past this the
        // thread is left to finish on its own -- it holds its own reference
        // to the Worker, so nothing it touches goes away under it.
        if (WaitForSingleObject(w->thread, 5000) != WAIT_OBJECT_0) {
            LogWarn(L"claude_usage: the worker for %ls didn't stop within 5s -- leaving it to finish",
                    ProfileForLog(w->profile).c_str());
        }
        CloseHandle(w->thread);
    }
    g_workers.clear();
}

} // namespace

void SyncClaudeUsageMonitor(HWND notifyWnd, UINT notifyMsg) {
    std::vector<std::wstring> profiles;
    if (g_config.usageMeterEnabled) {
        profiles.push_back(L"");
        for (const std::wstring& profile : OtherClaudeProfiles()) profiles.push_back(profile);
    }
    bool same = profiles.size() == g_workers.size() && g_config.usageMeterRefreshMinutes == g_intervalMinutes;
    for (size_t i = 0; same && i < profiles.size(); i++) {
        same = profiles[i] == g_workers[i]->profile && ClaudeProfileConfigDir(profiles[i]) == g_workers[i]->configDir &&
               ClaudeProfileEnvironment(profiles[i]) == g_workers[i]->environment;
    }
    if (same) return;

    // Kept across the restart, so the rings don't blank out on a reload.
    std::vector<std::shared_ptr<Worker>> previous = g_workers;
    StopWorkers();
    g_intervalMinutes = g_config.usageMeterRefreshMinutes;
    Log(L"claude_usage: monitoring %zu Claude profile(s), every %d minute(s)", profiles.size(), g_intervalMinutes);

    for (const std::wstring& profile : profiles) {
        auto w = std::make_shared<Worker>();
        w->profile = profile;
        w->configDir = ClaudeProfileConfigDir(profile);
        w->environment = ClaudeProfileEnvironment(profile);
        w->intervalMinutes = g_intervalMinutes;
        w->notifyWnd = notifyWnd;
        w->notifyMsg = notifyMsg;
        w->snapshot.profile = profile;
        for (const std::shared_ptr<Worker>& old : previous) {
            if (old->profile != profile || old->configDir != w->configDir) continue;
            w->snapshot = old->snapshot;
            w->loggedError = old->loggedError;
            w->loggedOk = old->loggedOk;
        }
        w->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w->thread = CreateThread(nullptr, 0, WorkerThread, new std::shared_ptr<Worker>(w), 0, nullptr);
        if (!w->thread) {
            LogWarn(L"claude_usage: couldn't start the worker for %ls (error %lu)", ProfileForLog(profile).c_str(),
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
        std::vector<std::wstring> notes;
        notes.swap(w->notes);
        ReleaseSRWLockExclusive(&g_lock);

        std::wstring who = ProfileForLog(w->profile);
        for (const std::wstring& note : notes) {
            Log(L"claude_usage: %ls: %ls", who.c_str(), note.c_str());
        }
        if (seq == w->loggedSeq) continue;
        w->loggedSeq = seq;
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

bool IsClaudeUsageUnavailable(const ClaudeUsageSnapshot& s) {
    return !s.error.empty() || (s.haveData && IsClaudeUsageStale(s));
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
    std::wstring text = L"Claude usage - " + ClaudeProfileName(s.profile);
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

