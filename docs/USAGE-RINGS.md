# Claude usage rings: design decisions

The usage rings show each account's Claude usage limits (the session and
weekly percentages Claude Code's `/usage` shows) at the right end of the
hub's band. This page records what was chosen and what was tried and
dropped, so later changes don't go back over the same ground. How to use
and configure them is in the README's "Claude usage rings" section. The
mockup the options were compared in is kept as
[usage-rings-mockup.html](usage-rings-mockup.html); it is an archive of the
exploration, not the current design.

![This account's rings reading normally, next to another account's ring showing the pulsing amber "!" of an expired login](../assets/usage-rings.png)

## What the rings look like

- **Twin rings per account.** The outer ring is the weekly limit, the
  inner ring the session (5-hour) limit, filled clockwise from 12 o'clock.
  A chip with bars and numbers was the first pick in the mockup, and thin
  lines along the band's edge were the space-saving one. The rings won as
  the clearest at a glance in the least space.
- **Colors:** green, amber from 70%, red from 90%. Grey when the numbers
  aren't current (the last read failed, or none has worked yet).
- **Exact numbers on hover**, in a tooltip drawn by the app itself. The
  common-controls tooltip crashed the app: without a manifest the exe gets
  comctl32 5.82.

## Where they sit

- **At the right end of the hub's band, above the system tray**, in a
  window of their own, so they stay at the screen edge wherever the hub is
  slid to. The hub stops short of them. They started out right after the
  hub's "+" button. Drawing them on the taskbar itself was considered and
  dropped: the taskbar belongs to Windows, and the space after the clock is
  its Show Desktop corner.

## Telling accounts apart

- **The profile's badge in front of its rings**, the same icon its windows
  carry, on the left of the rings. A badge in the middle of the rings was
  dropped: it shrinks to about 8 px and thins the rings.
  `user_profile_<N>_ring_badge=false` hides it on the rings only.
- **An optional background color per profile** (`user_profile_<N>_color`).
  Without one the rings sit straight on the band. Default tints per
  account were tried and dropped: a color nobody chose was confusing.
- **Dropped:** a colored stripe on the chip's left edge, which read as if
  it meant something; and highlighting the account whose VS Code window
  was used last (underline, then dimming the others), which was noise.

## Profile 1 is this account

User profile 1 is always the account the app runs as, so this account
takes the same `_icon`, `_vscode` and `_color` settings as the others
instead of settings of its own. Its `_account` holds a fixed placeholder
(`<the account running VS Code Border - do not edit>`) rather than a name to
fill in. Other accounts are profiles 2-5.

## Where the numbers come from

- `GET https://api.anthropic.com/api/oauth/usage` with the account's own
  Claude Code login token from `.credentials.json`. It's undocumented, so
  every way it can fail is reported instead of guessed at.
- **The token is only read, never refreshed.** Refreshing replaces the
  refresh token and invalidates the old one, which Claude Code still holds
  in memory: its next renewal would fail and sign it out. Writing new
  tokens back into Claude Code's login file would mean editing another
  program's sign-in, with that race still open. An expired token waits for
  Claude Code to renew it.
- **The app reads again within seconds of a renewal.** After a failed read
  it checks the login file every 3 seconds and fetches again once Claude
  Code rewrites it, instead of waiting for the next scheduled check.
- **Other accounts' tokens are read by a helper running as that account**
  (this exe with `--claude-usage-helper`, started with the password saved
  in Credential Manager), since their profile folders aren't readable from
  this one. It passes results back over a pipe and exits with the app.
- **Every failure is logged as a warning**, once per distinct error.

## When an account can't be read

Two kinds of failure look different, because only one of them needs you:

| Failure | Looks like |
|---|---|
| Login expired or missing, the API rejects the token (HTTP 401), the profile's password is missing or rejected | The rings are replaced by a grey ring around a pulsing amber "!" (one fade every 1.6 s) |
| Network error, rate limit, server error, unexpected answer | Static grey, last reading kept |

The first kind won't clear until someone uses Claude Code in that account
or saves a password, and grey rings looked like 0% usage. The "!" shows no
numbers at all, so it can't be misread as a reading. Also tried: the last
reading in grey fading in and out, a dashed amber ring, and an amber arc
chasing round the ring.
