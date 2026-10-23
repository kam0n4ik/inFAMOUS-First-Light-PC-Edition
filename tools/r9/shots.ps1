# R9 stand: screenshots of the game window at given seconds after it opened (the --press timeline's origin), for
# calibrating routes and checking what a scripted fight/flight showed (the bridge's shot@ does not work with
# --gpu-backend sspc). Started by tools\r9\r9run.ps1 -Shots as a job; PrintWindow (PW_RENDERFULLCONTENT) like the final
# screen.png of tools\run_phase0.ps1. Writes <Out>\shot_<s>.png.
#   powershell -File tools\r9\shots.ps1 -Out <dir> -At "40,50,58" [-WaitSeconds 120] [-Module tools\game_process.psm1]
# (as a job - Start-Job -FilePath - the script has no $PSScriptRoot: the caller passes the module's path)
param([Parameter(Mandatory)] [string]$Out, [Parameter(Mandatory)] [string]$At, [int]$WaitSeconds = 120,
      [string]$Module = "")
$ErrorActionPreference = "Continue"
if (-not $Module) { $Module = "$PSScriptRoot\..\game_process.psm1" }
Import-Module $Module -Force
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null
$times = @($At.Split(',', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object { [double]$_ } | Sort-Object)
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$hwnd = [IntPtr]::Zero
while ($hwnd -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline) {
    $p = Get-Process SecondSonPC -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) {
        foreach ($h in [SspcGameWindows]::TopLevel()) {
            $wpid = [uint32]0
            [void][SspcGameWindows]::GetWindowThreadProcessId($h, [ref]$wpid)
            if ($wpid -eq $p.Id -and [SspcGameWindows]::IsWindowVisible($h) -and [SspcGameWindows]::ClassOf($h) -eq "SDL_app") {
                $hwnd = $h
                break
            }
        }
    }
    if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 100 }
}
if ($hwnd -eq [IntPtr]::Zero) { "shots: no game window"; return }
$t0 = Get-Date
foreach ($t in $times) {
    $due = $t0.AddSeconds($t)
    while ((Get-Date) -lt $due) { Start-Sleep -Milliseconds 20 }
    $r = New-Object SspcGameWindows+RECT
    if (-not [SspcGameWindows]::GetWindowRect($hwnd, [ref]$r)) { break } # (the window closed)
    $wd = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($wd -le 0 -or $ht -le 0) { continue }
    $bmp = New-Object System.Drawing.Bitmap $wd, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][SspcGameWindows]::PrintWindow($hwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $Out ("shot_{0:000.0}.png" -f $t)), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
"shots: $($times.Count) at $($t0.ToString('HH:mm:ss.fff'))"
