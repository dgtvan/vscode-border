#include "claude_provider.h"

#include "account.h"
#include "config.h"
#include "file_util.h"
#include "json_scan.h"
#include "logger.h"
#include "text_util.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace {

const wchar_t* kEventNames[] = {
    L"SessionStart",       L"UserPromptSubmit", L"Stop",        L"StopFailure",
    L"SessionEnd",         L"PermissionRequest", L"Elicitation", L"ElicitationResult",
    L"SubagentStop", // treated the same as Stop -- see claude_status_hook.ps1
};

std::wstring GetSettingsPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf) + L"\\.claude\\settings.json";
}

// Where the hook script runs from, and so where its status files, its log
// and the install manifest live: next to the exe for this app's own
// account; the account's own folder in a status helper (see
// RunClaudeStatusHelper), which sets it.
std::wstring g_hookDir;

std::wstring HookDir() {
    return g_hookDir.empty() ? GetExeDir() : g_hookDir;
}

std::wstring GetManifestPath() {
    return HookDir() + L"\\ai_hooks_installed.ini";
}

std::wstring GetStatusDir() {
    return HookDir() + L"\\claude_status";
}

// The exact command string this app registers for every event -- the
// PowerShell script branches on hook_event_name from stdin, so one script
// path serves every event in kEventNames. Forward slashes are load-bearing, not
// cosmetic: Claude Code runs hook commands through Git Bash when it's
// installed, which silently eats unquoted backslashes (see README.md), so
// a backslash path here would "install" successfully but never actually
// fire. Quoted only when it has a space (another account's folder can), so
// an existing install's command stays the same string.
std::string BuildExpectedCommand() {
    std::wstring script = HookDir() + L"/claude_status_hook.ps1";
    std::replace(script.begin(), script.end(), L'\\', L'/');
    if (script.find(L' ') != std::wstring::npos) script = L"\"" + script + L"\"";
    return WideToUtf8(L"powershell -NoProfile -File " + script);
}

// Command strings this app has installed in the past, one escaped line
// each (text_util.h, same convention as label_aliases.ini) -- lets a
// relocated install (different absolute path -> different command string)
// still find and remove its *old* entries on uninstall, not just whatever
// the current path happens to be.
std::vector<std::string> LoadManifest() {
    std::vector<std::string> result;
    std::string content = ReadFileBytes(GetManifestPath());
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        pos = (eol == std::string::npos) ? content.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        result.push_back(WideToUtf8(UnescapeTextLine(line)));
    }
    return result;
}

void SaveManifest(const std::vector<std::string>& commands) {
    std::string out;
    for (const std::string& c : commands) out += EscapeTextLine(Utf8ToWide(c)) + "\n";
    WriteFileBytes(GetManifestPath(), out);
}

// Drills into one hooks-array element (`{"matcher":"","hooks":[{"type":
// "command","command":"..."}]}`) to extract its command string, or "" if
// the element doesn't have this exact shape (e.g. a "type":"http" hook, or
// anything not authored by this app).
std::string ExtractElementCommand(const std::string& content, const JsonScan::Span& element) {
    if (element.end <= element.start || content[element.start] != '{') return "";
    JsonScan::Span elemObj = {true, element.start + 1, element.end - 1};
    JsonScan::KeyEntry hooksKey = JsonScan::FindKeyInObject(content, elemObj.start, elemObj.end, "hooks");
    if (!hooksKey.found || hooksKey.valueStart >= content.size() || content[hooksKey.valueStart] != '[') return "";
    JsonScan::Span innerArr = {true, hooksKey.valueStart + 1, hooksKey.valueEnd - 1};
    std::vector<JsonScan::Span> innerElems = JsonScan::FindArrayElements(content, innerArr.start, innerArr.end);
    if (innerElems.empty() || content[innerElems[0].start] != '{') return "";
    JsonScan::Span firstObj = {true, innerElems[0].start + 1, innerElems[0].end - 1};
    JsonScan::KeyEntry cmdKey = JsonScan::FindKeyInObject(content, firstObj.start, firstObj.end, "command");
    if (!cmdKey.found || cmdKey.valueStart >= content.size() || content[cmdKey.valueStart] != '"') return "";
    std::string raw = content.substr(cmdKey.valueStart, cmdKey.valueEnd - cmdKey.valueStart);
    // Our own command strings are a forward-slash path + fixed flags, whose
    // only character that needs JSON escaping is the quote around a path
    // with a space, so undoing \" (and \\) is enough -- this never has to
    // decode an arbitrary JSON string, only recognize its own
    // previously-written one.
    std::string cmd;
    for (size_t i = 1; i + 1 < raw.size(); i++) {
        if (raw[i] == '\\' && i + 2 < raw.size() && (raw[i + 1] == '"' || raw[i + 1] == '\\')) i++;
        cmd += raw[i];
    }
    return cmd;
}

std::string BuildElementJson(const std::string& command) {
    std::string escaped;
    for (char c : command) {
        if (c == '"' || c == '\\') escaped += '\\';
        escaped += c;
    }
    return "{\"matcher\": \"\", \"hooks\": [{\"type\": \"command\", \"command\": \"" + escaped + "\"}]}";
}

// Confirms `pid` both exists right now and is currently a claude.exe
// process -- not just "some process is alive at this pid", so a pid
// Windows has since recycled for something unrelated doesn't read as
// "still running". `pid` is the ancestor claude.exe process the hook
// script found by walking up its own process tree at write time (see
// claude_status_hook.ps1's Find-ClaudeAncestorPid) -- looked up fresh
// here rather than trusting anything captured back then, since a stored
// snapshot (e.g. a remembered creation timestamp) can't tell us what's
// actually running right now the way a live query can.
bool IsClaudeProcessAlive(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &size) != 0;
    CloseHandle(h);
    if (!ok) return false;
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    std::wstring base = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    return _wcsicmp(base.c_str(), L"claude.exe") == 0;
}

