#include "codex_usage.h"
#include "usage_format.h"

#include "codex_usage_client.h"
#include "assistant_profile.h"
#include "profile_config.h"
#include "layered_rendering.h"
#include "logger.h"
#include "profile_icon.h"

#include <algorithm>
#include <map>
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

// Everything a worker reads is captured on the main thread. Reloading
// config never races with CLI discovery, paths, or environment construction.
struct Worker {
    std::wstring configDir, environment, exe;
    int intervalMinutes = 5;
    HWND notifyWnd = nullptr;
    UINT notifyMsg = 0;
    HANDLE stop = nullptr, thread = nullptr;
    CodexUsageSnapshot snapshot;
    unsigned seq = 0, loggedSeq = 0;
    std::wstring loggedError;
    int failures = 0;
    ~Worker() { if (stop) CloseHandle(stop); if (thread) CloseHandle(thread); }
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<std::shared_ptr<Worker>> g_workers;
int g_intervalMinutes = 5;

ULONGLONG AuthWriteTime(const std::wstring& dir) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW((dir + L"\\auth.json").c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
}

bool WaitForNextFetch(const Worker& w, bool failed) {
    ULONGLONG start = GetTickCount64(), authTime = AuthWriteTime(w.configDir);
    DWORD interval = (DWORD)w.intervalMinutes * 60000;
    while (GetTickCount64() - start < interval) {
        DWORD remaining = interval - (DWORD)(GetTickCount64() - start);
        if (WaitForSingleObject(w.stop, failed ? std::min<DWORD>(3000, remaining) : remaining) == WAIT_OBJECT_0)
            return false;
        // A login or external renewal can recover a failed read promptly.
        // Only file metadata is inspected; auth contents are never read.
        if (failed && AuthWriteTime(w.configDir) != authTime) return WaitForSingleObject(w.stop, 500) != WAIT_OBJECT_0;
    }
    return true;
}

DWORD WINAPI WorkerThread(LPVOID param) {
    auto owned = *(std::shared_ptr<Worker>*)param;
    delete (std::shared_ptr<Worker>*)param;
    Worker& w = *owned;
    do {
        CodexUsageSnapshot result;
        std::wstring error;
        bool transient = false;
        bool ok = FetchCodexUsage(w.stop, w.exe, w.configDir, w.environment, result, error, transient);
        if (WaitForSingleObject(w.stop, 0) == WAIT_OBJECT_0) break;
        AcquireSRWLockExclusive(&g_lock);
        if (!result.email.empty()) w.snapshot.email = result.email;
        if (ok) {
            w.snapshot.primary = result.primary;
            w.snapshot.secondary = result.secondary;
            w.snapshot.haveData = true;
            w.snapshot.fetchedAt = UnixNow();
            w.snapshot.error.clear();
            w.snapshot.transientError = false;
        } else {
            w.snapshot.error = error;
            w.snapshot.errorAt = UnixNow();
            w.snapshot.transientError = transient;
        }
        w.seq++;
        ReleaseSRWLockExclusive(&g_lock);
        PostMessageW(w.notifyWnd, w.notifyMsg, 0, 0);
        if (!WaitForNextFetch(w, !ok)) break;
    } while (true);
    return 0;
}

void StopWorkers() {
    // Signal everyone first, then wait: cancellation does not grow with
    // the number of profiles or depend on network timeouts.
    for (const auto& w : g_workers) SetEvent(w->stop);
    for (const auto& w : g_workers) {
        if (WaitForSingleObject(w->thread, 5000) != WAIT_OBJECT_0)
            LogWarn(L"codex_usage: worker did not stop within 5s; leaving it to finish");
    }
    g_workers.clear(); // a late worker retains ownership of its handles
}
} // namespace

void SyncCodexUsageMonitor(HWND notifyWnd, UINT notifyMsg) {
    std::vector<std::wstring> paths, environments, ids;
    if (g_config.usageMeterEnabled) {
        for (const auto& p : g_config.profiles) {
            if (p.type != ProfileType::Codex || !p.usageEnabled) continue;
            ids.push_back(ProfileId(p));
            paths.push_back(p.configDir);
            environments.push_back(ProfileEnvironment(ProfileId(p)));
        }
    }
    std::wstring exe = paths.empty() ? L"" : FindCodexUsageExecutable();
    bool same = paths.size() == g_workers.size() && g_intervalMinutes == g_config.usageMeterRefreshMinutes;
    for (size_t i = 0; same && i < paths.size(); i++) {
        same = ids[i] == g_workers[i]->snapshot.profile && paths[i] == g_workers[i]->configDir && environments[i] == g_workers[i]->environment &&
               exe == g_workers[i]->exe && notifyWnd == g_workers[i]->notifyWnd && notifyMsg == g_workers[i]->notifyMsg;
    }
    if (same) return;
    auto previous = g_workers;
    StopWorkers();
    g_intervalMinutes = g_config.usageMeterRefreshMinutes;
    Log(L"codex_usage: monitoring %zu Codex profile(s), every %d minute(s)", paths.size(), g_intervalMinutes);
    for (size_t i = 0; i < paths.size(); i++) {
        auto w = std::make_shared<Worker>();
        w->configDir = paths[i];
        w->environment = environments[i];
        w->exe = exe;
        w->intervalMinutes = g_intervalMinutes;
        w->notifyWnd = notifyWnd;
        w->notifyMsg = notifyMsg;
        AcquireSRWLockShared(&g_lock);
        for (const auto& old : previous) {
            if (_wcsicmp(old->configDir.c_str(), w->configDir.c_str()) != 0) continue;
            w->snapshot = old->snapshot;
            w->loggedError = old->loggedError;
            w->failures = old->failures;
        }
        ReleaseSRWLockShared(&g_lock);
        w->snapshot.profile = ids[i];
        w->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto param = new std::shared_ptr<Worker>(w);
        w->thread = w->stop ? CreateThread(nullptr, 0, WorkerThread, param, 0, nullptr) : nullptr;
        if (!w->thread) {
            delete param;
            LogWarn(L"codex_usage: couldn't start worker (Windows error %lu)", GetLastError());
            continue;
        }
        g_workers.push_back(w);
    }
}

