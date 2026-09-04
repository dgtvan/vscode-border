# Investigating warning logs

A reusable prompt/checklist for "the app logged a warning, look into it".
Written for this repo specifically -- the paths, prefixes and file formats
below are this app's, not generic advice.

## Prompt to reuse

> The app logged some warnings. Investigate and fix them, following
> `docs/INVESTIGATING-WARNINGS.md`. When you're finished the app must be
> running with no warnings at all -- restart it and show me the log to
> prove it.

## Definition of done

**The app runs and logs zero `[WARN]` lines. Nothing less counts.**

Not "the cause is understood", not "a fix is deployed", not "the remaining
warnings are benign" -- those are all mid-task states. If a warning is
still firing, the issue is still open: go back to step 1 with the new
warning as the input and keep going. Treat a leftover warning as a fresh
investigation, not as a footnote on a finished one.

Two specific traps, both of which have actually happened here:

- **Restarting alone does not clear the badge.** `HasWarnings()` resets on
  restart, but if whatever produced the warning is still on disk it re-fires
  within a second and the badge comes straight back. You must remove the
  *cause*, then restart -- in that order.
- **The cause may be state you didn't create.** A warning about a stale file
  in `bin\claude_status\` is not fixed by correcting the code that will
  write the *next* one. The already-stale file is its own separate cleanup,
  and until it is gone the app keeps warning about it.

Reporting a warning as "outstanding" or "self-clearing in 24 hours" is not
finishing. If something genuinely blocks you from clearing it -- a denied
permission, a decision that is the user's to make -- say so explicitly as a
blocker and ask, rather than delivering a still-warning app as though it
were done.

## Background you need first

- Warnings come from `LogWarn` (`src/logger.cpp`) and are always written,
  regardless of `verbose_logging`. They are prefixed **`[WARN]`** -- that
  prefix is the thing to grep for, not the words "warn"/"error", which
  appear all over ordinary diagnostic output.
- `LogWarn` also flips `HasWarnings()` on, which is what puts the badge on
  the tray icon. So "the tray icon has a warning badge" and "there is a
  `[WARN]` line in this run's log" are the same event.
- Logs live in `bin\logs\`, named
  `vscode_border_<session-start-timestamp>_<part>.log`. One run produces
  many parts (rotation at ~5 MB); the timestamp is shared across all parts
  of a run, the trailing number increments. **The timestamp is the run, not
  the file.**
- With `verbose_logging=true` a run generates megabytes per minute of
  `LogDiag` noise. Never read these files whole -- always filter first.
- `bin\logs\claude_hook_events.log` is separate: it's written by
  `claude_status_hook.ps1` (PowerShell, not the C++ app), append-only
  across all runs, one line per Claude Code hook invocation ever seen. It
  is the ground truth for anything about session status dots.

## Steps

### 1. Find the warnings

```bash
cd bin/logs
# every distinct warning across every run, with counts
grep -h "\[WARN\]" *.log | sed 's/^\[[0-9:]*\] //' | sort | uniq -c | sort -rn
# which run(s) they came from
grep -l "\[WARN\]" *.log
```

Deduplicating matters: a lot of warnings are rate-limited to once per
subject (e.g. `warnedMissingPid` in `claude_provider.cpp`), so a count of 1
does not mean it happened once -- it means it was *reported* once.

### 2. Establish scope before theorising

Get the ratio, not just the occurrence. One failure out of two is a broken
feature; one out of thirty is a race or a transient. For a warning about a
per-session/per-window subject, count the successes too:

```bash
# e.g. how many sessions recorded a pid vs. didn't
grep "event=SessionStart" claude_hook_events.log | grep -c "pid=[0-9]"
grep "event=SessionStart" claude_hook_events.log | grep -c "pid= "
```

Then look at *when*. Clustering in time (several sessions starting within
a couple of seconds) points at contention/races; an even spread points at
a logic bug.

### 3. Read the warning's own source comment

Find the `LogWarn` call:

```bash
grep -rn "LogWarn" --include=*.cpp src/
```

This codebase documents *why* each warning exists and what the fallback is,
right at the call site. Read that before deciding anything is broken --
several warnings describe a condition the app already degrades gracefully
around, where the real defect is upstream of the C++ entirely (in the hook
script, in a config file, in VS Code's own state).

### 4. Inspect the actual state the warning is about

Warnings here are almost always about a file the app read. Go look at it
and compare a bad one against a good one:

- session status: `bin\claude_status\<session-id>.ini`
- config: `bin\config.ini`
- favourites / aliases / ordering: `bin\*.ini`
- hook history for one session:
  `grep "<session-id-prefix>" bin/logs/claude_hook_events.log`

### 5. Reproduce the mechanism, don't guess at it

Prefer a real measurement over a plausible story. Examples that worked:

- walking a live process ancestry with `Get-CimInstance Win32_Process` to
  check whether a hop limit was actually the problem (it wasn't -- the real
  depth was 1);
- running `claude_status_hook.ps1` end-to-end from a scratch directory with
  a JSON payload on **redirected stdin** (the script reads
  `[Console]::OpenStandardInput()`, so a PowerShell pipe into it will hang
  -- use `-File script.ps1 < payload.json`, or `Start-Process
  -RedirectStandardInput`).

Copy the script to a scratch dir before testing it: it writes
`claude_status\` and `logs\` relative to `$PSScriptRoot`, so running the
repo copy in place pollutes real state.

### 6. Fix at the right layer

If the C++ already handles the condition (falls back, retries, cleans up
later), the fix usually belongs in whatever produced the bad state, not in
`claude_provider.cpp`. Two things worth adding whenever a lookup can fail:

- **a retry**, if the failure is transient rather than an answer; and
- **a recorded reason**, so the next occurrence says *why* it failed
  instead of leaving a blank field for someone to re-derive from scratch.

### 7. Verify, then deploy

- PowerShell syntax check:
  `[System.Management.Automation.PSParser]::Tokenize((Get-Content -Raw x.ps1), [ref]$errors)`
- Exercise every branch you touched (success, failure, fallback).
- `claude_status_hook.ps1` lives at the repo root; `bin\` holds a copy that
  `build.ps1` overwrites on every build and that `~/.claude/settings.json`
  actually points at. **Edit the root copy, then copy it into `bin\`** (or
  rebuild) -- editing only the root copy changes nothing at runtime.
- C++ changes need `powershell -ExecutionPolicy Bypass -File build.ps1`.
  Script-only changes do not.

### 8. Expect the first diagnosis to be wrong

The point of step 5 is that a plausible story is not a cause. A worked
example from this repo: a blank `pid=` was first attributed to WMI queries
failing under contention, because the one failure sat inside a burst of
sessions starting at once. Adding the *reason* to the log then showed the
truth on the very next occurrence -- `pid N not found at hop 2`, a query
succeeding and returning empty, i.e. a severed process chain, not a failed
query. The tell was in the split: all 4 failures ever were `SessionStart`,
none of the 1,176 other hook events. **Group failures by category and look
for the one that is 100% correlated** -- that is usually the mechanism.

Adding a diagnostic is therefore a legitimate first fix on its own, even
when you think you already know the answer.

### 9. Restart and confirm zero warnings -- this is the gate

Not optional, and not satisfied by reasoning that it should now be clean.
Close the app via its own window so the tray icon is removed properly
rather than left as a ghost, restart it, let it run a little, then check:

```powershell
# 1. graceful close -- WM_CLOSE to the app's own message window, so it runs
#    CleanupAndQuit and removes its tray icon (a hard Stop-Process leaves a
#    ghost icon behind until something makes Explorer repaint).
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@
$old = Get-Process vscode_border -ErrorAction SilentlyContinue
$h = [W]::FindWindow("VSCodeBorderAppWndClass", "VSCodeBorderApp")
if ($h -ne [IntPtr]::Zero) { $null = [W]::SendMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }
if ($old) { $old.WaitForExit(10000) | Out-Null }