// Fallback staleness check used only when a status file has no pid to
// verify liveness against (the ancestor walk in claude_status_hook.ps1
// failed to find a claude.exe process -- expected to be rare). A generous
// threshold, not a real "is this session done" signal like the pid check:
// it exists so this degraded case still cleans up on its own -- there's no
// manual recovery path, so it has to -- without misfiring on a genuinely
// long-running task the way a short timeout would (this never applies to
// a status that *has* a pid -- that path is checked precisely, every
// sync, regardless of age).
const DWORD kNoPidStaleMinutes = 24 * 60;

bool IsOlderThanMinutes(const FILETIME& ft, DWORD minutes) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = ft.dwLowDateTime;
    a.HighPart = ft.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return false;
    ULONGLONG ageTicks = b.QuadPart - a.QuadPart; // 100ns ticks
    return ageTicks > (ULONGLONG)minutes * 60ULL * 10000000ULL;
}

std::wstring GetProjectsDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf) + L"\\.claude\\projects";
}

// Claude Code names each project's transcript directory after that
// project's absolute path, with every non-alphanumeric character (':',
// '\', '/', '.', ' ', an already-literal '-', etc.) turned into its own
// '-' -- not collapsed, so a run of several non-alphanumeric characters in
// the original path becomes the same number of consecutive '-' -- and the
// drive letter lowercased. Confirmed against real directories on disk:
// D:\Src\Personal\vscode-border became d--Src-Personal-vscode-border (the
// colon and the leading backslash each become their own '-'), and a
// worktree path with a literal '.' in it
// (...\some-project.worktrees\...) became
// ...-some-project-worktrees-... -- the '.' isn't special-cased,
// it's just another non-alphanumeric character.
std::wstring EncodeProjectDirName(const std::wstring& cwd) {
    std::wstring encoded = cwd;
    if (!encoded.empty()) encoded[0] = towlower(encoded[0]);
    for (wchar_t& c : encoded) {
        if (!iswalnum(c)) c = L'-';
    }
    return encoded;
}

// How much more recently `newer` needs to have been written than `since`
// to count as "real, additional activity" rather than two hook/transcript
// writes that just happened to land within the same instant (the message
// that makes Claude Code fire e.g. PermissionRequest is typically appended
// to the transcript at essentially the same moment, so a zero-margin
// comparison would spuriously fire right as that very status is set).
const DWORD kTranscriptActivityMarginSeconds = 10;

// How recently the transcript itself must have been written, relative to
// *now*, to still count as "working" -- without this, a session whose
// transcript gets exactly one write after its last hook-reported snapshot
// (e.g. a metadata-only line like custom-title/bridge-session appended
// moments after the real Stop, before the hook script itself finishes and
// updates the status file) gets pinned to "working" forever: both
// timestamps are then frozen in the past, so "transcript newer than
// snapshot" stays true indefinitely with nothing left to ever re-touch
// either file and clear it. Confirmed against a real stuck case: transcript
// last written 08:53:20, status file last written 08:52:xx, both still
// exactly that an hour and 40+ minutes later -- every sync in between kept
// re-deriving "working" from those same two frozen timestamps.
const DWORD kTranscriptActivityRecentSeconds = 30;

bool IsNewerByMargin(const FILETIME& newer, const FILETIME& older, DWORD marginSeconds) {
    ULARGE_INTEGER a, b;
    a.LowPart = newer.dwLowDateTime;
    a.HighPart = newer.dwHighDateTime;
    b.LowPart = older.dwLowDateTime;
    b.HighPart = older.dwHighDateTime;
    if (a.QuadPart <= b.QuadPart) return false;
    return (a.QuadPart - b.QuadPart) > (ULONGLONG)marginSeconds * 10000000ULL;
}

// `recorded` is the transcript_path Claude Code itself passed to the hook
// (the status file's transcript= line) and wins whenever present. Deriving
// the path from cwd is only the fallback for status files written before
// that line existed, and it is wrong whenever the session has cd'd out of
// the project root: cwd follows the session's shell, so the derived
// directory may not exist at all. Observed on a session whose status file
// said cwd=...\proplyst\src\web\app\src -- every transcript check below
// silently found no file and did nothing for that session, which is how it
// sat on "attention" for 10+ minutes after the prompt was answered.
std::wstring GetTranscriptPath(const std::wstring& recorded, const std::wstring& cwd, const std::wstring& sessionId) {
    if (!recorded.empty()) return recorded;
    std::wstring projectsDir = GetProjectsDir();
    if (projectsDir.empty()) return L"";
    return projectsDir + L"\\" + EncodeProjectDirName(cwd) + L"\\" + sessionId + L".jsonl";
}

// Reads only the last `maxBytes` of a file, not the whole thing --
// transcripts grow to multiple MB over a long session, and this runs on
// every rescan, so re-reading the entire file each time just to look at
// its last line would mean repeatedly re-reading megabytes for no reason.
std::string ReadFileTail(const std::wstring& path, DWORD maxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return "";
    }
    LARGE_INTEGER offset;
    offset.QuadPart = (size.QuadPart > (LONGLONG)maxBytes) ? size.QuadPart - maxBytes : 0;
    if (!SetFilePointerEx(h, offset, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return "";
    }
    DWORD toRead = (DWORD)std::min<LONGLONG>(maxBytes, size.QuadPart);
    std::string buf(toRead, '\0');
    DWORD bytesRead = 0;
    bool ok = toRead == 0 || ReadFile(h, &buf[0], toRead, &bytesRead, nullptr) != 0;
    CloseHandle(h);
    if (!ok) return "";
    buf.resize(bytesRead);
    return buf;
}

