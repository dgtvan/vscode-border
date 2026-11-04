#include "codex_provider.h"

#include "config.h"
#include "json_scan.h"
#include "logger.h"
#include "text_util.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

// Codex's own layout under a profile's CODEX_HOME. One rollout file per
// thread, named rollout-<local-start-time>-<thread-id>.jsonl under a
// year\month\day folder, and one empty lock file per *live* thread.
const wchar_t* kSessionsDir = L"sessions";
const wchar_t* kLocksDir = L"thread-writer-locks";

// A rollout's first line is its session_meta, which carries the whole
// system prompt: 20KB in every real rollout on this machine, so the window
// has to be generous. Read once per session (that line never changes) and
// cached, not re-read on every sync.
const DWORD kMetaScanBytes = 256 * 1024;

// How far back to look for the newest turn-boundary event the first time a
// rollout is seen. A finished turn's task_complete is one of the last lines
// written, so the small window almost always answers it; the large one is
// for a turn still running, whose task_started can be a long way back
// behind the reasoning blobs and tool output of everything since.
const DWORD kInitialTailBytes = 256 * 1024;
const DWORD kMaxTailBytes = 4 * 1024 * 1024;

// A running turn appends to its rollout constantly (reasoning, token
// counts, one line per completed item), so silence this long means the turn
// is over even though no task_complete was ever written -- which does
// happen: a real local rollout ends on a completed assistant message with
// its task_started never closed, and has sat that way for hours. Without
// this, that session would read as "working" for good.
const unsigned kWorkingSilenceMinutes = 10;

std::string ReadRange(const std::wstring& path, ULONGLONG offset, DWORD maxBytes) {
    // Full sharing, including write: a live session has this file open and
    // must never be held up, or have a write fail, because of this read.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string buf;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && (ULONGLONG)size.QuadPart > offset) {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)offset;
        DWORD toRead = (DWORD)std::min<ULONGLONG>(maxBytes, (ULONGLONG)size.QuadPart - offset);
        buf.assign(toRead, '\0');
        DWORD read = 0;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) || !ReadFile(h, &buf[0], toRead, &read, nullptr)) read = 0;
        buf.resize(read);
    }
    CloseHandle(h);
    return buf;
}

// How big the rollout is now. Only the size is taken from the directory
// entry: its last-write time is not maintained while Codex keeps the file
// open, so it says nothing about how fresh the contents are (see
// ParseCodexRolloutActivity).
bool FileSize(const std::wstring& path, ULONGLONG& size) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    return true;
}

// The content span (between the braces) of one line's root object, or false
// for a line that isn't exactly one complete JSON object.
bool LineObject(const std::string& line, size_t& start, size_t& end) {
    size_t open = JsonScan::FindRootObjectOpenBrace(line);
    if (open == std::string::npos) return false;
    size_t close = JsonScan::FindValueEnd(line, open);
    if (close <= open + 1 || close > line.size() || line[close - 1] != '}') return false;
    start = open + 1;
    end = close - 1;
    return true;
}

// A string field's decoded text. Codex writes Windows paths with escaped
// backslashes and leaves non-ASCII as plain UTF-8 rather than \u escapes,
// so undoing the two-character escapes is enough here; anything else is
// passed through exactly as written.
std::string StringField(const std::string& s, size_t objStart, size_t objEnd, const char* key) {
    JsonScan::KeyEntry e = JsonScan::FindKeyInObject(s, objStart, objEnd, key);
    if (!e.found || e.valueEnd - e.valueStart < 2 || s[e.valueStart] != '"') return "";
    std::string out;
    for (size_t i = e.valueStart + 1; i + 1 < e.valueEnd; i++) {
        if (s[i] == '\\' && i + 2 < e.valueEnd) {
            char c = s[++i];
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            out += c;
            continue;
        }
        out += s[i];
    }
    return out;
}

