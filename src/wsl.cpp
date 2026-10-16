#include "wsl.h"
#include "logger.h"
#include "resource.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <utility>
#include <vector>

const wchar_t* const kWslgWindowClass = L"RAIL_WINDOW";

bool IsWslgHostExe(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return lower == L"msrdc.exe";
}

static bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    size_t len = wcslen(suffix);
    return s.size() >= len && s.compare(s.size() - len, len, suffix) == 0;
}

bool SplitWslgVSCodeTitle(const std::wstring& title, std::wstring* vscodeTitle, std::wstring* distro) {
    if (title.empty() || title.back() != L')') return false;
    size_t open = title.rfind(L" (");
    if (open == std::wstring::npos || open + 3 > title.size()) return false;
    std::wstring name = title.substr(open + 2, title.size() - open - 3);
    if (name.empty()) return false;
    std::wstring inner = title.substr(0, open);
    // "Visual Studio Code" alone is a window with nothing open yet.
    if (!EndsWith(inner, L"Visual Studio Code") && !EndsWith(inner, L"Visual Studio Code - Insiders")) return false;
    if (vscodeTitle) *vscodeTitle = inner;
    if (distro) *distro = name;
    return true;
}

bool MeasureWslgShadowInsets(HWND hwnd, RECT& insets) {
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return false;
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return false;
    // PrintWindow is a synchronous round-trip into msrdc -- don't let a hung
    // one stall this app's whole UI thread with it.
    if (!SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, nullptr)) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    const UINT kPwRenderFullContent = 2; // PW_RENDERFULLCONTENT
    bool printed = PrintWindow(hwnd, dc, kPwRenderFullContent) != 0;
    GdiFlush();
    const UINT32* px = (const UINT32*)bits;
    auto lit = [&](int x, int y) { return (px[y * w + x] & 0x00FFFFFFu) != 0; };

    // Several sample lines per side, taking the smallest inset any of them
    // finds, so one line that happens to cross dark content doesn't
    // overshoot. Capped well above the real margin: no hit within the cap
    // means this isn't a margin (or nothing's painted yet).
    const int kMaxInset = 160;
    const double kSamples[] = {0.25, 0.5, 0.75};
    auto scan = [&](bool horizontal, bool fromStart) {
        int best = -1;
        for (double f : kSamples) {
            int span = horizontal ? w : h;
            int fixed = (int)((horizontal ? h : w) * f);
            for (int i = 0; i < std::min(kMaxInset, span); i++) {
                int pos = fromStart ? i : span - 1 - i;
                if (horizontal ? lit(pos, fixed) : lit(fixed, pos)) {
                    if (best < 0 || i < best) best = i;
                    break;
                }
            }
        }
        return best;
    };
    int left = -1, top = -1, right = -1, bottom = -1;
    if (printed) {
        left = scan(true, true);
        right = scan(true, false);
        top = scan(false, true);
        bottom = scan(false, false);
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);

    if (left < 0 || top < 0 || right < 0 || bottom < 0 || left + right >= w || top + bottom >= h) return false;
    insets = {left, top, right, bottom};
    return true;
}