# 2. restart -- the single-instance mutex means the old one must be gone first
Start-Process bin\vscode_border.exe -WorkingDirectory bin
Start-Sleep -Seconds 20

# 3. check the CURRENT run only
$newest = Get-ChildItem bin\logs\vscode_border_*.log |
    Sort-Object LastWriteTime | Select-Object -Last 1
$sid = $newest.Name -replace '^vscode_border_(.+)_\d+\.log$','$1'
$files = (Get-ChildItem "bin\logs\vscode_border_${sid}_*.log").FullName
"lines: $((Get-Content $files | Measure-Object -Line).Lines)"
Select-String -Path $files -SimpleMatch "[WARN]"
```

Scope the check to the **current run's session id** -- old runs' logs keep
their warnings forever and will mislead you either way.

Give it enough lines to be meaningful before declaring victory: the status
sync runs within seconds of startup, so a few thousand lines with no
`[WARN]` is real evidence, while checking one second after launch is not.

**Any hit means you are not done.** Read it, and go back to step 1 -- it may
be a different warning than the one you started on, in which case it gets
the same treatment from scratch.

### 10. Report honestly

Say how many warnings there were, how often relative to successes, and
whether the fix addresses the cause or only the symptom. "One transient
failure in thirty, now retried and diagnosable" is a more useful report
than "fixed the warning".

Then state the verified end state plainly -- app running, current run's log,
zero `[WARN]` -- or say clearly that it is still warning and why. Do not
describe work as complete while a warning is still firing.
