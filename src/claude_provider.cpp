#include "claude_provider.h"

#include "config.h"
#include "file_util.h"
#include "json_scan.h"
#include "logger.h"
#include "text_util.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <set>
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

std::wstring GetManifestPath() {
    return GetExeDir() + L"\\ai_hooks_installed.ini";
}

std::wstring GetStatusDir() {
    return GetExeDir() + L"\\claude_status";
}

// The exact command string this app registers for every event -- the
// PowerShell script branches on hook_event_name from stdin, so one script
// path serves every event in kEventNames. Forward slashes are load-bearing, not
// cosmetic: Claude Code runs hook commands through Git Bash when it's
// installed, which silently eats unquoted backslashes (see README.md), so
// a backslash path here would "install" successfully but never actually
// fire.
std::string BuildExpectedCommand() {
    std::wstring exeDir = GetExeDir();
    std::replace(exeDir.begin(), exeDir.end(), L'\\', L'/');
    std::wstring full = L"powershell -NoProfile -File " + exeDir + L"/claude_status_hook.ps1";
    return WideToUtf8(full);
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
    // Our own command strings are plain ASCII (a forward-slash path + fixed
    // flags) with no characters that need JSON escaping, so a straight
    // substring between the quotes is enough -- this never has to decode
    // an arbitrary JSON string, only recognize its own previously-written one.
    return raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : "";
}

std::string BuildElementJson(const std::string& command) {
    return "{\"matcher\": \"\", \"hooks\": [{\"type\": \"command\", \"command\": \"" + command + "\"}]}";
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
// (...\ciam-app-terraform.worktrees\...) became
// ...-ciam-app-terraform-worktrees-... -- the '.' isn't special-cased,
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

bool IsNewerByMargin(const FILETIME& newer, const FILETIME& older, DWORD marginSeconds) {
    ULARGE_INTEGER a, b;
    a.LowPart = newer.dwLowDateTime;
    a.HighPart = newer.dwHighDateTime;
    b.LowPart = older.dwLowDateTime;
    b.HighPart = older.dwHighDateTime;
    if (a.QuadPart <= b.QuadPart) return false;
    return (a.QuadPart - b.QuadPart) > (ULONGLONG)marginSeconds * 10000000ULL;
}

std::wstring GetTranscriptPath(const std::wstring& cwd, const std::wstring& sessionId) {
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
bool HasNewerTranscriptActivity(const std::wstring& cwd, const std::wstring& sessionId, const FILETIME& sinceFt) {
    std::wstring transcriptPath = GetTranscriptPath(cwd, sessionId);
    if (transcriptPath.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(transcriptPath.c_str(), GetFileExInfoStandard, &data)) return false;
    return IsNewerByMargin(data.ftLastWriteTime, sinceFt, kTranscriptActivityMarginSeconds);
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

// True if the transcript's last entry is that marker -- meaning nothing
// has happened in this session since the interrupt, i.e. the turn that was
// reported "working" is actually over. Deliberately checks only the very
// last line (not "does this marker appear anywhere"), since an old
// interrupt earlier in a long session that Claude has since resumed work
// past is not still-relevant information.
bool WasLastTranscriptEntryInterrupted(const std::wstring& cwd, const std::wstring& sessionId) {
    std::wstring transcriptPath = GetTranscriptPath(cwd, sessionId);
    if (transcriptPath.empty()) return false;
    std::string tail = ReadFileTail(transcriptPath, 8192);
    if (tail.empty()) return false;

    // The read may start mid-line, and the file may end with a trailing
    // newline -- walk back to the last non-blank line within the tail.
    size_t end = tail.find_last_not_of("\r\n");
    if (end == std::string::npos) return false;
    size_t start = tail.find_last_of('\n', end);
    start = (start == std::string::npos) ? 0 : start + 1;
    std::string lastLine = tail.substr(start, end - start + 1);

    return lastLine.find(kInterruptedMarker) != std::string::npos;
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

void ClaudeProvider::SyncInstallation(bool enabled) {
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

std::vector<AiSessionStatus> ClaudeProvider::LoadStatuses() {
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
    std::vector<std::wstring> toDelete;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fileName(fd.cFileName);
        std::wstring path = dir + L"\\" + fileName;

        std::string content = ReadFileBytes(path);
        AiSessionStatus status;
        DWORD pid = 0;
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
        if (status.status != L"working" && HasNewerTranscriptActivity(status.cwd, sessionId, fd.ftLastWriteTime)) {
            // "Attention" (or "Waiting"): there's no "permission granted,
            // resuming" event, so once a permission prompt fires, nothing
            // updates the status again until the turn's eventual Stop,
            // even though the user may have long since answered it and
            // Claude gone back to work.
            Log(L"claude_provider: %ls's transcript has newer activity than its last %ls snapshot -- correcting to "
                L"working",
                fileName.c_str(), status.status.c_str());
            status.status = L"working";
        } else if (status.status == L"working" && WasLastTranscriptEntryInterrupted(status.cwd, sessionId)) {
            // "Working": Stop does not fire on a manual interrupt
            // (Escape/Ctrl+C mid-response) -- confirmed against a real
            // session that stayed reported as "working" over 20 minutes
            // after the user interrupted it, with no further hook ever
            // firing. The transcript's own interrupt marker is the only
            // signal that the turn actually ended.
            Log(L"claude_provider: %ls's transcript shows the turn was interrupted by the user -- correcting to "
                L"waiting",
                fileName.c_str());
            status.status = L"waiting";
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
