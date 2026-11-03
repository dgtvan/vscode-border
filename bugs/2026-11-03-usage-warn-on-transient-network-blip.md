# A one-off network blip warned that Claude usage couldn't be read

- **Fixed:** 2026-11-03
- **Commit(s):** (this commit)
- **Area:** Claude usage (usage rings)

## Symptom

A single failed usage poll -- the kind where the machine's network drops for a few seconds -- raised a
`[WARN]` and badged the tray icon, even though the next poll five minutes later read the usage fine.
Nothing was wrong by the time the user saw the badge, and there was nothing for them to do about it.
It happened on two consecutive runs.

## Evidence

Two runs, one blip each, each recovered at the very next poll with no action taken:

```
run 20261103-082402 (08:24 -> 17:00, ~103 polls per profile):
13:11:06 [WARN] claude_usage: can't read Claude usage for [van1]: couldn't reach api.anthropic.com (WinHTTP error 12002)
13:16:06 claude_usage: [van1] readable again: session 77% (resets 13:30), weekly 38% (resets Sun 14:00)

run 20261102-081903: the same with WinHTTP error 12007
```

12002 is `ERROR_WINHTTP_TIMEOUT` and 12007 is `ERROR_WINHTTP_NAME_NOT_RESOLVED` -- the request never
reached the API. One failure in ~103 polls, so a blip rather than a broken feature; and the recovery
line on the next poll is the proof that nothing needed fixing.

## Root cause

`FetchClaudeUsageOnce` made exactly one HTTP attempt per refresh interval, and
`ProcessClaudeUsageResults` warned on the first sight of any error text. A dropped request is the one
failure class that is not an answer: unlike a missing login or a rejected token, it carries no
information and usually stops being true within seconds. The code treated it like all the others, so a
few seconds of no network cost a warning plus a tray badge that outlived the condition by hours.

## Fix

`src/claude_usage.cpp`, two layers:

- `HttpGetUsage` now wraps `HttpGetUsageOnce` and retries up to 3 times, 3 s apart, when the request
  never reached the API (`TransientWinHttpError`: timeout, name-not-resolved, cannot-connect,
  connection-error -- a TLS or proxy refusal is an answer and is not retried). The wait takes the
  worker's stop event, so quitting doesn't sit out the retries. The error text now carries the attempt
  count: `..., 3 attempt(s) over 6 seconds`.
- Unreachability no longer warns on its first poll. `FetchResult`/`ClaudeUsageSnapshot` carry
  `transientError`, `Worker` counts consecutive `failures`, and `ProcessClaudeUsageResults` warns on
  the *second* consecutive unreachable poll (`-- still unreachable a refresh later`), logging the first
  as a diagnostic that says it will try again. Every other error still warns the first time it is seen.

Turned down: warning only after N minutes (the same thing, less legible), and dropping the warning
altogether (a genuinely offline machine should still badge -- the rings go stale and that is worth
knowing).

The rings are unaffected: `snapshot.error` is set on the first failure as before, so
`IsClaudeUsageUnavailable` still shows the usage as unreadable immediately. Only the logging and the
tray badge wait.

## Verification

Built a copy of the exe with the host changed to `api.anthropic.com.invalid` and ran it from a scratch
`bin` with `verbose_logging=true` and `usage_meter_refresh_minutes=1`, so every poll fails the way a
blip does:

```
17:07:46 claude_usage: couldn't read Claude usage for [van1]: couldn't reach api.anthropic.com (WinHTTP error 12007), 3 attempt(s) over 6 seconds -- trying again at the next refresh
17:08:52 [WARN] claude_usage: can't read Claude usage for [van1]: couldn't reach api.anthropic.com (WinHTTP error 12007), 3 attempt(s) over 6 seconds -- still unreachable a refresh later
17:09:58 claude_usage: still can't read Claude usage for [van1]: ...
17:11:06 claude_usage: still can't read Claude usage for [van1]: ...
```

One warning per profile, at the second failure, and none after it -- so a blip is now silent, a real
outage still warns once, and a persistent one doesn't repeat. The retry itself ran (`3 attempt(s) over
6 seconds`).

Then rebuilt from the unmodified source and restarted the app: run `20261103-171339` fetched both
profiles' usage at startup and logs zero `[WARN]`.

Not verified: the recovery path after a warning (`readable again`, which also resets the failure
count) was not re-run -- the test instance could never reach the API. That logging is unchanged apart
from the counter reset, and the real recovery was seen in run `20261103-082402` before the fix. The
non-transient branch (expired token, missing login) was not re-exercised either; its decision is the
same `s.error != loggedError` test as before.

## Detection added

The error text now says how many attempts were made and over how long, and a warning says explicitly
that the API was still unreachable a refresh later -- so a future occurrence distinguishes a blip from
an outage without re-deriving it. The first failure of each run is still logged (as a diagnostic) so a
suppressed blip is visible in a verbose log.

## History

- 2026-11-03: first fix (this commit).
