#include "vscode_settings.h"
#include "file_util.h"
#include "logger.h"

#include <windows.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

// The exact value this app sets/looks for, on Windows and in WSL distros
// alike. If this ever changes, README.md and window_title.cpp's
// ParseVSCodeTitle (which is tuned to this exact format) need matching
// updates. The trailing ${appName} is what marks a WSLg window as VS Code
// rather than some other Linux app (see wsl.h; ParseVSCodeTitle strips it)
// -- and it's the same value everywhere because VS Code's Settings Sync
// copies window.title between Windows and WSL VS Code, so any difference
// would just be overwritten by whichever synced last.
const char* kDesiredValueJson =
    "\"${activeRepositoryName} - ${activeRepositoryBranchName} - ${folderName} - ${appName}\"";
// What this app used to set, before ${appName} was added: still treated as
// its own value, so an install that set it keeps its ownership (and the
// original it would restore) and just moves on to kDesiredValueJson.
const char* kLegacyValueJson = "\"${activeRepositoryName} - ${activeRepositoryBranchName} - ${folderName}\"";
const char* kWindowTitleKey = "window.title";

struct TargetState {
    bool managed = false;      // true if this app currently owns window.title
    bool hadKey = false;       // only meaningful when managed: did a value exist before we set ours?
    std::string originalValueRaw; // raw JSON text (incl. quotes if a string) to restore, if hadKey
};

// ---------------------------------------------------------------------------
// Raw file IO
// ---------------------------------------------------------------------------

std::wstring GetStateFilePath() {
    return GetExeDir() + L"\\window_title_state.ini";
}

bool DirectoryExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// ---------------------------------------------------------------------------
// State file (window_title_state.ini) -- remembers, per VS Code variant,
// whether this app owns window.title and what to restore it to.
// original_value is stored with \\, \n, \r escaped so the file stays
// strictly line-based even though it holds a JSON fragment.
// ---------------------------------------------------------------------------

std::string EscapeForStateLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string UnescapeStateLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') { out += '\n'; i++; continue; }
            if (n == 'r') { out += '\r'; i++; continue; }
            if (n == '\\') { out += '\\'; i++; continue; }
        }
        out += s[i];
    }
    return out;
}

std::map<std::string, TargetState> LoadState() {
    std::map<std::string, TargetState> states;
    std::string content = ReadFileBytes(GetStateFilePath());
    std::string currentSection;

    size_t pos = 0;
    while (pos <= content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos ? content.size() : eol) - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (eol == std::string::npos) pos = content.size() + 1;
        else pos = eol + 1;

        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || currentSection.empty()) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        TargetState& st = states[currentSection];
        if (key == "managed") st.managed = (val == "1");
        else if (key == "had_key") st.hadKey = (val == "1");
        else if (key == "original_value") st.originalValueRaw = UnescapeStateLine(val);
    }
    return states;
}

void SaveState(const std::map<std::string, TargetState>& states) {
    std::string out;
    for (const auto& entry : states) {
        out += "[" + entry.first + "]\n";
        out += std::string("managed=") + (entry.second.managed ? "1" : "0") + "\n";
        out += std::string("had_key=") + (entry.second.hadKey ? "1" : "0") + "\n";
        out += "original_value=" + EscapeForStateLine(entry.second.originalValueRaw) + "\n";
    }
    WriteFileBytes(GetStateFilePath(), out);
}

// ---------------------------------------------------------------------------
// Minimal JSONC scanner -- just enough to find/replace/insert/remove one
// top-level key's value in a VS Code settings.json, without disturbing
// anything else (comments, formatting, other keys) in the file.
// ---------------------------------------------------------------------------

struct JsonEntry {
    bool found = false;
    size_t keyStart = 0;   // position of the key's opening quote
    size_t valueStart = 0; // start of the value (whitespace/comments already skipped)
    size_t valueEnd = 0;   // one past the last non-whitespace char of the value
};

size_t SkipWhitespaceAndComments(const std::string& s, size_t i) {
    size_t n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        break;
    }
    return i;
}

// Scans a JSON value starting at `start` (whitespace/comments already
// skipped by the caller). Returns the trimmed exclusive end of the value --
// stops at a comma or closing bracket that belongs to the *containing*
// structure, without consuming it.
size_t FindValueEnd(const std::string& s, size_t start) {
    size_t n = s.size();
    size_t i = start;
    int depth = 0;
    size_t lastNonWs = start;
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (c == '"') {
            i++;
            while (i < n) {
                if (s[i] == '\\') { i += 2; continue; }
                if (s[i] == '"') { i++; break; }
                i++;
            }
            lastNonWs = i;
            continue;
        }
        if (c == '{' || c == '[') { depth++; i++; lastNonWs = i; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0) break;
            depth--; i++; lastNonWs = i; continue;
        }
        if (c == ',' && depth == 0) break;
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) lastNonWs = i + 1;
        i++;
    }
    return lastNonWs;
}

