# Codex AI status

How the [AI status indicator](../README.md#ai-status-indicator) learns what
Codex is doing, why it reads files instead of installing anything, and what
it cannot tell. Investigated against Codex `0.164.0-alpha.11.1` (the CLI
bundled with the ChatGPT VS Code extension `26.1021.22027`) on 2026-11-04.

## What Codex offers

Three ways for another app to observe a Codex session exist in that build:

| Mechanism | Reports | Cost |
| --- | --- | --- |
| Session rollout files | turn start, turn end, interrupted turn | nothing to install, read-only |
| `notify` program in `config.toml` | `agent-turn-complete`, `approval-requested`, `plan-mode-prompt`, `async-question` | writes one key into the user's `config.toml`; only one `notify` slot exists per profile |
| Hooks in `config.toml` | the same lifecycle events Claude Code's hooks do | writes into the user's `config.toml` **and** needs a manual trust step (below) |

The rollout files were chosen: they need no write into Codex's own
configuration, nothing to keep in sync with Codex's own changes, and no
step the user has to perform before the indicator works.

### Why not hooks

Codex has a hook engine whose event names and payload fields match Claude
Code's closely enough that `claude_status_hook.ps1` would almost have
worked as-is -- `PreToolUse`, `PermissionRequest`, `PostToolUse`,
`PreCompact`, `PostCompact`, `SessionStart`, `SessionEnd`,
`UserPromptSubmit`, `SubagentStart`, `SubagentStop`, `Stop`, `Interrupt`,
with `session_id`, `cwd`, `hook_event_name`, `transcript_path`,
`tool_name`, `tool_input` and `tool_use_id` in the payloads. It is on by
default (`codex features list` reports `hooks` as `stable true`) and is
configured in a profile's `config.toml`:

```toml
[[hooks.Stop]]
matcher = "*"

[[hooks.Stop.hooks]]
type = "command"
command = "echo stop"
commandWindows = "cmd /c echo stop"
timeoutSec = 10
```

Codex's own `hooks/list` (app-server) accepts exactly that and reports the
hook back as:

```json
{"eventName":"stop","handlerType":"command","command":"cmd /c echo stop",
 "source":"user","enabled":true,"isManaged":false,
 "currentHash":"sha256:90ae5c9f...","trustStatus":"untrusted"}
```

`trustStatus: "untrusted"` is the problem. A hook has to be trusted inside
Codex before it runs, the trust is recorded against a hash of the hook
(`hooks.state.<key>` with `trusted_hash`/`trusted_content_hash`), and the
command string contains this app's own install path -- so trust would have
to be given again whenever the app moves. Writing that trust entry
ourselves would mean forging the approval Codex deliberately asks for. So
an indicator built on hooks would silently show nothing until the user went
into Codex and approved it, which is exactly the failure mode worth
avoiding. Rollouts have no such gate.

## What is read

Everything lives under a Codex profile's folder (`CODEX_HOME`,
`%USERPROFILE%\.codex` by default), per profile slot in `config.ini`:

- `thread-writer-locks\<thread-id>.lock` -- one empty file per **live**
  thread, plus Codex's own `.coordination.lock` (skipped: it starts with a
  dot, and is not a thread).
- `sessions\<year>\<month>\<day>\rollout-<local-start-time>-<thread-id>.jsonl`
  -- one append-only JSONL file per thread. Its first line is the
  `session_meta` (which carries `session_id`, `cwd`, `source`, and the whole
  system prompt -- 20KB in every real local rollout), and every event since
  follows one per line.

### Liveness comes from the lock file

Measured on a real machine with three VS Code windows running Codex:

| | count |
| --- | --- |
| `codex.exe app-server` processes | 3 |
| thread lock files (excluding `.coordination.lock`) | 6 |
| rollouts whose thread had a lock | 4 |
| rollouts whose thread had no lock | 4 |

The six locks were two per running app-server -- each window's open thread
plus one never used -- and the four rollouts with no lock were all threads
that had since been closed. So a lock file exists for exactly as long as
its thread is open, and the two extra locks had no rollout at all (a thread
with no turn sent writes no file). Nothing is reported for those.

The alternative liveness test, probing whether the rollout file itself is
still held open, was rejected: Codex opens it with full sharing, so the
only way to detect the open handle is to request exclusive access
ourselves, which would briefly deny Codex its own writes. The lock file
needs no such interference.

### Status comes from the newest turn boundary

Scanning a rollout's lines newest-first for the first `event_msg` whose
payload type is one of:

| event | status |
| --- | --- |
| `task_started` | Working |
| `task_complete` | Waiting |
| `turn_aborted` | Waiting -- an interrupted turn is over, same as a finished one |
| `exec_approval_request`, `apply_patch_approval_request`, `elicitation_request`, `request_user_input` | Attention |

The approval events are recognised but have never been seen in a local
rollout: across eight real rollouts (608 `item_completed`, 63
`task_started`, 62 `task_complete`) there was not one approval event, and
there is no sign Codex persists them at all. They cost four string
comparisons and would start working on their own if a later Codex writes
them, so they are left in; **Attention is not reported today**, and an
approval prompt reads as a turn still working.

### Why a silent turn stops being Working

One real rollout ends on a completed assistant message with its
`task_started` never closed -- 21 `task_started` against 20
`task_complete` for that thread -- and stayed that way for hours.
A running turn appends to its rollout constantly (reasoning items, token
counts, one line per completed item), so a `task_started` whose file has
not been written to for over 10 minutes is reported as Waiting instead.
The cost of that rule is a turn blocked for longer than 10 minutes on a
single silent step (a long build, or the approval prompt above) reading as
Waiting.

### Subagents

A subagent's session has its own thread, lock and rollout, with the same
`cwd` as its parent, and names itself in `session_meta.source` -- either as
the string `"subagent"` or as an object, e.g.
`{"subagent":{"other":"guardian"}}`. Both shapes are skipped: the parent
turn that spawned it already shows as Working, and an abandoned subagent
session would otherwise linger as a turn that never ended.

## Cost

`LoadStatuses` runs on every hub sync, so:

- The `session_meta` line is read once per thread and cached -- it never
  changes.
- A rollout is append-only, so after the first read only the bytes added
  since are scanned. The first read looks back 256KB, then 4MB if that
  found no boundary (a long turn's `task_started` can be a long way back).
- A thread's rollout path is found once and then only checked for still
  existing. Lookups try today's and yesterday's date folders in both local
  and UTC time; the whole `sessions` tree is walked at most once per thread
  per run, for a thread resumed days after it started.
- A thread that closes is forgotten, so resuming it is read from scratch.

## Verified 2026-11-04

- `tests/run.ps1`: 106 checks, 0 failures (11 new: `session_meta` shapes,
  turn-boundary precedence, the partial-first-line guard, live/closed/
  subagent sessions, incremental appends, the silence rule, and a Claude
  profile folder never being scanned for Codex sessions).
- Against the real `%USERPROFILE%\.codex` with three Codex windows open:
  four sessions reported -- three idle windows as Waiting, and the window
  whose turn was actually running as Working. Confirmed independently from
  that thread's rollout (newest boundary `task_started` at 04:38:27Z, file
  still growing) and from the subagent and closed-thread rollouts in the
  same folder being left out. The two live threads with no rollout yet
  produced nothing.
- Not verified: Attention (Codex never writes the events), and a Codex
  thread resumed from a rollout older than yesterday (the tree-walk path).
