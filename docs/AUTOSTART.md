# Autostart

`vscode_border.exe` can be registered to launch automatically when you log in.

## How it's configured

Running `install-autostart.ps1` adds one registry value:

| | |
|---|---|
| Key | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` |
| Value name | `VSCodeBorderApp` |
| Value data | full quoted path to `bin\vscode_border.exe` |

This is the standard per-user Windows autostart mechanism (the same list you see
under Task Manager > Startup apps). It requires no admin rights, installs no
service, and no scheduled task.

## Install

```powershell
powershell -ExecutionPolicy Bypass -File install-autostart.ps1
```

## Remove

```powershell
powershell -ExecutionPolicy Bypass -File uninstall-autostart.ps1
```

Or manually: open `regedit`, go to
`HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`, and delete
the `VSCodeBorderApp` value. You can also see/remove it from **Task Manager >
Startup apps** (it will be listed as `VSCodeBorderApp`), or via
**Settings > Apps > Startup**.

Removing the autostart entry does not stop an already-running instance --
use the tray icon's **Exit**, or `Stop-Process -Name vscode_border`, for that.