bool FixWslgClampedMaximize(HWND hwnd, int knownMargin) {
    RECT wr;
    MONITORINFO mi = {sizeof(mi)};
    if (IsIconic(hwnd) || !GetWindowRect(hwnd, &wr) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        return false;
    }
    // Per window: its last real restored rect, and the grown maximized rect
    // last applied. A window regrown here and then restored can come back
    // still at the grown rect, just no longer maximized (Windows took it as
    // the window's restored position) -- put it back where it was instead.
    struct State {
        RECT restored = {};
        RECT grown = {};
    };
    static std::map<HWND, State> states;
    State& st = states[hwnd];
    if (!IsZoomed(hwnd)) {
        if (!IsRectEmpty(&st.grown) && EqualRect(&wr, &st.grown) && !IsRectEmpty(&st.restored)) {
            RECT to = st.restored;
            st.grown = {};
            SetWindowPos(hwnd, nullptr, to.left, to.top, to.right - to.left, to.bottom - to.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Log(L"wsl: hwnd=%p restored but still at its regrown maximized rect -- moved back to (%ld,%ld)-(%ld,%ld)",
                hwnd, to.left, to.top, to.right, to.bottom);
            return true;
        }
        if (!EqualRect(&wr, &st.grown)) st.restored = wr;
        return false;
    }
    // Already hanging off the work area's top-left: either fixed, or a
    // window with no margin to hang (e.g. native title bar).
    if (wr.left < mi.rcWork.left || wr.top < mi.rcWork.top) return false;
    // Don't keep fighting (or re-measuring, a whole-window render each time)
    // if a fix somehow doesn't stick.
    static std::map<HWND, ULONGLONG> lastAttempt;
    ULONGLONG now = GetTickCount64();
    auto it = lastAttempt.find(hwnd);
    if (it != lastAttempt.end() && now - it->second < 2000) return false;

    int m = knownMargin;
    if (m <= 0) {
        // Clamped, the content is offset by exactly the margin at the
        // top-left (the right/bottom margin is what fell off-screen). No
        // offset: nothing to fix.
        RECT insets;
        if (!MeasureWslgShadowInsets(hwnd, insets) || insets.left <= 0 || insets.left != insets.top) return false;
        m = insets.left;
    }
    lastAttempt[hwnd] = now;
    RECT target = mi.rcWork;
    InflateRect(&target, m, m);
    // SWP_NOSENDCHANGING: msrdc caps a window's size at the screen's, which
    // the grown rect exceeds.
    SetWindowPos(hwnd, nullptr, target.left, target.top, target.right - target.left, target.bottom - target.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    st.grown = target;
    Log(L"wsl: hwnd=%p maximized but clamped to (%ld,%ld)-(%ld,%ld) -- regrown by its %dpx shadow margin", hwnd,
        wr.left, wr.top, wr.right, wr.bottom, m);
    return true;
}

// The WSL icon rendered to premultiplied ARGB (kWslIconSize^2 pixels),
// once; empty if the resource can't be loaded.
static const std::vector<UINT32>& WslIconPixels() {
    static std::vector<UINT32> out;
    static bool loaded = false;
    if (loaded) return out;
    loaded = true;
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_WSL), IMAGE_ICON, kWslIconSize,
                                   kWslIconSize, 0);
    if (!icon) {
        LogWarn(L"wsl: couldn't load the WSL icon resource, lastError=%lu", GetLastError());
        return out;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kWslIconSize;
    bmi.bmiHeader.biHeight = -kWslIconSize; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0, kWslIconSize * kWslIconSize * 4);
        DrawIconEx(dc, 0, 0, icon, kWslIconSize, kWslIconSize, 0, nullptr, DI_NORMAL);
        GdiFlush();
        const UINT32* src = (const UINT32*)bits;
        out.assign(src, src + kWslIconSize * kWslIconSize);
        SelectObject(dc, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    DestroyIcon(icon);
    return out;
}

HBITMAP GetWslIconMenuBitmap() {
    static HBITMAP bmp = nullptr;
    static bool tried = false;
    if (tried) return bmp;
    tried = true;
    int size = GetSystemMetrics(SM_CXSMICON);
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_WSL), IMAGE_ICON, size, size, 0);
    if (!icon) return nullptr;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0, size * size * 4);
        DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL); // keeps the icon's own alpha
        GdiFlush();
        SelectObject(dc, old);
    }
    DeleteDC(dc);
    DestroyIcon(icon);
    return bmp;
}

void BlendWslIcon(UINT32* pixels, int surfaceWidth, int surfaceHeight, int left, int top, const RECT& clip) {
    const std::vector<UINT32>& icon = WslIconPixels();
    if (icon.empty()) return;
    for (int y = 0; y < kWslIconSize; y++) {
        int row = top + y;
        if (row < 0 || row >= surfaceHeight || row < clip.top || row >= clip.bottom) continue;
        for (int x = 0; x < kWslIconSize; x++) {
            int col = left + x;
            if (col < 0 || col >= surfaceWidth || col < clip.left || col >= clip.right) continue;
            UINT32 s = icon[y * kWslIconSize + x];
            UINT32 a = s >> 24;
            if (!a) continue;
            UINT32 d = pixels[row * surfaceWidth + col];
            auto blend = [&](int shift) {
                UINT32 sc = (s >> shift) & 0xFF, dc = (d >> shift) & 0xFF;
                return std::min<UINT32>(255, sc + dc * (255 - a) / 255) << shift; // src is premultiplied
            };
            pixels[row * surfaceWidth + col] = 0xFF000000u | blend(16) | blend(8) | blend(0);
        }
    }
}