// The content span of an object-valued field, e.g. a line's "payload".
bool ObjectField(const std::string& s, size_t objStart, size_t objEnd, const char* key, size_t& start, size_t& end) {
    JsonScan::KeyEntry e = JsonScan::FindKeyInObject(s, objStart, objEnd, key);
    if (!e.found || e.valueEnd <= e.valueStart + 1 || s[e.valueStart] != '{') return false;
    start = e.valueStart + 1;
    end = e.valueEnd - 1;
    return true;
}

// Walks a region's lines newest first, calling visit(line) until it
// returns true. `floor` is where the first whole line starts.
template <typename F>
void ForEachLineBackwards(const std::string& region, size_t floor, F visit) {
    size_t end = region.size();
    while (end > floor) {
        size_t newline = region.find_last_of('\n', end - 1);
        size_t start = (newline == std::string::npos || newline < floor) ? floor : newline + 1;
        if (visit(region.substr(start, end - start))) return;
        end = start > floor ? start - 1 : floor;
    }
}

} // namespace

CodexRolloutMeta ParseCodexRolloutMeta(const std::string& head) {
    CodexRolloutMeta meta;
    size_t pos = 0;
    while (pos < head.size() && !meta.found) {
        size_t newline = head.find('\n', pos);
        if (newline == std::string::npos) break; // a first line still being written
        std::string line = head.substr(pos, newline - pos);
        pos = newline + 1;

        size_t objStart = 0, objEnd = 0, payloadStart = 0, payloadEnd = 0;
        if (!LineObject(line, objStart, objEnd)) continue;
        if (StringField(line, objStart, objEnd, "type") != "session_meta") continue;
        if (!ObjectField(line, objStart, objEnd, "payload", payloadStart, payloadEnd)) continue;

        std::string id = StringField(line, payloadStart, payloadEnd, "session_id");
        if (id.empty()) id = StringField(line, payloadStart, payloadEnd, "id");
        std::string cwd = StringField(line, payloadStart, payloadEnd, "cwd");
        if (id.empty() || cwd.empty()) continue;

        // "source" is the string "subagent" for a plain subagent session,
        // or an object naming which kind it is -- both shapes appear in
        // real rollouts, e.g. {"subagent":{"other":"guardian"}}.
        JsonScan::KeyEntry source = JsonScan::FindKeyInObject(line, payloadStart, payloadEnd, "source");
        if (source.found && source.valueStart < line.size()) {
            if (line[source.valueStart] == '"') {
                meta.subagent = StringField(line, payloadStart, payloadEnd, "source") == "subagent";
            } else if (line[source.valueStart] == '{' && source.valueEnd > source.valueStart + 1) {
                meta.subagent =
                    JsonScan::FindKeyInObject(line, source.valueStart + 1, source.valueEnd - 1, "subagent").found;
            }
        }
        meta.sessionId = Utf8ToWide(id);
        meta.cwd = Utf8ToWide(cwd);
        meta.found = true;
    }
    return meta;
}

std::wstring ParseCodexRolloutStatus(const std::string& region, bool startsMidLine) {
    size_t floor = 0;
    if (startsMidLine) {
        size_t newline = region.find('\n');
        if (newline == std::string::npos) return L"";
        floor = newline + 1;
    }

    std::wstring status;
    ForEachLineBackwards(region, floor, [&](const std::string& line) {
        size_t objStart = 0, objEnd = 0, payloadStart = 0, payloadEnd = 0;
        if (!LineObject(line, objStart, objEnd)) return false;
        if (StringField(line, objStart, objEnd, "type") != "event_msg") return false;
        if (!ObjectField(line, objStart, objEnd, "payload", payloadStart, payloadEnd)) return false;
        std::string event = StringField(line, payloadStart, payloadEnd, "type");

        // A turn starts on task_started and ends on task_complete, or on
        // turn_aborted when it was interrupted -- an interrupted turn is
        // over, same as a finished one.
        if (event == "task_started") status = L"working";
        else if (event == "task_complete" || event == "turn_aborted") status = L"waiting";
        // Codex does have events for a prompt it is blocked on, but whether
        // they ever reach a rollout is unconfirmed -- no local rollout has
        // contained one (see docs/CODEX-AI-STATUS.md). Recognised here so
        // that a Codex which does persist them reports Attention, and
        // harmless if they never arrive.
        else if (event == "exec_approval_request" || event == "apply_patch_approval_request" ||
                 event == "elicitation_request" || event == "request_user_input")
            status = L"attention";
        return !status.empty();
    });
    return status;
}

