# Claude usage rings: design decisions

The usage rings show each Claude profile's usage limits (the session and
weekly percentages Claude Code's `/usage` shows) at the right end of the
hub's band. This page records what was chosen and what was tried and
dropped, so later changes don't go back over the same ground. How to use
and configure them is in the README's "Claude usage rings" section. The
mockup the options were compared in is kept as
[usage-rings-mockup.html](usage-rings-mockup.html); it is an archive of the
exploration, not the current design.

![The default profile's rings reading normally, next to another profile's ring showing the pulsing amber "!" of unavailable usage](../assets/usage-rings.png)

## What the rings look like

- **Twin rings per profile.** The outer ring is the weekly limit, the
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

## Telling profiles apart

- **The profile's badge in front of its rings**, the same icon its windows
  carry, on the left of the rings. A badge in the middle of the rings was
  dropped: it shrinks to about 8 px and thins the rings.
  `claude_profile_<N>_ring_badge=false` hides it on the rings only.
- **An optional background color per profile** (`claude_profile_<N>_color`).
  Without one the rings sit straight on the band. Default tints per
  profile were tried and dropped: a color nobody chose was confusing.
- **Dropped:** a colored stripe on the chip's left edge, which read as if
  it meant something; and highlighting the profile whose VS Code window
  was used last (underline, then dimming the others), which was noise.

## Profile 1 is the default

Claude profile 1 is always the default one (the config folder Claude Code
uses without `CLAUDE_CONFIG_DIR`, unless its `_profile_path` names
another), so it takes the same `_icon`, `_color` and `_ring_badge` settings
as the others instead of settings of its own. Other profiles are 2-5.

Profiles used to be Windows accounts, each read by a helper process started
as that account with its password saved in Credential Manager. Claude
profiles in the one account replaced that: every profile's files are
readable directly, so there's no helper and no password.

## Where the numbers come from

- `GET https://api.anthropic.com/api/oauth/usage` with the profile's own
  Claude Code login token from `.credentials.json`. It's undocumented, so
  every way it can fail is reported instead of guessed at.
- **The token is only read, never refreshed.** Refreshing replaces the
  refresh token and invalidates the old one, which Claude Code still holds
  in memory: its next renewal would fail and sign it out. Writing new
  tokens back into Claude Code's login file would mean editing another
  program's sign-in, with that race still open.
- **An expired token is renewed by Claude Code itself.** The app runs
  Claude Code (on PATH, in `~/.local/bin`, or the newest one in the VS Code
  extensions) as that profile, with `CLAUDE_CONFIG_DIR` set to its folder,
  as `claude -p` with nothing on stdin: it renews the
  login as it starts, then stops with "Input must be provided" before any
  model call, so it uses no quota. Tested 2026-10-22 with Claude Code
  2.1.304 on an account expired for 7 hours; `claude auth status` and
  `claude mcp list` didn't renew. It's tried once per expired token, so a
  renewal that doesn't work isn't repeated every check; the log says what
  happened either way.
- **The app reads again within seconds of a renewal.** After a failed read
  it checks the login file every 3 seconds and fetches again once Claude
  Code rewrites it, instead of waiting for the next scheduled check.
- **Every failure is logged as a warning**, once per distinct error.

## When a profile's usage is unavailable

The pulsing "!" means one thing: there are no current numbers for that
profile, whatever the reason -- an expired or missing login, a network or
API error, an unexpected answer, or no
successful read for three refresh intervals. The rings are replaced by a
grey ring around a pulsing amber "!" (one fade every 1.6 s); the tooltip
says why. Only the first read after startup, still on its way, shows plain
grey rings.

Grey rings with the last reading looked like 0% usage, and splitting
failures into "needs you" and "passing" (static grey for network errors)
was tried and dropped: either way the numbers on screen aren't current,
and that's what matters. The "!" shows no numbers at all, so it can't be
misread as a reading. Also tried: the last reading in grey fading in and
out, a dashed amber ring, and an amber arc chasing round the ring.
