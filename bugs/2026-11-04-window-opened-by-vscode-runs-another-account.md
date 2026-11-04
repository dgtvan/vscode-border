# A window VS Code opened itself ran, and was shown as, a different assistant account

- **Fixed:** 2026-11-04
- **Commit(s):** (this commit)
- **Area:** assistant profiles

## Symptom

Opening another worktree in a new window from inside a window running one account produced a window
on a different account. Reported twice: from a Codex window (slot 3) the new window came up as the
first Claude profile, and with two Claude profiles configured, a window opened from the second one
did not run the second one either. Windows opened by the app itself (hub, Start VS Code, Reopen As)
were always right.

## Evidence

Run `20261104-131009`, with the parent window opened by the app:

```
[13:23:00] opened a new VS Code window for d:\Src\Personal\[project-a] as profile [Codex] (hud-recent-open-as)
[13:23:02] hwnd=...a00210 [[project-a]] runs as profile [Codex]
[13:25:30] hwnd=...630c5c [[project-a] - main] runs as profile [van1]
[13:27:55] hwnd=...0f0dc6 [[project-a] - main] runs as profile [van1]
```

The two later windows are the ones VS Code opened; neither was launched by the app. That the Claude
extension was live in the Codex window is in that account's own IDE lock, written 55 s after the app
had opened the window as Codex, naming the worktree the window had switched to:

```
~/.claude/ide/14313.lock  13:23:55  {"pid":20484,"workspaceFolders":["d:\\Src\\Personal\\[project-a]\\.claude\\worktrees\\[worktree]"], ...}
```

`pid` 20484 is the one VS Code process all of these windows belong to. That same process has written
locks into both configured Claude account folders, so the per-window environment does reach the
windows the app launches -- only the ones VS Code opens itself miss it.

## Root cause

Two platform behaviours, each defeating one half of the per-window isolation:

- `--disable-extension` is, per `code --help`, "not persisted and is effective only when the command
  opens a new window". Opening a folder in an existing window reloads it, so the flag is gone and
  the other assistant's extension activates again -- which is how the Claude extension came to
  register a Codex window.
- VS Code passes the launching CLI's environment only to the window it opens for that call. A window
  the workbench opens gets the VS Code process's environment, so `CLAUDE_CONFIG_DIR` /`CODEX_HOME`
  fall back to whatever the user account sets (here `%USERPROFILE%\.claude`, the first Claude slot).
  That is the account Claude Code in that window actually uses, so the indicator was reporting the
  window correctly in a sense -- the window really was on the wrong account.

On top of that, detection had nothing else to go on: `ProfileOfWindow` fell from the launch record
straight to Claude's `ide\*.lock` files, which only Claude profiles write (`RescanLocksIfDue` skips
other types), and all windows of one VS Code share a pid, so a lock left by the extension in the
default account matched any window on that folder.

## Fix

Open each account's windows with a VS Code profile of its own, and read that profile back:

- `profile_<n>_vscode_profile` (`config.h`, `profile_config.cpp`) names a VS Code profile;
  `ProfileWindowArguments` (`assistant_profile.cpp`) adds `--profile "<name>"` to the launch, and
  `RunVSCodeCli` passes it. A VS Code profile keeps its own enabled extensions and its own settings
  -- so its own `terminal.integrated.env.windows` -- survives a reload, and is inherited by windows
  VS Code opens itself. A name VS Code doesn't have is dropped with one warning, since `--profile`
  would otherwise silently create an empty profile.
- `vscode_profiles.*` (new) reads VS Code's `storage.json` -- `userDataProfiles` (location id ->
  name) and `profileAssociations.workspaces` (folder URI -> location id) -- and `ProfileOfWindow`
  consults it after the launch record and before the IDE locks. That is what identifies a window VS
  Code opened by itself, for either assistant.
- `JsonScan::FindObjectMembers`/`KeyName` were added for the association object, whose keys aren't
  known in advance; `FileUriToPath` moved to `text_util` (it was private to `worktree_resolver.cpp`).

Turned down: keeping only `--disable-extension` and fixing the display, which would have left the
window on the wrong account; and feeding live Codex threads' `cwd` into the evidence pool, which
would have covered Codex windows but not one Claude profile mistaken for another.

Not addressed, and inherent: VS Code records one profile per folder, so the same folder cannot be
open under two accounts at once -- the second open re-points it. `WarnIfVSCodeProfileDisagrees` says
so when it happens.

## Verification

- `tests/run.ps1`: 115 checks, 0 failures (9 new: URI decoding, folder -> profile resolution
  including case and trailing separator, the default profile and unknown folders resolving to
  nothing, missing/nameless state, a decoy `workspaces` object elsewhere in the file, member
  enumeration, and an unknown `_vscode_profile` name never reaching `--profile`).
- Release build clean.
- Against the real `storage.json` on the machine where this was reported (34,690 bytes): the one
  profile VS Code has there is listed, `VSCodeProfileExists` answers for a present and an absent
  name, and every folder resolves to nothing, since they are all on the default profile -- so
  detection falls back to the previous behaviour until VS Code profiles are created.
- Not verified: the end-to-end fix with real VS Code profiles in place. That needs the profiles
  created in VS Code first, which is a manual step on the user's machine. The inheritance it relies
  on was measured separately, in a VS Code instance with its own `--user-data-dir`: a folder opened
  into a profiled instance with no `--profile` flag was associated with that same profile, and both
  windows' titles showed it.

## Detection added

`claude_profile: ... is open as profile [X], but VS Code has the folder on profile [Y]` warns once
per folder when the folder's VS Code profile names a different account than the window is running
(so the next window VS Code opens for it would change account). `vscode_profiles:` logs how many
profiles and associated folders each `storage.json` yielded, as a diagnostic. `config:` now logs
each slot's `vscode_profile`.

## History

- 2026-11-04: first fix (this commit).
