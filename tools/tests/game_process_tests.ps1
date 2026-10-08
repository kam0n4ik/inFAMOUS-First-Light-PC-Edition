# Tests for tools\game_process.psm1 (window choice, closing, exit classification). The game is
# never started.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\game_process_tests.ps1 [-SkipLive]
# 1. Parse: every tools\*.ps1, tools\*.psm1 and tools\tests\*.ps1 parses without errors.
# 2. Unit: Select-GameWindow, Get-ExitCodeInfo, Get-RunOutcome on test doubles.
# 3. Live: only processes this script starts (powershell.exe with a console window; the stand-in
#    tools\tests\fake_game_window.ps1 with an SDL_app window). Consoles start minimized, windows are
#    shown without focus; every process is closed or terminated through its own Process object and
#    any left over is terminated in the finally block. Exit code 0 = all checks passed.
param([switch]$SkipLive)
$ErrorActionPreference = 'Stop'
$tools = Split-Path $PSScriptRoot -Parent
Import-Module "$tools\game_process.psm1" -Force

$script:passed = 0
$script:failed = 0
function Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) { $script:passed++; "PASS $Name" }
    else { $script:failed++; "FAIL $Name $Detail" }
}

# --- 1. Parse ---------------------------------------------------------------------------------
$scripts = @(Get-ChildItem "$tools\*.ps1", "$tools\*.psm1", "$tools\tests\*.ps1")
foreach ($f in $scripts) {
    $tokens = $null; $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errors)
    Check "parse $($f.Name)" ($errors.Count -eq 0) (($errors | ForEach-Object { "line $($_.Extent.StartLineNumber): $($_.Message)" }) -join '; ')
}

# --- 2. Unit ----------------------------------------------------------------------------------
function Win([string]$Class, [int64]$Hwnd = 1, [int]$W = 800, [int]$H = 600, [bool]$Thread = $true,
             [bool]$Visible = $true, [bool]$Owned = $false) {
    [pscustomobject]@{ Hwnd = [IntPtr]$Hwnd; ThreadId = 7; ThreadInProcess = $Thread; Class = $Class
        Visible = $Visible; Owned = $Owned; Width = $W; Height = $H }
}

$s = Select-GameWindow @((Win 'ConsoleWindowClass' 1 1920 1080), (Win 'SDL_app' 2 1280 720))
Check 'select: console (larger) + SDL -> SDL' ($s.Window.Hwnd -eq [IntPtr]2 -and $s.Reason -eq 'sdl-window') "$($s.Reason)"
Check 'select: console is reported as rejected' ($s.Rejected.Count -eq 1 -and $s.Rejected[0].Reason -eq 'console-window')
$s = Select-GameWindow @((Win 'ConsoleWindowClass' 1))
Check 'select: console only -> none' ($null -eq $s.Window -and $s.Reason -eq 'no-game-window')
$s = Select-GameWindow @((Win 'PseudoConsoleWindow' 1))
Check 'select: ConPTY console -> none' ($null -eq $s.Window)
$s = Select-GameWindow @((Win 'SDL_app' 1 -Thread $false))
Check 'select: window of a thread outside the process -> none' ($null -eq $s.Window -and $s.Rejected[0].Reason -eq 'thread-not-in-process')
$s = Select-GameWindow @((Win 'SDL_app' 1 -Visible $false), (Win 'Other' 2))
Check 'select: hidden SDL + visible other -> other' ($s.Window.Hwnd -eq [IntPtr]2 -and $s.Reason -eq 'only-window-not-sdl') "$($s.Reason)"
$s = Select-GameWindow @((Win 'SDL_app' 1 320 240), (Win 'SDL_app' 2 1280 720), (Win 'Other' 3 1920 1080))
Check 'select: two SDL windows -> larger SDL' ($s.Window.Hwnd -eq [IntPtr]2 -and $s.Reason -eq 'largest-of-2-sdl-windows') "$($s.Reason)"
$s = Select-GameWindow @((Win 'Other' 1 320 240), (Win 'Other2' 2 1280 720))
Check 'select: two non-SDL windows -> larger' ($s.Window.Hwnd -eq [IntPtr]2 -and $s.Reason -eq 'largest-of-2-windows-not-sdl') "$($s.Reason)"
$s = Select-GameWindow @((Win 'SDL_app' 1 -Owned $true))
Check 'select: owned popup only -> none' ($null -eq $s.Window -and $s.Rejected[0].Reason -eq 'owned-window')
$s = Select-GameWindow @((Win '#32770' 1 400 200), (Win 'ConsoleWindowClass' 2))
Check 'select: message box of Fatal() without game window -> message box' ($s.Window.Hwnd -eq [IntPtr]1 -and $s.Reason -eq 'only-window-not-sdl')
$s = Select-GameWindow @()
Check 'select: no windows -> none' ($null -eq $s.Window -and $s.Reason -eq 'no-game-window')

