# R8-V5: stress scenarios on the stand (docs/NATIVE_ENGINE_R8_VERIFICATION.md §6). One scenario = one stand run
# (E:\research\r6\cp\run_engine_extra.ps1: the stand's preset, its locks and queue) with its own scripted route and
# length, then tools\r8\run_check.py over its window (exit, log, frames, verify, validation).
#   powershell -NoProfile -File tools\r8\stress.ps1 -Worktree <wt> -Out <runs> -LogsDir <logs> -Owner r8-verify `
#       -Scenario long|camera|traverse|combat|menus|beach|harbor|village|validate -Tag <tag> [-Env "A=1;B=2"]
#       [-MoreArgs "..."] [-Seconds N] [-RefValidation <run dir>]
# The routes start like the stand's city route (space@12 continue, space@22 load) and act from 40 s on (the world is
# there by then on the stand's save). Keys: the PC defaults (input/settings.cpp): w/a/s/d move, Space jump (cross),
# mouse left/right fire/aim (R2/L2), Return pause (options), Tab map (touch pad); sprint (Left Shift, L3) is not
# scripted: its SDL name has a space, and the route is one command-line argument.
param(
    [Parameter(Mandatory)] [string]$Worktree,
    [Parameter(Mandatory)] [string]$Out,
    [Parameter(Mandatory)] [string]$LogsDir,
    [Parameter(Mandatory)] [string]$Owner,
    [Parameter(Mandatory)] [ValidateSet("long", "camera", "traverse", "combat", "menus", "beach", "harbor", "village",
                                        "validate")] [string]$Scenario,
    [Parameter(Mandatory)] [string]$Tag,
    [string]$Env = "",
    [string]$MoreArgs = "",
    [int]$Seconds = 0,
    [string]$RefValidation = "",
    [int]$QuietSeconds = 60
)
$ErrorActionPreference = "Stop"
$inv = [Globalization.CultureInfo]::InvariantCulture
function F([double]$x) { $x.ToString("0.##", $inv) }
$start = "space@12,space@22"

function Route([string]$kind, [int]$len) {
    $ev = New-Object System.Collections.Generic.List[string]
    switch ($kind) {
        "long" {
            # a slow loop for the whole session: walk, turn, walk back, look around (every 30 s), a run now and then
            for ($t = 40; $t -lt $len - 10; $t += 30) {
                $ev.Add("w@$(F $t):4"); $ev.Add("move:-2400:0@$(F ($t + 5)):1"); $ev.Add("w@$(F ($t + 7)):4")
                $ev.Add("move:2400:0@$(F ($t + 12)):1"); $ev.Add("move:0:-600@$(F ($t + 14)):0.5")
                $ev.Add("move:0:600@$(F ($t + 15)):0.5")
                if ((($t - 40) / 30) % 3 -eq 2) { $ev.Add("w@$(F ($t + 17)):3") }
            }
        }
        "camera" {
            # fast camera: full turns in a quarter second, both directions, pitch sweeps; the view changes every frame
            for ($t = 40; $t -lt $len - 5; $t += 2) {
                $ev.Add("move:-6000:0@$(F $t):0.25"); $ev.Add("move:6000:0@$(F ($t + 0.5)):0.25")
                $ev.Add("move:0:-1500@$(F ($t + 1)):0.2"); $ev.Add("move:0:1500@$(F ($t + 1.3)):0.2")
            }
        }
        "traverse" {
            # streaming: run forward through the city with turns and jumps - new areas, new resources and materials
            for ($t = 40; $t -lt $len - 10; $t += 12) {
                $ev.Add("w@$(F $t):10"); $ev.Add("space@$(F ($t + 4))")
                $ev.Add("move:-1800:0@$(F ($t + 6)):0.5"); $ev.Add("space@$(F ($t + 8))")
            }
        }
        "combat" {
            # effects: aim and fire (particles, smoke, transparency, dynamic lights and shadows), move between bursts
            for ($t = 40; $t -lt $len - 10; $t += 8) {
                $ev.Add("mouse_right@$(F $t):4"); $ev.Add("mouse_left@$(F ($t + 0.5)):3")
                $ev.Add("move:-900:0@$(F ($t + 4.5)):0.5"); $ev.Add("w@$(F ($t + 5)):2")
            }
        }
        "menus" {
            # transitions: pause menu and map in and out (full-screen UI passes, the world paused and resumed)
            for ($t = 40; $t -lt $len - 10; $t += 10) {
                $ev.Add("return@$(F $t)"); $ev.Add("return@$(F ($t + 3))"); $ev.Add("tab@$(F ($t + 5))")
                $ev.Add("tab@$(F ($t + 8))")
            }
        }
        default { return "" }
    }
    return "$start," + ($ev -join ",")
}

$scene = "city"; $press = ""; $len = $Seconds; $lo = 70; $hi = 148
switch ($Scenario) {
    "long" { if ($len -le 0) { $len = 1200 }; $press = Route "long" $len; $lo = 70; $hi = $len - 5 }
    "camera" { if ($len -le 0) { $len = 150 }; $press = Route "camera" $len }
    "traverse" { if ($len -le 0) { $len = 240 }; $press = Route "traverse" $len; $hi = $len - 5 }
    "combat" { if ($len -le 0) { $len = 150 }; $press = Route "combat" $len }
    "menus" { if ($len -le 0) { $len = 150 }; $press = Route "menus" $len }
    "validate" { if ($len -le 0) { $len = 150 }; if ($MoreArgs -notmatch "gpu-native-validate") { $MoreArgs = "$MoreArgs --gpu-native-validate".Trim() } }
    default { $scene = $Scenario; if ($len -le 0) { $len = 90 }; $lo = 62; $hi = 88 }
}
$runArgs = @("-NoProfile", "-File", "E:\research\r6\cp\run_engine_extra.ps1", "-Worktree", $Worktree, "-Out", $Out,
          "-LogsDir", $LogsDir, "-Owner", $Owner, "-Scene", $scene, "-Tag", $Tag, "-Seconds", $len,
          "-QuietSeconds", $QuietSeconds)
if ($Env) { $runArgs += @("-Env", $Env) }
if ($press) { $runArgs += @("-Press", $press) }
if ($MoreArgs) { $runArgs += @("-MoreArgs", $MoreArgs) }
"stress $Scenario -> $Tag ($scene, $len s, window $lo..$hi s)"
& powershell @runArgs
$run = Join-Path $Out $Tag
$check = @("-I", "-X", "utf8", "$Worktree\tools\r8\run_check.py", $run, "--from", $lo, "--to", $hi, "--json",
           "$run\check.json")
if ($Scenario -eq "validate") { $check += @("--min-fps", "2") }
if ($RefValidation) { $check += @("--ref-validation", $RefValidation) }
& py @check
if (Test-Path "$run\save") { Remove-Item "$run\save" -Recurse -Force }
