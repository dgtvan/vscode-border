# A running Codex turn showed as Waiting, because a live rollout's last-write time isn't maintained

- **Fixed:** 2026-11-04
- **Commit(s):** (this commit)
- **Area:** AI status (Codex)

## Symptom

Codex was working in a window and its hub dot did not say so. Reported as "codex is running but the ai
indicator does not show that" for a worktree window whose Codex session the app otherwise knew about.

## Evidence

The window, its folder and its session all matched correctly -- a probe over the live state resolved
the window's folder to `d:\Work\example-service\.worktrees\[worktree]`, found the Codex session with
that `cwd`, and matched them. What was wrong was the status.

Measuring the seven live rollouts at `08:10:52Z` (directory entry vs. an open handle vs. the newest
`timestamp` in the file):

```
rollout A   dirent mtime=06:27:04Z   newest event=08:02:45.989Z   size 2919889 = handle 2919889
rollout B   dirent mtime=02:32:13Z   newest event=04:04:49.297Z   size  386212 = handle  386212
rollout C   dirent mtime=03:31:16Z   newest event=04:06:42.423Z   size  489630 = handle  489630
(four others: mtime equal to their newest event)
```

Rollout A's recorded last-write time was 95 minutes behind its own contents, while its recorded size
was exact.

## Root cause

Windows does not keep a file's last-write time up to date in the directory entry while a handle keeps
appending to it; it is brought up to date when the handle is flushed or closed. Codex holds its
session rollout open for the life of the thread, so for a live thread that timestamp stops moving.

`CollectSession` used exactly that timestamp for the silence rule -- a `task_started` with no write
for over 10 minutes is reported as Waiting, which exists so that a turn that never recorded finishing
doesn't stay Working forever. With a timestamp that stops moving, every turn that ran longer than ten
minutes, or whose thread had been open a while, looked silent and was downgraded to Waiting. The rule
meant to catch one dangling turn was firing on every running one.

## Fix

Silence is now measured from the newest event's own `timestamp`, which Codex writes on every line
(`ParseCodexRolloutActivity`, `IsCodexTimestampStale` in `codex_provider.cpp`), cached per thread
alongside the status so an incremental read keeps the last one seen. A timestamp that cannot be parsed
is never treated as stale, so a format change cannot silently downgrade every turn.

The file's last-write time is no longer read at all (`FileInfo` became `FileSize`). The size still
comes from the directory entry: it was exact in every case measured, directory entry and open handle
agreeing, and the incremental reads depend on it.

## Verification

- `tests/run.ps1`: 118 checks, 0 failures. Three new: a running turn stays Working when only the
  file's recorded time is stale (this bug, with the fixture's last-write time set back 90 minutes), a
  turn whose newest *event* is 30 minutes old reads as Waiting, and the staleness/parse edges (empty,
  malformed, and a timestamp in the future are never stale; the newest complete line wins and a
  partial line is ignored).
- Release build clean.
- Not verified: a live running turn showing Working on screen after this change -- that needs a Codex
  turn in flight, and no turn was running by the time the fix was built. The live rollouts all ended
  on `task_complete` and still read as Waiting, which is correct for them.

## Detection added

The downgrade log line now names the timestamp it judged: `codex_provider: <id>'s turn never reported
finishing and its newest rollout event is older than 10 minutes (<timestamp>) -- reporting it as
waiting`. It appears once per turn, so a Codex window stuck on Waiting can be checked against what
the rollout actually said.

## History

- 2026-11-04: first fix (this commit).
