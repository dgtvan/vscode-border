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
    exit 0
}

$statusDir = Join-Path $PSScriptRoot "claude_status"

if (-not $data.session_id) { exit 0 }
$file = Join-Path $statusDir "$($data.session_id).ini"

if ($data.hook_event_name -eq "SessionEnd") {
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

# A background task (a long-running dev server, a log watcher, a
# background subagent -- anything started without blocking the main turn)
# can still be running after the main turn itself finishes. Stop/
# StopFailure/SubagentStop's payload includes a live background_tasks
# list -- confirmed empirically (a real session showed 3 running shell
# tasks in its Stop payload even though its main turn had ended) -- so
# treat that as still "working" rather than "waiting", since real work is
# still happening even though the conversation itself is idle. Doesn't
# override "attention": a pending permission/MCP prompt is more urgent
# than "something's still running in the background".
#
# This is a weaker signal than a genuinely active turn (UserPromptSubmit),
# though: it's a one-time snapshot with no equivalent "the background task
# just finished" hook to refresh it later, so it can go stale -- e.g. the
# background task finishes minutes later and nothing ever re-fires to
# notice. Marked via_background_tasks so claude_provider.cpp can apply a
# staleness bound to *this* specific reason for "working" without touching
# the real UserPromptSubmit-driven case (which can legitimately stay
# "working" for a long time and shouldn't be second-guessed by a timeout).
$viaBackgroundTasks = $false
if ($status -eq "waiting" -and $data.background_tasks -and $data.background_tasks.Count -gt 0) {
    $status = "working"
    $viaBackgroundTasks = $true
}

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
if ($viaBackgroundTasks) { $content += "via_background_tasks=1`n" }
[System.IO.File]::WriteAllText($file, $content, [System.Text.UTF8Encoding]::new($false))

exit 0
