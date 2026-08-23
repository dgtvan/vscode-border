# Claude Code hook script for vscode-border's project list HUD status dots.
# Not meant to be edited in bin\ -- that copy is overwritten on every build
# (see build.ps1); edit this file at the repo root instead.
#
# Configure in ~/.claude/settings.json to run on SessionStart, UserPromptSubmit,
# Stop, StopFailure, SessionEnd, PermissionRequest, Elicitation,
# ElicitationResult, and SubagentStop, pointing at this file's path in bin\
# (see README.md's "AI status indicator" section for the exact JSON).
#
# Writes one file per Claude Code session -- bin\claude_status\<session_id>.ini
# -- read by src/claude_provider.cpp. One file per session (not one shared
# file) means concurrent sessions never race on the same file.

$ErrorActionPreference = "SilentlyContinue"

# Always-on event log (not gated by config.ini's verbose_logging -- that
# setting only controls the C++ app's own LogDiag calls, and this script
# has no way to read config.ini anyway) recording every hook invocation
# this script ever sees, one line each, appended -- never truncated by this
# script. This is the ground truth for diagnosing "the indicator shows the
# wrong thing" reports: the .ini status files only ever show the *latest*
# write, so if hook events fire out of order (e.g. a straggling
# SubagentStop from a background subagent arriving after a fresh
# UserPromptSubmit and overwriting "working" back to "waiting"), the .ini
# file alone can't reveal that -- this log can, since every event is kept,
# in the order this script actually observed them. Cross-reference against
# src/claude_provider.cpp's LogDiag output (config.ini's verbose_logging)
# and the corresponding bin\claude_status\<session_id>.ini file's content.
$hookLogDir = Join-Path $PSScriptRoot "logs"
if (-not (Test-Path $hookLogDir)) { New-Item -ItemType Directory -Path $hookLogDir -Force | Out-Null }
$hookLogFile = Join-Path $hookLogDir "claude_hook_events.log"

# Cheap unbounded-growth guard, checked once per invocation before
# appending -- keeps only the most recent ~5000 lines once the file passes
# 5 MB, rather than letting it grow forever across every hook firing on
# the machine for however long this feature stays enabled.
if ((Test-Path $hookLogFile) -and (Get-Item $hookLogFile).Length -gt 5MB) {
    $tail = Get-Content $hookLogFile -Tail 5000
    [System.IO.File]::WriteAllLines($hookLogFile, $tail, [System.Text.UTF8Encoding]::new($false))
}

function Write-HookEventLog {
    param([string]$Line)
    $stamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
    $full = "[$stamp] $Line`n"
    [System.IO.File]::AppendAllText($hookLogFile, $full, [System.Text.UTF8Encoding]::new($false))
}

# Read stdin as UTF-8 explicitly -- Windows PowerShell 5.1's default
# console input encoding isn't reliably UTF-8, which could otherwise mangle
# non-ASCII paths before ConvertFrom-Json ever sees them.
$stdin = [Console]::OpenStandardInput()
$reader = New-Object System.IO.StreamReader($stdin, [System.Text.Encoding]::UTF8)
$json = $reader.ReadToEnd()
if (-not $json) { exit 0 }

try {
    $data = $json | ConvertFrom-Json
} catch {
    Write-HookEventLog "PARSE_ERROR raw=$($json.Substring(0, [Math]::Min(200, $json.Length)))"
    exit 0
}

$statusDir = Join-Path $PSScriptRoot "claude_status"

if (-not $data.session_id) {
    Write-HookEventLog "event=$($data.hook_event_name) session=MISSING"
    exit 0
}
$file = Join-Path $statusDir "$($data.session_id).ini"

if ($data.hook_event_name -eq "SessionEnd") {
    Write-HookEventLog "event=SessionEnd session=$($data.session_id) (status file removed)"
    Remove-Item -Path $file -Force -ErrorAction SilentlyContinue
    exit 0
}

$status = switch ($data.hook_event_name) {
    "UserPromptSubmit" { "working" }
    "ElicitationResult" { "working" } # MCP prompt answered -- resuming
    "PermissionRequest" { "attention" } # blocked: waiting on an Allow/Deny decision
    "Elicitation" { "attention" }       # blocked: an MCP server is asking the user something
    default { "waiting" } # SessionStart, Stop, StopFailure, SubagentStop
}

# Stop/StopFailure/SubagentStop's payload includes a live background_tasks
# list of anything still running in the background (a dev server, a log
# watcher, a background subagent). This was tried as a "working" signal --
# treating a non-empty list as still "working" rather than "waiting" -- but
# real captured data (see bgDetail below and bin\logs\claude_hook_events.log)
# showed that doesn't work: a background_tasks entry has no way to
# distinguish "about to finish any second" from "a server that was started
# once and will just sit there running for the rest of the session" -- both
# report the same live "running" status on every single check, forever, in
# the latter case. No staleness window can fix that (a long window pins
# "working" for the server's entire lifetime; a short one just flickers
# "working" for a few seconds after every Stop with the conversational turn
# already over, which isn't informative either way) -- so this is
# deliberately *not* factored into $status at all. Still logged below
# (bgCount/bgDetail) purely for visibility into what's running, in case a
# more targeted signal becomes possible later.
if (-not (Test-Path $statusDir)) {
    New-Item -ItemType Directory -Path $statusDir -Force | Out-Null
}

# Walks up this hook process's own ancestry (self -> parent -> grandparent
# -> ...) looking for the real, long-running Claude Code CLI process
# (claude.exe) -- empirically confirmed stable across separate hook
# invocations of the same session (same pid every time, several minutes
# apart). Bounded to a handful of hops rather than assuming an exact
# depth, since the shell wrapping in between (Git Bash, typically two
# nested bash.exe hops) isn't a documented, guaranteed shape. Recorded so
# vscode-border can later confirm -- with a fresh, live lookup, not by
# trusting this snapshot -- whether that process is still actually
# running, closing the "terminal force-closed, SessionEnd never fired"
# gap. Left blank (see claude_provider.cpp's handling) if not found.
function Find-ClaudeAncestorPid {
    param([int]$StartPid, [int]$MaxHops = 6)
    $currentPid = $StartPid
    for ($i = 0; $i -lt $MaxHops; $i++) {
        $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$currentPid" -ErrorAction SilentlyContinue
        if (-not $proc) { return $null }
        if ($proc.Name -ieq "claude.exe") { return $proc.ProcessId }
        if (-not $proc.ParentProcessId) { return $null }
        $currentPid = $proc.ParentProcessId
    }
    return $null
}
$claudePid = Find-ClaudeAncestorPid -StartPid $PID

$content = "status=$status`ncwd=$($data.cwd)`n"
if ($claudePid) { $content += "pid=$claudePid`n" }
[System.IO.File]::WriteAllText($file, $content, [System.Text.UTF8Encoding]::new($false))

$bgCount = if ($data.background_tasks) { $data.background_tasks.Count } else { 0 }
$bgDetail = ""
if ($bgCount -gt 0) {
    $parts = $data.background_tasks | ForEach-Object { "$($_.id):$($_.status):$($_.description)" }
    $bgDetail = " bgDetail=[" + ($parts -join "; ") + "]"
}
Write-HookEventLog "event=$($data.hook_event_name) session=$($data.session_id) status=$status pid=$claudePid cwd=$($data.cwd)$bgDetail"

exit 0