// Cross-checks a status snapshot against independent evidence: Claude
// Code's own transcript file for that session (see EncodeProjectDirName)
// gets a new line appended for every message and tool call, regardless of
// which hooks happen to be wired up -- so its last-write-time answers "has
// anything actually happened in this session more recently than our last
// hook-reported snapshot?" without guessing off a wall-clock timeout.
//
// This exists because some transitions have no hook to report them: there's
// no "permission granted, resuming" event (unlike MCP elicitation, which
// has ElicitationResult), so a session can sit on "attention" long after
// the user has actually answered the prompt and Claude has gone back to
// work, with nothing to correct it before the eventual Stop. Confirmed
// against a real stuck case: PermissionRequest fired at 20:27, the
// transcript kept growing until 20:41 when Stop finally fired -- for that
// whole 14 minutes, this check would have correctly shown the transcript
// was still moving, well past this margin.
bool HasNewerTranscriptActivity(const std::wstring& transcriptPath, const FILETIME& sinceFt) {
    if (transcriptPath.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(transcriptPath.c_str(), GetFileExInfoStandard, &data)) return false;
    if (!IsNewerByMargin(data.ftLastWriteTime, sinceFt, kTranscriptActivityMarginSeconds)) return false;

    // Also require the transcript write itself to be recent -- see
    // kTranscriptActivityRecentSeconds' comment. Without this, the check
    // above never expires on its own once true.
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return !IsNewerByMargin(now, data.ftLastWriteTime, kTranscriptActivityRecentSeconds);
}

// The literal marker Claude Code appends to the transcript, as a plain
// user-role message, the moment the user interrupts a turn (Escape/Ctrl+C
// mid-response) -- confirmed against a real interrupted session's
// transcript. There's no hook for this at all (Stop does not fire on a
// manual interrupt -- confirmed by the same real case: the last hook this
// session ever fired was the UserPromptSubmit that started the turn, over
// 20 minutes before this was written), so this is the only signal telling
// us the turn actually ended.
const char* kInterruptedMarker = "[Request interrupted by user]";

// Enough to reach back past the metadata lines that can follow an interrupt
// (an api-request line alone runs to a couple of KB).
const DWORD kInterruptScanBytes = 64 * 1024;

// The top-level "type" of one transcript line, e.g. "user", "assistant",
// "custom-title"; empty if the line isn't a JSON object.
std::string GetTranscriptLineType(const std::string& line) {
    size_t open = JsonScan::FindRootObjectOpenBrace(line);
    if (open == std::string::npos) return "";
    size_t close = JsonScan::FindValueEnd(line, open);
    if (close <= open + 1) return "";
    JsonScan::KeyEntry type = JsonScan::FindKeyInObject(line, open + 1, close - 1, "type");
    if (!type.found || type.valueEnd - type.valueStart < 2 || line[type.valueStart] != '"') return "";
    return line.substr(type.valueStart + 1, type.valueEnd - type.valueStart - 2);
}

// True if the transcript's last message is that marker -- meaning nothing
// has happened in this session since the interrupt, i.e. the turn that was
// reported "working" is actually over. Deliberately checks only the last
// message (not "does this marker appear anywhere"), since an old interrupt
// earlier in a long session that Claude has since resumed work past is not
// still-relevant information. Lines that aren't messages are skipped:
// renaming the session after interrupting it appends custom-title and
// agent-name lines after the marker, and checking only the very last line
// left such a session on "working" for good.
bool WasLastTranscriptEntryInterrupted(const std::wstring& transcriptPath) {
    if (transcriptPath.empty()) return false;
    std::string tail = ReadFileTail(transcriptPath, kInterruptScanBytes);
    if (tail.empty()) return false;

    // A full read may start mid-line; that partial line is never examined.
    size_t floor = 0;
    if (tail.size() >= kInterruptScanBytes) {
        floor = tail.find('\n');
        if (floor == std::string::npos) return false;
        ++floor;
    }

    size_t end = tail.size();
    while (end > floor) {
        size_t start = tail.find_last_of('\n', end - 1);
        start = (start == std::string::npos || start < floor) ? floor : start + 1;
        std::string line = tail.substr(start, end - start);
        end = start > floor ? start - 1 : floor;

        std::string type = GetTranscriptLineType(line);
        if (type == "user" || type == "assistant") return line.find(kInterruptedMarker) != std::string::npos;
    }
    return false;
}

// Reads up to `maxBytes` starting at `offset` (clamped to the file's end).
std::string ReadFileRange(const std::wstring& path, ULONGLONG offset, DWORD maxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size;
    std::string buf;
    if (GetFileSizeEx(h, &size) && (ULONGLONG)size.QuadPart > offset) {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)offset;
        DWORD toRead = (DWORD)std::min<ULONGLONG>(maxBytes, (ULONGLONG)size.QuadPart - offset);
        buf.assign(toRead, '\0');
        DWORD bytesRead = 0;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) || !ReadFile(h, &buf[0], toRead, &bytesRead, nullptr))
            bytesRead = 0;
        buf.resize(bytesRead);
    }
    CloseHandle(h);
    return buf;
}

// Enough to span the answer to a prompt plus whatever landed alongside it
// (parallel tool results, a long command's output); the result is written the
// moment the prompt is answered, so it sits near the start of this window.
const DWORD kPendingToolScanBytes = 4 * 1024 * 1024;

// Whether the tool call a PermissionRequest was blocked on (the status
// file's pending_tool=, see claude_status_hook.ps1) has been answered.
//
// This is what actually clears "attention". There is no hook for "the user
// answered the prompt"; PostToolUse only fires once the tool has also run to
// completion, which for an approved long command can be minutes later.
// But the answer always reaches the transcript as that exact call's
// tool_result -- the approved tool's output, the user's AskUserQuestion
// answers, or the rejection -- carrying `"tool_use_id":"<id>"`. Checked
// across 131 real transcripts: all 15,043 occurrences of that string were
// on the tool_result for that id and nowhere else, so its presence is proof
// the prompt was answered, not a heuristic. Unlike HasNewerTranscriptActivity
// it has no recency window to expire, which matters: a session that answers
// a prompt and then spends ten minutes writing one long response writes
// nothing to its transcript in the meantime.
//
// Only the bytes after pending_offset are read (the result cannot predate
// the hook), and a hit is remembered per session so the file is not read
// again on every sync once answered.
bool IsPendingToolAnswered(const std::wstring& sessionId, const std::string& toolUseId,
                           const std::wstring& transcriptPath, ULONGLONG offset) {
    static std::map<std::wstring, std::string> answered; // sessionId -> last answered tool_use_id
    auto it = answered.find(sessionId);
    if (it != answered.end() && it->second == toolUseId) return true;
    if (transcriptPath.empty()) return false;

    std::string region = ReadFileRange(transcriptPath, offset, kPendingToolScanBytes);
    if (region.find("\"tool_use_id\":\"" + toolUseId + "\"") == std::string::npos) return false;
    answered[sessionId] = toolUseId;
    Log(L"claude_provider: %ls's pending tool call %ls has its result in the transcript -- the prompt was "
        L"answered, correcting attention to working",
        sessionId.c_str(), Utf8ToWide(toolUseId).c_str());
    return true;
}

