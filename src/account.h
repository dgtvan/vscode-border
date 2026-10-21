#pragma once

#include <windows.h>

#include <string>
#include <vector>

// VS Code can also run as another Windows account on this desktop (a "user
// profile": runas, or this app's own new-window menu). Such a window is an
// ordinary Code.exe window here -- same class, same title shape -- so the
// only thing that tells it apart is who its process runs as. It gets that
// account's icon in front of its label, the way a WSLg window gets the WSL
// icon (see wsl.h).
//
// The accounts offered for new windows are config.ini's user profile slots
// (user_profile_<N>_account), each with an optional icon
// (user_profile_<N>_icon) and VS Code install (user_profile_<N>_vscode). A
// window from an account that isn't listed still gets an icon: the letter
// badge.

// The account name (e.g. "second") process `pid` runs as, if that isn't the
// account this app runs as; empty for this app's own account, or if the
// owner can't be read.
std::wstring OtherAccountOfProcess(DWORD pid);

// The account this app runs as.
std::wstring CurrentAccountName();

// The other accounts of config.ini's user profiles -- slot 2 on, in slot
// order (slot 1 is this app's own account).
std::vector<std::wstring> ConfiguredUserProfiles();

// The account whose icon goes in front of a window's label (or a usage ring
// pair, see claude_usage.h), given the account it runs as (empty for this
// app's own): another account always has one -- its profile's icon, or the
// letter badge -- and this account only when user_profile_1_icon is set.
// Empty for none.
std::wstring IconAccountFor(const std::wstring& account);

// Size (px, square) and trailing gap of the account icon drawn in front of
// another account's window's label -- on its border's label chip and its hub
// item alike.
const int kAccountIconSize = 14, kAccountIconGap = 4;

// Composites `account`'s icon (kAccountIconSize square) onto an opaque 32bpp
// surface with its top-left at (left, top), clipped to `clip` and to the
// surface. The icon is the account's profile icon (user_profile_<N>_icon:
// an image file, or the image itself as base64; any format GDI+ reads --
// .png, .ico, ...), or else a round badge with the account name's first
// letter.
void BlendAccountIcon(const std::wstring& account, UINT32* pixels, int surfaceWidth, int surfaceHeight, int left,
                      int top, const RECT& clip);

// `account`'s icon as a 32bpp premultiplied-alpha bitmap at the system's
// small icon size, for a menu item's MENUITEMINFO::hbmpItem. Kept for the
// life of the process (a menu only borrows it); null if it can't be drawn.
HBITMAP GetAccountIconMenuBitmap(const std::wstring& account);

// `account` as a menu item's text: a menu takes "&" as a mnemonic marker,
// and an account name can contain a literal one.
std::wstring AccountMenuText(const std::wstring& account);

// Opens a new VS Code window (with `folder` open, if not empty) as
// `account`, through that account's VS Code CLI shim. Which VS Code is
// found by itself (see vscode_installs.h): its remembered install, else a
// search as that account, else another account's install every account can
// use. Starting a process as another account takes its password: the one
// saved in Windows Credential Manager by an earlier launch, or else asked
// for in a dialog owned by `owner` (which offers to save it). A saved
// password that no longer works is dropped and asked for again. `reason`
// tags the focus-trace launch record.
void OpenNewVSCodeWindowAs(HWND owner, const std::wstring& account, const std::wstring& folder,
                           const wchar_t* reason);

// Starts `application` with `commandLine` as `account`, hidden, with its
// profile loaded and stdin/stdout/stderr on `stdIn`/`stdOut`, using only the
// password saved in Credential Manager -- it never asks for one. Returns the
// process handle (the caller closes it), or null with `error` set:
// ERROR_NOT_FOUND when no password is saved, ERROR_LOGON_FAILURE when the
// saved one is wrong (left saved -- the caller decides), else the Win32
// error. Doesn't log, so it's safe off the main thread.
HANDLE StartHiddenProcessAsAccount(const std::wstring& account, const std::wstring& application,
                                   std::wstring commandLine, const std::wstring& workingDir, HANDLE stdIn,
                                   HANDLE stdOut, DWORD& error);

// Why starting a process as another account failed with Win32 error `err`
// (from OpenNewVSCodeWindowAs or StartHiddenProcessAsAccount), in plain
// words with what to do about it where that's known -- e.g. for
// ERROR_PASSWORD_MUST_CHANGE -- else Windows' own message, ending with
// "(error N)". A lower-case clause (bar a leading "Windows"), to follow a
// colon. Doesn't log, so it's safe off the main thread.
std::wstring StartAsErrorText(DWORD err);

// Command-line switch that runs the exe as a one-shot search for VS Code,
// run as another account by OpenNewVSCodeWindowAs (see RunFindVSCode).
extern const wchar_t* const kFindVSCodeArg;

// Entry point of that search: writes the Code.exe of the VS Code install
// found for the account this runs as (see vscode_cli.h's
// FindInstalledVSCodeCli) to stdout, one line; exit code 1 if none. Never
// logs -- it runs as another account.
int RunFindVSCode();

// Whether a password for `account` is saved, and dropping it.
bool HasSavedAccountPassword(const std::wstring& account);
void ForgetSavedAccountPassword(const std::wstring& account);
