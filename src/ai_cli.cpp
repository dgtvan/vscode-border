#include "ai_cli.h"

#include "assistant_profile.h"
#include "logger.h"

#include <cwchar>

namespace {

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}

std::wstring Expand(const wchar_t* text) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = ExpandEnvironmentStringsW(text, buf, MAX_PATH * 2);
    return n > 0 && n <= MAX_PATH * 2 ? std::wstring(buf) : std::wstring(text);
}

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// One provider's terminal CLI. `exeNames` are searched on PATH, in order;
// `installPaths` are the per-user locations the provider's own installers
// use, tried after PATH, with %ENV% expanded. `supported` off means the row
// is kept ready but never offered.
struct AiCliTool {
    ProfileType type;
    const wchar_t* command;
    bool supported;
    std::vector<const wchar_t*> exeNames;
    std::vector<const wchar_t*> installPaths;
};

// The only provider-specific knowledge in this file. Everything else --
// which accounts are offered, which terminal hosts the session, how the
// account environment is applied -- is shared. A row being here and
// `supported` is what offers that provider; whether its CLI is installed is
// not checked before launching (see ai_cli.h).
const std::vector<AiCliTool>& AiClis() {
    static const std::vector<AiCliTool> tools = {
        {ProfileType::Claude, L"claude", true,
         {L"claude.exe", L"claude.cmd"},
         {L"%USERPROFILE%\\.local\\bin\\claude.exe", L"%APPDATA%\\npm\\claude.cmd"}},
        {ProfileType::Codex, L"codex", true,
         {L"codex.exe", L"codex.cmd"},
         {L"%USERPROFILE%\\.local\\bin\\codex.exe", L"%APPDATA%\\npm\\codex.cmd"}},
    };
    return tools;
}

const AiCliTool* FindTool(ProfileType type) {
    for (const AiCliTool& tool : AiClis()) {
        if (tool.type == type) return &tool;
    }
    return nullptr;
}

ProfileType TypeOfProfile(const std::wstring& id) {
    const Profile* p = FindProfile(id);
    return p ? p->type : ProfileType::None;
}

// Windows Terminal, when installed. Reached through the app execution alias
// in WindowsApps (which is on PATH), then that alias by its full path, for
// the case where PATH has been trimmed.
bool FindWindowsTerminal(std::wstring& outPath) {
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        outPath = found;
        return true;
    }
    std::wstring alias = Expand(L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\wt.exe");
    if (FileExists(alias)) {
        outPath = alias;
        return true;
    }
    return false;
}

std::wstring CmdExe() {
    wchar_t system32[MAX_PATH] = {};
    GetSystemDirectoryW(system32, MAX_PATH);
    return std::wstring(system32) + L"\\cmd.exe";
}

// The command cmd.exe runs: point the provider at the account folder, then
// start the CLI. The account variable is applied with `set` inside the
// command rather than through a process environment block because Windows
// Terminal does not necessarily create the shell as a child of this app --
// `wt.exe` is a stub that hands the request to an already-running Windows
// Terminal, which then spawns the shell with its OWN environment. An
// environment block passed to `wt.exe` would therefore reach the session
// only when no Windows Terminal was running yet, i.e. the account would be
// silently wrong about half the time. Carried in the command line it applies
// either way, and identically under the console host.
//
// `set VAR=value` deliberately leaves `value` unquoted and is followed
// immediately by `&` with no space: cmd takes everything up to the `&` as
// the value, spaces included, and a space before `&` would end up inside the
// value. /k rather than /c so the window stays after the CLI exits, which is
// where its last output (including an error) is.
//
// `cli` is already quoted if it needs to be -- a resolved path is, a bare
// command name is not, so a CLI that isn't installed is reported by cmd as
// `'codex' is not recognized` rather than `'"codex"' is not recognized`.
std::wstring CmdCommand(const std::wstring& cli, const std::wstring& profile) {
    std::wstring assignment = ProfileEnvironmentAssignment(profile);
    std::wstring command = L"/d /k ";
    if (!assignment.empty()) command += L"set " + assignment + L"&";
    return command + cli;
}