// Whether every background task a "working" status was left waiting on (the
// status file's background_tasks=, see claude_status_hook.ps1) has ended.
//
// A task that finishes normally wakes Claude for a new turn, whose hooks
// rewrite the status file, so this is not needed there. It exists for a task
// stopped while the session is idle (from the panel or /tasks): Claude Code
// then starts no turn and fires no hook, but it does queue the task's
// <task-notification> into the transcript -- checked against claude.exe
// 2.1.301 with a headless session and a stop_task control request. Each
// notification names its task in a <task-id> element, which only exists once
// that task has ended.
//
// Only the bytes after background_offset are read, and a finished set is
// remembered per session so the file is not read again on every sync.
bool AreBackgroundTasksFinished(const std::wstring& sessionId, const std::string& taskIds,
                                const std::wstring& transcriptPath, ULONGLONG offset) {
    static std::map<std::wstring, std::string> finished; // sessionId -> last finished background_tasks value
    auto it = finished.find(sessionId);
    if (it != finished.end() && it->second == taskIds) return true;
    if (transcriptPath.empty()) return false;

    std::string region = ReadFileRange(transcriptPath, offset, kPendingToolScanBytes);
    size_t pos = 0;
    while (pos <= taskIds.size()) {
        size_t comma = taskIds.find(',', pos);
        std::string id = taskIds.substr(pos, (comma == std::string::npos ? taskIds.size() : comma) - pos);
        pos = (comma == std::string::npos) ? taskIds.size() + 1 : comma + 1;
        if (id.empty()) continue;
        if (region.find("<task-id>" + id + "</task-id>") == std::string::npos) return false;
    }
    finished[sessionId] = taskIds;
    Log(L"claude_provider: %ls's background tasks (%ls) have all ended per the transcript -- correcting working to "
        L"waiting",
        sessionId.c_str(), Utf8ToWide(taskIds).c_str());
    return true;
}

struct HooksLocation {
    bool ok = false;
    JsonScan::Span root;
    JsonScan::KeyEntry hooksEntry;
    JsonScan::Span hooksSpan; // content span between "hooks"'s braces
};

// Re-locates the root object and "hooks" key/span after any mutation
// (insert/remove shifts every position after the edit point) -- called
// after every content-changing step below instead of trying to track
// offset deltas by hand.
HooksLocation LocateHooks(std::string& content, const std::wstring& settingsPath) {
    HooksLocation loc;
    loc.root = JsonScan::EnsureRootObject(content);
    loc.hooksEntry = JsonScan::FindKeyInObject(content, loc.root.start, loc.root.end, "hooks");
    if (!loc.hooksEntry.found) return loc; // caller inserts "hooks" itself when needed
    if (loc.hooksEntry.valueStart >= content.size() || content[loc.hooksEntry.valueStart] != '{') {
        LogWarn(L"claude_provider: %ls's \"hooks\" value isn't an object -- skipping sync", settingsPath.c_str());
        return loc;
    }
    loc.hooksSpan = {true, loc.hooksEntry.valueStart + 1, loc.hooksEntry.valueEnd - 1};
    loc.ok = true;
    return loc;
}

} // namespace

