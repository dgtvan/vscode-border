#include "claude_profile.h"

#include "config.h"
#include "file_util.h"
#include "json_scan.h"
#include "layered_rendering.h"
#include "logger.h"
#include "text_util.h"

#include <gdiplus.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <vector>

namespace {

std::wstring TrimTrailingSeparators(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool SamePathText(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? buf : L"";
}

std::wstring LeafName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

ULONGLONG FileTimeValue(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// A JSON string token (quotes included) as text: undoes \", \\, \/, \n,
// \t, \r, \b, \f and \uXXXX (surrogate pairs too).
std::wstring DecodeJsonString(const std::string& raw) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return L"";
    std::wstring out;
    std::string run;
    auto flush = [&]() {
        out += Utf8ToWide(run);
        run.clear();
    };
    for (size_t i = 1; i + 1 < raw.size(); i++) {
        if (raw[i] != '\\' || i + 2 >= raw.size()) {
            run += raw[i];
            continue;
        }
        char c = raw[++i];
        if (c == 'u' && i + 4 < raw.size()) {
            flush();
            out += (wchar_t)strtoul(raw.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            continue;
        }
        run += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
    }
    flush();
    return out;
}

// The default profile: slot 1, which LoadConfig always fills in.
const ClaudeProfile* DefaultProfile() {
    for (const ClaudeProfile& p : g_config.claudeProfiles) {
        if (p.isDefault) return &p;
    }
    return nullptr;
}

// ---- Who's signed in ----

// Where Claude Code keeps profile `id`'s global state (.claude.json, with
// the signed-in account) when CLAUDE_CONFIG_DIR names its folder -- as it
// always does for a window this app opens: inside that folder.
std::wstring GlobalStatePath(const std::wstring& id) {
    return ClaudeProfileConfigDir(id) + L"\\.claude.json";
}

// ---- Icons ----

// A config.ini path, made absolute (relative to config.ini's folder).
std::wstring ConfigRelativePath(const std::wstring& p) {
    if (p.empty()) return p;
    bool absolute = p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
    return absolute ? p : GetExeDir() + L"\\" + p;
}

// Whether an icon value is the image itself rather than a path to one: a
// data URI ("data:image/png;base64,..."), or bare base64 -- which never has
// a '.', '\\' or ':', while a path to an image file always has one of them.
bool IsBase64Icon(const std::wstring& icon) {
    if (_wcsnicmp(icon.c_str(), L"data:", 5) == 0) return true;
    return icon.find_first_of(L".\\:") == std::wstring::npos;
}

// Profile `id`'s _icon as config.ini has it: an absolute image path, the
// base64 image as is, or empty.
std::wstring ConfiguredIconSource(const std::wstring& id) {
    const ClaudeProfile* p = FindClaudeProfile(id);
    if (!p || p->icon.empty()) return L"";
    return IsBase64Icon(p->icon) ? p->icon : ConfigRelativePath(p->icon);
}

// For log lines: a base64 icon by its size, not its contents.
std::wstring DescribeIconSource(const std::wstring& source) {
    if (source.empty()) return L"the letter badge";
    if (IsBase64Icon(source)) return L"a base64 image (" + std::to_wstring(source.size()) + L" chars)";
    return source;
}

// Decodes base64 (a data URI's payload, or bare), skipping whitespace; false
// on anything else outside the base64 alphabet.
bool DecodeBase64(const std::wstring& text, std::vector<BYTE>& out) {
    size_t start = 0;
    if (_wcsnicmp(text.c_str(), L"data:", 5) == 0) {
        start = text.find(L',');
        if (start == std::wstring::npos) return false;
        start++;
    }
    out.clear();
    UINT32 acc = 0;
    int bits = 0;
    for (size_t i = start; i < text.size(); i++) {
        wchar_t c = text[i];
        int v;
        if (c >= L'A' && c <= L'Z') v = c - L'A';
        else if (c >= L'a' && c <= L'z') v = c - L'a' + 26;
        else if (c >= L'0' && c <= L'9') v = c - L'0' + 52;
        else if (c == L'+' || c == L'-') v = 62;
        else if (c == L'/' || c == L'_') v = 63;
        else if (c == L'=') break;
        else if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        else return false;
        acc = (acc << 6) | (UINT32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((BYTE)(acc >> bits));
        }
    }
    return !out.empty();
}

// Loads an icon source (see ConfiguredIconSource) as a GDI+ bitmap, or null.
Gdiplus::Bitmap* LoadIconImage(const std::wstring& source) {
    if (!IsBase64Icon(source)) return new Gdiplus::Bitmap(source.c_str());
    std::vector<BYTE> bytes;
    if (!DecodeBase64(source, bytes)) return nullptr;
    IStream* stream = SHCreateMemStream(bytes.data(), (UINT)bytes.size());
    if (!stream) return nullptr;
    // The bitmap reads from the stream for as long as it lives, and holds
    // its own reference to it.
    Gdiplus::Bitmap* image = new Gdiplus::Bitmap(stream);
    stream->Release();
    return image;
}

// A profile's icon rendered to premultiplied ARGB (S^2 pixels) from
// `iconSource` (see ConfiguredIconSource), or the badge with `name`'s first
// letter if that's empty or can't be read.
std::vector<UINT32> RenderProfileIcon(const std::wstring& name, const std::wstring& iconSource, int S) {
    std::vector<UINT32> out;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
        LogWarn(L"claude_profile: GDI+ didn't start -- no profile icon");
        return out;
    }
    {
        Gdiplus::Bitmap target(S, S, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(&target);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        bool drawn = false;
        if (!iconSource.empty()) {
            Gdiplus::Bitmap* image = LoadIconImage(iconSource);
            if (image && image->GetLastStatus() == Gdiplus::Ok && image->GetWidth() > 0 && image->GetHeight() > 0) {
                // Without tile-flip wrapping, bicubic downscaling blends the
                // image's edge with transparent black, leaving a dark rim.
                Gdiplus::ImageAttributes attrs;
                attrs.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
                g.DrawImage(image, Gdiplus::Rect(0, 0, S, S), 0, 0, (INT)image->GetWidth(), (INT)image->GetHeight(),
                            Gdiplus::UnitPixel, &attrs);
                drawn = true;
            } else {
                LogWarn(L"claude_profile: can't read the icon for [%ls] from %ls -- using the letter badge",
                        name.c_str(), DescribeIconSource(iconSource).c_str());
            }
            delete image;
        }
        if (!drawn) {
            Gdiplus::SolidBrush circle(Gdiplus::Color(255, 0x3B, 0x42, 0x52));
            g.FillEllipse(&circle, 0.0f, 0.0f, (Gdiplus::REAL)S, (Gdiplus::REAL)S);
            // A folder name like ".claude-work" starts with its first letter.
            size_t first = name.find_first_not_of(L".");
            wchar_t letter[2] = {first == std::wstring::npos ? L'?' : (wchar_t)towupper(name[first]), 0};
            Gdiplus::Font font(L"Segoe UI", S * 0.62f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat fmt;
            fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
            g.DrawString(letter, 1, &font, Gdiplus::RectF(0, 0.5f, (Gdiplus::REAL)S, (Gdiplus::REAL)S), &fmt, &white);
        }

        Gdiplus::Rect rect(0, 0, S, S);
        Gdiplus::BitmapData data;
        if (target.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) == Gdiplus::Ok) {
            out.resize(S * S);
            for (int y = 0; y < S; y++) {
                const UINT32* row = (const UINT32*)((const BYTE*)data.Scan0 + y * data.Stride);
                std::copy(row, row + S, out.begin() + y * S);
            }
            target.UnlockBits(&data);
        }
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return out;
}

// ---- Which profile a window runs ----

// One ide\*.lock file Claude Code's VS Code extension writes when it starts
// in a window: {"pid":<VS Code's main process>,"workspaceFolders":[...],...}.
// Not always removed when the window closes, and every window of a VS Code
// shares the pid, so a lock can be left from a window closed since.
struct IdeLock {
    ULONGLONG written = 0; // the file's last write time, as parsed
    DWORD pid = 0;
    std::vector<std::wstring> folders;
};

// Every lock file seen, by full path, re-parsed only when rewritten.
std::map<std::wstring, IdeLock> g_lockFiles;

// A live window's lock: its profile, and its folders.
struct LiveLock {
    std::wstring profile;
    ULONGLONG written = 0;
    std::vector<std::wstring> folders;
};
std::vector<LiveLock> g_liveLocks;
ULONGLONG g_lockScanTick = 0;
std::wstring g_lockScanDirs; // the config folders the last scan covered, to rescan right after a reload

// Folders this app opened, lowercased without a trailing separator -> the
// profile it opened them as, and when.
struct Launch {
    std::wstring profile;
    ULONGLONG at = 0; // a FILETIME, comparable with a lock's write time
};
std::map<std::wstring, Launch> g_launches;

const ULONGLONG kLockRescanMs = 2000;

IdeLock ParseLockFile(const std::wstring& path, ULONGLONG written) {
    IdeLock lock;
    lock.written = written;
    std::string content = ReadFileBytes(path);
    if (JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) return lock;
    JsonScan::Span root = JsonScan::EnsureRootObject(content);
    JsonScan::KeyEntry pid = JsonScan::FindKeyInObject(content, root.start, root.end, "pid");
    if (pid.found) lock.pid = (DWORD)strtoul(content.c_str() + pid.valueStart, nullptr, 10);
    JsonScan::KeyEntry folders = JsonScan::FindKeyInObject(content, root.start, root.end, "workspaceFolders");
    if (folders.found && folders.valueStart < content.size() && content[folders.valueStart] == '[') {
        for (const JsonScan::Span& e : JsonScan::FindArrayElements(content, folders.valueStart + 1, folders.valueEnd - 1)) {
            std::wstring folder = DecodeJsonString(content.substr(e.start, e.end - e.start));
            if (!folder.empty()) lock.folders.push_back(TrimTrailingSeparators(folder));
        }
    }
    return lock;
}

// Whether `pid` is still the VS Code that a lock written at `written`
// names: a running Code.exe started before then -- not one that has reused
// the pid of a VS Code that's gone.
bool IsLockOwnerAlive(DWORD pid, ULONGLONG written) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    FILETIME created, exited, kernel, user;
    DWORD exitCode = 0;
    bool alive = QueryFullProcessImageNameW(process, 0, path, &size) &&
                 GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                 GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE && FileTimeValue(created) <= written;
    CloseHandle(process);
    std::wstring name = LeafName(path);
    return alive && (_wcsicmp(name.c_str(), L"Code.exe") == 0 || _wcsicmp(name.c_str(), L"Code - Insiders.exe") == 0);
}

void RescanLocksIfDue() {
    std::wstring dirs;
    for (const ClaudeProfile& p : g_config.claudeProfiles) dirs += p.configDir + L"|";
    ULONGLONG now = GetTickCount64();
    if (dirs == g_lockScanDirs && now - g_lockScanTick < kLockRescanMs) return;
    g_lockScanDirs = dirs;
    g_lockScanTick = now;

    g_liveLocks.clear();
    std::map<std::wstring, IdeLock> seen;
    for (const ClaudeProfile& p : g_config.claudeProfiles) {
        std::wstring ideDir = p.configDir + L"\\ide\\";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((ideDir + L"*.lock").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring path = ideDir + fd.cFileName;
            ULONGLONG written = FileTimeValue(fd.ftLastWriteTime);
            auto it = g_lockFiles.find(path);
            IdeLock lock = it != g_lockFiles.end() && it->second.written == written ? it->second
                                                                                    : ParseLockFile(path, written);
            seen[path] = lock;
            if (lock.pid == 0 || !IsLockOwnerAlive(lock.pid, written)) continue;
            g_liveLocks.push_back({p.isDefault ? L"" : p.configDir, written, lock.folders});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_lockFiles.swap(seen);
}

} // namespace

std::vector<std::wstring> OtherClaudeProfiles() {
    std::vector<std::wstring> out;
    for (const ClaudeProfile& p : g_config.claudeProfiles) {
        if (!p.isDefault) out.push_back(p.configDir);
    }
    return out;
}

const ClaudeProfile* FindClaudeProfile(const std::wstring& id) {
    if (id.empty()) return DefaultProfile();
    for (const ClaudeProfile& p : g_config.claudeProfiles) {
        if (!p.isDefault && SamePathText(p.configDir, id)) return &p;
    }
    return nullptr;
}

bool SameClaudeProfile(const std::wstring& a, const std::wstring& b) {
    return SamePathText(a, b);
}

std::wstring ClaudeProfileConfigDir(const std::wstring& id) {
    const ClaudeProfile* p = FindClaudeProfile(id);
    if (p) return p->configDir;
    return id.empty() ? EnvVar(L"USERPROFILE") + L"\\.claude" : id;
}

std::wstring ClaudeProfileEmail(const std::wstring& id) {
    // Per file, with the write time it was read at: .claude.json is large
    // and rewritten often, but its email only changes on a new sign-in.
    struct Cached {
        ULONGLONG written = 0;
        std::wstring email;
    };
    static std::map<std::wstring, Cached> cache;
    std::wstring path = GlobalStatePath(id);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return L"";
    ULONGLONG written = FileTimeValue(data.ftLastWriteTime);
    Cached& c = cache[path];
    if (c.written == written) return c.email;
    c.written = written;
    c.email.clear();
    std::string content = ReadFileBytes(path);
    if (JsonScan::FindRootObjectOpenBrace(content) == std::string::npos) return L"";
    JsonScan::Span root = JsonScan::EnsureRootObject(content);
    JsonScan::KeyEntry account = JsonScan::FindKeyInObject(content, root.start, root.end, "oauthAccount");
    if (!account.found || account.valueStart >= content.size() || content[account.valueStart] != '{') return L"";
    JsonScan::KeyEntry email =
        JsonScan::FindKeyInObject(content, account.valueStart + 1, account.valueEnd - 1, "emailAddress");
    if (email.found) c.email = DecodeJsonString(content.substr(email.valueStart, email.valueEnd - email.valueStart));
    return c.email;
}

std::wstring ClaudeProfileName(const std::wstring& id) {
    const ClaudeProfile* p = FindClaudeProfile(id);
    if (!p) return id.empty() ? L"default" : id;
    if (!p->name.empty()) return p->name;
    std::wstring email = ClaudeProfileEmail(id);
    if (!email.empty()) return email;
    return LeafName(p->configDir);
}

std::wstring ClaudeProfileMenuText(const std::wstring& id) {
    std::wstring out;
    for (wchar_t c : ClaudeProfileName(id)) {
        if (c == L'&') out += L'&';
        out += c;
    }
    return out;
}

bool ShowsClaudeProfileIcon(const std::wstring& id) {
    const ClaudeProfile* p = FindClaudeProfile(id);
    return !p || !p->isDefault || !p->icon.empty();
}

void BlendClaudeProfileIcon(const std::wstring& id, UINT32* pixels, int surfaceWidth, int surfaceHeight, int left,
                            int top, const RECT& clip) {
    // Per profile, with the icon source and name it was rendered from, so a
    // changed _icon or _name takes effect on Reload Config.
    struct CachedIcon {
        std::wstring source, name;
        std::vector<UINT32> pixels;
    };
    static std::map<std::wstring, CachedIcon> cache;
    std::wstring source = ConfiguredIconSource(id), name = ClaudeProfileName(id);
    auto it = cache.find(id);
    if (it == cache.end() || it->second.source != source || it->second.name != name) {
        Log(L"claude_profile: icon for [%ls] from %ls", name.c_str(), DescribeIconSource(source).c_str());
        it = cache.insert_or_assign(id, CachedIcon{source, name, RenderProfileIcon(name, source, kProfileIconSize)})
                 .first;
    }
    if (it->second.pixels.empty()) return;
    BlendPremultipliedIcon(it->second.pixels.data(), kProfileIconSize, pixels, surfaceWidth, surfaceHeight, left, top,
                           clip);
}

HBITMAP GetClaudeProfileMenuBitmap(const std::wstring& id) {
    if (!ShowsClaudeProfileIcon(id)) return nullptr;
    // Keyed like BlendClaudeProfileIcon's cache; a bitmap replaced by a
    // changed icon is kept rather than freed, since a menu may still be
    // showing it.
    static std::map<std::wstring, HBITMAP> cache;
    std::wstring source = ConfiguredIconSource(id), name = ClaudeProfileName(id);
    std::wstring key = id + L"|" + name + L"|" + source;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    int size = GetSystemMetrics(SM_CXSMICON);
    std::vector<UINT32> icon = RenderProfileIcon(name, source, size);
    HBITMAP bmp = nullptr;
    if (!icon.empty()) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = size;
        bmi.bmiHeader.biHeight = -size; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) memcpy(bits, icon.data(), icon.size() * sizeof(UINT32));
    }
    cache[key] = bmp;
    return bmp;
}

bool ClaudeProfileOfWindow(const std::wstring& path, const std::wstring& folderName, std::wstring& profile) {
    RescanLocksIfDue();
    profile.clear();
    std::wstring folder = TrimTrailingSeparators(path);
    // By path; without one, by folder name. Either way the newest lock wins.
    const LiveLock* best = nullptr;
    for (const LiveLock& lock : g_liveLocks) {
        for (const std::wstring& f : lock.folders) {
            bool match = !folder.empty() ? SamePathText(f, folder)
                                         : !folderName.empty() && _wcsicmp(LeafName(f).c_str(), folderName.c_str()) == 0;
            if (match && (!best || lock.written > best->written)) best = &lock;
        }
    }
    // The app's own launch of the folder, when newer than every lock for it:
    // the new window's lock isn't written until Claude Code's extension has
    // started, and until then an older lock may be one left by a window of
    // that folder closed since.
    std::wstring key = folder;
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    auto launch = folder.empty() ? g_launches.end() : g_launches.find(key);
    if (launch != g_launches.end() && (!best || launch->second.at > best->written)) {
        profile = launch->second.profile;
        return true;
    }
    if (best) {
        profile = best->profile;
        return true;
    }
    if (folder.empty() && folderName.empty()) {
        // A window with no folder open: its lock lists none, so nothing
        // says which of them it is -- unless every such window runs the
        // same profile.
        const std::wstring* bare = nullptr;
        for (const LiveLock& lock : g_liveLocks) {
            if (!lock.folders.empty()) continue;
            if (bare && !SamePathText(*bare, lock.profile)) return false;
            bare = &lock.profile;
        }
        if (bare) profile = *bare;
        return bare != nullptr;
    }
    return false;
}

void NoteClaudeProfileLaunch(const std::wstring& folder, const std::wstring& id) {
    std::wstring key = TrimTrailingSeparators(folder);
    if (key.empty()) return;
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    g_launches[key] = {id, FileTimeValue(now)};
}

std::wstring ClaudeProfileEnvironment(const std::wstring& id) {
    std::vector<std::wstring> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"CLAUDE_CONFIG_DIR=", 18) != 0) vars.push_back(p);
        }
        FreeEnvironmentStringsW(block);
    }
    // Set even for the default profile: VS Code lays a window's CLI
    // environment over the one VS Code itself was started with, so leaving
    // the variable out would let the window inherit another profile's.
    vars.push_back(L"CLAUDE_CONFIG_DIR=" + ClaudeProfileConfigDir(id));
    // Windows wants the block sorted by name, case-insensitively.
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        std::wstring na = a.substr(0, a.find(L'=', 1)), nb = b.substr(0, b.find(L'=', 1));
        return CompareStringOrdinal(na.c_str(), -1, nb.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const std::wstring& v : vars) out += v + L'\0';
    out += L'\0';
    return out;
}

void CheckClaudeProfileFolders() {
    const ClaudeProfile* p = DefaultProfile();
    std::wstring home = EnvVar(L"USERPROFILE");
    if (!p || !SamePathText(p->configDir, home + L"\\.claude")) return;
    std::wstring inside = p->configDir + L"\\.claude.json", outside = home + L"\\.claude.json";
    if (GetFileAttributesW(inside.c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(outside.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    LogWarn(L"claude_profile: the default Claude profile's state is in %ls, but windows are opened with "
            L"CLAUDE_CONFIG_DIR=%ls, where Claude Code looks for %ls -- copy the file there (and set "
            L"CLAUDE_CONFIG_DIR for your user account too, so Claude Code run from a terminal agrees), or Claude "
            L"Code in those windows starts the profile over",
            outside.c_str(), p->configDir.c_str(), inside.c_str());
}