$exitCases = @(
    @(0, '0x00000000', 'ok'),
    @(-1073741510, '0xC000013A', 'console-control'),
    @(3221225786, '0xC000013A', 'console-control'),
    @(-1073741819, '0xC0000005', 'exception'),
    @(-1073740791, '0xC0000409', 'exception'),
    @(-2147483645, '0x80000003', 'exception'),
    @(-529697949, '0xE06D7363', 'exception'),
    @(-1, '0xFFFFFFFF', 'terminated'),
    @(1, '0x00000001', 'error'),
    @(3, '0x00000003', 'error')
)
foreach ($c in $exitCases) {
    $i = Get-ExitCodeInfo $c[0]
    Check "exit code $($c[0]) -> $($c[1]) $($c[2])" ($i.Hex -eq $c[1] -and $i.Kind -eq $c[2]) "got $($i.Hex) $($i.Kind)"
}
Check 'exit code: unsigned form gives the signed Code' ((Get-ExitCodeInfo 3221225786).Code -eq -1073741510)

$outcomeCases = @(
    @('none', '', 0, 'exited-self', $false),
    @('none', '', -1073741510, 'console-closed', $null),
    @('none', '', -1073741819, 'crashed', $true),
    @('none', '', -1, 'terminated-externally', $null),
    @('none', '', 1, 'exited-error', $false),
    @('close-requested', '', 0, 'closed-by-test', $false),
    @('close-requested', '', -1073741510, 'console-closed', $null),
    @('close-requested', '', -1073741819, 'crashed-while-closing', $true),
    @('close-requested', '', 1, 'closed-with-error', $false),
    @('killed', 'close-timeout', -1, 'killed-close-timeout', $null),
    @('killed', 'no-window', -1, 'killed-no-window', $null),
    @('killed', 'close-post-failed', -1, 'killed-close-post-failed', $null)
)
foreach ($c in $outcomeCases) {
    $o = Get-RunOutcome -Action $c[0] -KillReason $c[1] -Exited $true -ExitCode $c[2]
    $crashOk = if ($null -eq $c[4]) { $null -eq $o.CrashByExitCode } else { $o.CrashByExitCode -eq $c[4] }
    Check "outcome $($c[0]) $($c[1]) $($c[2]) -> $($c[3])" ($o.How -eq $c[3] -and $crashOk) "got $($o.How) crash=$($o.CrashByExitCode)"
}
$o = Get-RunOutcome -Action 'close-requested' -Exited $true -ExitCode 0
Check 'outcome: closed-by-test says it is not proof of no crash' ($o.Note -match 'not proof')
$o = Get-RunOutcome -Action 'killed' -KillReason 'close-timeout' -Exited $true -ExitCode 0
Check 'outcome: killed but own exit code is flagged' ($o.How -eq 'killed-close-timeout' -and $o.Note -match 'ended by itself')
$o = Get-RunOutcome -Action 'killed' -KillReason 'close-timeout' -Exited $false
Check 'outcome: not exited -> still-running' ($o.How -eq 'still-running' -and $null -eq $o.Exit)