static void SyncLocalInstallation(bool enabled) {
    std::wstring settingsPath = GetSettingsPath();
    if (settingsPath.empty()) {
        LogWarn(L"claude_provider: %%USERPROFILE%% not available, skipping hook sync");
        return;
    }

    std::string expectedCommand = BuildExpectedCommand();
    std::vector<std::string> manifest = LoadManifest();
    std::vector<std::string> ownedCommands = manifest;
    if (std::find(ownedCommands.begin(), ownedCommands.end(), expectedCommand) == ownedCommands.end()) {
        ownedCommands.push_back(expectedCommand);
    }

    std::string content = ReadFileBytes(settingsPath);
    bool hadBom = content.size() >= 3 && (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB &&
                  (unsigned char)content[2] == 0xBF;
    std::string bom = hadBom ? content.substr(0, 3) : "";
    if (hadBom) content.erase(0, 3);

    // A file that exists and is non-empty but has no recognizable JSON
    // object anywhere (hand-edit gone badly wrong, truncated write, etc.)
    // must never be treated as "start fresh" -- EnsureRootObject's
    // synthesize-if-missing fallback would silently discard it on write.
    if (!content.empty() && JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) {
        LogWarn(L"claude_provider: %ls exists but has no recognizable JSON object -- skipping sync rather than "
                L"risk overwriting it",
                settingsPath.c_str());
        return;
    }

    std::string original = content;

    HooksLocation loc = LocateHooks(content, settingsPath);
    if (!loc.ok && loc.hooksEntry.found) return; // found but malformed -- LocateHooks already warned
    if (!loc.hooksEntry.found) {
        if (!enabled) return; // nothing to install, no "hooks" key to remove from either
        content = JsonScan::InsertIntoObject(content, loc.root.start, loc.root.end, "hooks", "{\n    }");
        loc = LocateHooks(content, settingsPath);
        if (!loc.ok) return;
    }

    for (const wchar_t* eventNameW : kEventNames) {
        std::string eventName = WideToUtf8(eventNameW);

        JsonScan::KeyEntry eventEntry =
            JsonScan::FindKeyInObject(content, loc.hooksSpan.start, loc.hooksSpan.end, eventName);
        if (!eventEntry.found) {
            if (!enabled) continue;
            content = JsonScan::InsertIntoObject(content, loc.hooksSpan.start, loc.hooksSpan.end, eventName,
                                                 "[\n    ]");
            loc = LocateHooks(content, settingsPath);
            if (!loc.ok) return;
            eventEntry = JsonScan::FindKeyInObject(content, loc.hooksSpan.start, loc.hooksSpan.end, eventName);
        }
        if (eventEntry.valueStart >= content.size() || content[eventEntry.valueStart] != '[') {
            LogWarn(L"claude_provider: %ls's \"hooks.%ls\" value isn't an array -- skipping this event",
                    settingsPath.c_str(), eventNameW);
            continue;
        }

        JsonScan::Span arrSpan = {true, eventEntry.valueStart + 1, eventEntry.valueEnd - 1};
        std::vector<JsonScan::Span> elements = JsonScan::FindArrayElements(content, arrSpan.start, arrSpan.end);

        bool alreadyHasExpected = false;
        std::vector<JsonScan::Span> toRemove;
        for (const JsonScan::Span& elem : elements) {
            std::string cmd = ExtractElementCommand(content, elem);
            if (cmd.empty()) continue; // not one of our entries -- e.g. a hook the user added themselves
            if (cmd == expectedCommand) {
                alreadyHasExpected = true;
                if (!enabled) toRemove.push_back(elem);
            } else if (std::find(ownedCommands.begin(), ownedCommands.end(), cmd) != ownedCommands.end()) {
                // A stale entry from a relocated install -- always replace
                // it, whether installing or uninstalling, since only one
                // (the current) entry should ever be present.
                toRemove.push_back(elem);
            }
        }
        for (auto it = toRemove.rbegin(); it != toRemove.rend(); ++it) {
            content = JsonScan::RemoveArrayElement(content, *it);
        }
        if (!toRemove.empty()) {
            loc = LocateHooks(content, settingsPath);
            if (!loc.ok) return;
        }

        if (enabled && !alreadyHasExpected) {
            eventEntry = JsonScan::FindKeyInObject(content, loc.hooksSpan.start, loc.hooksSpan.end, eventName);
            arrSpan = {true, eventEntry.valueStart + 1, eventEntry.valueEnd - 1};
            content = JsonScan::InsertIntoArray(content, arrSpan.start, arrSpan.end, BuildElementJson(expectedCommand));
            loc = LocateHooks(content, settingsPath);
            if (!loc.ok) return;
        }
    }

    if (content != original) {
        if (!WriteFileBytes(settingsPath, bom + content)) {
            LogWarn(L"claude_provider: failed to write %ls", settingsPath.c_str());
            return;
        }
        Log(L"claude_provider: %ls hooks in %ls", enabled ? L"installed/refreshed" : L"removed",
            settingsPath.c_str());
    }

    if (enabled && std::find(manifest.begin(), manifest.end(), expectedCommand) == manifest.end()) {
        manifest.push_back(expectedCommand);
        SaveManifest(manifest);
    }
}

static std::vector<AiSessionStatus> LoadLocalStatuses() {
    std::vector<AiSessionStatus> result;

    std::wstring dir = GetStatusDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result; // no directory yet -- no sessions have ever reported in

    // Warned-about-missing-pid sessions, so that case logs once per session
    // rather than once per sync (this function runs frequently). Deletions
    // are collected and applied after FindNextFileW is done iterating,
    // rather than mutating the directory mid-enumeration.
    static std::set<std::wstring> warnedMissingPid;
    // Sessions already logged as interrupted, so that correction logs once
    // per interrupt rather than on every sync for as long as it holds.
    static std::set<std::wstring> loggedInterrupted;
    std::vector<std::wstring> toDelete;
    wchar_t home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fileName(fd.cFileName);
        std::wstring path = dir + L"\\" + fileName;

        std::string content = ReadFileBytes(path);
        AiSessionStatus status;
        status.home = home;
        DWORD pid = 0;
        std::wstring recordedTranscript, pendingTranscript;
        std::string pendingTool, backgroundTasks;
        ULONGLONG pendingOffset = 0, backgroundOffset = 0;
        size_t pos = 0;
        while (pos < content.size()) {
            size_t eol = content.find('\n', pos);
            std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
            pos = (eol == std::string::npos) ? content.size() : eol + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();

            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            std::string rawValue = line.substr(eq + 1);
            if (key == "status") status.status = Utf8ToWide(rawValue);
            else if (key == "cwd") status.cwd = Utf8ToWide(rawValue);
            else if (key == "pid") pid = (DWORD)atol(rawValue.c_str());
            else if (key == "transcript") recordedTranscript = Utf8ToWide(rawValue);
            else if (key == "pending_tool") pendingTool = rawValue;
            else if (key == "pending_transcript") pendingTranscript = Utf8ToWide(rawValue);
            else if (key == "pending_offset") pendingOffset = _strtoui64(rawValue.c_str(), nullptr, 10);
            else if (key == "background_tasks") backgroundTasks = rawValue;
            else if (key == "background_offset") backgroundOffset = _strtoui64(rawValue.c_str(), nullptr, 10);
        }
        if (status.status.empty() || status.cwd.empty()) continue;

        LogDiag(L"claude_provider: read %ls: status=%ls cwd=%ls pid=%lu", fileName.c_str(), status.status.c_str(),
                status.cwd.c_str(), pid);

        if (pid != 0) {
            if (!IsClaudeProcessAlive(pid)) {
                // Confirmed dead: the process this status came from is gone
                // (or that pid now belongs to something else entirely), so
                // SessionEnd clearly never got the chance to clean this up
                // itself -- delete it now, automatically. This is what
                // actually closes the "forced kill" detection gap, not
                // just documents it.
                toDelete.push_back(path);
                Log(L"claude_provider: pid=%lu for %ls is no longer a live claude.exe process -- treating its "
                    L"status as ended",
                    pid, fileName.c_str());
                continue;
            }
        } else if (IsOlderThanMinutes(fd.ftLastWriteTime, kNoPidStaleMinutes)) {
            // No pid to verify liveness against, and this hasn't been
            // touched in a long time -- treat it as abandoned. See
            // kNoPidStaleMinutes' comment for why this is a generous,
            // separate threshold from the precise pid-based check above,
            // not a general staleness timeout.
            toDelete.push_back(path);
            warnedMissingPid.erase(fileName);
            Log(L"claude_provider: %ls has no pid recorded and hasn't been updated in over %lu minutes -- "
                L"treating it as abandoned",
                fileName.c_str(), kNoPidStaleMinutes);
            continue;
        } else if (warnedMissingPid.insert(fileName).second) {
            // A missing pid is expected to be rare (the hook's ancestor
            // walk failing to find a claude.exe process, or a leftover file
            // from before this feature existed) -- degrades gracefully
            // (still shown, still eventually auto-cleaned by the staleness
            // check above) rather than failing, but is still worth
            // surfacing rather than silently doing nothing.
            LogWarn(L"claude_provider: %ls has no pid recorded -- can't verify liveness for it, will fall back to "
                    L"a %lu-minute staleness check",
                    fileName.c_str(), kNoPidStaleMinutes);
        }

        // The hook-reported status can be stale in ways that have no hook
        // to correct them. Cross-checking against the session's own
        // transcript file (see GetTranscriptPath and friends) catches both
        // directions with real evidence instead of a wall-clock guess:
        std::wstring sessionId = fileName.substr(0, fileName.size() - 4); // strip ".ini"
        std::wstring transcriptPath = GetTranscriptPath(recordedTranscript, status.cwd, sessionId);
        if (status.status == L"attention" && !pendingTool.empty()) {
            // A permission prompt whose tool call is known: its own result
            // is the exact answer (see IsPendingToolAnswered), so the
            // looser activity check below is deliberately not consulted --
            // unrelated transcript writes while the prompt is still open
            // (a parallel tool's result, a title update) must not hide a
            // session that genuinely needs the user.
            if (IsPendingToolAnswered(sessionId, pendingTool,
                                      pendingTranscript.empty() ? transcriptPath : pendingTranscript, pendingOffset))
                status.status = L"working";
        } else if (status.status != L"working" && HasNewerTranscriptActivity(transcriptPath, fd.ftLastWriteTime)) {
            // "Attention" (or "Waiting"): there's no "permission granted,
            // resuming" event, so once a permission prompt fires, nothing
            // updates the status again until the turn's eventual Stop,
            // even though the user may have long since answered it and
            // Claude gone back to work.
            Log(L"claude_provider: %ls's transcript has newer activity than its last %ls snapshot -- correcting to "
                L"working",
                fileName.c_str(), status.status.c_str());
            status.status = L"working";
        } else if (status.status == L"working" && !backgroundTasks.empty() &&
                   AreBackgroundTasksFinished(sessionId, backgroundTasks, transcriptPath, backgroundOffset)) {
            // "Working" only because a turn ended with background work
            // running, and that work has since been stopped with no new
            // turn to say so -- see AreBackgroundTasksFinished.
            status.status = L"waiting";
        }
        // Deliberately not chained to the corrections above: a session one
        // of them just moved to "working" can equally have been interrupted
        // since (answering a prompt and then pressing Escape leaves exactly
        // that shape), and the marker is still the last word on it.
        if (status.status == L"working" && WasLastTranscriptEntryInterrupted(transcriptPath)) {
            // "Working": Stop does not fire on a manual interrupt
            // (Escape/Ctrl+C mid-response) -- confirmed against a real
            // session that stayed reported as "working" over 20 minutes
            // after the user interrupted it, with no further hook ever
            // firing. The transcript's own interrupt marker is the only
            // signal that the turn actually ended.
            if (loggedInterrupted.insert(fileName).second)
                Log(L"claude_provider: %ls's transcript shows the turn was interrupted by the user -- correcting to "
                    L"waiting",
                    fileName.c_str());
            status.status = L"waiting";
        } else {
            loggedInterrupted.erase(fileName);
        }

        result.push_back(status);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (const std::wstring& path : toDelete) DeleteFileW(path.c_str());

    if (g_config.verboseLogging) {
        std::wstring summary;
        for (const AiSessionStatus& s : result) summary += L"[" + s.status + L" " + s.cwd + L"] ";
        LogDiag(L"claude_provider: LoadStatuses returning %zu session(s): %ls", result.size(), summary.c_str());
    }

    return result;
}

const wchar_t* const kClaudeStatusHelperArg = L"--claude-status-helper";

namespace {

// How often a status helper re-reads its account's sessions. Every read
// re-runs the same transcript checks the app runs for its own account on
// each hub sync.
const DWORD kHelperPollMs = 1000;

// How long before a helper that couldn't start, or exited, is started again.
const DWORD kHelperRetryMs = 30 * 1000;

// Another account's sessions, reported by a status helper running as it
// (see RunClaudeStatusHelper). Shared between its worker thread and the
// main thread, under g_remoteLock.
struct RemoteAccount {
    std::wstring account;
    bool install = true; // false: a one-shot helper that only removes the hooks
    bool verbose = false;
    HANDLE stopEvent = nullptr;
    HANDLE child = nullptr; // the running helper, for StopRemote to end
    bool finished = false;  // the worker has returned

    std::vector<AiSessionStatus> sessions; // the helper's latest report
    // Lines for the main thread to log (Log isn't safe off it): the helper's
    // own, and the worker's. true = a warning.
    std::vector<std::pair<bool, std::wstring>> logLines;
};

SRWLOCK g_remoteLock = SRWLOCK_INIT;
std::vector<std::shared_ptr<RemoteAccount>> g_remotes; // main thread only

void QueueRemoteLog(RemoteAccount& r, bool warning, const std::wstring& line) {
    AcquireSRWLockExclusive(&g_remoteLock);
    r.logLines.emplace_back(warning, line);
    ReleaseSRWLockExclusive(&g_remoteLock);
}

// Runs one helper as r.account until it exits. Returns the Win32 error when
// one couldn't be started, else 0.
DWORD RunStatusHelperOnce(RemoteAccount& r) {
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE childIn = nullptr, ourIn = nullptr, ourOut = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &ourIn, &inherit, 0) || !CreatePipe(&ourOut, &childOut, &inherit, 0)) {
        DWORD err = GetLastError();
        if (childIn) CloseHandle(childIn);
        if (ourIn) CloseHandle(ourIn);
        return err;
    }
    // Only the child's ends may be inherited: a copy of ours in the child
    // would keep its stdin from ever reaching end-of-file.
    SetHandleInformation(ourIn, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ourOut, HANDLE_FLAG_INHERIT, 0);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" " + kClaudeStatusHelperArg +
                               (r.install ? L" install" : L" uninstall") + (r.verbose ? L" verbose" : L"");
    DWORD err = 0;
    HANDLE child = StartHiddenProcessAsAccount(r.account, exe, commandLine, GetExeDir(), childIn, childOut, err);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (!child) {
        CloseHandle(ourIn);
        CloseHandle(ourOut);
        return err ? err : ERROR_GEN_FAILURE;
    }
    AcquireSRWLockExclusive(&g_remoteLock);
    r.child = child;
    ReleaseSRWLockExclusive(&g_remoteLock);
    if (WaitForSingleObject(r.stopEvent, 0) == WAIT_OBJECT_0) TerminateProcess(child, 0);

    std::wstring home;
    std::vector<AiSessionStatus> pending;
    std::string buffered;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(ourOut, buf, sizeof(buf), &read, nullptr) && read > 0) {
        buffered.append(buf, read);
        size_t eol;
        while ((eol = buffered.find('\n')) != std::string::npos) {
            std::wstring line = Utf8ToWide(buffered.substr(0, eol));
            buffered.erase(0, eol + 1);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.rfind(L"log ", 0) == 0 && line.size() >= 6) {
                QueueRemoteLog(r, line[4] == L'W', line.substr(6));
            } else if (line.rfind(L"home ", 0) == 0) {
                home = line.substr(5);
            } else if (line == L"begin") {
                pending.clear();
            } else if (line.rfind(L"s ", 0) == 0) {
                size_t tab = line.find(L'\t');
                if (tab == std::wstring::npos) continue;
                AiSessionStatus s;
                s.status = line.substr(2, tab - 2);
                s.cwd = line.substr(tab + 1);
                s.account = r.account;
                s.home = home;
                pending.push_back(s);
            } else if (line == L"end") {
                AcquireSRWLockExclusive(&g_remoteLock);
                r.sessions = pending;
                ReleaseSRWLockExclusive(&g_remoteLock);
            }
        }
    }

    AcquireSRWLockExclusive(&g_remoteLock);
    r.child = nullptr;
    r.sessions.clear();
    ReleaseSRWLockExclusive(&g_remoteLock);
    WaitForSingleObject(child, 2000);
    DWORD code = 0;
    GetExitCodeProcess(child, &code);
    if (WaitForSingleObject(r.stopEvent, 0) != WAIT_OBJECT_0 && (r.install || code != 0)) {
        wchar_t text[200];
        swprintf(text, 200, L"claude_provider: the status helper running as [%ls] exited (code %lu)",
                 r.account.c_str(), code);
        QueueRemoteLog(r, r.install, text);
    }
    CloseHandle(child);
    CloseHandle(ourIn);
    CloseHandle(ourOut);
    return 0;
}

