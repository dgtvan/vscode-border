#pragma once

#include <windows.h>

#include <string>

// Where VS Code is installed, so a new window can still be opened once every
// VS Code window is closed (see vscode_cli.h's ResolveVSCodeCliShim) without
// config.ini having to say. Learnt from the VS Code windows this app tracks
// -- every Windows VS Code window's process is its install's Code.exe -- and
// kept in vscode_installs.ini next to config.ini, across restarts.

// Notes the install of process `pid` -- a tracked VS Code window's. Ignores
// anything that isn't a Windows VS Code (a WSLg window's process is WSL's
// msrdc.exe).
void NoteVSCodeInstall(DWORD pid);

// The remembered Code.exe, or empty.
std::wstring RememberedVSCodeInstall();
