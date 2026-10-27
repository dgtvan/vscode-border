# vscode-border

## Bug records (`bugs/`)

Every bug fix gets a record in `bugs/`, so it's possible to trace back later what went wrong, how it was
found, and why the fix is what it is. Only bug fixes go there.

**What counts as a bug:** the app did something it was already meant not to do -- wrong position, wrong
status, a crash, a leak, a warning that shouldn't fire, a regression. **What doesn't:** new features,
changed behaviour the user asked for, refactors, performance work with no wrong behaviour, docs, build or
config changes. If it's not clear which it is, ask before writing a record. Don't write records for
those other changes, even when they are committed in the same session as a bug fix.

**Rules:**
- One file per bug: `bugs/YYYY-MM-DD-<short-slug>.md`. The date is the day the fix was made (UTC). Follow
  `bugs/TEMPLATE.md`.
- Write the record in the same commit as the fix, never in a later "docs" commit. A commit with a `fix:`
  prefix must add or update a record. A commit that adds a record must start with `fix:`.
- If a later change turns out to fix the same bug better, or the first fix was wrong, update that bug's
  existing record (add to its History section). Don't start a new file.
- Add one line for the bug to the index in `bugs/README.md`, newest first.
- Record evidence as facts: the log lines, snapshot numbers or reproduction steps that showed the cause.
  Keep only the parts that prove the point.
- Say plainly what was and wasn't verified (e.g. "not re-tested on a 150% monitor").
- This repo is public. Records follow `D:\Src\Personal\CLAUDE.md`: logs and snapshots hold real window
  titles, folder, repo and branch names, so replace them with placeholders (`[project-a]`,
  `D:\Work\example-service`) before quoting.