DWORD WINAPI RemoteWorkerThread(LPVOID param) {
    std::shared_ptr<RemoteAccount> owned = *(std::shared_ptr<RemoteAccount>*)param;
    delete (std::shared_ptr<RemoteAccount>*)param;
    RemoteAccount& r = *owned;
    DWORD lastErr = 0;
    do {
        DWORD err = RunStatusHelperOnce(r);
        if (err != 0 && err != lastErr) {
            wchar_t text[400];
            if (err == ERROR_NOT_FOUND) {
                swprintf(text, 400,
                         L"claude_provider: no password saved for [%ls], so %ls -- open a VS Code window as it "
                         L"from the hub's + button and tick Remember",
                         r.account.c_str(),
                         r.install ? L"its windows show no AI status" : L"its Claude Code hooks can't be removed");
            } else if (err == ERROR_LOGON_FAILURE) {
                swprintf(text, 400,
                         L"claude_provider: the saved password for [%ls] was rejected, so its windows show no AI "
                         L"status -- not trying it again until Reload Config",
                         r.account.c_str());
            } else {
                swprintf(text, 400, L"claude_provider: couldn't start the status helper as [%ls]: %ls",
                         r.account.c_str(), StartAsErrorText(err).c_str());
            }
            // Removing the hooks only matters to someone who uses the
            // feature on that account, so its failures aren't warnings.
            QueueRemoteLog(r, r.install, text);
        }
        lastErr = err;
        // Never retry a password that was rejected: that could lock the
        // account out.
        if (!r.install || err == ERROR_LOGON_FAILURE) break;
    } while (WaitForSingleObject(r.stopEvent, kHelperRetryMs) != WAIT_OBJECT_0);
    AcquireSRWLockExclusive(&g_remoteLock);
    r.finished = true;
    ReleaseSRWLockExclusive(&g_remoteLock);
    return 0;
}

