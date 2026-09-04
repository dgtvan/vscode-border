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
#
# Each hop is a WMI query, which can transiently fail -- rather than answer
# "no such process" -- when several sessions start at once and all run this
# hook within a second or two of each other. That was observed once as a
# lone SessionStart writing no pid at all while the sixteen sessions around
# it wrote theirs fine, so a failed query is retried before the walk gives
# up, and the reason the walk stopped is reported back so a future blank
# pid says *why* in claude_hook_events.log instead of just "pid=".
function Find-ClaudeAncestorPid {
    param([int]$StartPid, [int]$MaxHops = 6, [int]$AttemptsPerHop = 3)
    $currentPid = $StartPid
    for ($i = 0; $i -lt $MaxHops; $i++) {
        $proc = $null
        $queryError = $null
        for ($attempt = 0; $attempt -lt $AttemptsPerHop; $attempt++) {
            try {
                # -ErrorAction Stop (overriding this script's
                # SilentlyContinue default) is what makes a genuinely
                # failed query distinguishable from an empty result: "the
                # query broke" is worth retrying, "that process is gone" is
                # a real answer and isn't.
                $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$currentPid" -ErrorAction Stop
                $queryError = $null
                break
            } catch {
                $queryError = $_.Exception.Message
                Start-Sleep -Milliseconds 150
            }
        }
        if ($queryError) {
            $script:pidLookupNote = "query failed at hop $i (pid $currentPid) after $AttemptsPerHop attempts: $queryError"
            return $null
        }
        if (-not $proc) { $script:pidLookupNote = "pid $currentPid not found at hop $i"; return $null }
        if ($proc.Name -ieq "claude.exe") { return $proc.ProcessId }
        if (-not $proc.ParentProcessId) {
            $script:pidLookupNote = "no parent above $($proc.Name) (pid $currentPid) at hop $i"
            return $null
        }
        $currentPid = $proc.ParentProcessId
    }
    $script:pidLookupNote = "no claude.exe within $MaxHops hops of pid $StartPid"
    return $null
}
# Second route to the same pid, used only when the ancestry walk comes up
# empty. Every failure ever observed has been a SessionStart (4 of 332;
# zero across 1176 Stop/UserPromptSubmit/PermissionRequest/SubagentStop
# events), where an intermediate shell had already exited by the time this
# hook walked up -- the walk reports "pid N not found at hop 2" and there
# is no chain left to follow. SessionStart is also the one event with no
# earlier pid on file to fall back to, so without this those sessions stay
# pid-less for their whole life.
#
# A resumed session carries its own id on claude.exe's command line
# (--resume=<session_id>), which is an unambiguous identification -- the id
# is unique, so a match is the right process by definition. Two known
# limits: it does nothing for a *fresh* session (no --resume on the command
# line at all -- 3 of 9 live processes, when this was checked), and it
# leans on an undocumented command-line shape layered on top of the already
# undocumented ancestry assumption, so it is written to fail quietly and
# leave a note rather than to be relied on.
function Find-ClaudePidBySessionId {
    param([string]$SessionId)
    if (-not $SessionId) { return $null }
    try {
        $candidates = @(Get-CimInstance Win32_Process -Filter "Name='claude.exe'" -ErrorAction Stop |
            Where-Object { $_.CommandLine -and $_.CommandLine.Contains("--resume=$SessionId") })
    } catch {
        $script:pidLookupNote += "; --resume scan failed: $($_.Exception.Message)"
        return $null
    }
    if ($candidates.Count -eq 1) { return $candidates[0].ProcessId }
    if ($candidates.Count -gt 1) {
        # Can't happen with unique session ids, so if it ever does the
        # assumption above is wrong -- say so instead of picking one.
        $script:pidLookupNote += "; --resume matched $($candidates.Count) processes, too ambiguous to use"
        return $null
    }
    $script:pidLookupNote += "; no live claude.exe with --resume=$SessionId (fresh session, or already exited)"
    return $null
}

$script:pidLookupNote = ""
$claudePid = Find-ClaudeAncestorPid -StartPid $PID

# Tried before the recorded-pid fallback below: this is a live, positive
# identification of a running process, where that one is only a snapshot
# from an earlier invocation.
if (-not $claudePid) {
    $claudePid = Find-ClaudePidBySessionId -SessionId $data.session_id
    if ($claudePid) { $script:pidLookupNote += " (recovered pid $claudePid via --resume match)" }
}

# A lookup that failed for a session we already recorded a pid for is a
# transient blip, not evidence the process went away -- carrying the known
# pid forward keeps that session on the precise liveness check instead of
# silently demoting it to the 24-hour staleness fallback for the rest of
# its life. Nothing is taken on trust here: claude_provider.cpp re-verifies
# the pid is still a live claude.exe every time it reads statuses.
if ((-not $claudePid) -and (Test-Path $file)) {
    $priorPid = [regex]::Match([System.IO.File]::ReadAllText($file), '(?m)^pid=(\d+)\s*$')
    if ($priorPid.Success) {
        $claudePid = [int]$priorPid.Groups[1].Value
        $script:pidLookupNote += " (reusing pid $claudePid recorded earlier for this session)"
    }
}

$content = "status=$status`ncwd=$($data.cwd)`n"
if ($claudePid) { $content += "pid=$claudePid`n" }
[System.IO.File]::WriteAllText($file, $content, [System.Text.UTF8Encoding]::new($false))

$bgCount = if ($data.background_tasks) { $data.background_tasks.Count } else { 0 }
$bgDetail = ""
if ($bgCount -gt 0) {
    $parts = $data.background_tasks | ForEach-Object { "$($_.id):$($_.status):$($_.description)" }
    $bgDetail = " bgDetail=[" + ($parts -join "; ") + "]"
}
$pidNote = if ($script:pidLookupNote) { " pidLookup=[$($script:pidLookupNote.Trim())]" } else { "" }
Write-HookEventLog "event=$($data.hook_event_name) session=$($data.session_id) status=$status pid=$claudePid$pidNote cwd=$($data.cwd)$bgDetail"

exit 0
