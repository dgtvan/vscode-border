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
    default { "waiting" } # SessionStart, Stop, StopFailure, SubagentStop -- see background_tasks below
}

# A turn can end while Claude is still due to carry on by itself: it started
# background work (a subagent, a build, a "poll the PR until checks finish"
# loop) and Claude Code will wake it up with the result, or it scheduled a
# wakeup (ScheduleWakeup, /loop). Stop/StopFailure/SubagentStop's payload
# lists both -- background_tasks and session_crons -- and either keeps the
# session "working" rather than "waiting".
#
# This deliberately follows Claude Code's own rule for "is this session
# still busy" -- its session runner (claude.exe 2.1.281, the "follow-up
# hold" logic) -- and does not try to be smarter than it:
#   - Any live background task counts, whatever it is. Claude Code tells
#     Claude "you will be notified when it completes" for every one, a dev
#     server included, and doesn't record whether Claude means to wait. So a
#     dev server left running keeps the session "working" until it stops,
#     exactly as the runner counts it.
#   - Except "monitor" tasks (MCP/websocket monitors, including the artifact
#     live-update watchers Claude Code starts by itself) and "teammate"
#     tasks, which the runner leaves out.
#   - A pending one-shot wakeup counts, as the runner holds the session busy
#     until a ScheduleWakeup is due. A recurring cron never ends, so doesn't.
#
# When a task finishes or a wakeup fires, Claude runs a new turn that ends
# in another Stop, which rewrites this with the updated lists.
function Test-CountedBackgroundTask {
    param($Task)
    return $Task.status -in "running", "pending" -and $Task.type -notin "monitor", "teammate"
}

