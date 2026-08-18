# Builds vscode_border.exe using the WinLibs MinGW-w64 g++ toolchain.
$ErrorActionPreference = "Stop"

$gxx = (Get-Command g++.exe -ErrorAction SilentlyContinue).Source
if (-not $gxx) {
    $gxx = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter "g++.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $gxx) {
    throw "g++.exe not found on PATH or under WinGet packages. Install a MinGW-w64 toolchain, e.g. via 'winget install BrechtSanders.WinLibs.POSIX.UCRT'."
}
$toolsDir = Split-Path $gxx -Parent
$windres = Join-Path $toolsDir "windres.exe"
if (-not (Test-Path $windres)) {
    throw "windres.exe not found next to g++.exe at $toolsDir."
}

$root = $PSScriptRoot
$srcDir = Join-Path $root "src"
$sources = Get-ChildItem -Path $srcDir -Filter "*.cpp" | ForEach-Object { $_.FullName }
$rc = Join-Path $srcDir "resource.rc"
$outDir = Join-Path $root "bin"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outExe = Join-Path $outDir "vscode_border.exe"
$resObj = Join-Path $outDir "resource.res.o"

& $windres $rc -O coff -o $resObj
if ($LASTEXITCODE -ne 0) {
    throw "Resource compile failed."
}

& $gxx -O2 -municode -mwindows -std=c++17 -static -static-libgcc -static-libstdc++ `
    @sources $resObj -o $outExe -ldwmapi -lshell32 -lpsapi -luser32 -lgdi32
if ($LASTEXITCODE -ne 0) {
    throw "Build failed."
}

$configTemplate = Join-Path $root "config.ini"
$configOut = Join-Path $outDir "config.ini"
if (-not (Test-Path $configOut)) {
    Copy-Item $configTemplate $configOut
    Write-Host "Copied default config.ini to bin\ (edit that copy; it won't be overwritten by future builds)"
}

Write-Host "Built: $outExe"