std::string ParseCodexRolloutActivity(const std::string& region, bool startsMidLine) {
    size_t floor = 0;
    if (startsMidLine) {
        size_t newline = region.find('\n');
        if (newline == std::string::npos) return "";
        floor = newline + 1;
    }

    std::string timestamp;
    ForEachLineBackwards(region, floor, [&](const std::string& line) {
        size_t objStart = 0, objEnd = 0;
        if (!LineObject(line, objStart, objEnd)) return false;
        timestamp = StringField(line, objStart, objEnd, "timestamp");
        return !timestamp.empty();
    });
    return timestamp;
}

bool IsCodexTimestampStale(const std::string& timestamp, unsigned minutes) {
    SYSTEMTIME st = {};
    if (sscanf(timestamp.c_str(), "%hu-%hu-%huT%hu:%hu:%hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute,
               &st.wSecond) != 6)
        return false;
    FILETIME then, now;
    if (!SystemTimeToFileTime(&st, &then)) return false;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = then.dwLowDateTime;
    a.HighPart = then.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return false; // clock skew, or a rollout written just now
    return (b.QuadPart - a.QuadPart) > (ULONGLONG)minutes * 60ULL * 10000000ULL;
}

void CodexProvider::SyncInstallation(bool /*enabled*/) {
    // Nothing to install or remove: this provider only reads what Codex
    // already writes for itself. Codex does have a Claude-Code-shaped hook
    // engine, but a hook installed into a profile's config.toml comes back
    // from Codex's own hooks/list as trustStatus "untrusted" until the user
    // approves it inside Codex, and again whenever the command changes --
    // so hooks would report nothing at all until that manual step, which
    // the rollout files don't need. See docs/CODEX-AI-STATUS.md.
}