void StartRemote(const std::wstring& account, bool install) {
    auto r = std::make_shared<RemoteAccount>();
    r->account = account;
    r->install = install;
    r->verbose = g_config.verboseLogging;
    r->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, RemoteWorkerThread, new std::shared_ptr<RemoteAccount>(r), 0, nullptr);
    if (!thread) {
        LogWarn(L"claude_provider: couldn't start the status worker for [%ls] (error %lu)", account.c_str(),
                GetLastError());
        return;
    }
    CloseHandle(thread); // it holds its own reference to r
    g_remotes.push_back(r);
}

void StopRemote(RemoteAccount& r) {
    SetEvent(r.stopEvent);
    AcquireSRWLockExclusive(&g_remoteLock);
    if (r.child) TerminateProcess(r.child, 0);
    r.sessions.clear();
    ReleaseSRWLockExclusive(&g_remoteLock);
}

void FlushRemoteLogs() {
    std::vector<std::pair<bool, std::wstring>> lines;
    AcquireSRWLockExclusive(&g_remoteLock);
    for (const std::shared_ptr<RemoteAccount>& r : g_remotes) {
        lines.insert(lines.end(), r->logLines.begin(), r->logLines.end());
        r->logLines.clear();
    }
    ReleaseSRWLockExclusive(&g_remoteLock);
    for (const auto& l : lines) (l.first ? LogWarn : Log)(L"%ls", l.second.c_str());
}