// Launches `commandLine` with `workingDir` as the working directory. The
// command line is passed on its own (no lpApplicationName) so the
// WindowsApps execution alias resolves the way it would from a shell.
bool Launch(std::wstring commandLine, const std::wstring& workingDir, const wchar_t* host) {
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &commandLine[0], nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                        workingDir.c_str(), &si, &pi)) {
        LogWarn(L"couldn't start the AI CLI in %ls (%ls) in [%ls], lastError=%lu", host, commandLine.c_str(),
                workingDir.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

bool AiCliSupported(ProfileType type) {
    const AiCliTool* tool = FindTool(type);
    return tool && tool->supported;
}

const wchar_t* AiCliCommand(ProfileType type) {
    const AiCliTool* tool = FindTool(type);
    return tool && tool->supported ? tool->command : L"";
}

bool FindAiCli(ProfileType type, std::wstring& outExePath) {
    const AiCliTool* tool = FindTool(type);
    if (!tool || !tool->supported) return false;
    for (const wchar_t* name : tool->exeNames) {
        wchar_t found[MAX_PATH] = {};
        if (SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr) > 0) {
            outExePath = found;
            return true;
        }
    }
    for (const wchar_t* candidate : tool->installPaths) {
        std::wstring path = Expand(candidate);
        if (FileExists(path)) {
            outExePath = path;
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> AiCliProfileIds() {
    std::vector<std::wstring> ids;
    for (const std::wstring& id : ConfiguredProfileIds()) {
        if (AiCliSupported(TypeOfProfile(id))) ids.push_back(id);
    }
    return ids;
}

bool OpenAiCliSession(const std::wstring& folder, const wchar_t* reason, const std::wstring& profile) {
    // An account dropped from the config between the menu being built and an
    // item being picked would otherwise start the CLI on whatever account the
    // environment happens to name -- the same refusal RunVSCodeCli makes.
    if (!profile.empty() && !FindProfile(profile)) {
        LogWarn(L"AI CLI launch refused: selected profile is no longer configured");
        return false;
    }
    // With no account the launch is account-less (ProfileType::None has no
    // CLI of its own): run the first supported provider's CLI, leaving the
    // environment to say which account it signs in as.
    ProfileType type = profile.empty() ? ProfileType::None : TypeOfProfile(profile);
    if (!AiCliSupported(type)) {
        for (const AiCliTool& tool : AiClis()) {
            if (tool.supported) {
                type = tool.type;
                break;
            }
        }
    }
    const AiCliTool* tool = FindTool(type);
    if (!tool || !tool->supported) {
        LogWarn(L"AI CLI launch refused: no supported CLI to run for profile [%ls]", ProfileName(profile).c_str());
        return false;
    }
    // Not finding it is not a refusal: the bare command name goes to cmd,
    // which reports it is not recognized, in a window that stays open. That
    // is a clearer answer than a silent no-op, and installing the CLI is not
    // this app's job (see ai_cli.h).
    std::wstring exePath;
    std::wstring cli;
    if (FindAiCli(type, exePath)) {
        cli = L"\"" + exePath + L"\"";
    } else {
        cli = tool->command;
        Log(L"AI CLI: no %ls executable on PATH or in the usual install locations -- running it by name, so the "
            L"terminal reports whether it exists",
            tool->command);
    }

    std::wstring workingDir = folder;
    if (workingDir.empty() || !DirectoryExists(workingDir)) {
        if (!workingDir.empty()) {
            Log(L"AI CLI: [%ls] is not an existing directory -- starting in the user profile folder instead",
                workingDir.c_str());
        }
        workingDir = EnvVar(L"USERPROFILE");
    }

    std::wstring cmdCommand = CmdCommand(cli, profile);
    std::wstring windowsTerminal;
    bool wantWindowsTerminal = g_config.aiCliTerminal != AiCliTerminal::Cmd;
    bool haveWindowsTerminal = wantWindowsTerminal && FindWindowsTerminal(windowsTerminal);
    if (wantWindowsTerminal && !haveWindowsTerminal && g_config.aiCliTerminal == AiCliTerminal::WindowsTerminal) {
        Log(L"AI CLI: ai_cli_terminal=windows-terminal, but wt.exe isn't installed -- using a console window");
    }
    // `;` is how wt.exe separates one tab/pane from the next, and it has no
    // escape that survives being re-split by the Windows Terminal the
    // request is handed to: a folder or account path containing one would be
    // cut in half into two bogus tabs. The console host has no such parsing,
    // so it takes those launches instead.
    if (haveWindowsTerminal &&
        (workingDir.find(L';') != std::wstring::npos || cmdCommand.find(L';') != std::wstring::npos)) {
        Log(L"AI CLI: the folder or account path contains ';', which wt.exe would read as a tab separator -- "
            L"using a console window");
        haveWindowsTerminal = false;
    }

    std::wstring title = ProfileName(profile);
    if (title.empty()) title = AiCliCommand(type);
    size_t slash = workingDir.find_last_of(L"\\/");
    std::wstring leaf = slash == std::wstring::npos ? workingDir : workingDir.substr(slash + 1);
    if (!leaf.empty()) title += L" - " + leaf;

    bool ok = haveWindowsTerminal
                  ? Launch(L"\"" + windowsTerminal + L"\" --title \"" + title + L"\" -d \"" + workingDir + L"\" \"" +
                               CmdExe() + L"\" " + cmdCommand,
                           workingDir, L"Windows Terminal")
                  : Launch(L"\"" + CmdExe() + L"\" " + cmdCommand, workingDir, L"a console window");
    if (!ok) return false;
    Log(L"started the %ls CLI in %ls for %ls as profile [%ls] (%ls)", AiCliCommand(type),
        haveWindowsTerminal ? L"Windows Terminal" : L"a console window", workingDir.c_str(),
        ProfileName(profile).c_str(), reason);
    return true;
}
