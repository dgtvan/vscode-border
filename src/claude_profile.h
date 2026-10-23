#pragma once

#include "config.h"

#include <windows.h>

#include <string>
#include <vector>

// VS Code windows can run Claude Code under different Claude profiles: each
// is a Claude Code config folder (what CLAUDE_CONFIG_DIR names) with its own
// login, settings, hooks and sessions. config.ini's Claude profile slots
// (claude_profile_<N>_*) list them. Slot 1 is the default profile -- the
// folder Claude Code uses when CLAUDE_CONFIG_DIR isn't set, %USERPROFILE%\
// .claude, unless its _profile_path names another.
//
// A window is started as a profile by running the VS Code CLI with
// CLAUDE_CONFIG_DIR set to that folder -- the default profile's too: VS
// Code lays the CLI's environment over the one VS Code itself was started
// with ({...initialUserEnv, ...userEnv} in its windowsMainService), so a
// variable left out can't undo an inherited one. Claude Code keeps a
// profile's .claude.json inside the folder whenever CLAUDE_CONFIG_DIR is
// set, so the default profile's must be there too (see
// CheckClaudeProfileFolders), not in %USERPROFILE% where Claude Code puts it
// without the variable. Which profile a
// window runs is read back from the lock files Claude Code's VS Code
// extension keeps in each config folder's ide\ (see ClaudeProfileOfWindow).
//
// A profile is identified by an id: empty for the default profile, else its
// config folder -- unlike a slot number it stays the same when slots are
// rearranged, and it's what favourites.ini stores. Each profile's windows
// show its icon in front of their label (the default profile's only when
// claude_profile_1_icon is set).

// The ids of the configured profiles other than the default, in slot order.
std::vector<std::wstring> OtherClaudeProfiles();

// The configured profile with id `id`, or null if none has it (e.g. a
// favourite's profile since dropped from config.ini).
const ClaudeProfile* FindClaudeProfile(const std::wstring& id);

// Whether ids `a` and `b` are the same profile (folder paths compare
// case-insensitively).
bool SameClaudeProfile(const std::wstring& a, const std::wstring& b);

// Profile `id`'s Claude Code config folder; for an id no longer configured,
// the id itself.
std::wstring ClaudeProfileConfigDir(const std::wstring& id);

// The account's email Claude Code signed in with in profile `id`
// (oauthAccount.emailAddress in its .claude.json), or empty.
std::wstring ClaudeProfileEmail(const std::wstring& id);

// Profile `id`'s name, for menus and hover text: its _name, else the email
// it's signed in with, else its config folder's name. For an id no longer
// configured, the id itself.
std::wstring ClaudeProfileName(const std::wstring& id);

// `id`'s name as a menu item's text: a menu takes "&" as a mnemonic marker.
std::wstring ClaudeProfileMenuText(const std::wstring& id);

// Whether profile `id`'s icon goes in front of its windows' labels (and its
// usage rings, see claude_usage.h): always for a profile other than the
// default -- its _icon, or the letter badge -- and for the default only
// when claude_profile_1_icon is set.
bool ShowsClaudeProfileIcon(const std::wstring& id);

// Size (px, square) and trailing gap of the profile icon drawn in front of a
// window's label -- on its border's label chip and its hub item alike.
const int kProfileIconSize = 14, kProfileIconGap = 4;

// Composites profile `id`'s icon (kProfileIconSize square) onto an opaque
// 32bpp surface with its top-left at (left, top), clipped to `clip` and to
// the surface. The icon is the profile's _icon (an image file, or the image
// itself as base64; any format GDI+ reads -- .png, .ico, ...), or else a
// round badge with its name's first letter.
void BlendClaudeProfileIcon(const std::wstring& id, UINT32* pixels, int surfaceWidth, int surfaceHeight, int left,
                            int top, const RECT& clip);

// Profile `id`'s icon as a 32bpp premultiplied-alpha bitmap at the system's
// small icon size, for a menu item's MENUITEMINFO::hbmpItem -- null when it
// shows none (see ShowsClaudeProfileIcon) or it can't be drawn. Kept for
// the life of the process (a menu only borrows it).
HBITMAP GetClaudeProfileMenuBitmap(const std::wstring& id);

// The profile the VS Code window with folder `path` open (empty if unknown,
// with `folderName` its leaf name) runs Claude Code as. Read from the
// ide\*.lock files Claude Code's VS Code extension writes into its config
// folder when it starts in a window -- VS Code's pid and the window's
// folders -- counting only those whose VS Code is still running, the newest
// for the folder. The profile this app opened the folder as (see
// NoteClaudeProfileLaunch) wins over an older lock, which can be one left
// by a window closed since; without either, the default. A window with no folder open is told apart only when
// every such window runs the same profile. Cheap: the lock files are
// rescanned every few seconds at most.
std::wstring ClaudeProfileOfWindow(const std::wstring& path, const std::wstring& folderName);

// Remembers that `folder` was just opened as profile `id`, for
// ClaudeProfileOfWindow until the window's own lock file says.
void NoteClaudeProfileLaunch(const std::wstring& folder, const std::wstring& id);

// This process's environment for a process run as profile `id`, as a
// CreateProcessW(CREATE_UNICODE_ENVIRONMENT) block: CLAUDE_CONFIG_DIR set to
// its config folder.
std::wstring ClaudeProfileEnvironment(const std::wstring& id);

// Warns about a profile whose folder has no .claude.json while
// %USERPROFILE%\.claude.json exists -- the default profile as Claude Code
// leaves it without CLAUDE_CONFIG_DIR: launched with the variable set,
// Claude Code would start it over from a fresh .claude.json, so it should
// be moved into the folder first. Call at startup and on Reload Config.
void CheckClaudeProfileFolders();
