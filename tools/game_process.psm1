# Test runs of SecondSonPC.exe: find the window of the exact process a script started, close it,
# classify how the process ended. Used by tools\run_phase0.ps1; tests: tools\tests\game_process_tests.ps1.
#
# SecondSonPC.exe is a console program (PE subsystem 3), so Start-Process gives it a console window
# of its own. Process.MainWindowHandle may return that console window: it is created by conhost.exe,
# but GetWindowThreadProcessId reports the client's pid and even one of the client's thread ids for
# it (tools\tests\game_process_tests.ps1, live check L1). WM_CLOSE posted there makes conhost raise
# CTRL_CLOSE_EVENT and the game ends with 0xC000013A, which says nothing about the game itself.
# The game window is therefore chosen explicitly (Select-GameWindow): a top-level window of the
# started pid, created by one of that process's threads, not a console window class (the check
# that actually excludes the console), visible and not owned by another window. Class SDL_app is
# only a preference when several windows qualify.
# Nothing here looks up processes by name, and nothing is ever posted to or terminated in a process
# the caller did not start.

if (-not ('SspcGameWindows' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class SspcGameWindows {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr param);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32.dll", SetLastError = true)] static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hwnd, uint cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);

    public static IntPtr[] TopLevel() {
        var found = new List<IntPtr>();
        EnumWindowsProc callback = (h, p) => { found.Add(h); return true; };
        EnumWindows(callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return found.ToArray();
    }

    public static string ClassOf(IntPtr hwnd) {
        var name = new StringBuilder(256);
        return GetClassName(hwnd, name, name.Capacity) > 0 ? name.ToString() : "";
    }

    // 0 = WM_CLOSE posted, otherwise the Win32 error of PostMessage.
    public static int PostClose(IntPtr hwnd) {
        return PostMessage(hwnd, 0x0010, IntPtr.Zero, IntPtr.Zero) ? 0 : Marshal.GetLastWin32Error();
    }
}
"@
}

$script:ConsoleWindowClasses = @('ConsoleWindowClass', 'PseudoConsoleWindow')

$script:ExitCodeNames = @{
    '0x00000000' = 'success'
    '0x00000001' = 'SecondSonPC fatal error or bad arguments (see SecondSonPC.log)'
    '0xC000013A' = 'STATUS_CONTROL_C_EXIT: console window closed, Ctrl+C or Ctrl+Break'
    '0xC0000005' = 'STATUS_ACCESS_VIOLATION'
    '0xC000001D' = 'STATUS_ILLEGAL_INSTRUCTION'
    '0xC0000096' = 'STATUS_PRIVILEGED_INSTRUCTION'
    '0xC0000094' = 'STATUS_INTEGER_DIVIDE_BY_ZERO'
    '0xC00000FD' = 'STATUS_STACK_OVERFLOW'
    '0xC0000017' = 'STATUS_NO_MEMORY'
    '0xC0000374' = 'STATUS_HEAP_CORRUPTION'
    '0xC0000409' = 'STATUS_STACK_BUFFER_OVERRUN (fail fast / abort)'
    '0xC0000420' = 'STATUS_ASSERTION_FAILURE'
    '0x80000003' = 'STATUS_BREAKPOINT'
    '0xE06D7363' = 'unhandled C++ exception'
    '0xFFFFFFFF' = 'exit code -1 (TerminateProcess / Process.Kill)'
}

function Start-GameProcess {
    param(
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$WorkingDirectory = '',
        [ValidateSet('Normal', 'Minimized', 'Hidden', 'Maximized')] [string]$WindowStyle = 'Normal'
    )
    $start = @{ FilePath = $FilePath; PassThru = $true; WindowStyle = $WindowStyle }
    if ($ArgumentList.Count) { $start.ArgumentList = $ArgumentList }
    if ($WorkingDirectory) { $start.WorkingDirectory = $WorkingDirectory }
    $p = Start-Process @start
    # Holding the handle keeps ExitCode readable after the exit and the pid from being reused.
    $null = $p.Handle
    $p
}

# All top-level windows whose reported owner pid is the given process (console window included).
function Get-GameWindowCandidates {
    param([Parameter(Mandatory = $true)] [System.Diagnostics.Process]$Process)
    $threads = @{}
    try {
        $Process.Refresh()
        foreach ($t in $Process.Threads) { $threads[[uint32]$t.Id] = $true }
    } catch { }
    $list = @()
    foreach ($h in [SspcGameWindows]::TopLevel()) {
        [uint32]$owner = 0
        $tid = [SspcGameWindows]::GetWindowThreadProcessId($h, [ref]$owner)
        if ($owner -ne [uint32]$Process.Id) { continue }
        $r = New-Object SspcGameWindows+RECT
        [void][SspcGameWindows]::GetWindowRect($h, [ref]$r)
        $list += [pscustomobject]@{
            Hwnd            = $h
            ThreadId        = $tid
            ThreadInProcess = $threads.ContainsKey([uint32]$tid)
            Class           = [SspcGameWindows]::ClassOf($h)
            Visible         = [SspcGameWindows]::IsWindowVisible($h)
            Owned           = ([SspcGameWindows]::GetWindow($h, 4) -ne [IntPtr]::Zero) # GW_OWNER
            Width           = $r.Right - $r.Left
            Height          = $r.Bottom - $r.Top
        }
    }
    $list # callers wrap the call in @() (none or one window)
}

# Pure: picks the game window from Get-GameWindowCandidates output (or test doubles).
function Select-GameWindow {
    param([object[]]$Candidates = @())
    $usable = @()
    $rejected = @()
    foreach ($w in $Candidates) {
        $why = $null
        if (-not $w.ThreadInProcess) { $why = 'thread-not-in-process' }
        elseif ($script:ConsoleWindowClasses -contains $w.Class) { $why = 'console-window' }
        elseif (-not $w.Visible) { $why = 'hidden' }
        elseif ($w.Owned) { $why = 'owned-window' }
        if ($why) { $rejected += [pscustomobject]@{ Window = $w; Reason = $why } }
        else { $usable += $w }
    }
    $pick = $null
    $reason = 'no-game-window'
    if ($usable.Count) {
        $sdl = @($usable | Where-Object { $_.Class -eq 'SDL_app' })
        $pool = if ($sdl.Count) { $sdl } else { $usable }
        $pick = $pool | Sort-Object -Property @{ Expression = { [int64]$_.Width * [int64]$_.Height } } -Descending |
            Select-Object -First 1
        $reason = if ($sdl.Count -eq 1) { 'sdl-window' }
                  elseif ($sdl.Count -gt 1) { "largest-of-$($sdl.Count)-sdl-windows" }
                  elseif ($usable.Count -eq 1) { 'only-window-not-sdl' }
                  else { "largest-of-$($usable.Count)-windows-not-sdl" }
    }
    [pscustomobject]@{ Window = $pick; Reason = $reason; Usable = $usable.Count; Rejected = $rejected }
}

# Pure: exit code -> hex, name, kind (ok | console-control | terminated | exception | error).
function Get-ExitCodeInfo {
    param([Parameter(Mandatory = $true)] [int64]$Code)
    $u = [uint32]($Code -band 0xFFFFFFFFL)
    $signed = [BitConverter]::ToInt32([BitConverter]::GetBytes($u), 0) # [int32] casts are checked
    $hex = '0x{0:X8}' -f $u
    $kind = if ($u -eq 0) { 'ok' }
            elseif ($hex -eq '0xC000013A') { 'console-control' }
            elseif ($hex -eq '0xFFFFFFFF') { 'terminated' }
            elseif (($u -band 0xC0000000L) -eq 0xC0000000L -or $hex -eq '0x80000003') { 'exception' }
            else { 'error' }
    $name = if ($script:ExitCodeNames.ContainsKey($hex)) { $script:ExitCodeNames[$hex] } else { "exit code $signed" }
    [pscustomobject]@{ Code = $signed; Hex = $hex; Name = $name; Kind = $kind }
}

# Pure: what the script did (Action) + how the process ended -> one classification.
#   Action none            - the process ended before the script tried to stop it
#   Action close-requested - WM_CLOSE was posted to the game window and the process ended in time
#   Action killed          - TerminateProcess by the script (KillReason: close-timeout | no-window |
#                            close-post-failed)
# CrashByExitCode: $true / $false when the exit code tells, $null when it cannot (killed, console
# control, still running). A clean exit after the script's WM_CLOSE is not proof that nothing
# failed earlier in the run: the log analysis (tools\analyze_run.py) answers that.
function Get-RunOutcome {
    param(
        [Parameter(Mandatory = $true)] [ValidateSet('none', 'close-requested', 'killed')] [string]$Action,
        [string]$KillReason = '',
        [bool]$Exited = $true,
        $ExitCode = $null
    )
    if (-not $Exited -or $null -eq $ExitCode) {
        return [pscustomobject]@{ How = 'still-running'; CrashByExitCode = $null; Exit = $null
            Note = 'the process had not exited when the script gave up waiting' }
    }
    $info = Get-ExitCodeInfo $ExitCode
    $how = $null; $crash = $null; $note = $info.Name
    switch ($Action) {
        'none' {
            switch ($info.Kind) {
                'ok' { $how = 'exited-self'; $crash = $false
                    $note = 'ended by itself with 0 before the run time was over; the game returns 0 only after its window is closed (by a person or another program)' }
                'console-control' { $how = 'console-closed'
                    $note = '0xC000013A: its console was closed or got Ctrl+C/Ctrl+Break from outside this script; neither a crash nor a pass' }
                'terminated' { $how = 'terminated-externally'
                    $note = 'exit code -1: terminated by another program or exit(-1)' }
                'exception' { $how = 'crashed'; $crash = $true }
                default { $how = 'exited-error'; $crash = $false }
            }
        }
        'close-requested' {
            switch ($info.Kind) {
                'ok' { $how = 'closed-by-test'; $crash = $false
                    $note = 'clean shutdown after this script''s WM_CLOSE; not proof that nothing failed earlier (see the log analysis)' }
                'console-control' { $how = 'console-closed'
                    $note = '0xC000013A after WM_CLOSE to the game window: the console was closed at the same time from elsewhere; investigate' }
                'terminated' { $how = 'terminated-externally'
                    $note = 'exit code -1 after WM_CLOSE: terminated by another program or exit(-1)' }
                'exception' { $how = 'crashed-while-closing'; $crash = $true }
                default { $how = 'closed-with-error'; $crash = $false }
            }
        }
        'killed' {
            $how = "killed-$KillReason"
            $note = "TerminateProcess by this script ($KillReason); the exit code is the script's, not the game's"
            if ($info.Hex -ne '0xFFFFFFFF') {
                $note = "$note; but the code is $($info.Hex) ($($info.Name)), so the process ended by itself at the same moment"
            }
        }
    }
    [pscustomobject]@{ How = $how; CrashByExitCode = $crash; Exit = $info; Note = $note }
}

function Format-Hwnd([IntPtr]$Hwnd) { '0x{0:X}' -f $Hwnd.ToInt64() }

function ConvertTo-WindowRecord($Window, [string]$Rejected = '') {
    $rec = [ordered]@{
        hwnd              = Format-Hwnd $Window.Hwnd
        class             = $Window.Class
        thread_id         = $Window.ThreadId
        thread_in_process = $Window.ThreadInProcess
        visible           = $Window.Visible
        owned             = $Window.Owned
        size              = "$($Window.Width)x$($Window.Height)"
    }
    if ($Rejected) { $rec.rejected = $Rejected }
    $rec
}

# Waits up to WindowWaitMs for the game window, calls OnWindow (screenshot) with it, posts WM_CLOSE,
# waits CloseTimeoutMs; terminates the process (the given Process object only) when there is no
# window, posting failed or the close timed out. Returns a record for logs\runs\<tag>\run.json.
function Stop-GameProcess {
    param(
        [Parameter(Mandatory = $true)] [System.Diagnostics.Process]$Process,
        [int]$WindowWaitMs = 5000,
        [int]$CloseTimeoutMs = 15000,
        [int]$KillTimeoutMs = 10000,
        [scriptblock]$OnWindow = $null
    )
    $r = [ordered]@{
        pid = $Process.Id; action = 'none'; window = $null; window_reason = $null; candidates = @()
        close_posted = $false; close_error = $null; close_wait_ms = $null
        kill_reason = $null; kill_error = $null; kill_wait_ms = $null; on_window_error = $null
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    if (-not $Process.HasExited) {
        $sel = $null
        $cands = @()
        do {
            $cands = @(Get-GameWindowCandidates -Process $Process)
            $sel = Select-GameWindow -Candidates $cands
            if ($sel.Window -or $Process.HasExited) { break }
            Start-Sleep -Milliseconds 250
        } while ($sw.ElapsedMilliseconds -lt $WindowWaitMs)
        $rejectedBy = @{}
        foreach ($x in $sel.Rejected) { $rejectedBy[$x.Window.Hwnd] = $x.Reason }
        $r.candidates = @($cands | ForEach-Object { ConvertTo-WindowRecord $_ $rejectedBy[$_.Hwnd] })
        $r.window_reason = $sel.Reason

        if (-not $Process.HasExited) {
            if ($sel.Window) {
                $r.window = ConvertTo-WindowRecord $sel.Window
                if ($OnWindow) {
                    try { $null = & $OnWindow $sel.Window } catch { $r.on_window_error = "$_" }
                }
                $err = [SspcGameWindows]::PostClose($sel.Window.Hwnd)
                if ($err -eq 0) {
                    $r.close_posted = $true
                    $r.action = 'close-requested'
                    $sw.Restart()
                    $done = $Process.WaitForExit($CloseTimeoutMs)
                    $r.close_wait_ms = $sw.ElapsedMilliseconds
                    if (-not $done) { $r.kill_reason = 'close-timeout' }
                } else {
                    $r.close_error = $err
                    if (-not $Process.HasExited) { $r.kill_reason = 'close-post-failed' }
                }
            } else {
                $r.kill_reason = 'no-window'
            }
        }

        if ($r.kill_reason) {
            $sw.Restart()
            try {
                $Process.Kill()
                $r.action = 'killed'
            } catch {
                $r.kill_error = "$_" # already gone: keep the action, the exit code is the process's own
            }
            [void]$Process.WaitForExit($KillTimeoutMs)
            $r.kill_wait_ms = $sw.ElapsedMilliseconds
        }
    }
    $exited = $Process.HasExited
    $code = if ($exited) { $Process.ExitCode } else { $null }
    $o = Get-RunOutcome -Action $r.action -KillReason $r.kill_reason -Exited $exited -ExitCode $code
    $r.exited = $exited
    $r.exit_code = $code
    $r.exit_hex = if ($o.Exit) { $o.Exit.Hex } else { $null }
    $r.exit_name = if ($o.Exit) { $o.Exit.Name } else { $null }
    $r.how = $o.How
    $r.crash_by_exit_code = $o.CrashByExitCode
    $r.note = $o.Note
    $r
}

Export-ModuleMember -Function Start-GameProcess, Get-GameWindowCandidates, Select-GameWindow, Get-ExitCodeInfo,
    Get-RunOutcome, Stop-GameProcess, Format-Hwnd
