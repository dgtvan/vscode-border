#pragma once

// Keeps VS Code's global settings.json "window.title" in sync with the
// "show_label" config option, so getting the repo/branch label (instead of
// just the folder name) needs zero manual settings.json editing -- see
// README.md "Getting the repo/branch label instead of just the folder
// name".
//
// Call with the current show_label value on every config load (startup and
// Reload Config). Idempotent and safe to call even when nothing changed:
//
// - enable=true:  if window.title isn't already exactly the value this app
//   wants, its current value (or absence) is remembered and it's set to
//   that value. If it's already exactly that value, it's left untouched
//   (covers both "we set it last time" and "the user happened to configure
//   the same thing manually" -- either way there's nothing to do).
// - enable=false: if this app remembers setting it, and the file still has
//   exactly the value this app wrote (i.e. nothing else changed it in the
//   meantime), it's restored to the remembered original value, or removed
//   entirely if the key didn't exist before. If this app never set it, or
//   something else changed it since, nothing is touched.
//
// Applies to both VS Code stable and Insiders' settings.json, whichever are
// installed (checked via %APPDATA%\<variant>\User). Remembered
// original-value state is stored in window_title_state.ini next to
// config.ini.
void SyncWindowTitleSetting(bool enableLabel);