# --- 3. Live ----------------------------------------------------------------------------------
if (-not $SkipLive) {
    $fake = "$PSScriptRoot\fake_game_window.ps1"
    $started = New-Object System.Collections.ArrayList
    function Start-Console {
        $p = Start-GameProcess -FilePath 'powershell.exe' -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 120') -WindowStyle Minimized
        [void]$started.Add($p)
        $p
    }
    function Start-Fake([string]$Mode, [uint32]$Code = 0) {
        $p = Start-GameProcess -FilePath 'powershell.exe' -WindowStyle Minimized -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $fake, '-Mode', $Mode, '-ExitCode', "$Code")
        [void]$started.Add($p)
        $p
    }
    # Polls until the stand-in shows its window (Add-Type compiles for a few seconds).
    function Wait-GameWindow($p, [int]$Ms = 30000) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        do {
            $sel = Select-GameWindow -Candidates @(Get-GameWindowCandidates -Process $p)
            if ($sel.Window -or $p.HasExited) { return $sel }
            Start-Sleep -Milliseconds 200
        } while ($sw.ElapsedMilliseconds -lt $Ms)
        $sel
    }
    function Wait-Console($p, [int]$Ms = 15000) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        do {
            $c = @(Get-GameWindowCandidates -Process $p | Where-Object { $_.Class -eq 'ConsoleWindowClass' })
            if ($c.Count) { return $c[0] }
            Start-Sleep -Milliseconds 200
        } while ($sw.ElapsedMilliseconds -lt $Ms)
        $null
    }

    try {
        # L1. A console program: its MainWindowHandle is the console window (the old bug's source).
        $p = Start-Console
        $con = Wait-Console $p
        $p.Refresh()
        Check 'live: console window is reported with the program''s own pid' ($null -ne $con)
        Check 'live: MainWindowHandle of a console program = its console window' ($null -ne $con -and $p.MainWindowHandle -eq $con.Hwnd)
        if ($con) { "  info: console window thread_in_process=$($con.ThreadInProcess) (thread $($con.ThreadId))" }
        $r = Stop-GameProcess -Process $p -WindowWaitMs 1000 -CloseTimeoutMs 2000
        Check 'live: console only -> no WM_CLOSE, terminated' (-not $r.close_posted -and $r.how -eq 'killed-no-window' -and $r.exit_hex -eq '0xFFFFFFFF') "$($r.how) $($r.exit_hex)"

        # L2. The old behaviour on a test process: WM_CLOSE to the console window -> 0xC000013A.
        $p = Start-Console
        $con = Wait-Console $p
        if ($con) { [void][SspcGameWindows]::PostClose($con.Hwnd) }
        $done = $p.WaitForExit(10000)
        $o = if ($done) { Get-RunOutcome -Action 'none' -Exited $true -ExitCode $p.ExitCode } else { $null }
        Check 'live: WM_CLOSE to a console window ends the program with 0xC000013A' ($done -and $o.Exit.Hex -eq '0xC000013A' -and $o.How -eq 'console-closed') "$(if ($o) { $o.Exit.Hex })"
        if (-not $done) { $p.Kill() }

        # L3. Stand-in game: the SDL_app window is chosen, not the console; clean exit 0.
        $p = Start-Fake 'normal'
        $sel = Wait-GameWindow $p
        $cands = @(Get-GameWindowCandidates -Process $p)
        Check 'live: candidates are single windows (console + SDL_app)' ($cands.Count -ge 2 -and @($cands | Where-Object { $_.Hwnd -isnot [IntPtr] }).Count -eq 0) "count $($cands.Count)"
        Check 'live: stand-in -> its SDL_app window' ($sel.Window -and $sel.Window.Class -eq 'SDL_app' -and $sel.Reason -eq 'sdl-window') "$($sel.Reason)"
        Check 'live: stand-in console window rejected' (@($sel.Rejected | Where-Object { $_.Window.Class -eq 'ConsoleWindowClass' }).Count -eq 1)
        $r = Stop-GameProcess -Process $p
        Check 'live: normal -> closed-by-test, exit 0' ($r.how -eq 'closed-by-test' -and $r.exit_code -eq 0 -and $r.window.class -eq 'SDL_app') "$($r.how) $($r.exit_hex)"

        # L4. WM_CLOSE ignored -> terminated after the close timeout.
        $p = Start-Fake 'ignore-close'
        [void](Wait-GameWindow $p)
        $r = Stop-GameProcess -Process $p -CloseTimeoutMs 1500
        Check 'live: ignore-close -> killed-close-timeout' ($r.close_posted -and $r.how -eq 'killed-close-timeout' -and $r.close_wait_ms -ge 1400) "$($r.how) waited $($r.close_wait_ms)"

        # L5. Crash while closing.
        $p = Start-Fake 'crash-on-close' 3221225477
        [void](Wait-GameWindow $p)
        $r = Stop-GameProcess -Process $p
        Check 'live: crash-on-close -> crashed-while-closing 0xC0000005' ($r.how -eq 'crashed-while-closing' -and $r.exit_hex -eq '0xC0000005' -and $r.crash_by_exit_code) "$($r.how) $($r.exit_hex)"

        # L6. Crash before the end of the run: no WM_CLOSE, classified by the exit code.
        $p = Start-Fake 'exit-early' 3221225477
        [void](Wait-GameWindow $p)
        [void]$p.WaitForExit(10000)
        $r = Stop-GameProcess -Process $p
        Check 'live: exit-early -> crashed, no action' ($r.action -eq 'none' -and $r.how -eq 'crashed' -and -not $r.close_posted) "$($r.action) $($r.how)"

        # L7. Two SDL windows -> the larger one.
        $p = Start-Fake 'two-windows'
        [void](Wait-GameWindow $p)
        Start-Sleep -Milliseconds 300 # the second window is created right after the first
        $sel = Select-GameWindow -Candidates @(Get-GameWindowCandidates -Process $p)
        Check 'live: two windows -> 640x480' ($sel.Reason -eq 'largest-of-2-sdl-windows' -and $sel.Window.Width -eq 640) "$($sel.Reason) $($sel.Window.Width)"
        $r = Stop-GameProcess -Process $p
        Check 'live: two windows -> closed-by-test' ($r.how -eq 'closed-by-test') "$($r.how)"

        # L8. Two instances at once: closing one leaves the other untouched.
        $a = Start-Fake 'normal'
        $b = Start-Fake 'normal'
        $selA = Wait-GameWindow $a
        $selB = Wait-GameWindow $b
        Check 'live: two instances -> different windows' ($selA.Window -and $selB.Window -and $selA.Window.Hwnd -ne $selB.Window.Hwnd)
        $r = Stop-GameProcess -Process $a
        Start-Sleep -Milliseconds 500
        $selB2 = Select-GameWindow -Candidates @(Get-GameWindowCandidates -Process $b)
        Check 'live: closing A leaves B running with its window' ($r.how -eq 'closed-by-test' -and -not $b.HasExited -and $selB2.Window.Hwnd -eq $selB.Window.Hwnd) "$($r.how) B exited=$($b.HasExited)"
        $r = Stop-GameProcess -Process $b
        Check 'live: then B -> closed-by-test' ($r.how -eq 'closed-by-test') "$($r.how)"

        # L9. No game window (only the console): never posts WM_CLOSE to the console.
        $p = Start-Fake 'no-window'
        [void](Wait-Console $p)
        Start-Sleep -Seconds 3
        $r = Stop-GameProcess -Process $p -WindowWaitMs 1500
        Check 'live: no window -> killed-no-window, console untouched' (-not $r.close_posted -and $r.how -eq 'killed-no-window' -and $r.exit_hex -eq '0xFFFFFFFF') "$($r.how) $($r.exit_hex)"

        # L10. tools\run_phase0.ps1 end to end on the stand-in, logs in a scratch folder (the real
        # logs\ is not touched): run.json, screenshot of the game window, printed result.
        $scratch = Join-Path ([IO.Path]::GetTempPath()) "sspc_run_phase0_test"
        Remove-Item $scratch -Recurse -ErrorAction SilentlyContinue
        $e2e = @(
            @('normal', 0, 15, 'closed-by-test'),
            @('ignore-close', 0, 2, 'killed-close-timeout'),
            @('exit-early', 3221225477, 15, 'crashed')
        )
        foreach ($c in $e2e) {
            $tag = "e2e_$($c[0])"
            $text = & "$tools\run_phase0.ps1" -Exe 'powershell.exe' -LogsDir $scratch -Seconds 10 -Tag $tag -NoReport `
                -CloseTimeout $c[2] -ExtraArgs "-NoProfile -ExecutionPolicy Bypass -File $fake -Mode $($c[0]) -ExitCode $($c[1])" 2>&1 | Out-String
            $runJson = "$scratch\runs\$tag\run.json"
            $run = if (Test-Path $runJson) { Get-Content -Raw -Encoding UTF8 $runJson | ConvertFrom-Json } else { $null }
            Check "run_phase0 $($c[0]) -> run.json how=$($c[3])" ($run -and $run.how -eq $c[3]) "$(if ($run) { $run.how } else { $text })"
            Check "run_phase0 $($c[0]) -> result line printed" ($text -match "result: how=$($c[3]) ")
            if ($c[0] -ne 'exit-early') {
                Check "run_phase0 $($c[0]) -> SDL_app window, screenshot" ($run -and $run.window.class -eq 'SDL_app' -and (Test-Path "$scratch\runs\$tag\screen.png"))
                Check "run_phase0 $($c[0]) -> console recorded as not the game window" ($run -and @($run.candidates | Where-Object { $_.class -eq 'ConsoleWindowClass' -and $_.rejected -eq 'console-window' }).Count -eq 1)
            } else {
                Check "run_phase0 exit-early -> exited_before_deadline, no WM_CLOSE" ($run -and $run.exited_before_deadline -and -not $run.close_posted)
            }
        }
        Remove-Item $scratch -Recurse -ErrorAction SilentlyContinue

        # L11. tools\sspc002_runs.ps1 on the stand-in, in a scratch root with its own logs\ and
        # userdata\home\ (the real saves and logs are not touched).
        $root = Join-Path ([IO.Path]::GetTempPath()) "sspc_sspc002_test"
        Remove-Item $root -Recurse -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force "$root\userdata\home\1000\savedata" | Out-Null
        Set-Content -Encoding ASCII "$root\userdata\home\1000\savedata\dis.dat" "save"
        $prefixArgs = "-NoProfile -ExecutionPolicy Bypass -File $fake -Mode normal"
        $text = & "$tools\sspc002_runs.ps1" -Root $root -Exe 'powershell.exe' -ExeArgsPrefix $prefixArgs -Runs 2 `
            -Seconds 20 -Loaders 'sspc,shadps4' 2>&1 | Out-String
        $code = $LASTEXITCODE
        $lines = @(Get-Content "$root\logs\runs\sspc002_summary.jsonl" -ErrorAction SilentlyContinue)
        Check 'sspc002_runs: 2 clean runs -> exit 0, both loaders, saves unchanged' ($code -eq 0 -and
            $text -match 'sspc002_sspc_1: closed-by-test' -and $text -match 'sspc002_shadps4_2: closed-by-test' -and
            $text -match 'real saves unchanged: yes' -and $lines.Count -eq 2) "exit $code`n$text"
        Check 'sspc002_runs: profile copy removed, snapshot kept' (-not (Test-Path "$root\userdata\sspc002\run") -and
            @(Get-ChildItem "$root\userdata\sspc002" -Directory -Filter 'snapshot_*').Count -eq 1)
        $prefixArgs = "-NoProfile -ExecutionPolicy Bypass -File $fake -Mode exit-early -ExitCode 3221225477"
        $text = & "$tools\sspc002_runs.ps1" -Root $root -Exe 'powershell.exe' -ExeArgsPrefix $prefixArgs -Runs 3 `
            -Seconds 20 -Loaders 'sspc' -Prefix 'crashing' 2>&1 | Out-String
        $code = $LASTEXITCODE
        Check 'sspc002_runs: crash -> stops after run 1, exit 2, profile kept' ($code -eq 2 -and
            $text -match 'crashing_sspc_1: crashed' -and $text -notmatch 'crashing_sspc_2' -and
            (Test-Path "$root\userdata\sspc002\crash_crashing_sspc_1\home\1000\savedata\dis.dat")) "exit $code`n$text"
        $text = & "$tools\sspc002_runs.ps1" -Root $root -Runs 51 -DryRun 2>&1 | Out-String
        Check 'sspc002_runs: more than 50 runs refused' ($LASTEXITCODE -eq 1 -and $text -match 'Runs must be 1\.\.50')
        $text = & "$tools\sspc002_runs.ps1" -Root $root -Exe 'powershell.exe' -Runs 3 -Loaders 'sspc,shadps4' -DryRun 2>&1 | Out-String
        Check 'sspc002_runs: dry run starts nothing' ($LASTEXITCODE -eq 0 -and $text -match 'run 3: sspc002_sspc_3' -and
            -not (Test-Path "$root\logs\runs\sspc002_sspc_3"))
        Remove-Item $root -Recurse -ErrorAction SilentlyContinue
    } finally {
        foreach ($q in $started) {
            if (-not $q.HasExited) { "cleanup: terminating test process $($q.Id)"; $q.Kill() }
        }
    }
}

""
"game_process_tests: $script:passed passed, $script:failed failed"
if ($script:failed) { exit 1 }
exit 0
