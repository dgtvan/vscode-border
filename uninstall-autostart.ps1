# Removes the autostart registration added by install-autostart.ps1.
# Does not stop an already-running instance; use the tray icon's Exit,
# or Stop-Process -Name vscode_border, for that.
$ErrorActionPreference = "Stop"

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$valueName = "VSCodeBorderApp"

$existing = Get-ItemProperty -Path $runKey -Name $valueName -ErrorAction SilentlyContinue
if ($null -eq $existing) {
    Write-Host "No autostart entry found ($runKey\$valueName) -- nothing to do."
} else {
    Remove-ItemProperty -Path $runKey -Name $valueName
    Write-Host "Removed autostart entry: $runKey\$valueName"
}