void StopCodexUsageMonitor() { StopWorkers(); }

void ProcessCodexUsageResults() {
    for (const auto& w : g_workers) {
        AcquireSRWLockShared(&g_lock);
        auto s = w->snapshot;
        unsigned seq = w->seq;
        ReleaseSRWLockShared(&g_lock);
        if (seq == w->loggedSeq) continue;
        w->loggedSeq = seq;
        if (!s.error.empty()) {
            ++w->failures;
            bool warn = s.transientError ? w->failures == 2 : s.error != w->loggedError;
            (warn ? LogWarn : LogDiag)(L"codex_usage: profile folder [%ls]: %ls", w->configDir.c_str(), s.error.c_str());
            w->loggedError = s.error;
        } else {
            (w->loggedSeq == 1 || !w->loggedError.empty() ? Log : LogDiag)(
                L"codex_usage: profile folder [%ls]: primary %.0f%%, secondary %.0f%%%ls", w->configDir.c_str(),
                s.primary.percent, s.secondary.percent, w->loggedError.empty() ? L"" : L" (readable again)");
            w->loggedError.clear();
            w->failures = 0;
        }
    }
}

std::vector<CodexUsageSnapshot> GetCodexUsageSnapshots() {
    std::vector<CodexUsageSnapshot> out;
    AcquireSRWLockShared(&g_lock);
    for (const auto& w : g_workers) out.push_back(w->snapshot);
    ReleaseSRWLockShared(&g_lock);
    return out;
}

bool IsCodexUsageStale(const CodexUsageSnapshot& s) {
    return !s.haveData || !s.error.empty() || UnixNow() - s.fetchedAt > 3LL * g_intervalMinutes * 60;
}

bool IsCodexUsageUnavailable(const CodexUsageSnapshot& s) {
    return !s.error.empty() || (s.haveData && IsCodexUsageStale(s));
}

double CurrentCodexUsagePercent(const CodexUsageLimit& limit) {
    if (limit.percent < 0) return -1;
    return limit.resetsAt > 0 && limit.resetsAt <= UnixNow() ? 0 : limit.percent;
}

std::wstring CodexUsageProfileName(const CodexUsageSnapshot& s) {
    if (const auto* p = FindProfile(s.profile)) {
        if (!p->name.empty()) return p->name;
    }
    if (!s.email.empty()) return s.email;
    size_t slash = s.profile.find_last_of(L"\\/");
    return slash == std::wstring::npos ? s.profile : s.profile.substr(slash + 1);
}

std::wstring DescribeCodexUsage(const CodexUsageSnapshot& s) {
    std::wstring text = L"Codex usage - " + CodexUsageProfileName(s);
    long long now = UnixNow();
    auto describe = [&](const std::wstring& label, const CodexUsageLimit& limit) {
        text += L"\n";
        text += label;
        double pct = CurrentCodexUsagePercent(limit);
        if (pct < 0) text += L": not reported";
        else {
            wchar_t number[40];
            swprintf(number, 40, L": %.0f%%", pct);
            text += number;
            if (limit.resetsAt) {
                bool reset = limit.resetsAt <= now;
                text += (reset ? L" - reset at " : L" - resets ") + FormatUsageLocalTime(limit.resetsAt);
                if (!reset) text += L" (in " + FormatUsageDuration(limit.resetsAt - now) + L")";
            }
        }
    };
    if (s.haveData) {
        std::wstring sessionLabel = L"Session", weeklyLabel = L"Weekly";
        if (s.primary.windowMinutes && s.primary.windowMinutes != 300)
            sessionLabel += L" (" + FormatUsageDuration(60LL * s.primary.windowMinutes) + L")";
        if (s.secondary.windowMinutes && s.secondary.windowMinutes != 10080)
            weeklyLabel = L"Secondary (" + FormatUsageDuration(60LL * s.secondary.windowMinutes) + L")";
        describe(sessionLabel, s.primary);
        describe(weeklyLabel, s.secondary);
        text += L"\nUpdated " + FormatUsageLocalTime(s.fetchedAt);
    } else if (s.error.empty()) text += L"\nNot read yet";
    if (!s.error.empty()) text += L"\n\nLast update failed at " + FormatUsageLocalTime(s.errorAt) + L":\n" + s.error;
    return text;
}