$countedBgTasks = @($data.background_tasks | Where-Object { $_ -and (Test-CountedBackgroundTask $_) })
$pendingWakeups = @($data.session_crons | Where-Object { $_ -and -not $_.recurring })
if ($status -eq "waiting" -and ($countedBgTasks.Count -gt 0 -or $pendingWakeups.Count -gt 0)) {
    $status = "working"
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

# Finds the id of the tool call a PermissionRequest is blocked on, from the
# transcript. Needed because the payload does not carry it: the hook docs
# show tool_use_id on PermissionRequest, but Claude Code 2.1.270 builds that
# input as {common fields, hook_event_name, tool_name, tool_input,
# permission_suggestions} -- read from its own bundled source, and confirmed
# by the first real PermissionRequest after pending_tool was introduced,
# which arrived without one. (PermissionDenied, built right next to it, does
# include it.) Payload's own id still wins if a later version adds it.
#
# The call's tool_use block is already in the transcript by the time the
# hook runs (written ~1.5s earlier in both cases looked at), so this parses
# the transcript's tail properly -- a text match is not safe: ~2% of
# "tool_use" occurrences are inside escaped strings, e.g. a transcript
# quoted in a message -- and picks among the calls with this tool_name that
# have no tool_result yet:
#   exact-input     one whose input equals the payload's tool_input;
#   latest-message  else the first unanswered one in the newest assistant
#                   message. Calls within a message run in order, so the
#                   earlier ones are answered by now; and anchoring to the
#                   newest message keeps clear of calls from killed or
#                   interrupted turns that never got a result at all (37 of
#                   ~16,000 across the transcripts on this machine).
# The transcript is "written asynchronously and may lag" per the docs, so a
# miss is retried briefly before giving up; giving up just leaves the old
# activity-based fallback in claude_provider.cpp in charge.
function Resolve-PendingToolUseId {
    param([string]$TranscriptPath, [string]$ToolName, $ToolInput)
    $want = if ($null -ne $ToolInput) { $ToolInput | ConvertTo-Json -Compress -Depth 50 } else { "" }
    for ($attempt = 0; $attempt -lt 6; $attempt++) {
        if ($attempt -gt 0) { Start-Sleep -Milliseconds 150 }
        try {
            $fs = [System.IO.File]::Open($TranscriptPath, 'Open', 'Read', 'ReadWrite, Delete')
            try {
                $start = [Math]::Max(0, $fs.Length - 1MB)
                [void]$fs.Seek($start, 'Begin')
                $buf = New-Object byte[] ($fs.Length - $start)
                $read = $fs.Read($buf, 0, $buf.Length)
            } finally { $fs.Close() }
        } catch { return @{ Note = "transcript unreadable: $($_.Exception.Message)" } }
        $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $read)
        $lines = $text -split "`n"
        if ($start -gt 0 -and $lines.Count -gt 0) { $lines = $lines[1..($lines.Count - 1)] } # first one is partial

        $answered = @{}
        foreach ($m in [regex]::Matches($text, '"tool_use_id":"(toolu_[A-Za-z0-9_]+)"')) { $answered[$m.Groups[1].Value] = $true }

        $candidates = @()
        $nameNeedle = '"name":"' + $ToolName + '"'
        foreach ($line in $lines) {
            if (-not $line.Contains('"tool_use"') -or -not $line.Contains($nameNeedle)) { continue }
            try { $o = $line | ConvertFrom-Json } catch { continue }
            if ($o.type -ne "assistant" -or -not ($o.message.content -is [array])) { continue }
            foreach ($b in $o.message.content) {
                if ($b.type -ne "tool_use" -or $b.name -ne $ToolName -or $answered[$b.id]) { continue }
                $candidates += @{ Id = $b.id; Msg = $o.message.id; Input = ($b.input | ConvertTo-Json -Compress -Depth 50) }
            }
        }
        if ($candidates.Count -eq 0) { continue } # not written yet, or lagging -- retry

        $exact = @($candidates | Where-Object { $want -and $_.Input -eq $want })
        if ($exact.Count -gt 0) { return @{ Id = $exact[0].Id; Rule = "exact-input"; Note = "" } }
        $latestMsg = $candidates[-1].Msg
        $pick = @($candidates | Where-Object { $_.Msg -eq $latestMsg })[0]
        return @{ Id = $pick.Id; Rule = "latest-message"; Note = "$($candidates.Count) unanswered $ToolName call(s)" }
    }
    return @{ Note = "no unanswered $ToolName call in the transcript tail after 6 tries" }
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

# Last resort, SessionStart only: every route above has come up empty, which
# on this event means either a genuinely fresh session whose ancestry chain
# was momentarily unwalkable, or -- the only case actually observed -- a
# session that was already being torn down before this hook got to look. One
# more --resume scan after a short pause tells those apart: a live process
# is still there to be found a moment later, a dead one never comes back.
if ((-not $claudePid) -and ($data.hook_event_name -eq "SessionStart")) {
    $noteBeforeRetry = $script:pidLookupNote
    Start-Sleep -Milliseconds 250
    $claudePid = Find-ClaudePidBySessionId -SessionId $data.session_id
    if ($claudePid) {
        $script:pidLookupNote += " (recovered pid $claudePid via --resume match on retry)"
    } else {
        # The scan appends its own "no live claude.exe" note again; keep the
        # first one and say it was retried rather than logging it twice.
        $script:pidLookupNote = $noteBeforeRetry + "; still absent 250ms later"
    }
}

# A SessionStart that cannot locate its own process by any route describes a
# session that isn't running -- observed when several VS Code windows reload
# at once and their sessions fire SessionStart on the way out (chain already
# severed at hop 2, claude.exe already gone). Writing a file for it creates a
# pid-less orphan that no later event ever corrects (there are no later
# events) and that claude_provider.cpp must then carry, and warn about, for a
# full kNoPidStaleMinutes day. SessionStart is uniquely safe to skip here: it
# is the one event with no prior state worth preserving, and a session that
# is genuinely alive re-announces itself on its next event with a pid
# attached. Every other event still writes unconditionally.
if ((-not $claudePid) -and ($data.hook_event_name -eq "SessionStart")) {
    $skipNote = if ($script:pidLookupNote) { " pidLookup=[$($script:pidLookupNote.Trim())]" } else { "" }
    Write-HookEventLog "event=$($data.hook_event_name) session=$($data.session_id) status=$status pid=$skipNote cwd=$($data.cwd) (no status file written -- session process not locatable, treating as already ended)"
    exit 0
}

$content = "status=$status`ncwd=$($data.cwd)`n"
if ($claudePid) { $content += "pid=$claudePid`n" }

# The session's transcript, as Claude Code itself reports it. Recorded
# rather than left for claude_provider.cpp to derive from cwd, because cwd
# is not the project root: it follows the session's shell, so after a `cd`
# into a subfolder the derived path names a transcript directory that does
# not exist -- observed with cwd=...\proplyst\src\web\app\src, whose
# transcript is under the directory for ...\proplyst.
if ($data.transcript_path) { $content += "transcript=$($data.transcript_path)`n" }

# Which tool call this "attention" is blocked on. There is no hook for "the
# permission was answered" (PostToolUse does not fire until the tool has
# also *finished*, which for an approved long command can be minutes later),
# but the answer always lands in the transcript as that tool call's
# tool_result, keyed by its id -- claude_provider.cpp watches for it there.
# Recorded with the file the result will be written to (a subagent's tool
# calls go to its own transcript, not the session's) and that file's length
# right now: the result cannot exist before this hook returns, so the reader
# only ever needs to look past this point. The id itself has to be dug out
# of the transcript -- see Resolve-PendingToolUseId.
$pendingNote = ""
if ($data.hook_event_name -eq "PermissionRequest") {
    $pendingTranscript = $data.transcript_path
    if ($data.agent_id -and $pendingTranscript -and $pendingTranscript -notmatch '[\\/]subagents[\\/]') {
        $agentTranscript = Join-Path ($pendingTranscript -replace '\.jsonl$', '') "subagents\agent-$($data.agent_id).jsonl"
        if (Test-Path -LiteralPath $agentTranscript) { $pendingTranscript = $agentTranscript }
    }
    $pendingOffset = 0
    if ($pendingTranscript -and (Test-Path -LiteralPath $pendingTranscript)) {
        $pendingOffset = (Get-Item -LiteralPath $pendingTranscript).Length
    }

    $pendingId = $data.tool_use_id
    $pendingRule = "payload"
    $resolveNote = ""
    if (-not $pendingId -and $pendingTranscript -and $data.tool_name) {
        $resolved = Resolve-PendingToolUseId -TranscriptPath $pendingTranscript -ToolName $data.tool_name -ToolInput $data.tool_input
        $pendingId = $resolved.Id
        $pendingRule = $resolved.Rule
        $resolveNote = $resolved.Note
    }
    if ($pendingId) {
        $content += "pending_tool=$pendingId`npending_transcript=$pendingTranscript`npending_offset=$pendingOffset`n"
        $pendingNote = " tool=$($data.tool_name) toolUseId=$pendingId (via $pendingRule)"
    } else {
        $pendingNote = " tool=$($data.tool_name) toolUseId= (unresolved)"
    }
    if ($resolveNote) { $pendingNote += " resolve=[$resolveNote]" }
    if ($data.agent_id) { $pendingNote += " agent=$($data.agent_id)" }
}
[System.IO.File]::WriteAllText($file, $content, [System.Text.UTF8Encoding]::new($false))

$bgCount = if ($data.background_tasks) { $data.background_tasks.Count } else { 0 }
$bgDetail = ""
if ($bgCount -gt 0) {
    $parts = $data.background_tasks | ForEach-Object {
        $counted = if (Test-CountedBackgroundTask $_) { "counted" } else { "ignored" }
        "$($_.id):$($_.status):$($_.type):$($counted):$($_.description)"
    }
    $bgDetail = " bgDetail=[" + ($parts -join "; ") + "]"
}
if ($pendingWakeups.Count -gt 0) {
    $bgDetail += " wakeups=[" + (($pendingWakeups | ForEach-Object { "$($_.id):$($_.schedule)" }) -join "; ") + "]"
}
$pidNote = if ($script:pidLookupNote) { " pidLookup=[$($script:pidLookupNote.Trim())]" } else { "" }
Write-HookEventLog "event=$($data.hook_event_name) session=$($data.session_id) status=$status pid=$claudePid$pidNote cwd=$($data.cwd)$pendingNote$bgDetail"

exit 0
