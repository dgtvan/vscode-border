#include "codex_usage_client.h"
#include "codex_usage_protocol.h"
#include "file_util.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace {
std::wstring Env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::wstring value(n, L'\0');
    DWORD read = GetEnvironmentVariableW(name, &value[0], n);
    if (!read || read >= n) return L"";
    value.resize(read);
    return value;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// Each request has a deadline and reads bounded JSONL frames. A job object
// owns this subprocess tree, including on cancel, timeout, reload and exit.
class AppServer {
    Handle job, process, input, output;
    HANDLE stop;
    std::string pending;
    int nextId = 0;

    bool Send(const std::string& line, std::wstring& error) {
        DWORD written = 0;
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
            !WriteFile(input.value, line.data(), (DWORD)line.size(), &written, nullptr) || written != line.size()) {
            error = L"Codex app-server's input pipe closed";
            return false;
        }
        return true;
    }

public:
    explicit AppServer(HANDLE stopEvent) : stop(stopEvent) {}
    ~AppServer() {
        // Closing stdin normally exits stdio app-server. Kill the job as a
        // backstop so a stalled server (or one of its children) cannot linger.
        if (input.value) { CloseHandle(input.value); input.value = nullptr; }
        if (process.value && WaitForSingleObject(process.value, 250) != WAIT_OBJECT_0 && job.value) {
            TerminateJobObject(job.value, 1);
            WaitForSingleObject(process.value, 1000);
        }
    }

    bool Start(const std::wstring& exe, std::wstring& environment, std::wstring& error) {
        SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
        Handle childInput, childOutput, childError;
        if (!CreatePipe(&childInput.value, &input.value, &inherit, 0) ||
            !CreatePipe(&output.value, &childOutput.value, &inherit, 0) ||
            !SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0)) {
            error = L"Couldn't create private pipes for Codex usage";
            return false;
        }
        // Do not put Codex's stderr (which may contain private paths or
        // diagnostics) in the protocol pipe or in this app's log.
        childError.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                       OPEN_EXISTING, 0, nullptr);
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (childError.value == INVALID_HANDLE_VALUE || !job.value ||
            !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            error = L"Couldn't prepare the Codex usage subprocess";
            return false;
        }
        // Limit handle inheritance: several profile workers can start at
        // once and must never inherit each other's pipe ends.
        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.StartupInfo.wShowWindow = SW_HIDE;
        si.StartupInfo.hStdInput = childInput.value;
        si.StartupInfo.hStdOutput = childOutput.value;
        si.StartupInfo.hStdError = childError.value;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<BYTE> attrs(bytes);
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrs.data();
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes)) {
            error = L"Couldn't initialize Codex usage process attributes";
            return false;
        }
        HANDLE handles[] = {childInput.value, childOutput.value, childError.value};
        bool ready = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                               handles, sizeof(handles), nullptr, nullptr) != FALSE;
        std::wstring command = L"\"" + exe + L"\" app-server";
        PROCESS_INFORMATION pi = {};
        bool started = ready && CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                            EXTENDED_STARTUPINFO_PRESENT, &environment[0], GetExeDir().c_str(), &si.StartupInfo, &pi);
        DWORD err = GetLastError();
        DeleteProcThreadAttributeList(si.lpAttributeList);
        if (!started) {
            error = L"Couldn't start codex app-server (Windows error " + std::to_wstring(err) + L")";
            return false;
        }
        process.value = pi.hProcess;
        bool assigned = AssignProcessToJobObject(job.value, process.value) != FALSE;
        if (!assigned || ResumeThread(pi.hThread) == (DWORD)-1) {
            TerminateProcess(process.value, 1);
            CloseHandle(pi.hThread);
            error = L"Couldn't place Codex usage in its subprocess job";
            return false;
        }
        CloseHandle(pi.hThread);
        return true;
    }

    bool Request(const char* method, const char* params, std::string& result, std::wstring& error,
                  bool& transientError) {
        using namespace CodexJson;
        std::string id = std::to_string(nextId++);
        if (!Send("{\"id\":" + id + ",\"method\":\"" + method + "\",\"params\":" + params + "}\n", error))
            return false;
        ULONGLONG started = GetTickCount64();
        const size_t kMaxFrameBytes = 1024 * 1024;
        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            // Bound all output processed during a request, including floods
            // of unrelated notifications, by the same wall-clock deadline.
            if (GetTickCount64() - started >= 45000) {
                error = L"Codex usage request timed out after 45 seconds";
                transientError = true;
                return false;
            }
            size_t newline = pending.find('\n');
            if (newline != std::string::npos) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                auto root = Root(line);
                if (!root.found) { error = L"Codex app-server returned malformed JSON"; return false; }
                if (Raw(line, root, "id") != id) continue; // unsolicited notifications
                auto rpcError = Object(line, root, "error");
                if (rpcError.found) {
                    // Never log raw responses or credentials. Use messages
                    // only to classify a failure; expose a fixed safe hint.
                    std::wstring message = String(Raw(line, rpcError, "message"));
                    std::transform(message.begin(), message.end(), message.begin(), ::towlower);
                    bool auth = message.find(L"401") != std::wstring::npos ||
                                message.find(L"auth") != std::wstring::npos ||
                                message.find(L"login") != std::wstring::npos ||
                                message.find(L"sign in") != std::wstring::npos;
                    bool rate = message.find(L"429") != std::wstring::npos;
                    bool unsupported = Raw(line, rpcError, "code") == "-32601";
                    error = unsupported ? L"This Codex version doesn't support account usage; update Codex"
                          : auth ? L"Codex couldn't authenticate; sign in with ChatGPT in this profile"
                          : rate ? L"Codex rate-limited the usage query; try a longer usage_meter_refresh_minutes"
                                 : L"Codex couldn't read account usage; check its login and network connection";
                    bool network = false;
                    for (const wchar_t* hint : {L"network", L"connect", L"timed out", L"timeout", L"dns",
                                                L"transport", L"error sending request"})
                        network = network || message.find(hint) != std::wstring::npos;
                    transientError = !auth && !rate && !unsupported && network;
                    return false;
                }
                auto obj = Object(line, root, "result");
                if (!obj.found) { error = L"Codex app-server returned no result object"; return false; }
                result = line.substr(obj.start - 1, obj.end - obj.start + 2);
                return true;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr)) {
                error = L"Codex app-server exited before returning usage; check the profile's Codex configuration";
                return false;
            }
            if (available) {
                char chunk[8192];
                DWORD read = 0;
                if (!ReadFile(output.value, chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr) || !read) {
                    error = L"Couldn't read Codex usage from its output pipe";
                    return false;
                }
                pending.append(chunk, read);
                if (pending.size() > kMaxFrameBytes) {
                    error = L"Codex app-server's response exceeded the usage reader's size limit";
                    return false;
                }
            } else {
                WaitForSingleObject(stop, 50);
            }
        }
        error = L"Codex usage read cancelled";
        return false;
    }

    bool Initialized(std::wstring& error) { return Send("{\"method\":\"initialized\"}\n", error); }
};
} // namespace

