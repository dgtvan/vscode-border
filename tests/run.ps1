# Regression checks; no login or network required. All output is temporary.
$ErrorActionPreference = 'Stop'
$repoDir = Split-Path $PSScriptRoot -Parent
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoDir 'bin'))
$testOutDir = Join-Path $buildRoot ('test-run-' + [Guid]::NewGuid().ToString('N'))
$compiler = (Get-Command g++.exe -ErrorAction Stop).Source
$sourceNames = @('config', 'codex_usage', 'codex_usage_client', 'codex_usage_protocol',
                 'file_util', 'json_scan', 'text_util', 'logger', 'layered_rendering', 'profile_icon', 'assistant_profile', 'window_profile', 'profile_config', 'usage_rings',
                 'claude_usage', 'usage_format', 'vscode_cli', 'vscode_installs', 'focus_trace', 'dpi_util')
$testSources = $sourceNames | ForEach-Object { Join-Path $repoDir "src\$_.cpp" }
$testExe = Join-Path $testOutDir 'codex_usage_tests.exe'
New-Item -ItemType Directory -Force -Path $testOutDir | Out-Null
try {
    & $compiler -O2 -std=c++17 -municode -static -static-libgcc -static-libstdc++ `
        '-Wall' '-Wextra' '-Wno-missing-field-initializers' '-Wno-cast-function-type' `
        (Join-Path $PSScriptRoot 'codex_usage_tests.cpp') @testSources `
        '-I' (Join-Path $repoDir 'src') '-o' $testExe '-lgdiplus' '-lshlwapi' '-lgdi32' '-luser32' '-lwinhttp' '-ldwmapi'
    if ($LASTEXITCODE -ne 0) { throw 'Codex usage tests failed to compile.' }
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw 'Codex usage tests failed.' }
} finally {
    $resolvedTestDir = (Resolve-Path -LiteralPath $testOutDir).Path
    if (![string]::Equals([IO.Path]::GetDirectoryName($resolvedTestDir), $buildRoot,
                         [StringComparison]::OrdinalIgnoreCase) -or
        ![IO.Path]::GetFileName($resolvedTestDir).StartsWith('test-run-')) {
        throw 'Refusing to remove a test directory outside the build folder.'
    }
    if ((Get-Item -LiteralPath $resolvedTestDir -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refusing to remove a linked test directory.'
    }
    if (Get-ChildItem -LiteralPath $resolvedTestDir -Force -Recurse |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw 'Refusing to recursively remove a test directory containing filesystem links.'
    }
    Remove-Item -LiteralPath $resolvedTestDir -Recurse -Force
}
