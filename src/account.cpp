#include "account.h"
#include "config.h"
#include "file_util.h"
#include "focus_trace.h"
#include "layered_rendering.h"
#include "logger.h"
#include "resource.h"
#include "text_util.h"
#include "vscode_cli.h"
#include "vscode_installs.h"

#include <gdiplus.h>
#include <shlwapi.h>
#include <wincred.h>

#include <algorithm>
#include <map>
#include <vector>

const wchar_t* const kFindVSCodeArg = L"--find-vscode";

// The SID inside a TOKEN_USER buffer read from `process`'s token, or empty.
static std::vector<BYTE> ProcessUserSid(HANDLE process) {
    std::vector<BYTE> out;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return out;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    if (size && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
        PSID sid = ((TOKEN_USER*)buf.data())->User.Sid;
        out.assign((BYTE*)sid, (BYTE*)sid + GetLengthSid(sid));
    }
    CloseHandle(token);
    return out;
}

std::wstring OtherAccountOfProcess(DWORD pid) {
    static const std::vector<BYTE> ownSid = ProcessUserSid(GetCurrentProcess());
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        LogDiag(L"account: can't open pid=%lu, lastError=%lu", pid, GetLastError());
        return L"";
    }
    std::vector<BYTE> sid = ProcessUserSid(process);
    CloseHandle(process);
    if (sid.empty()) {
        LogDiag(L"account: can't read the owner of pid=%lu, lastError=%lu", pid, GetLastError());
        return L"";
    }
    if (sid == ownSid) return L"";

    wchar_t name[256] = {}, domain[256] = {};
    DWORD nameLen = 256, domainLen = 256;
    SID_NAME_USE use;
    if (!LookupAccountSidW(nullptr, sid.data(), name, &nameLen, domain, &domainLen, &use)) {
        // ERROR_NONE_MAPPED: the account was deleted while this process kept
        // running -- not a fault here, so not a warning.
        DWORD err = GetLastError();
        Log(L"account: pid=%lu runs as another account whose name can't be looked up%ls, lastError=%lu", pid,
            err == ERROR_NONE_MAPPED ? L" (deleted?)" : L"", err);
        return L"?";
    }
    return name;
}

std::wstring CurrentAccountName() {
    wchar_t name[256] = {};
    DWORD len = 256;
    return GetUserNameW(name, &len) ? name : L"";
}

std::vector<std::wstring> ConfiguredUserProfiles() {
    std::vector<std::wstring> out;
    for (const UserProfile& p : g_config.userProfiles) {
        if (!p.thisAccount) out.push_back(p.account);
    }
    return out;
}

std::wstring IconAccountFor(const std::wstring& account) {
    if (!account.empty()) return account;
    for (const UserProfile& p : g_config.userProfiles) {
        if (p.thisAccount && !p.icon.empty()) return p.account;
    }
    return L"";
}

// Windows account names are case-insensitive, non-ASCII letters included
// (which _wcsicmp only folds under a matching locale), so config.ini's
// spelling can differ in case from the account's own.
static bool SameAccountName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

static const UserProfile* FindUserProfile(const std::wstring& account) {
    for (const UserProfile& p : g_config.userProfiles) {
        if (SameAccountName(p.account, account)) return &p;
    }
    return nullptr;
}

// A config.ini path, made absolute (relative to config.ini's folder).
static std::wstring ConfigRelativePath(const std::wstring& p) {
    if (p.empty()) return p;
    bool absolute = p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
    return absolute ? p : GetExeDir() + L"\\" + p;
}

// Whether an icon value is the image itself rather than a path to one: a
// data URI ("data:image/png;base64,..."), or bare base64 -- which never has
// a '.', '\\' or ':', while a path to an image file always has one of them.
static bool IsBase64Icon(const std::wstring& icon) {
    if (_wcsnicmp(icon.c_str(), L"data:", 5) == 0) return true;
    return icon.find_first_of(L".\\:") == std::wstring::npos;
}

// `account`'s profile icon as config.ini has it: an absolute image path, the
// base64 image as is, or empty.
static std::wstring ConfiguredIconSource(const std::wstring& account) {
    const UserProfile* p = FindUserProfile(account);
    if (!p || p->icon.empty()) return L"";
    return IsBase64Icon(p->icon) ? p->icon : ConfigRelativePath(p->icon);
}

// For log lines: a base64 icon by its size, not its contents.
static std::wstring DescribeIconSource(const std::wstring& source) {
    if (source.empty()) return L"the letter badge";
    if (IsBase64Icon(source)) return L"a base64 image (" + std::to_wstring(source.size()) + L" chars)";
    return source;
}

