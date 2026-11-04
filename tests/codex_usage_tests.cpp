#include "codex_usage.h"
#include "assistant_profile.h"
#include "profile_config.h"
#include "usage_rings.h"
#include "vscode_cli.h"
#include "codex_usage_client.h"
#include "codex_usage_protocol.h"
#include "file_util.h"
#include "profile_icon.h"
#include "text_util.h"

#include <algorithm>
#include <iostream>
#include <thread>

namespace {
int failures = 0, checks = 0;
void Check(bool pass, const char* label) {
    ++checks;
    if (!pass) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

std::wstring Env(const wchar_t* name) {
    wchar_t out[32768] = {};
    GetEnvironmentVariableW(name, out, 32768);
    return out;
}

std::string Limits(int percent = 23) {
    return "{\"rateLimits\":{\"limitId\":\"codex\",\"primary\":{\"usedPercent\":" + std::to_string(percent) +
           ",\"windowDurationMins\":300,\"resetsAt\":1900000000},\"secondary\":{\"usedPercent\":61,"
           "\"windowDurationMins\":10080,\"resetsAt\":1900500000}}}";
}

// A deterministic stand-in for codex app-server. The production pipe/job
// code is exercised without credentials, network access, or model calls.
int FixtureServer() {
    using namespace CodexJson;
    auto mode = Env(L"VSCODE_BORDER_USAGE_TEST_MODE");
    bool initialized = false;
    std::string line;
    while (std::getline(std::cin, line)) {
        auto root = Root(line);
        auto method = String(Raw(line, root, "method"));
        std::string id = Raw(line, root, "id");
        if (method == L"initialized") { initialized = true; continue; }
        if (mode == L"hang") { Sleep(120000); continue; }
        if (mode == L"exit") return 0;
        if (mode == L"oversize") { std::cout << std::string(1024 * 1024 + 8192, 'x') << std::flush; continue; }
        if (mode == L"malformed") { std::cout << "not json\n" << std::flush; continue; }
        std::string result = "{}";
        if (method != L"initialize" && !initialized) return 2;
        if (method == L"account/read") {
            if (mode == L"apikey") result = "{\"account\":{\"type\":\"apiKey\"}}";
            else if (mode == L"nologin") result = "{\"account\":null}";
            else result = "{\"account\":{\"type\":\"chatgpt\",\"email\":\"example@example.com\"}}";
        } else if (method == L"account/rateLimits/read") {
            if (mode == L"error" || mode == L"autherror" || mode == L"unsupported") {
                std::string code = mode == L"unsupported" ? "-32601" : "-32000";
                std::string message = mode == L"autherror" ? "401 unauthorized private-secret-marker"
                                    : mode == L"unsupported" ? "Method not found" : "network failed private-secret-marker";
                std::cout << "{\"id\":" << id << ",\"error\":{\"code\":" << code
                          << ",\"message\":\"" << message << "\"}}\n" << std::flush;
                continue;
            }
            int pct = Env(L"CODEX_HOME").find(L"profile-b") == std::wstring::npos ? 23 : 73;
            result = Limits(pct);
        } else if (method != L"initialize") return 3; // never start a model turn
        // Exercise unrelated notifications and a response split across reads.
        std::cout << "{\"method\":\"account/updated\",\"params\":{}}\n{\"id\":" << id << std::flush;
        Sleep(10);
        std::cout << ",\"result\":" << result << "}\n" << std::flush;
    }
    return 0;
}

void ProtocolTests() {
    CodexUsageSnapshot s;
    std::wstring error;
    Check(ParseCodexRateLimits(Limits(), s, error) && s.primary.percent == 23 && s.secondary.percent == 61 &&
          s.primary.windowMinutes == 300 && s.secondary.windowMinutes == 10080 && s.primary.resetsAt == 1900000000,
          "read both windows and reset times");
    auto map = R"({"rateLimitsByLimitId":{"codex_other":{"primary":{"usedPercent":99}},"codex":{"primary":{"usedPercent":12,"windowDurationMins":15},"secondary":null}},"rateLimits":{"primary":{"usedPercent":88}}})";
    Check(ParseCodexRateLimits(map, s, error) && s.primary.percent == 12 && s.primary.windowMinutes == 15 &&
          s.secondary.percent == -1, "prefer codex bucket and preserve unknown secondary");
    Check(ParseCodexRateLimits(R"({"rateLimits":{"primary":null,"secondary":{"usedPercent":42}}})", s, error) &&
          s.primary.percent == -1 && s.secondary.percent == 42, "secondary-only and older single-bucket response");
    for (const auto& json : {"", "null", "[]", "{", "{}", "{\"rateLimits\":null}",
          "{\"rateLimits\":{\"primary\":null,\"secondary\":null}}",
          "{\"rateLimits\":{\"limitId\":\"codex_other\",\"primary\":{\"usedPercent\":1}}}"}) {
        Check(!ParseCodexRateLimits(json, s, error) && !error.empty(), "missing limits stay unavailable");
    }
    for (const auto& percent : {"null", "true", "\"12\"", "-1", "101", "1e999", "0junk", "NaN"}) {
        Check(!ParseCodexRateLimits("{\"rateLimits\":{\"primary\":{\"usedPercent\":" + std::string(percent) + "}}}",
                                    s, error), "invalid percentage cannot become zero");
    }
    for (const auto& fields : {"\"resetsAt\":-1", "\"resetsAt\":1.5", "\"resetsAt\":1e99",
                               "\"windowDurationMins\":0", "\"windowDurationMins\":-300",
                               "\"windowDurationMins\":1.5"}) {
        Check(!ParseCodexRateLimits("{\"rateLimits\":{\"primary\":{\"usedPercent\":2," +
                                    std::string(fields) + "}}}", s, error), "invalid window metadata is rejected");
    }
    Check(ParseCodexRateLimits(R"({"rateLimits":{"primary":{"usedPercent":0,"resetsAt":null,"windowDurationMins":null},"secondary":{"usedPercent":100}}})",
                               s, error) && s.primary.percent == 0 && s.secondary.percent == 100, "zero and full usage");
    for (const auto& json : {R"({"rateLimits":{"primary":{"usedPercent":0}})",
                            R"({"rateLimits":{"primary":{"usedPercent":0}},})",
                            R"({"rateLimits":{"primary":{"usedPercent":01}}})",
                            R"({"rateLimits":{"primary":{"usedPercent":0.}}})",
                            R"({"rateLimits":{"primary":{"usedPercent":0}},"extra":[1,]})"}) {
        Check(!ParseCodexRateLimits(json, s, error), "malformed JSON cannot be displayed as zero usage");
    }
    Check(CodexJson::String(R"("a\u0040b\n\"c")") == L"a@b\n\"c", "decode account name escapes");
    Check(CurrentCodexUsagePercent({72, 1, 300}) == 0 && CurrentCodexUsagePercent({-1, 1, 300}) == -1,
          "expired limits reset while unknown limits remain unknown");
}

void ConfigTests(const std::wstring& dir) {
    std::wstring path = dir + L"\\config-test.ini";
    SetEnvironmentVariableW(L"VSCODE_BORDER_USAGE_TEST_ROOT", dir.c_str());
    WriteFileBytes(path, "");
    Check(LoadConfig(path).profiles.empty(), "missing profile configuration means no assistant");
    WriteFileBytes(path,
        "usage_meter_refresh_minutes=99\nprofile_1_type=cOdEx\nprofile_1_name=First\n"
        "profile_1_profile_path=%VSCODE_BORDER_USAGE_TEST_ROOT%/profile-a/\n"
        "profile_1_color=123ABC\nprofile_1_icon=icons/test.png\nprofile_1_ring_badge=false\n"
        "profile_2_type=Claude\nprofile_2_profile_path=%VSCODE_BORDER_USAGE_TEST_ROOT%/profile-a\n"
        "profile_3_type=Codex\nprofile_3_profile_path=%VSCODE_BORDER_USAGE_TEST_ROOT%/PROFILE-A/\n"
        "profile_4_type=Claude\nprofile_4_profile_path=\n"
        "profile_5_type=None\nprofile_5_profile_path=C:/should-not-be-used\n"
        "profile_6_type=Invalid\nprofile_6_profile_path=C:/invalid-provider\n"
        "profile_7_type=Codex\nprofile_7_profile_path=relative/path\n"
        "profile_8_type=\nprofile_8_profile_path=C:/ignored\n"
        "profile_10_type=Codex\nprofile_10_profile_path=%VSCODE_BORDER_USAGE_TEST_ROOT%/profile-b\n"
        "profile_10_usage_enabled=false\n");
    auto cfg = LoadConfig(path);
    Check(cfg.profiles.size() == 3 && cfg.usageMeterRefreshMinutes == 60, "only explicit valid account slots are active");
    const auto& p = cfg.profiles[0];
    Check(p.type == ProfileType::Codex && p.name == L"First" && p.configDir == dir + L"\\profile-a" &&
          p.hasColor && p.color == RGB(0x12, 0x3A, 0xBC) && p.icon == L"icons/test.png" && !p.ringBadge,
          "shared appearance fields and normalized path");
    Check(cfg.profiles[1].type == ProfileType::Claude && cfg.profiles[1].slot == 2 && cfg.profiles[2].slot == 10 &&
          !cfg.profiles[2].usageEnabled, "provider-scoped deduplication, ten slots, and per-profile usage");
    auto identity = ProfileId(p);
    Profile moved = p;
    moved.slot = 9; moved.name = L"Renamed";
    Check(ProfileId(moved) == identity && ProfileId(cfg.profiles[1]) != identity, "stable identity includes provider, not slot or name");
    Check(NormalizeProfilePath(L"C:\\sample\\..\\account\\") == L"C:\\account" &&
          NormalizeProfilePath(L"\\\\server\\share\\codex\\") == L"\\\\server\\share\\codex" &&
          NormalizeProfilePath(L"%UNKNOWN_BORDER_TEST_VARIABLE%\\account").empty(), "dot segments, UNC and unresolved variables");
    g_config = cfg;
    auto environmentValue = [](const std::wstring& block, const wchar_t* key) {
        std::wstring prefix = std::wstring(key) + L"=";
        for (const wchar_t* v = block.c_str(); *v; v += wcslen(v) + 1)
            if (_wcsnicmp(v, prefix.c_str(), prefix.size()) == 0) return std::wstring(v + prefix.size());
        return std::wstring(L"<missing>");
    };
    std::wstring oldClaude = Env(L"CLAUDE_CONFIG_DIR"), oldCodex = Env(L"CODEX_HOME");
    SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", L"C:\\wrong-claude");
    SetEnvironmentVariableW(L"CODEX_HOME", L"C:\\wrong-codex");
    auto none = ProfileEnvironment(L"");
    auto codex = ProfileEnvironment(ProfileId(cfg.profiles[0]));
    auto claude = ProfileEnvironment(ProfileId(cfg.profiles[1]));
    Check(environmentValue(none, L"CLAUDE_CONFIG_DIR").empty() && environmentValue(none, L"CODEX_HOME").empty(),
          "None overrides both inherited provider variables");
    Check(environmentValue(codex, L"CODEX_HOME") == cfg.profiles[0].configDir &&
          environmentValue(codex, L"CLAUDE_CONFIG_DIR").empty(), "Codex sets only its selected account folder");
    Check(environmentValue(claude, L"CLAUDE_CONFIG_DIR") == cfg.profiles[1].configDir &&
          environmentValue(claude, L"CODEX_HOME").empty(), "Claude sets only its selected account folder");
    Check(ProfileExtensionArguments(L"").find(L"anthropic.claude-code") != std::wstring::npos &&
          ProfileExtensionArguments(L"").find(L"openai.chatgpt") != std::wstring::npos, "None disables both chat extensions");
    Check(ProfileExtensionArguments(ProfileId(cfg.profiles[0])) == L" --disable-extension anthropic.claude-code" &&
          ProfileExtensionArguments(ProfileId(cfg.profiles[1])) == L" --disable-extension openai.chatgpt", "selected provider disables the other extension");
    Check(!RunVSCodeCli(L"nonexistent.cmd", L"-n", L"codex:C:\\removed-account"), "removed profile cannot fall back to another account");
    SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", oldClaude.empty() ? nullptr : oldClaude.c_str());
    SetEnvironmentVariableW(L"CODEX_HOME", oldCodex.empty() ? nullptr : oldCodex.c_str());
    DeleteFileW(path.c_str());
}

void MigrationTests(const std::wstring& dir) {
    std::string original = "thickness=7\r\n; keep this comment\r\nclaude_profile_1_name=Claude One\r\n"
        "claude_profile_1_profile_path=\r\nclaude_profile_2_name=Claude Two\r\n"
        "claude_profile_2_profile_path=C:/accounts/claude-two\r\nclaude_profile_2_icon=base64-image\r\n"
        "claude_profile_2_ring_badge=false\r\ncodex_usage_meter_enabled=true\r\n"
        "codex_profile_1_name=Codex One\r\ncodex_profile_1_profile_path=\r\ncodex_profile_1_color=172B36\r\n";
    std::string converted;
    Check(MigrateLegacyProfiles(original, converted), "legacy config migration detected");
    Config cfg;
    ReadProfileConfig(converted, cfg);
    Check(cfg.profiles.size() == 3 && cfg.profiles[0].type == ProfileType::Claude &&
          cfg.profiles[1].icon == L"base64-image" && !cfg.profiles[1].ringBadge &&
          cfg.profiles[2].type == ProfileType::Codex && cfg.profiles[2].color == RGB(0x17, 0x2B, 0x36),
          "migration preserves accounts, icons, colors and badge visibility");
    Check(!cfg.profiles[0].path.empty() && !cfg.profiles[2].path.empty() && converted.find("thickness=7\r\n") == 0 &&
          converted.find("; keep this comment") != std::string::npos, "implicit legacy defaults become explicit and unrelated settings survive");
    std::string again;
    Check(!MigrateLegacyProfiles(converted, again) && again == converted, "migration is idempotent");
    Check(!MigrateLegacyProfiles("profile_1_type=\n" + original, again), "new empty type cannot reactivate legacy accounts");
    std::string allAccounts;
    for (int i = 1; i <= 5; ++i) {
        allAccounts += "claude_profile_" + std::to_string(i) + "_profile_path=C:/accounts/claude-" + std::to_string(i) + "\n";
        allAccounts += "codex_profile_" + std::to_string(i) + "_profile_path=C:/accounts/codex-" + std::to_string(i) + "\n";
    }
    Check(MigrateLegacyProfiles(allAccounts, again), "all legacy slots convert");
    Config ten;
    ReadProfileConfig(again, ten);
    Check(ten.profiles.size() == 10 && ten.profiles.back().slot == 10, "all ten existing accounts fit without loss");
    Config bom;
    ReadProfileConfig("\xEF\xBB\xBFprofile_1_type=Codex\nprofile_1_profile_path=C:/accounts/bom\n", bom);
    Check(bom.profiles.size() == 1, "UTF-8 BOM does not disable the first profile");
    Check(MigrateLegacyProfiles("\xEF\xBB\xBF" + original.substr(original.find("claude_profile_")), again) &&
          again.rfind("\xEF\xBB\xBF", 0) == 0 &&
          again.find("claude_profile_") == std::string::npos, "legacy migration preserves BOM and removes old keys");
    Check(MigrateLegacyProfiles("claude_profile_2_profile_path=C:/accounts/two\n", again), "legacy implicit primary account migration");
    Config implicit;
    ReadProfileConfig(again, implicit);
    Check(implicit.profiles.size() == 2 && implicit.profiles[0].configDir == DefaultProviderPath(ProfileType::Claude),
          "legacy primary account retained even when its keys were omitted");
    Check(MigrateLegacyProfiles("codex_profile_1_profile_path=C:/accounts/codex\ncodex_usage_meter_enabled=false\n", again),
          "disabled Codex account still migrates");
    Config disabled;
    ReadProfileConfig(again, disabled);
    Check(disabled.profiles.size() == 2 && disabled.profiles[0].type == ProfileType::Claude &&
          !disabled.profiles[1].usageEnabled, "old implicit Claude and disabled Codex visibility both survive migration");
    Check(MigrateLegacyProfiles("codex_usage_meter_enabled=true\n", again), "legacy enablement-only file is recognized");
    Config defaults;
    ReadProfileConfig(again, defaults);
    Check(defaults.profiles.size() == 2 && defaults.profiles[0].type == ProfileType::Claude &&
          defaults.profiles[1].type == ProfileType::Codex, "legacy enablement-only file preserves both old default accounts");
    std::wstring file = dir + L"\\migration-test.ini";
    WriteFileBytes(file, original);
    auto loaded = LoadConfig(file);
    Check(loaded.profiles.size() == 3 && ReadFileBytes(file) == converted, "migration persists new config atomically");
    WIN32_FIND_DATAW found;
    HANDLE files = FindFirstFileW((file + L".pre-unified-profiles.*.bak").c_str(), &found);
    bool backedUp = false;
    if (files != INVALID_HANDLE_VALUE) {
        do {
            auto backup = dir + L"\\" + found.cFileName;
            backedUp = backedUp || ReadFileBytes(backup) == original;
            DeleteFileW(backup.c_str());
        } while (FindNextFileW(files, &found));
        FindClose(files);
    }
    Check(backedUp, "migration saves exact original bytes");
    DeleteFileW(file.c_str());
}

void WindowProfileTests(const std::wstring& dir) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BorderProfileUnitTestWindow";
    RegisterClassW(&wc);
    auto create = [&]() { return CreateWindowExW(0, wc.lpszClassName, L"profile-test", WS_OVERLAPPED, 0, 0, 1, 1,
                                               nullptr, nullptr, wc.hInstance, nullptr); };
    HWND oldWindow = create();
    auto request = NoteProfileLaunch(dir + L"\\account-project", L"codex:test-account");
    std::wstring id;
    Check(!ResolveLaunchedWindowProfile(oldWindow, dir + L"\\account-project", L"account-project", id),
          "reactivating an old window cannot change its profile");
    HWND newWindow = create();
    Check(ResolveLaunchedWindowProfile(newWindow, dir + L"\\account-project", L"account-project", id) && id == L"codex:test-account",
          "new project window gets requested profile");
    Check(ResolveLaunchedWindowProfile(newWindow, dir + L"\\another-folder", L"another-folder", id) && id == L"codex:test-account",
          "opening a different folder keeps the window's environment selection");
    Check(!ReadFileBytes(dir + L"\\window_profiles.ini").empty(), "window selection saved for app restart");
    std::wstring childCommand = L"\"" + dir + L"\\codex_usage_tests.exe\" --check-assignment " +
                                std::to_wstring((uintptr_t)newWindow) + L" \"codex:test-account\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child = {};
    bool childStarted = CreateProcessW(nullptr, &childCommand[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                       nullptr, dir.c_str(), &startup, &child) != FALSE;
    DWORD childExit = 1;
    if (childStarted) {
        if (WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &childExit);
        else TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }
    Check(childStarted && childExit == 0, "a fresh process restores the window assignment from disk");
    CancelProfileLaunch(request);
    ForgetWindowProfile(newWindow);
    DestroyWindow(newWindow);
    auto noneRequest = NoteProfileLaunch(L"", L"");
    HWND empty = create();
    Check(ResolveLaunchedWindowProfile(empty, L"", L"", id) && id.empty(), "new empty window can explicitly select None");
    ForgetWindowProfile(empty); DestroyWindow(empty);
    auto cancelled = NoteProfileLaunch(L"", L"codex:cancelled");
    CancelProfileLaunch(cancelled);
    HWND cancelledWindow = create();
    Check(!ResolveLaunchedWindowProfile(cancelledWindow, L"", L"", id), "failed launch cannot label a later window");
    CancelProfileLaunch(noneRequest);
    DestroyWindow(cancelledWindow); DestroyWindow(oldWindow);
    DeleteFileW((dir + L"\\window_profiles.ini").c_str());
}

void ClientTests(const std::wstring& dir, const std::wstring& exe) {
    std::wstring a = dir + L"\\profile-a", b = dir + L"\\profile-b";
    CreateDirectoryW(a.c_str(), nullptr);
    CreateDirectoryW(b.c_str(), nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto fetch = [&](const wchar_t* mode, const std::wstring& home, CodexUsageSnapshot& snapshot,
                     std::wstring& error, bool& transient) {
        SetEnvironmentVariableW(L"VSCODE_BORDER_USAGE_TEST_MODE", mode);
        return FetchCodexUsage(stop, exe, home, CodexUsageEnvironment(home), snapshot, error, transient);
    };
    CodexUsageSnapshot s;
    std::wstring error;
    bool transient = false;
    Check(fetch(L"success", a, s, error, transient) && s.primary.percent == 23 && s.email == L"example@example.com",
          "app-server handshake, notifications, fragmented responses, and account name");
    Check(fetch(L"success", b, s, error, transient) && s.primary.percent == 73, "each subprocess receives its own Codex home");
    for (const wchar_t* mode : {L"apikey", L"nologin", L"malformed", L"oversize", L"exit"}) {
        Check(!fetch(mode, a, s, error, transient) && !error.empty(), "unavailable states report an error");
    }
    Check(!fetch(L"error", a, s, error, transient) && transient && error.find(L"private-secret-marker") == std::wstring::npos,
          "network failures classified without exposing raw response");
    Check(!fetch(L"autherror", a, s, error, transient) && !transient &&
          error.find(L"sign in") != std::wstring::npos && error.find(L"private-secret-marker") == std::wstring::npos,
          "authentication failures are actionable and redact raw response");
    Check(!fetch(L"unsupported", a, s, error, transient) && !transient && error.find(L"update Codex") != std::wstring::npos,
          "older protocol versions have an update hint");
    Check(!fetch(L"success", a + L"\\missing", s, error, transient), "missing folder does not start Codex");
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    for (int i = 0; i < 3; i++) Check(fetch(L"success", a, s, error, transient), "repeated fetch");
    GetProcessHandleCount(GetCurrentProcess(), &after);
    Check(after <= before + 1, "subprocess and pipe handles are released");
    auto started = GetTickCount64();
    std::thread cancel([&]() { Sleep(200); SetEvent(stop); });
    Check(!fetch(L"hang", a, s, error, transient), "hung server cancelled");
    cancel.join();
    Check(GetTickCount64() - started < 2500, "cancellation does not wait for network deadline");
    CloseHandle(stop);

    // The fixture named codex.exe is found by production CLI discovery.
    // No account or native user install is touched by these worker tests.
    std::wstring fixture = dir + L"\\codex.exe";
    CopyFileW(exe.c_str(), fixture.c_str(), FALSE);
    std::wstring oldPath = Env(L"PATH");
    SetEnvironmentVariableW(L"PATH", (dir + L";" + oldPath).c_str());
    SetEnvironmentVariableW(L"VSCODE_BORDER_USAGE_TEST_MODE", L"success");
    g_config = Config();
    Profile first, second, claude;
    first.type = second.type = ProfileType::Codex;
    claude.type = ProfileType::Claude;
    first.name = L"First"; first.configDir = a; first.slot = 1;
    second.name = L"Second"; second.configDir = b; second.slot = 3;
    claude.name = L"Claude"; claude.configDir = a; claude.slot = 2;
    Profile disabled = second;
    disabled.configDir += L"-disabled";
    disabled.usageEnabled = false;
    Profile none;
    g_config.profiles = {first, claude, second, disabled, none};
    SyncCodexUsageMonitor(nullptr, WM_APP + 17);
    std::vector<CodexUsageSnapshot> snapshots;
    started = GetTickCount64();
    do {
        snapshots = GetCodexUsageSnapshots();
        if (snapshots.size() == 2 && snapshots[0].haveData && snapshots[1].haveData) break;
        Sleep(25);
    } while (GetTickCount64() - started < 7000);
    Check(snapshots.size() == 2 && snapshots[0].haveData && snapshots[0].primary.percent == 23 &&
          snapshots[1].haveData && snapshots[1].primary.percent == 73, "concurrent profile workers stay isolated");
    auto rings = BuildProfileUsageRings();
    Check(rings.size() == 3 && rings[0].profile == ProfileId(first) && rings[1].profile == ProfileId(claude) &&
          rings[2].profile == ProfileId(second), "rings interleave providers in slot order");
    if (snapshots.size() == 2) {
        Check(!IsCodexUsageUnavailable(snapshots[0]) && !IsCodexUsageStale(snapshots[0]), "fresh worker result is current");
        Check(DescribeCodexUsage(snapshots[0]).find(L"Session:") != std::wstring::npos &&
              DescribeCodexUsage(snapshots[0]).find(L"Weekly:") != std::wstring::npos, "standard Codex limits use Claude-style Session and Weekly labels");
        g_config.profiles[0].name = L"Renamed";
        SyncCodexUsageMonitor(nullptr, WM_APP + 17);
        Check(CodexUsageProfileName(snapshots[0]) == L"Renamed" && GetCodexUsageSnapshots()[0].haveData,
              "appearance reload keeps usage data");
        g_config.usageMeterRefreshMinutes = 1;
        SyncCodexUsageMonitor(nullptr, WM_APP + 17);
        Check(GetCodexUsageSnapshots()[0].haveData, "interval reload preserves previous readings");
    }
    g_config.usageMeterEnabled = false;
    SyncCodexUsageMonitor(nullptr, WM_APP + 17);
    Check(GetCodexUsageSnapshots().empty(), "master usage switch stops all Codex workers");
    Check(BuildProfileUsageRings().empty(), "master usage switch clears the shared ring list");
    StopCodexUsageMonitor();
    SetEnvironmentVariableW(L"PATH", oldPath.c_str());
    DeleteFileW(fixture.c_str());
    RemoveDirectoryW(a.c_str());
    RemoveDirectoryW(b.c_str());
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"app-server") return FixtureServer();
    if (argc == 4 && std::wstring(argv[1]) == L"--check-assignment") {
        HWND window = (HWND)(uintptr_t)wcstoull(argv[2], nullptr, 10);
        std::wstring id;
        return ResolveLaunchedWindowProfile(window, L"", L"", id) && id == argv[3] ? 0 : 1;
    }
    std::wstring dir = GetExeDir();
    ProtocolTests();
    ConfigTests(dir);
    MigrationTests(dir);
    WindowProfileTests(dir);
    ClientTests(dir, dir + L"\\codex_usage_tests.exe");
    auto icon = RenderProfileIcon(L"Example", L"", kProfileIconSize);
    Check(icon.size() == kProfileIconSize * kProfileIconSize &&
          std::any_of(icon.begin(), icon.end(), [](UINT32 pixel) { return pixel != 0; }), "shared profile badge rendering");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