// One argument quoted for a Windows command line (CommandLineToArgvW rules).
static std::wstring QuoteArg(const std::wstring& arg) {
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

static std::wstring WslExePath() {
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    return std::wstring(sysDir) + L"\\wsl.exe";
}

bool RunWslExe(const std::wstring& args, const std::string& input, std::string* output) {
    std::wstring cmd = QuoteArg(WslExePath()) + L" " + args;

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

    // stderr goes nowhere: callers parse stdout (a file's contents, a
    // listing), and wsl.exe's own warnings mustn't end up mixed into it.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = nul;
    PROCESS_INFORMATION pi = {};
    bool started = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                  &pi) != 0;
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        Log(L"wsl: couldn't run wsl.exe, lastError=%lu", GetLastError());
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

bool RunInWsl(const std::wstring& distro, const std::vector<std::wstring>& argv, const std::string& input,
              std::string* output) {
    // --cd ~: otherwise wsl.exe starts in this app's own Windows working
    // folder, which a distro without drive mounts can't translate -- it warns
    // and carries on, but there's no reason to make it try.
    std::wstring args = L"--cd ~ -d " + distro + L" -e";
    for (const std::wstring& a : argv) args += L" " + QuoteArg(a);
    return RunWslExe(args, input, output);
}

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

static const wchar_t* kWslgWindowCtl = L"/mnt/wslg/runtime-dir/wslg-window-ctl";

bool WslgWindowControlAvailable(const std::wstring& distro) {
    struct Check {
        bool available = false;
        ULONGLONG tick = 0;
        bool done = false;
    };
    static std::map<std::wstring, Check> checks;
    Check& c = checks[distro];
    if (c.done && (c.available || GetTickCount64() - c.tick < 60000)) return c.available;
    c.done = true;
    c.tick = GetTickCount64();
    bool was = c.available;
    c.available = RunInWsl(distro, {L"test", L"-p", kWslgWindowCtl}, "", nullptr);
    if (c.available != was || !was) {
        Log(L"wsl: distro [%ls] %ls the WSLg window control FIFO", distro.c_str(),
            c.available ? L"has" : L"doesn't have (stock WSLg modules) --");
    }
    return c.available;
}

// One relay per distro: wsl.exe running `cat` into the control FIFO, fed
// through its stdin. Restarted (once per send) if it has gone away, e.g.
// after WSLg restarted.
struct WslgRelay {
    HANDLE process = nullptr;
    HANDLE stdinWrite = nullptr;
};
static std::map<std::wstring, WslgRelay> g_relays;

static void CloseRelay(WslgRelay& r) {
    if (r.stdinWrite) CloseHandle(r.stdinWrite);
    if (r.process) {
        TerminateProcess(r.process, 0);
        CloseHandle(r.process);
    }
    r = {};
}

static bool StartRelay(const std::wstring& distro, WslgRelay& r) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return false;
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    std::wstring cmd = QuoteArg(WslExePath()) + L" --cd ~ -d " + distro + L" -e sh -c " +
                       QuoteArg(std::wstring(L"f=") + kWslgWindowCtl + L"; [ -p \"$f\" ] || exit 3; exec cat > \"$f\"");
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi = {};
    bool ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    CloseHandle(inRead);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(inWrite);
        LogWarn(L"wsl: couldn't start the window control relay for [%ls], lastError=%lu", distro.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    r.process = pi.hProcess;
    r.stdinWrite = inWrite;
    Log(L"wsl: window control relay started for [%ls]", distro.c_str());
    return true;
}

void SendWslgWindowRect(HWND hwnd, const std::wstring& distro, const RECT& windowRect) {
    if (distro.empty() || !WslgWindowControlAvailable(distro)) return;
    // msrdc stores 0x1_0000_0000 | <RAIL window id> on each of its windows.
    ULONGLONG prop = (ULONGLONG)(ULONG_PTR)GetPropW(hwnd, L"WslgServerWindowId");
    if (!prop) return;
    unsigned id = (unsigned)(prop & 0xFFFFFFFFu);
    char line[96];
    snprintf(line, sizeof(line), "move %x %ld %ld %ld %ld\n", id, windowRect.left, windowRect.top, windowRect.right,
             windowRect.bottom);
    WslgRelay& r = g_relays[distro];
    for (int attempt = 0; attempt < 2; attempt++) {
        if (r.process && WaitForSingleObject(r.process, 0) == WAIT_OBJECT_0) CloseRelay(r); // it exited
        if (!r.process && !StartRelay(distro, r)) return;
        DWORD written = 0;
        if (WriteFile(r.stdinWrite, line, (DWORD)strlen(line), &written, nullptr)) return;
        CloseRelay(r);
    }
}

bool LaunchInWsl(const std::wstring& distro, const std::vector<std::wstring>& argv) {
    std::wstring cmd = QuoteArg(WslExePath()) + L" --cd ~ -d " + distro + L" -e"; // --cd ~: see RunInWsl
    for (const std::wstring& a : argv) cmd += L" " + QuoteArg(a);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        LogWarn(L"wsl: couldn't launch [%ls] in distro [%ls], lastError=%lu", argv.empty() ? L"" : argv[0].c_str(),
                distro.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

std::vector<std::wstring> LinuxVSCodeOpenCommand(const std::wstring& linuxPath) {
    return {L"env", L"DONT_PROMPT_WSL_INSTALL=1", L"code", L"-n", linuxPath};
}

std::wstring WslUncPath(const std::wstring& distro, const std::wstring& linuxPath) {
    std::wstring tail = linuxPath;
    std::replace(tail.begin(), tail.end(), L'/', L'\\');
    return L"\\\\wsl.localhost\\" + distro + tail;
}

bool ParseWslUncPath(const std::wstring& path, std::wstring* distro, std::wstring* linuxPath) {
    for (const wchar_t* prefix : {L"\\\\wsl.localhost\\", L"\\\\wsl$\\"}) {
        size_t n = wcslen(prefix);
        if (path.size() <= n || _wcsnicmp(path.c_str(), prefix, n) != 0) continue;
        size_t slash = path.find(L'\\', n);
        std::wstring name = path.substr(n, slash == std::wstring::npos ? std::wstring::npos : slash - n);
        if (name.empty()) return false;
        std::wstring tail = slash == std::wstring::npos ? L"/" : path.substr(slash);
        std::replace(tail.begin(), tail.end(), L'\\', L'/');
        if (distro) *distro = name;
        if (linuxPath) *linuxPath = tail;
        return true;
    }
    return false;
}
