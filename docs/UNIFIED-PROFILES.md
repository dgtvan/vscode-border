# Unified assistant profiles

## Contract

- Ten `profile_<N>_*` slots replace five Claude and five Codex slots.
- A slot has `type` (`Claude`, `Codex`, or `None`, case-insensitive),
  `name`, `profile_path`, `icon`, `color`, `ring_badge`, and `usage_enabled`.
- Empty type, `None`, or empty path means no assistant. There is no implicit
  default account in the new format. Relative/invalid paths and unknown types
  warn and cannot fall back to an account.
- Launch menus contain configured accounts in slot order and show their
  names without provider prefixes. None is not offered as a menu entry.
  None has no usage ring. With no accounts configured, ordinary open/start
  actions launch a window without an assistant.
- Identity is provider plus normalized folder, independent of slot/name.
- Launching sets the selected provider's folder and disables the other
  provider's VS Code extension; None disables both. Other extensions remain
  enabled. Terminal CLIs remain independently available.
- Reopening changes an existing window's selection after its normal close
  flow. Simply activating an already-open project does not change its account.
- Profile badges reflect app launch records or Claude's live IDE locks.
  Unknown external windows are not claimed to use a configured account.

## Migration

Only a file with legacy keys and no new profile keys is migrated. Save an
exact backup before replacing the profile sections; retain unrelated config
and comments. Preserve legacy default accounts by writing explicit paths,
Claude profiles first, then configured Codex profiles. Preserve disabled
Codex usage through the shared per-profile `usage_enabled` setting. Never
write credentials, alter provider account files, or change favourites.

## Implementation and verification plan

1. Introduce a shared profile model, parser, normalizer, and backed-up migration.
2. Generalize naming, icons, environments, menus, account detection and durable
   per-window selection. Keep provider-specific usage/auth and Claude hooks.
3. Use a single slot-ordered ring list; make None/Codex windows ignore Claude
   status hooks. Remove separate Codex enablement and default-Claude behavior.
4. Cover migration, empty/invalid configuration, stable IDs, environments,
   launch arguments, ring ordering, reload, and per-window persistence in tests.
5. Run real VS Code windows with a temporary local probe extension to verify
   selected folders and enabled/disabled assistant extensions. Close only probe
   windows. No model prompts are submitted.
6. Build, migrate the local config with backup, gracefully restart the app,
   and verify live account queries and the on-screen usage strip.

The acceptance check is all three launch modes working without touching
existing editor windows, with the existing accounts and ring appearance
preserved after migration and restart.

## Verified 2026-11-04

- Release build completed with `build.ps1 -OutputName vscode_border.unified.exe`.
- `tests/run.ps1`: 95 checks passed, including all ten legacy slots, empty
  and invalid config, UTF-8 headers, enablement-only legacy files, exact
  backups, stable IDs, environments, launch flags, mixed-provider rings,
  worker cancellation and restoring window assignments in a fresh process.
- Real VS Code test windows passed for None, Claude and Codex: the probe
  confirmed the selected account folder and which assistant extensions were
  enabled. Test windows were closed; existing editor windows stayed open.
- The live configuration retained two Claude accounts and one Codex account.
  Names, image data, colors, badge settings and unrelated values matched the
  original. The migration backup matched the pre-deployment config exactly.
- The installed executable matched the final build. All three live usage
  queries succeeded with no startup warnings. The three-ring strip was
  inspected on screen and retained the configured appearance.

The one-time desktop probe scripts, workspaces, screenshots and deployment
helpers were removed after verification. Recovery copies are archived in
`bin/backups`. The reusable regression suite now builds in a unique temporary
directory and removes its output in a `finally` block.
