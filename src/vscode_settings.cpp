#include "vscode_settings.h"
#include "file_util.h"
#include "logger.h"

#include <windows.h>

#include <map>
#include <string>

namespace {

// The exact value this app sets/looks for. If this ever changes, README.md
// and window_title.cpp's ParseVSCodeTitle (which is tuned to this exact
// format) need matching updates.
const char* kDesiredValueJson =
    "\"${activeRepositoryName} - ${activeRepositoryBranchName} - ${folderName}\"";
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

bool WriteFileBytes(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"vscode_settings: failed to open %ls for write, lastError=%lu", path.c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != content.size()) {
        Log(L"vscode_settings: failed to write %ls, lastError=%lu", path.c_str(), GetLastError());
        return false;
    }
    return true;
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

void ApplyToTarget(std::map<std::string, TargetState>& states, const wchar_t* variantDirName,
                    const std::string& section, bool enable) {
    wchar_t appDataBuf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appDataBuf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        Log(L"vscode_settings: %%APPDATA%% not available");
        return;
    }

    std::wstring userDir = std::wstring(appDataBuf) + L"\\" + variantDirName + L"\\User";
    if (!DirectoryExists(userDir)) return; // this VS Code variant isn't installed

    std::wstring settingsPath = userDir + L"\\settings.json";
    std::string content = ReadFileBytes(settingsPath);

    bool hadBom = content.size() >= 3 && (unsigned char)content[0] == 0xEF &&
                  (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF;
    std::string bom = hadBom ? content.substr(0, 3) : "";
    if (hadBom) content.erase(0, 3);

    JsonEntry entry = FindTopLevelKey(content, kWindowTitleKey);
    std::string currentRaw = entry.found ? content.substr(entry.valueStart, entry.valueEnd - entry.valueStart) : "";

    TargetState& st = states[section];
    bool changed = false;

    if (enable) {
        if (entry.found && currentRaw == kDesiredValueJson) {
            return; // already exactly what we want -- ours or coincidentally the user's, either way nothing to do
        }
        st.managed = true;
        st.hadKey = entry.found;
        st.originalValueRaw = currentRaw;
        content = entry.found
            ? content.substr(0, entry.valueStart) + kDesiredValueJson + content.substr(entry.valueEnd)
            : InsertTopLevelKey(content, kWindowTitleKey, kDesiredValueJson);
        changed = true;
        Log(L"vscode_settings: [%hs] enabling label -- set window.title (previously %ls)", section.c_str(),
            entry.found ? L"had a value" : L"was unset");
    } else {
        if (!st.managed) return; // this app never took ownership here -- nothing to restore
        if (!entry.found || currentRaw != kDesiredValueJson) {
            Log(L"vscode_settings: [%hs] window.title changed externally since we set it -- leaving as-is, "
                L"dropping ownership",
                section.c_str());
        } else if (st.hadKey) {
            content = content.substr(0, entry.valueStart) + st.originalValueRaw + content.substr(entry.valueEnd);
            changed = true;
            Log(L"vscode_settings: [%hs] disabling label -- restored original window.title", section.c_str());
        } else {
            content = RemoveTopLevelKey(content, entry);
            changed = true;
            Log(L"vscode_settings: [%hs] disabling label -- removed window.title (didn't exist before)",
                section.c_str());
        }
        st.managed = false;
        st.hadKey = false;
        st.originalValueRaw.clear();
    }

    if (changed) WriteFileBytes(settingsPath, bom + content);
}

} // namespace

void SyncWindowTitleSetting(bool enableLabel) {
    std::map<std::string, TargetState> states = LoadState();
    ApplyToTarget(states, L"Code", "Code", enableLabel);
    ApplyToTarget(states, L"Code - Insiders", "CodeInsiders", enableLabel);
    SaveState(states);
}
