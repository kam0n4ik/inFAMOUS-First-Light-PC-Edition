# SecondSonPC P4.5.4: short runs of the available game scenes (saves), one command.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\scene_matrix.ps1 -Tag <name>
#       [-Scenes beach,user1022,usercur] [-Backend sspc|bridge] [-Validate] [-Seconds 75] [-Env "NAME=v;NAME2=v"]
# Scenes (each run starts from a fresh copy; the originals are never written):
#   beach    logs\performance\p43_ab\snapshot          the P4.3/P4.4 reference route (beach, foliage)
#   user1022 logs\p32\scene_user_1022                  the user's autosave of 2026-10-10 10:22 (P3.2's second scene)
#   usercur  userdata\home (copied)                    the user's current progress (a copy; userdata is not touched)
#   early    userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc   the user's earliest backed-up progress
# Per scene: tools\run_phase0.ps1 (title -> Continue -> three camera turns at 56..60 s, WM_CLOSE at the end), then
# logs\p45\scenes\<Tag>\<scene>\: SecondSonPC.log, gpu_native.txt, vk_validation.log, screen.png, run.json and a line in
# logs\p45\scenes\<Tag>\summary.txt: device lost lines, validation messages, [Error] lines, flips, 10-s FPS windows,
# the screenshot's mean brightness.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Scenes = "beach,user1022,usercur",
    [ValidateSet("sspc", "bridge")][string]$Backend = "sspc",
    [switch]$Validate,
    [int]$Seconds = 75,
    [string]$Env = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$sources = @{ # each scene's home directory (the save)
    beach    = "$root\logs\performance\p43_ab\snapshot\home"
    user1022 = "$root\logs\p32\scene_user_1022\home"
    usercur  = "$root\userdata\home"
    early    = "$root\userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc"
}
$outRoot = "$root\logs\p45\scenes\$Tag"
New-Item -ItemType Directory -Force $outRoot | Out-Null
$summary = "$outRoot\summary.txt"
"scene matrix '$Tag': backend $Backend, validate $($Validate.IsPresent), $Seconds s, env '$Env'" | Out-File $summary -Encoding utf8

foreach ($scene in ($Scenes -split ',' | Where-Object { $_ })) {
    if (-not $sources.ContainsKey($scene)) { throw "unknown scene '$scene'" }
    $profileDir = "$root\logs\p45\scenes\profile"
    if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
    New-Item -ItemType Directory -Force $profileDir | Out-Null
    # only the save (home\...) is copied: the profile holds nothing else
    Copy-Item $sources[$scene] "$profileDir\home" -Recurse
    foreach ($f in "gpu_device_lost.txt", "vk_validation.log", "gpu_native.txt") {
        Remove-Item "$root\logs\$f" -ErrorAction SilentlyContinue
    }
    $press = "space@12,space@22,move:-3000:0@56:0.25,move:-3000:0@58:0.25,move:-3000:0@60:0.25"
    $gameArgs = "--loader sspc --user-data $profileDir --vram-log 5 --press $press"
    if ($Backend -eq "sspc") { $gameArgs += " --gpu-backend sspc" }
    if ($Validate) { $gameArgs += " --gpu-native-validate" }
    $saved = @{}
    foreach ($kv in ($Env -split ';' | Where-Object { $_ })) {
        $n, $v = $kv -split '=', 2
        $saved[$n] = [Environment]::GetEnvironmentVariable($n)
        [Environment]::SetEnvironmentVariable($n, $v)
    }
    $runTag = "p45s_${Tag}_$scene"
    try {
        $ErrorActionPreference = "Continue"
        powershell -ExecutionPolicy Bypass -File "$root\tools\run_phase0.ps1" -Seconds $Seconds -Tag $runTag -NoReport -ExtraArgs $gameArgs 2>&1 |
            Select-Object -Last 1 | Out-Null
    } finally {
        $ErrorActionPreference = "Stop"
        foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
    }
    $out = "$outRoot\$scene"
    New-Item -ItemType Directory -Force $out | Out-Null
    foreach ($f in "gpu_device_lost.txt", "gpu_native.txt", "SecondSonPC.log", "vk_validation.log") {
        if (Test-Path "$root\logs\$f") { Copy-Item "$root\logs\$f" $out }
    }
    foreach ($f in "screen.png", "run.json") {
        if (Test-Path "$root\logs\runs\$runTag\$f") { Copy-Item "$root\logs\runs\$runTag\$f" $out }
    }
    $log = "$out\SecondSonPC.log"
    $lost = (Select-String -Path $log -Pattern "VK_ERROR_DEVICE_LOST|device is lost" -ErrorAction SilentlyContinue | Measure-Object).Count
    $errors = (Select-String -Path $log -Pattern "\[Error\]" -ErrorAction SilentlyContinue | Measure-Object).Count
    $vval = if (Test-Path "$out\vk_validation.log") { (Get-Content "$out\vk_validation.log" | Measure-Object -Line).Lines } else { "n/a" }
    $series = @()
    foreach ($m in (Select-String -Path $log -Pattern '^\[([0-9]+):([0-9]+):([0-9.]+)\].*frames \(sceGnmSubmitAndFlipCommandBuffers\) ([0-9]+)' -ErrorAction SilentlyContinue)) {
        $g = $m.Matches[0].Groups
        $series += , @(([double]$g[1].Value * 3600 + [double]$g[2].Value * 60 + [double]$g[3].Value), [double]$g[4].Value)
    }
    $fps = @()
    for ($i = 1; $i -lt $series.Count; $i++) {
        $dt = $series[$i][0] - $series[$i - 1][0]
        if ($dt -gt 0) { $fps += [math]::Round(($series[$i][1] - $series[$i - 1][1]) / $dt, 1) }
    }
    $flips = if ($series.Count -gt 0) { $series[-1][1] } else { 0 }
    $bright = "n/a"
    if (Test-Path "$out\screen.png") {
        $bright = (py -I -c "from PIL import Image, ImageStat; print(round(ImageStat.Stat(Image.open(r'$out\screen.png').convert('L')).mean[0], 1))" 2>$null)
    }
    $status = (Select-String -Path "$out\gpu_native.txt" -Pattern "^STATUS" -ErrorAction SilentlyContinue | Select-Object -First 1).Line
    $line = "{0,-9} device lost {1} | validation {2} | [Error] {3} | flips {4} | FPS windows {5} | screen mean {6} | {7}" -f `
        $scene, $lost, $vval, $errors, $flips, ($fps -join ' '), $bright, $status
    $line | Out-File $summary -Append -Encoding utf8
    $line
}
"summary: $summary"