namespace {

// Per-rollout scan state. A rollout is append-only, so after the first
// sight of one only the bytes added since are read, rather than re-reading
// the tail of a multi-megabyte file on every hub sync. Main thread only
// (LoadStatuses runs from SyncProjectListHud), like the Claude provider.
struct SessionState {
    bool metaKnown = false;
    bool skip = false; // a subagent's own session, or one whose id doesn't match its file
    std::wstring cwd;
    ULONGLONG scannedTo = 0; // bytes consumed; always lands just past a newline
    std::wstring status;     // newest turn boundary seen so far
    std::string activity;    // newest event timestamp seen so far, as Codex wrote it
    bool loggedSilent = false;
};
std::map<std::wstring, SessionState> g_sessions; // rollout path -> state

struct PathCache {
    std::wstring path;   // "" until found
    bool walked = false; // the whole sessions tree has been searched once for this id
};
std::map<std::wstring, PathCache> g_paths; // "<codex home>|<thread id>" -> where its rollout is

bool IsDir(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Every thread Codex currently has open in this CODEX_HOME. A lock file
// exists for exactly as long as its thread is live -- confirmed on a real
// machine: three running app-servers held six locks, one per open thread,
// while the four rollouts whose threads had since closed had no lock left.
// So nothing is reported for a window whose Codex panel is gone. A lock
// left behind by a killed process would keep its last status visible; the
// silence rule above still moves "working" off it.
std::vector<std::wstring> LiveThreadIds(const std::wstring& codexHome) {
    std::vector<std::wstring> ids;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((codexHome + L"\\" + kLocksDir + L"\\*.lock").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return ids;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name(fd.cFileName);
        if (name.empty() || name[0] == L'.') continue; // Codex's own coordination lock, not a thread
        ids.push_back(name.substr(0, name.size() - 5)); // strip ".lock"
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ids;
}

std::wstring FindInDir(const std::wstring& dir, const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        found = dir + L"\\" + fd.cFileName;
        break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// Depth-limited search of the sessions tree (year\month\day), for a live
// thread whose rollout predates the dates tried below -- a session resumed
// days later keeps appending to its original file.
std::wstring WalkForRollout(const std::wstring& dir, const std::wstring& pattern, int depth) {
    std::wstring found = FindInDir(dir, pattern);
    if (!found.empty() || depth == 0) return found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        found = WalkForRollout(dir + L"\\" + name, pattern, depth - 1);
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

std::wstring DateDir(const std::wstring& sessions, const SYSTEMTIME& st) {
    wchar_t buf[32];
    swprintf(buf, 32, L"\\%04u\\%02u\\%02u", st.wYear, st.wMonth, st.wDay);
    return sessions + buf;
}

// Where a thread's rollout file is, remembered per id: found once, then
// only re-checked for still being there. A live thread with no rollout yet
// (a Codex panel open, no prompt sent) is normal, and costs one cheap
// lookup per sync until its first turn creates the file.
std::wstring RolloutPathFor(const std::wstring& codexHome, const std::wstring& id) {
    PathCache& cache = g_paths[codexHome + L"|" + id];
    if (!cache.path.empty()) {
        if (GetFileAttributesW(cache.path.c_str()) != INVALID_FILE_ATTRIBUTES) return cache.path;
        cache.path.clear();
    }

    std::wstring sessions = codexHome + L"\\" + kSessionsDir;
    std::wstring pattern = L"rollout-*" + id + L".jsonl";
    // The folder is named after the day the thread started, and the file
    // after the local time it started at. Today and yesterday, in both
    // local and UTC time, cover a new thread whichever of the two the
    // folder itself follows.
    SYSTEMTIME local, utc;
    GetLocalTime(&local);
    GetSystemTime(&utc);
    std::vector<std::wstring> dirs = {DateDir(sessions, local), DateDir(sessions, utc)};
    FILETIME ft;
    if (SystemTimeToFileTime(&local, &ft)) {
        ULARGE_INTEGER t;
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        t.QuadPart -= 24ULL * 60ULL * 60ULL * 10000000ULL;
        ft.dwLowDateTime = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        SYSTEMTIME yesterday;
        if (FileTimeToSystemTime(&ft, &yesterday)) dirs.push_back(DateDir(sessions, yesterday));
    }
    for (const std::wstring& dir : dirs) {
        std::wstring found = IsDir(dir) ? FindInDir(dir, pattern) : L"";
        if (!found.empty()) {
            cache.path = found;
            return found;
        }
    }

    // Once per id per run: the dates above are retried cheaply on every
    // sync, but walking the whole tree is not worth repeating for a thread
    // that simply hasn't written its first turn yet.
    if (!cache.walked) {
        cache.walked = true;
        cache.path = WalkForRollout(sessions, pattern, 3);
        if (!cache.path.empty())
            LogDiag(L"codex_provider: %ls's rollout is outside the recent dates: %ls", id.c_str(),
                    cache.path.c_str());
    }
    return cache.path;
}

void CollectSession(const std::wstring& codexHome, const std::wstring& id, const std::wstring& path,
                    const std::wstring& home, std::vector<AiSessionStatus>& result) {
    SessionState& state = g_sessions[path];
    if (!state.metaKnown) {
        CodexRolloutMeta meta = ParseCodexRolloutMeta(ReadRange(path, 0, kMetaScanBytes));
        if (!meta.found) return; // first line still being written, or a layout this can't read
        state.metaKnown = true;
        state.cwd = meta.cwd;
        if (meta.subagent) {
            // A subagent runs inside its parent's turn, which is already
            // reported as working, so its own session adds nothing -- and
            // an abandoned one would linger as a turn that never ended.
            state.skip = true;
            LogDiag(L"codex_provider: %ls is a subagent's own session -- not reported", id.c_str());
        } else if (!meta.sessionId.empty() && _wcsicmp(meta.sessionId.c_str(), id.c_str()) != 0) {
            state.skip = true;
            LogWarn(L"codex_provider: %ls says its session is %ls -- not reporting it rather than attributing it to "
                    L"the wrong window",
                    path.c_str(), meta.sessionId.c_str());
        }
    }
    if (state.skip || state.cwd.empty()) return;

    ULONGLONG size = 0;
    if (!FileSize(path, size) || size == 0) return;

    if (size < state.scannedTo) { // replaced or rewritten -- start over
        state.scannedTo = 0;
        state.status.clear();
        state.activity.clear();
    }
    if (state.scannedTo == 0) {
        for (DWORD window : {kInitialTailBytes, kMaxTailBytes}) {
            ULONGLONG offset = size > window ? size - window : 0;
            std::string region = ReadRange(path, offset, window);
            std::string activity = ParseCodexRolloutActivity(region, offset > 0);
            if (!activity.empty()) state.activity = activity;
            std::wstring status = ParseCodexRolloutStatus(region, offset > 0);
            if (!status.empty()) {
                state.status = status;
                break;
            }
            if (offset == 0) break;
        }
        state.scannedTo = size;
    } else if (size > state.scannedTo) {
        DWORD toRead = (DWORD)std::min<ULONGLONG>(kMaxTailBytes, size - state.scannedTo);
        std::string region = ReadRange(path, state.scannedTo, toRead);
        size_t lastNewline = region.find_last_of('\n');
        if (lastNewline != std::string::npos) {
            std::string whole = region.substr(0, lastNewline + 1);
            std::string activity = ParseCodexRolloutActivity(whole, false);
            if (!activity.empty()) state.activity = activity;
            std::wstring status = ParseCodexRolloutStatus(whole, false);
            if (!status.empty()) state.status = status;
            state.scannedTo += lastNewline + 1;
        }
    }
    if (state.status.empty()) return;

    std::wstring status = state.status;
    if (status == L"working" && IsCodexTimestampStale(state.activity, kWorkingSilenceMinutes)) {
        if (!state.loggedSilent) {
            state.loggedSilent = true;
            Log(L"codex_provider: %ls's turn never reported finishing and its newest rollout event is older than %u "
                L"minutes (%hs) -- reporting it as waiting",
                id.c_str(), kWorkingSilenceMinutes, state.activity.c_str());
        }
        status = L"waiting";
    } else {
        state.loggedSilent = false;
    }

    AiSessionStatus session;
    session.status = status;
    session.cwd = state.cwd;
    session.home = home;
    result.push_back(session);
    LogDiag(L"codex_provider: %ls in %ls (%ls) is %ls", id.c_str(), state.cwd.c_str(), codexHome.c_str(),
            status.c_str());
}

} // namespace

std::vector<AiSessionStatus> CodexProvider::LoadStatuses() {
    std::vector<AiSessionStatus> result;
    wchar_t home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);

    std::set<std::wstring> seen;
    for (const Profile& p : g_config.profiles) {
        if (p.type != ProfileType::Codex || p.configDir.empty()) continue;
        for (const std::wstring& id : LiveThreadIds(p.configDir)) {
            std::wstring path = RolloutPathFor(p.configDir, id);
            if (path.empty()) continue; // a thread open, but with no turn yet
            seen.insert(path);
            CollectSession(p.configDir, id, path, home, result);
        }
    }

    // A thread that has closed is forgotten, so resuming it later is read
    // from scratch rather than from where its scan last left off.
    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        it = seen.count(it->first) ? std::next(it) : g_sessions.erase(it);
    }

    if (g_config.verboseLogging) {
        std::wstring summary;
        for (const AiSessionStatus& s : result) summary += L"[" + s.status + L" " + s.cwd + L"] ";
        LogDiag(L"codex_provider: LoadStatuses returning %zu session(s): %ls", result.size(), summary.c_str());
    }
    return result;
}
