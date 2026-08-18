# Registers vscode_border.exe to launch automatically at user logon.
#
# Mechanism: adds a value under the per-user Run key
#   HKCU\Software\Microsoft\Windows\CurrentVersion\Run
# named "VSCodeBorderApp", pointing at the built exe. This is the standard
# lightweight per-user autostart mechanism -- no admin rights, no scheduled
# task, no service. See docs/AUTOSTART.md for how to remove it manually.
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$exePath = Join-Path $root "bin\vscode_border.exe"
if (-not (Test-Path $exePath)) {
    throw "vscode_border.exe not found at $exePath. Run build.ps1 first."
}

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$valueName = "VSCodeBorderApp"
Set-ItemProperty -Path $runKey -Name $valueName -Value "`"$exePath`""

Write-Host "Registered autostart:"
Write-Host "  Key:   $runKey"
Write-Host "  Value: $valueName = `"$exePath`""
Write-Host ""
Write-Host "It will launch automatically at your next logon. To start it now:"
Write-Host "  Start-Process `"$exePath`""
Write-Host ""
Write-Host "To remove autostart later, run uninstall-autostart.ps1 (see docs/AUTOSTART.md)."