// Decodes base64 (a data URI's payload, or bare), skipping whitespace; false
// on anything else outside the base64 alphabet.
static bool DecodeBase64(const std::wstring& text, std::vector<BYTE>& out) {
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
static Gdiplus::Bitmap* LoadIconImage(const std::wstring& source) {
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

// `account`'s icon rendered to premultiplied ARGB (S^2 pixels) from
// `iconSource` (see ConfiguredIconSource), or the letter badge if that's
// empty or can't be read.
static std::vector<UINT32> RenderAccountIcon(const std::wstring& account, const std::wstring& iconSource, int S) {
    std::vector<UINT32> out;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
        LogWarn(L"account: GDI+ didn't start -- no account icon");
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
                LogWarn(L"account: can't read the icon for [%ls] from %ls -- using the letter badge", account.c_str(),
                        DescribeIconSource(iconSource).c_str());
            }
            delete image;
        }
        if (!drawn) {
            Gdiplus::SolidBrush circle(Gdiplus::Color(255, 0x3B, 0x42, 0x52));
            g.FillEllipse(&circle, 0.0f, 0.0f, (Gdiplus::REAL)S, (Gdiplus::REAL)S);
            wchar_t letter[2] = {account.empty() ? L'?' : (wchar_t)towupper(account[0]), 0};
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

void BlendAccountIcon(const std::wstring& account, UINT32* pixels, int surfaceWidth, int surfaceHeight, int left,
                      int top, const RECT& clip) {
    // Per account, with the icon source it was rendered from, so a changed
    // user_profile_<N>_icon takes effect on Reload Config.
    struct CachedIcon {
        std::wstring source;
        std::vector<UINT32> pixels;
    };
    static std::map<std::wstring, CachedIcon> cache;
    std::wstring source = ConfiguredIconSource(account);
    auto it = cache.find(account);
    if (it == cache.end() || it->second.source != source) {
        Log(L"account: icon for [%ls] from %ls", account.c_str(), DescribeIconSource(source).c_str());
        it = cache.insert_or_assign(account, CachedIcon{source, RenderAccountIcon(account, source, kAccountIconSize)})
                 .first;
    }
    if (it->second.pixels.empty()) return;
    BlendPremultipliedIcon(it->second.pixels.data(), kAccountIconSize, pixels, surfaceWidth, surfaceHeight, left, top,
                           clip);
}

HBITMAP GetAccountIconMenuBitmap(const std::wstring& account) {
    // Keyed by account and icon source, like BlendAccountIcon's cache; a
    // bitmap replaced by a changed icon is kept rather than freed, since a
    // menu may still be showing it.
    static std::map<std::wstring, HBITMAP> cache;
    std::wstring source = ConfiguredIconSource(account);
    std::wstring key = account + L"|" + source;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    int size = GetSystemMetrics(SM_CXSMICON);
    std::vector<UINT32> icon = RenderAccountIcon(account, source, size);
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

std::wstring AccountMenuText(const std::wstring& account) {
    std::wstring out;
    for (wchar_t c : account) {
        if (c == L'&') out += L'&';
        out += c;
    }
    return out;
}

// ---- Starting VS Code as another account ----

static std::wstring CredentialTarget(const std::wstring& account) {
    return L"vscode-border/user-profile/" + account;
}

static bool LoadSavedPassword(const std::wstring& account, std::wstring& password) {
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(CredentialTarget(account).c_str(), CRED_TYPE_GENERIC, 0, &cred)) return false;
    password.assign((const wchar_t*)cred->CredentialBlob, cred->CredentialBlobSize / sizeof(wchar_t));
    SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return true;
}

static void SavePassword(const std::wstring& account, const std::wstring& password) {
    std::wstring target = CredentialTarget(account);
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = (LPWSTR)target.c_str();
    cred.UserName = (LPWSTR)account.c_str();
    cred.CredentialBlobSize = (DWORD)(password.size() * sizeof(wchar_t));
    cred.CredentialBlob = (LPBYTE)password.data();
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (CredWriteW(&cred, 0)) {
        Log(L"account: saved the password for [%ls] in Credential Manager (%ls)", account.c_str(), target.c_str());
    } else {
        LogWarn(L"account: couldn't save the password for [%ls], lastError=%lu", account.c_str(), GetLastError());
    }
}

bool HasSavedAccountPassword(const std::wstring& account) {
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(CredentialTarget(account).c_str(), CRED_TYPE_GENERIC, 0, &cred)) return false;
    CredFree(cred);
    return true;
}

void ForgetSavedAccountPassword(const std::wstring& account) {
    if (CredDeleteW(CredentialTarget(account).c_str(), CRED_TYPE_GENERIC, 0)) {
        Log(L"account: forgot the saved password for [%ls]", account.c_str());
    }
}

static void WipeString(std::wstring& s) {
    if (!s.empty()) SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
    s.clear();
}

struct PasswordPrompt {
    std::wstring account;
    bool retry = false;    // the last password tried was wrong
    std::wstring password; // out
    bool remember = true;  // in/out
};

static INT_PTR CALLBACK PasswordDialogProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    PasswordPrompt* p = (PasswordPrompt*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG: {
            p = (PasswordPrompt*)lParam;
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)p);
            std::wstring text = std::wstring(p->retry ? L"Wrong password. " : L"") +
                                L"Password for the Windows account \"" + p->account + L"\", to start VS Code as it:";
            SetDlgItemTextW(dlg, IDC_PASSWORD_PROMPT, text.c_str());
            CheckDlgButton(dlg, IDC_PASSWORD_REMEMBER, p->remember ? BST_CHECKED : BST_UNCHECKED);
            // Above the hub, which is topmost itself.
            SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(dlg);
            SetFocus(GetDlgItem(dlg, IDC_PASSWORD_EDIT));
            return FALSE; // focus already set
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                HWND edit = GetDlgItem(dlg, IDC_PASSWORD_EDIT);
                int len = GetWindowTextLengthW(edit);
                p->password.assign(len + 1, L'\0');
                GetWindowTextW(edit, &p->password[0], len + 1);
                p->password.resize(len);
                SetWindowTextW(edit, L"");
                p->remember = IsDlgButtonChecked(dlg, IDC_PASSWORD_REMEMBER) == BST_CHECKED;
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// Runs `commandLine` as `account`, hidden, with its profile loaded. 0 on
// success, else the Win32 error.
static DWORD LaunchAs(const std::wstring& account, const std::wstring& password, const std::wstring& application,
                      std::wstring commandLine, const std::wstring& workingDir) {
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessWithLogonW(account.c_str(), L".", password.c_str(), LOGON_WITH_PROFILE, application.c_str(),
                                 &commandLine[0], CREATE_NO_WINDOW, nullptr, workingDir.c_str(), &si, &pi)) {
        return GetLastError();
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}


// Starts `application` with `commandLine` as `account` (with its profile
// loaded, hidden) and stdin/stdout/stderr on `stdIn`/`stdOut`. The process
// handle, or null with `error` set.
static HANDLE StartAs(const std::wstring& account, const std::wstring& password, const std::wstring& application,
                      std::wstring commandLine, const std::wstring& workingDir, HANDLE stdIn, HANDLE stdOut,
                      DWORD& error) {
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = stdIn;
    si.hStdOutput = stdOut;
    si.hStdError = stdOut;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessWithLogonW(account.c_str(), L".", password.c_str(), LOGON_WITH_PROFILE, application.c_str(),
                                 &commandLine[0], CREATE_NO_WINDOW, nullptr, workingDir.c_str(), &si, &pi)) {
        error = GetLastError();
        return nullptr;
    }
    error = 0;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// Searches for VS Code as `account` -- its PATH, its own per-user install,
// a machine-wide one (see vscode_cli.h's FindInstalledVSCodeCli) -- by
// running this exe with kFindVSCodeArg as that account, since this account
// can't look into its profile. The Code.exe found, or empty; `error` is set
// when the search couldn't even start (ERROR_LOGON_FAILURE for a wrong
// password).
static std::wstring FindVSCodeAs(const std::wstring& account, const std::wstring& password, DWORD& error) {
    error = 0;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE ourOut = nullptr, childOut = nullptr;
    if (!CreatePipe(&ourOut, &childOut, &inherit, 0)) {
        error = GetLastError();
        return L"";
    }
    SetHandleInformation(ourOut, HANDLE_FLAG_INHERIT, 0);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" " + kFindVSCodeArg;
    HANDLE child = StartAs(account, password, exe, commandLine, GetExeDir(), nullptr, childOut, error);
    CloseHandle(childOut);
    std::string output;
    if (child) {
        char buf[512];
        DWORD read = 0;
        while (ReadFile(ourOut, buf, sizeof(buf), &read, nullptr) && read > 0) output.append(buf, read);
        WaitForSingleObject(child, 5000);
        CloseHandle(child);
    }
    CloseHandle(ourOut);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return Utf8ToWide(output);
}

int RunFindVSCode() {
    std::wstring shim;
    if (!FindInstalledVSCodeCli(shim)) return 1;
    std::string line = WideToUtf8(VSCodeExeForShim(shim)) + "\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), (DWORD)line.size(), &written, nullptr);
    return 0;
}

void OpenNewVSCodeWindowAs(HWND owner, const std::wstring& account, const std::wstring& folder,
                           const wchar_t* reason) {
    wchar_t system32[MAX_PATH] = {};
    GetSystemDirectoryW(system32, MAX_PATH);
    std::wstring cmdExe = std::wstring(system32) + L"\\cmd.exe";

    PasswordPrompt prompt;
    prompt.account = account;
    for (int attempt = 0; attempt < 5; attempt++) {
        std::wstring password;
        bool saved = LoadSavedPassword(account, password);
        if (!saved) {
            if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PASSWORD), owner, PasswordDialogProc,
                                (LPARAM)&prompt) != IDOK) {
                Log(L"account: new window as [%ls] cancelled at the password prompt", account.c_str());
                return;
            }
            password = prompt.password;
            WipeString(prompt.password);
        }

        // Which VS Code: see vscode_installs.h. (A running window of the
        // account is what keeps its remembered install current.)
        DWORD err = 0;
        const wchar_t* source = L"its remembered install";
        std::wstring exe = RememberedVSCodeInstall(account);
        if (exe.empty()) {
            source = L"a search as that account";
            exe = FindVSCodeAs(account, password, err);
        }
        if (exe.empty() && err != ERROR_LOGON_FAILURE) {
            std::vector<std::wstring> shared = SharedVSCodeInstalls(account);
            if (!shared.empty()) {
                source = L"another account's shared install";
                exe = shared.front();
            }
        }

        std::wstring shim, commandLine;
        if (!exe.empty()) {
            // Through the install's CLI shim, for the same reason as
            // vscode_cli.h's ResolveVSCodeCliShim: raw Code.exe rejects -n.
            shim = VSCodeShimForExe(exe);
            commandLine =
                L"cmd.exe /d /c \"\"" + shim + L"\" -n" + (folder.empty() ? L"" : L" \"" + folder + L"\"") + L"\"";
            std::wstring installDir = exe.substr(0, exe.find_last_of(L"\\/"));
            NoteWindowLaunchRequest(reason, 1);
            err = LaunchAs(account, password, cmdExe, commandLine, installDir);
            if (err == 0) {
                Log(L"account: opened a new VS Code window as [%ls] via %ls (%ls)%ls%ls", account.c_str(),
                    shim.c_str(), source, folder.empty() ? L"" : L" folder=", folder.c_str());
                if (!saved && prompt.remember) SavePassword(account, password);
                WipeString(password);
                return;
            }
        }
        WipeString(password);
        if (err == ERROR_LOGON_FAILURE) {
            Log(L"account: wrong %ls password for [%ls]", saved ? L"saved" : L"entered", account.c_str());
            if (saved) ForgetSavedAccountPassword(account);
            prompt.retry = true;
            continue;
        }

        wchar_t text[1024];
        if (exe.empty()) {
            LogWarn(L"account: couldn't find VS Code for [%ls] -- nothing remembered, none found as that account, "
                    L"and no other account's install it can share",
                    account.c_str());
            swprintf(text, 1024,
                     L"Couldn't find VS Code for \"%ls\".\n\nStart VS Code once as that account (or install it "
                     L"there) -- the app then remembers where it is.",
                     account.c_str());
        } else if (err == ERROR_DIRECTORY) {
            // The working directory is the install folder, so this is where a
            // missing install shows up: this account can't look into the
            // other account's profile to check beforehand.
            LogWarn(L"account: [%ls]'s VS Code (%ls, from %ls) isn't there", account.c_str(), exe.c_str(), source);
            ForgetVSCodeInstall(account);
            swprintf(text, 1024,
                     L"VS Code for \"%ls\" isn't at:\n%ls\n\nStart VS Code once as that account -- the app then "
                     L"remembers where it is.",
                     account.c_str(), exe.c_str());
        } else {
            LogWarn(L"account: couldn't start VS Code as [%ls] (%ls), error=%lu", account.c_str(), commandLine.c_str(),
                    err);
            swprintf(text, 1024, L"Couldn't start VS Code as \"%ls\" (error %lu).\n\nVS Code used: %ls",
                     account.c_str(), err, exe.c_str());
        }
        MessageBoxW(owner, text, L"VS Code Border", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        return;
    }
}

HANDLE StartHiddenProcessAsAccount(const std::wstring& account, const std::wstring& application,
                                   std::wstring commandLine, const std::wstring& workingDir, HANDLE stdIn,
                                   HANDLE stdOut, DWORD& error) {
    std::wstring password;
    if (!LoadSavedPassword(account, password)) {
        error = ERROR_NOT_FOUND;
        return nullptr;
    }
    HANDLE process = StartAs(account, password, application, commandLine, workingDir, stdIn, stdOut, error);
    WipeString(password);
    return process;
}
