#pragma once
#include "config.h"
#include "profile_icon.h"

// Empty ID is None. IDs include provider and normalized folder, independent of slot/name.
std::vector<std::wstring> ConfiguredProfileIds();
const Profile* FindProfile(const std::wstring& id);
bool SameProfile(const std::wstring& a, const std::wstring& b);
std::wstring ProfileConfigDir(const std::wstring& id);
std::wstring ProfileEmail(const std::wstring& id);
std::wstring ProfileName(const std::wstring& id);
std::wstring ProfileMenuText(const std::wstring& id);
bool ShowsProfileIcon(const std::wstring& id);
void BlendProfileIcon(const std::wstring& id, UINT32* pixels, int width, int height, int left, int top, const RECT& clip);
HBITMAP GetProfileMenuBitmap(const std::wstring& id);

// App assignments win; external Claude windows can be inferred from live IDE locks.
// External Codex windows with an unknown account remain unknown.
bool ProfileOfWindow(HWND target, const std::wstring& path, const std::wstring& folderName, std::wstring& profile);
std::wstring ProfileEnvironment(const std::wstring& id);
std::wstring ProfileExtensionArguments(const std::wstring& id);
void CheckClaudeProfileFolders();

// Capture existing windows before launch; never relabel a window merely reactivated by the CLI.
ULONGLONG NoteProfileLaunch(const std::wstring& folder, const std::wstring& id);
void CancelProfileLaunch(ULONGLONG request);
bool ResolveLaunchedWindowProfile(HWND target, const std::wstring& path, const std::wstring& folderName,
                                  std::wstring& profile);
void ForgetWindowProfile(HWND target);