std::wstring CodexUsageEnvironment(const std::wstring& configDir) {
    std::vector<std::wstring> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"CODEX_HOME=", 11) != 0) vars.push_back(p);
        }
        FreeEnvironmentStringsW(block);
    }
    vars.push_back(L"CODEX_HOME=" + configDir);
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const auto& v : vars) out += v + L'\0';
    out += L'\0';
    return out;
}

std::wstring FindCodexUsageExecutable() {
    wchar_t found[32768] = {};
    DWORD n = SearchPathW(nullptr, L"codex.exe", nullptr, 32768, found, nullptr);
    if (n > 0 && n < 32768) return found;
    for (const std::wstring& path : {Env(L"USERPROFILE") + L"\\.local\\bin\\codex.exe",
                                   Env(L"LOCALAPPDATA") + L"\\Programs\\OpenAI\\Codex\\bin\\codex.exe"}) {
        if (FileExists(path)) return path;
    }
    std::wstring best;
    ULONGLONG bestTime = 0;
    for (const wchar_t* root : {L"\\.vscode\\extensions\\", L"\\.vscode-insiders\\extensions\\"}) {
        std::wstring extensions = Env(L"USERPROFILE") + root;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((extensions + L"openai.chatgpt-*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            SYSTEM_INFO system = {};
            GetNativeSystemInfo(&system);
            const wchar_t* arch = system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64
                                    ? L"windows-aarch64" : L"windows-x86_64";
            std::wstring exe = extensions + fd.cFileName + L"\\bin\\" + arch + L"\\codex.exe";
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (!GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &data)) continue;
            ULONGLONG time = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
            if (time > bestTime) { bestTime = time; best = exe; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}

bool FetchCodexUsage(HANDLE stop, const std::wstring& exe, const std::wstring& configDir,
                     std::wstring environment, CodexUsageSnapshot& snapshot, std::wstring& error,
                     bool& transientError) {
    using namespace CodexJson;
    error.clear();
    transientError = false;
    if (exe.empty()) {
        error = L"No native Codex executable found; install the Codex VS Code extension or add codex.exe to PATH";
        return false;
    }
    DWORD attrs = GetFileAttributesW(configDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        error = L"The Codex profile folder doesn't exist; create it and sign in with ChatGPT first";
        return false;
    }
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) { error = L"Codex usage read cancelled"; return false; }
    AppServer server(stop);
    if (!server.Start(exe, environment, error)) return false;
    std::string result;
    if (!server.Request("initialize", "{\"clientInfo\":{\"name\":\"vscode_border\",\"title\":\"VS Code Border\",\"version\":\"1.0\"}}",
                         result, error, transientError) || !server.Initialized(error) ||
        !server.Request("account/read", "{\"refreshToken\":false}", result, error, transientError)) return false;
    auto account = Object(result, Root(result), "account");
    if (!account.found || String(Raw(result, account, "type")) != L"chatgpt") {
        error = L"No ChatGPT login in this Codex profile; run codex login with its CODEX_HOME (API keys have no usage rings)";
        return false;
    }
    snapshot.email = String(Raw(result, account, "email"));
    // Codex handles token renewal while making its own account request. Do
    // not force refreshToken=true: normal refreshes should remain Codex's decision.
    if (!server.Request("account/rateLimits/read", "{}", result, error, transientError)) return false;
    return ParseCodexRateLimits(result, snapshot, error);
}