// Finds the first `{` not inside a string/comment -- the root object's open
// brace. Returns std::string::npos if none exists (empty/missing file).
size_t FindRootObjectOpenBrace(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            i += 2;
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (c == '{') return i;
        i++;
    }
    return std::string::npos;
}

// Finds `"key": <value>` directly inside the root object (depth 1) --
// ignores matches inside comments, strings, or nested objects/arrays.
JsonEntry FindTopLevelKey(const std::string& content, const std::string& key) {
    JsonEntry result;
    int depth = 0;
    size_t i = 0, n = content.size();
    std::string quotedKey = "\"" + key + "\"";

    while (i < n) {
        char c = content[i];
        if (c == '/' && i + 1 < n && content[i + 1] == '/') {
            i += 2;
            while (i < n && content[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && content[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(content[i] == '*' && content[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        if (c == '"') {
            size_t stringStart = i;
            i++;
            while (i < n) {
                if (content[i] == '\\') { i += 2; continue; }
                if (content[i] == '"') { i++; break; }
                i++;
            }
            if (depth == 1 && content.compare(stringStart, i - stringStart, quotedKey) == 0) {
                size_t j = SkipWhitespaceAndComments(content, i);
                if (j < n && content[j] == ':') {
                    j = SkipWhitespaceAndComments(content, j + 1);
                    result.found = true;
                    result.keyStart = stringStart;
                    result.valueStart = j;
                    result.valueEnd = FindValueEnd(content, j);
                    return result;
                }
            }
            continue;
        }
        if (c == '{' || c == '[') { depth++; i++; continue; }
        if (c == '}' || c == ']') { depth--; i++; continue; }
        i++;
    }
    return result;
}

// Removes a top-level `"key": <value>` span, plus whichever adjacent comma
// keeps the object valid, plus the indentation/newline the entry occupied
// -- so the result looks like the entry was never there, not like a blank
// line was left behind.
std::string RemoveTopLevelKey(const std::string& content, const JsonEntry& entry) {
    size_t removeStart = entry.keyStart;
    size_t removeEnd = entry.valueEnd;

    size_t j = SkipWhitespaceAndComments(content, removeEnd);
    bool ateTrailingComma = false;
    if (j < content.size() && content[j] == ',') {
        removeEnd = j + 1;
        ateTrailingComma = true;
    }

    while (removeStart > 0 && (content[removeStart - 1] == ' ' || content[removeStart - 1] == '\t')) removeStart--;

    if (ateTrailingComma) {
        // Eat the newline that started this entry's own line too, so the
        // following entry's newline becomes the sole separator instead of
        // leaving a blank line behind.
        if (removeStart > 0 && content[removeStart - 1] == '\n') {
            removeStart--;
            if (removeStart > 0 && content[removeStart - 1] == '\r') removeStart--;
        }
    } else {
        // Last entry in the object -- eat a preceding comma (if any),
        // through the whitespace that separated it from this entry, so the
        // new last entry doesn't end with a dangling comma.
        size_t k = removeStart;
        while (k > 0 && (content[k - 1] == ' ' || content[k - 1] == '\t' ||
                         content[k - 1] == '\r' || content[k - 1] == '\n')) {
            k--;
        }
        if (k > 0 && content[k - 1] == ',') removeStart = k - 1;
    }
    return content.substr(0, removeStart) + content.substr(removeEnd);
}

std::string InsertTopLevelKey(std::string content, const std::string& key, const std::string& valueJson) {
    size_t brace = FindRootObjectOpenBrace(content);
    if (brace == std::string::npos) {
        content = "{\n}\n";
        brace = 0;
    }

    size_t afterBrace = SkipWhitespaceAndComments(content, brace + 1);
    bool emptyObject = afterBrace < content.size() && content[afterBrace] == '}';
    // No trailing newline in the insertion itself -- whatever already
    // follows the opening brace (another entry's own leading newline, or
    // the closing brace) provides it, keeping existing formatting intact.
    std::string insertion = "\n    \"" + key + "\": " + valueJson + (emptyObject ? "" : ",");
    content.insert(brace + 1, insertion);
    return content;
}

// ---------------------------------------------------------------------------
// Per-target apply
// ---------------------------------------------------------------------------

// Applies `enable` to one settings.json's text (`content`, BOM and all) and
// to its entry in `states`. True if `content` changed and needs writing back.
bool ApplyToContent(std::map<std::string, TargetState>& states, const std::string& section, bool enable,
                    std::string& content) {
    bool hadBom = content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
                  (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF;
    std::string bom = hadBom ? content.substr(0, 3) : "";
    std::string body = hadBom ? content.substr(3) : content;

    JsonEntry entry = FindTopLevelKey(body, kWindowTitleKey);
    std::string currentRaw = entry.found ? body.substr(entry.valueStart, entry.valueEnd - entry.valueStart) : "";

    TargetState& st = states[section];
    bool changed = false;

    if (enable) {
        if (entry.found && currentRaw == kDesiredValueJson) {
            return false; // already exactly what we want -- ours or coincidentally the user's, either way nothing to do
        }
        if (st.managed && entry.found && currentRaw == kLegacyValueJson) {
            // Ours from before -- keep the original it would restore.
            Log(L"vscode_settings: [%hs] updating window.title from this app's older value", section.c_str());
        } else {
            st.managed = true;
            st.hadKey = entry.found;
            st.originalValueRaw = currentRaw;
            Log(L"vscode_settings: [%hs] enabling label -- set window.title (previously %ls)", section.c_str(),
                entry.found ? L"had a value" : L"was unset");
        }
        body = entry.found
            ? body.substr(0, entry.valueStart) + kDesiredValueJson + body.substr(entry.valueEnd)
            : InsertTopLevelKey(body, kWindowTitleKey, kDesiredValueJson);
        changed = true;
    } else {
        if (!st.managed) return false; // this app never took ownership here -- nothing to restore
        if (!entry.found || (currentRaw != kDesiredValueJson && currentRaw != kLegacyValueJson)) {
            Log(L"vscode_settings: [%hs] window.title changed externally since we set it -- leaving as-is, "
                L"dropping ownership",
                section.c_str());
        } else if (st.hadKey) {
            body = body.substr(0, entry.valueStart) + st.originalValueRaw + body.substr(entry.valueEnd);
            changed = true;
            Log(L"vscode_settings: [%hs] disabling label -- restored original window.title", section.c_str());
        } else {
            body = RemoveTopLevelKey(body, entry);
            changed = true;
            Log(L"vscode_settings: [%hs] disabling label -- removed window.title (didn't exist before)",
                section.c_str());
        }
        st.managed = false;
        st.hadKey = false;
        st.originalValueRaw.clear();
    }

    if (changed) content = bom + body;
    return changed;
}

// `userDir` is a VS Code "User" settings folder on Windows; does nothing if
// it doesn't exist (that VS Code variant isn't installed).
void ApplyToWindowsDir(std::map<std::string, TargetState>& states, const std::wstring& userDir,
                       const std::string& section, bool enable) {
    if (!DirectoryExists(userDir)) return;
    std::wstring settingsPath = userDir + L"\\settings.json";
    std::string content = ReadFileBytes(settingsPath);
    if (ApplyToContent(states, section, enable, content)) WriteFileBytes(settingsPath, content);
}

std::string NarrowAscii(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out.push_back(c > 0 && c <= 0x7F ? (char)c : '_');
    return out;
}

// One argument quoted for a Windows command line (CommandLineToArgvW rules).
std::wstring QuoteArg(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            backslashes++;
            continue;
        }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

// Runs wsl.exe with `args` (already a command-line tail), feeding it
// `input` and collecting its stdout+stderr. True if it ran and exited 0
// within the timeout.
bool RunWslExe(const std::wstring& args, const std::string& input, std::string* output) {
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring cmd = QuoteArg(std::wstring(sysDir) + L"\\wsl.exe") + L" " + args;

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return false;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return false;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi = {};
    bool started = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                  &pi) != 0;
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!started) {
        Log(L"vscode_settings: couldn't run wsl.exe, lastError=%lu", GetLastError());
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return false;
    }

    DWORD written = 0;
    if (!input.empty()) WriteFile(inWrite, input.data(), (DWORD)input.size(), &written, nullptr);
    CloseHandle(inWrite);
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(outRead, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(outRead);

    DWORD exitCode = 1;
    bool finished = WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0;
    if (!finished) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (output) *output = out;
    return finished && exitCode == 0;
}

// Runs `argv` inside the distro as its default user (wsl.exe -d <distro> -e
// ...). Used instead of the \\wsl.localhost share, which a distro can have
// switched off (e.g. with interop disabled), while wsl.exe from Windows
// always works. The distro name goes in unquoted: wsl.exe takes -d's value
// verbatim, quotes and all (names can't contain spaces anyway).
bool RunInWsl(const std::wstring& distro, const std::vector<std::wstring>& argv, const std::string& input,
              std::string* output) {
    std::wstring args = L"-d " + distro + L" -e";
    for (const std::wstring& a : argv) args += L" " + QuoteArg(a);
    return RunWslExe(args, input, output);
}

// Distros running right now. wsl.exe's own output is UTF-16, one name per
// line. Doesn't start anything.
std::set<std::wstring> RunningWslDistros() {
    std::set<std::wstring> running;
    std::string raw;
    if (!RunWslExe(L"--list --running --quiet", "", &raw)) return running;
    std::wstring text((const wchar_t*)raw.data(), raw.size() / sizeof(wchar_t));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        std::wstring name = text.substr(pos, (eol == std::wstring::npos ? text.size() : eol) - pos);
        pos = eol == std::wstring::npos ? text.size() : eol + 1;
        while (!name.empty() && (name.back() == L'\r' || name.back() == L' ' || name.back() == 0)) name.pop_back();
        if (!name.empty()) running.insert(name);
    }
    return running;
}

void ApplyToWslDistro(std::map<std::string, TargetState>& states, const std::wstring& distro, bool enable) {
    // Whichever of VS Code's two variants has a settings folder in the
    // default user's home -- one path per line.
    std::string listing;
    if (!RunInWsl(distro,
                  {L"sh", L"-c",
                   L"for d in \"$HOME/.config/Code/User\" \"$HOME/.config/Code - Insiders/User\"; do "
                   L"[ -d \"$d\" ] && echo \"$d\"; done; true"},
                  "", &listing)) {
        Log(L"vscode_settings: couldn't look for VS Code settings in WSL distro [%ls] -- window.title left as-is "
            L"(wsl.exe said: %hs)",
            distro.c_str(), listing.c_str());
        return;
    }
    size_t pos = 0;
    while (pos < listing.size()) {
        size_t eol = listing.find('\n', pos);
        std::string dir = listing.substr(pos, (eol == std::string::npos ? listing.size() : eol) - pos);
        pos = eol == std::string::npos ? listing.size() : eol + 1;
        if (dir.empty()) continue;
        bool insiders = dir.find("Code - Insiders") != std::string::npos;
        std::string section = "WSL:" + NarrowAscii(distro) + ":" + (insiders ? "CodeInsiders" : "Code");
        std::wstring path = std::wstring(dir.begin(), dir.end()) + L"/settings.json"; // $HOME paths are ASCII in practice

        std::string content;
        if (!RunInWsl(distro, {L"sh", L"-c", L"cat -- \"$1\" 2>/dev/null; true", L"sh", path}, "", &content)) {
            Log(L"vscode_settings: [%hs] couldn't read settings.json", section.c_str());
            continue;
        }
        if (!ApplyToContent(states, section, enable, content)) continue;
        if (!RunInWsl(distro, {L"sh", L"-c", L"cat > \"$1\"", L"sh", path}, content, nullptr)) {
            Log(L"vscode_settings: [%hs] couldn't write settings.json", section.c_str());
        }
    }
}

// Distros a Linux VS Code window has been seen in this session (see
// NoteWslVSCodeDistro) -- the only ones SyncWindowTitleSetting touches, so
// it never wakes a stopped distro just by reading its files.
std::set<std::wstring> g_seenWslDistros;

} // namespace

void SyncWindowTitleSetting(bool enableLabel) {
    wchar_t appDataBuf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appDataBuf, MAX_PATH);
    std::map<std::string, TargetState> states = LoadState();
    if (n == 0 || n >= MAX_PATH) {
        Log(L"vscode_settings: %%APPDATA%% not available");
    } else {
        std::wstring appData(appDataBuf);
        ApplyToWindowsDir(states, appData + L"\\Code\\User", "Code", enableLabel);
        ApplyToWindowsDir(states, appData + L"\\Code - Insiders\\User", "CodeInsiders", enableLabel);
    }
    // Also distros this app manages from an earlier session that are running
    // right now -- e.g. with a VS Code window open whose title has lost the
    // "Visual Studio Code" marker (see kDesiredValueJson), so it can't be
    // recognised, and so seen, until this puts it back.
    bool anyWslManaged = false;
    for (const auto& kv : states) anyWslManaged |= kv.first.compare(0, 4, "WSL:") == 0 && kv.second.managed;
    if (anyWslManaged) {
        std::set<std::wstring> running = RunningWslDistros();
        for (const auto& kv : states) {
            if (kv.first.compare(0, 4, "WSL:") != 0 || !kv.second.managed) continue;
            size_t end = kv.first.find(':', 4);
            std::wstring distro(kv.first.begin() + 4, end == std::string::npos ? kv.first.end() : kv.first.begin() + end);
            if (running.count(distro)) g_seenWslDistros.insert(distro);
        }
    }
    for (const std::wstring& distro : g_seenWslDistros) ApplyToWslDistro(states, distro, enableLabel);
    SaveState(states);
}

void NoteWslVSCodeDistro(const std::wstring& distro, bool enableLabel) {
    if (distro.empty() || !g_seenWslDistros.insert(distro).second) return;
    std::map<std::string, TargetState> states = LoadState();
    ApplyToWslDistro(states, distro, enableLabel);
    SaveState(states);
}
