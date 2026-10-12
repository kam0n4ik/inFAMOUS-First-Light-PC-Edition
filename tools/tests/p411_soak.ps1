# SecondSonPC P4.11: a soak run - one game session of -Minutes with scripted play (not a still camera), its
# telemetry collected for tools\tests\p411_soak_report.py.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p411_soak.ps1 -Tag <name> -Scene user1022|beach|early
#       [-Minutes 30] [-Play walk|fight] [-Env "A=b;C=d"] [-Exe <exe>] [-Validate]
# Scenes as tools\tests\p46_runs.ps1 (each from a fresh copy of the save; the originals are never written).
# Play (SecondSonPC's keyboard layout, --press): after the title (Continue: space at 12 s and 22 s) a 40 s cycle
# repeated until the end - walk forward while turning, dash (E = circle), jump (space), aim (right mouse = L2) and
# fire powers (left mouse = R2), R1 (r), a fast 180-degree camera turn, walk back, strafe, look up and down, melee
# (v = square); `fight` fires twice as often (smoke and particles). The game is closed by WM_CLOSE at the end
# (tools\run_phase0.ps1: an orderly exit, its exit code in run.json). Telemetry: gpu_frames.csv (every frame),
# SecondSonPC.log (--vram-log 5: VRAM and commit samples; every 10 s the backend's rate and "soak:" lines),
# gpu_native.txt (the end), run.json.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [ValidateSet("user1022", "beach", "early")][string]$Scene = "user1022",
    [int]$Minutes = 30,
    [ValidateSet("walk", "fight")][string]$Play = "walk",
    [string]$Env = "",
    [string]$Exe = "",
    [switch]$Validate,
    [string]$OutRoot = "E:\SecondSonPC\logs\p411\soak"
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$sources = @{
    beach    = "$root\logs\performance\p43_ab\snapshot\home"
    early    = "$root\userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc"
    user1022 = "$root\logs\p32\scene_user_1022\home"
}
$out = "$OutRoot\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$profileDir = "$root\logs\p411\soak_profile"
if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
New-Item -ItemType Directory -Force $profileDir | Out-Null
Copy-Item $sources[$Scene] "$profileDir\home" -Recurse
foreach ($f in "gpu_device_lost.txt", "vk_validation.log", "gpu_native.txt", "gpu_frames.csv") {
    Remove-Item "$root\logs\$f" -ErrorAction SilentlyContinue
}

# the scripted play: key@second[:hold]
$seconds = $Minutes * 60
$ev = New-Object System.Collections.Generic.List[string]
$ev.Add("space@12"); $ev.Add("space@22")
$fire = if ($Play -eq "fight") { @(0.0, 0.4, 0.8, 1.2, 1.6, 2.0, 2.4) } else { @(0.3, 0.9, 1.5) }
for ($t = 40; $t -lt $seconds - 45; $t += 40) {
    $ev.Add("w@$($t):4"); $ev.Add("move:1500:0@$($t + 1):2")          # walk forward, turning
    $ev.Add("e@$($t + 5)"); $ev.Add("space@$($t + 6)")                 # dash, jump
    $ev.Add("mouse_right@$($t + 8):3")                                  # aim ...
    foreach ($f in $fire) { $ev.Add("mouse_left@$($t + 8.5 + $f):0.12") } # ... and fire
    $ev.Add("r@$($t + 12)")                                             # R1
    $ev.Add("move:-3000:0@$($t + 13):0.5")                              # fast turn
    $ev.Add("s@$($t + 14):2"); $ev.Add("a@$($t + 16.5):2"); $ev.Add("d@$($t + 19):2")
    $ev.Add("move:0:-600@$($t + 21.5):0.5"); $ev.Add("move:0:600@$($t + 22.5):0.5")
    $ev.Add("w@$($t + 24):5"); $ev.Add("move:-1200:0@$($t + 25):3")
    $ev.Add("v@$($t + 30)"); $ev.Add("v@$($t + 30.6)"); $ev.Add("v@$($t + 31.2)") # melee
    if ($Play -eq "fight") {
        $ev.Add("mouse_right@$($t + 33):3")
        foreach ($f in $fire) { $ev.Add("mouse_left@$($t + 33.5 + $f):0.12") }
    }
}
$press = ($ev | ForEach-Object { $_ -replace ',', '.' }) -join ','
$gameArgs = "--loader sspc --user-data $profileDir --vram-log 5 --gpu-backend sspc --press $press"
if ($Validate) { $gameArgs += " --gpu-native-validate" }

$saved = @{}
foreach ($kv in ($Env -split ';' | Where-Object { $_ })) {
    $n, $v = $kv -split '=', 2
    $saved[$n] = [Environment]::GetEnvironmentVariable($n)
    [Environment]::SetEnvironmentVariable($n, $v)
}
$runTag = "p411_soak_$Tag"
$exeArgs = @()
if ($Exe) { $exeArgs = @("-Exe", $Exe) }
"soak '$Tag': scene $Scene, $Minutes min, play $Play, env '$Env', exe '$Exe', validate $($Validate.IsPresent), $($ev.Count) input events" |
    Out-File "$out\summary.txt" -Encoding utf8
$t0 = Get-Date
try {
    $ErrorActionPreference = "Continue"
    powershell -ExecutionPolicy Bypass -File "$root\tools\run_phase0.ps1" -Seconds $seconds -Tag $runTag -NoReport -CloseTimeout 60 -ExtraArgs $gameArgs @exeArgs 2>&1 |
        Out-File "$out\run_console.txt" -Encoding utf8
} finally {
    $ErrorActionPreference = "Stop"
    foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
}
foreach ($f in "gpu_device_lost.txt", "gpu_native.txt", "gpu_frames.csv", "SecondSonPC.log", "vk_validation.log") {
    if (Test-Path "$root\logs\$f") { Copy-Item "$root\logs\$f" $out }
}
foreach ($f in "screen.png", "run.json") {
    if (Test-Path "$root\logs\runs\$runTag\$f") { Copy-Item "$root\logs\runs\$runTag\$f" $out }
}
"wall time {0:N1} min" -f ((Get-Date) - $t0).TotalMinutes | Out-File "$out\summary.txt" -Append -Encoding utf8
Get-Content "$out\run_console.txt" | Select-String "result:" | ForEach-Object { $_.Line } | Out-File "$out\summary.txt" -Append -Encoding utf8
$ErrorActionPreference = "Continue"
$window = if ($Minutes -le 5) { 30 } else { 120 }
py -I "$root\tools\tests\p411_soak_report.py" $out --window $window 2>&1 | Out-File "$out\summary.txt" -Append -Encoding utf8
Get-Content "$out\summary.txt"
