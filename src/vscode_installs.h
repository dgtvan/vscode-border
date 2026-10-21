#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Where each Windows account's VS Code is installed, so a new window can be
// opened as any account (see account.h) without config.ini having to say.
// Learnt from the VS Code windows this app tracks -- every Windows VS Code
// window's process is its install's Code.exe -- and kept in
// vscode_installs.ini next to config.ini, so it's still known once every
// window of that account is closed, across restarts.
//
// Opening a window as an account tries, in order (see vscode_cli.h's
// ResolveVSCodeCliShim for this account, account.h's OpenNewVSCodeWindowAs
// for another one): a running window of that account, its remembered
// install, a search as that account (its PATH, then the usual per-user and
// machine-wide install folders), and last another account's remembered
// install that isn't inside anyone's profile folder (see
// SharedVSCodeInstalls) -- so a portable install shared by every account is
// found for an account that has never run it.

// Notes the install of process `pid` -- a tracked VS Code window's -- for
// `account` (empty for this app's own). Ignores anything that isn't a
// Windows VS Code (a WSLg window's process is WSL's msrdc.exe).
void NoteVSCodeInstall(DWORD pid, const std::wstring& account);

// `account`'s remembered Code.exe (empty for this app's own account), or
// empty.
std::wstring RememberedVSCodeInstall(const std::wstring& account);

// Drops `account`'s remembered install, e.g. once launching it failed.
void ForgetVSCodeInstall(const std::wstring& account);

// Other accounts' remembered installs (not `account`'s) that any account
// can use: outside every account's profile folder, and readable from here.
// Most recently seen first.
std::vector<std::wstring> SharedVSCodeInstalls(const std::wstring& account);

// The CLI shim of the install whose Code.exe is `exe`: bin\code.cmd, or
// bin\code-insiders.cmd for Insiders. By name -- it may be inside a profile
// folder this account can't look into.
std::wstring VSCodeShimForExe(const std::wstring& exe);

// The Code.exe of the install whose CLI shim is `shim` (see VSCodeShimForExe).
std::wstring VSCodeExeForShim(const std::wstring& shim);