bool SameAccount(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Matches the status helpers to `enabled` and the user profiles: one per
// profile while enabled, which installs the hooks for its account and
// reports its sessions; and a one-shot helper removing the hooks from every
// profile while disabled, and from one dropped from the config.
void SyncRemoteInstallations(bool enabled) {
    FlushRemoteLogs();
    std::vector<std::wstring> profiles = ConfiguredUserProfiles();
    auto isProfile = [&](const std::wstring& account) {
        return std::any_of(profiles.begin(), profiles.end(),
                           [&](const std::wstring& p) { return SameAccount(p, account); });
    };

    std::vector<std::wstring> uninstall = enabled ? std::vector<std::wstring>() : profiles;
    std::vector<std::shared_ptr<RemoteAccount>> kept;
    for (const std::shared_ptr<RemoteAccount>& r : g_remotes) {
        AcquireSRWLockShared(&g_remoteLock);
        bool finished = r->finished;
        ReleaseSRWLockShared(&g_remoteLock);
        // A one-shot uninstall ends on its own, kept until then for its
        // lines to be logged.
        bool wanted = r->install ? enabled && isProfile(r->account) && r->verbose == g_config.verboseLogging
                                 : !finished;
        if (wanted) {
            kept.push_back(r);
            continue;
        }
        if (!r->install) continue;
        StopRemote(*r);
        if (!isProfile(r->account)) uninstall.push_back(r->account);
    }
    FlushRemoteLogs();
    g_remotes = kept;

    for (const std::wstring& account : uninstall) StartRemote(account, false);
    if (!enabled) return;
    for (const std::wstring& p : profiles) {
        bool running = std::any_of(kept.begin(), kept.end(), [&](const std::shared_ptr<RemoteAccount>& r) {
            return r->install && SameAccount(r->account, p);
        });
        if (!running) StartRemote(p, true);
    }
}

// The status helper's stdout protocol, one UTF-8 line each:
//   log W|I <text>    a line for the app's log (W: a warning)
//   home <path>       the account's home directory, once, before any report
//   begin             a report of every session follows...
//   s <status>\t<cwd> ...one per session...
//   end               ...and it's complete
// A report is sent at start and whenever it changes.
HANDLE g_helperOut = nullptr;

bool WriteHelperLine(const std::wstring& line) {
    std::string bytes = WideToUtf8(line);
    for (char& c : bytes) {
        if (c == '\r' || c == '\n') c = ' ';
    }
    bytes += '\n';
    DWORD written = 0;
    return WriteFile(g_helperOut, bytes.data(), (DWORD)bytes.size(), &written, nullptr) != 0;
}

void HelperLogSink(bool warning, const std::wstring& line) {
    WriteHelperLine(std::wstring(warning ? L"log W " : L"log I ") + L"[" + CurrentAccountName() + L"] " + line);
}

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

// Points HookDir at a folder of the account this runs as, and copies the
// hook script there: that account can't be trusted to write this app's
// folder, and its sessions must not mix with this account's. The script's
// status files and log go next to it, so they land there too.
bool PrepareOwnHookDir() {
    wchar_t localAppData[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        LogWarn(L"claude_provider: %%LOCALAPPDATA%% not available, so there's nowhere to put the hook script");
        return false;
    }
    std::wstring dir = std::wstring(localAppData) + L"\\vscode-border";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring source = GetExeDir() + L"\\claude_status_hook.ps1", target = dir + L"\\claude_status_hook.ps1";
    std::string script = ReadFileBytes(source);
    if (script.empty()) {
        LogWarn(L"claude_provider: can't read %ls", source.c_str());
        return false;
    }
    if (ReadFileBytes(target) != script && !WriteFileBytes(target, script)) {
        LogWarn(L"claude_provider: can't copy the hook script to %ls", target.c_str());
        return false;
    }
    g_hookDir = dir;
    return true;
}

} // namespace

void ClaudeProvider::SyncInstallation(bool enabled) {
    SyncLocalInstallation(enabled);
    SyncRemoteInstallations(enabled);
}

std::vector<AiSessionStatus> ClaudeProvider::LoadStatuses() {
    FlushRemoteLogs();
    std::vector<AiSessionStatus> result = LoadLocalStatuses();
    AcquireSRWLockShared(&g_remoteLock);
    for (const std::shared_ptr<RemoteAccount>& r : g_remotes) {
        result.insert(result.end(), r->sessions.begin(), r->sessions.end());
    }
    ReleaseSRWLockShared(&g_remoteLock);
    return result;
}

int RunClaudeStatusHelper(bool install, bool verbose) {
    g_helperOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_config.verboseLogging = verbose;
    SetLogSink(HelperLogSink);
    if (!PrepareOwnHookDir()) return 1;
    SyncLocalInstallation(install);
    if (!install) return 0;

    wchar_t home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (!WriteHelperLine(std::wstring(L"home ") + home)) return 1;

    HANDLE stdinClosed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, StdinWatchThread, new StdinWatch{GetStdHandle(STD_INPUT_HANDLE), stdinClosed}, 0,
                 nullptr);
    std::vector<std::wstring> lastReport;
    bool first = true;
    do {
        std::vector<std::wstring> report;
        for (const AiSessionStatus& s : LoadLocalStatuses()) report.push_back(L"s " + s.status + L"\t" + s.cwd);
        if (!first && report == lastReport) continue;
        first = false;
        lastReport = report;
        bool ok = WriteHelperLine(L"begin");
        for (const std::wstring& line : report) ok = ok && WriteHelperLine(line);
        if (!ok || !WriteHelperLine(L"end")) return 1;
    } while (WaitForSingleObject(stdinClosed, kHelperPollMs) != WAIT_OBJECT_0);
    return 0;
}
