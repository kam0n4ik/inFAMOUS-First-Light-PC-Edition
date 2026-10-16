# SecondSon Native CP: the picture of command processor modes compared (docs/NATIVE_CP.md).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\cp_image_ab.ps1 -Tag <name>
#       [-Scene beach] [-Modes parallel,parallel,native] [-FirstFlip 3000] [-Flips 4] [-Seconds 125] [-Validate]
# Each mode runs the scene once (tools\tests\p46_runs.ps1, the same route) and is frozen with the game's own pause
# (Options = Return at 70 s: the world is still rendered every frame, the simulation stopped); the scanout image is
# captured at flips FirstFlip.. (SSPC_DIAG_CAPTURE_IMAGE). tools\tests\cp_image_compare.py compares the first run's
# images with every other run's: two runs of one mode give the noise floor (what differs between runs at all).
# Output: logs\cp\<Tag>\<mode>_<n>\ (the run, captures\).
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Scene = "beach",
    [string]$Modes = "parallel,parallel,native",
    [int]$FirstFlip = 3000,
    [int]$Flips = 4,
    [int]$Seconds = 125,
    [switch]$Validate,
    [string]$Exe = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$out = "$root\logs\cp\$Tag"
if (Test-Path $out) { throw "$out exists: pick another -Tag" }
New-Item -ItemType Directory -Force $out | Out-Null
$scanout = "26c568000" # the flip's image (RENDER_ENGINE_PASSES.md: the post context's target)
$press = "space@12,space@22,move:-3000:0@56:0.25,move:-3000:0@58:0.25,move:-3000:0@60:0.25,return@70"
$n = 0
$dirs = @()
foreach ($mode in ($Modes -split ',' | Where-Object { $_ })) {
    $n++
    $run = "${mode}_$n"
    $cap = "$out\$run\captures"
    New-Item -ItemType Directory -Force $cap | Out-Null
    $envs = "SSPC_CP_MODE=$mode;SSPC_DIAG_CAPTURE_IMAGE=${scanout}:$FirstFlip+$Flips;SSPC_DIAG_CAPTURE_DIR=$cap"
    $args46 = @("-ExecutionPolicy", "Bypass", "-File", "$root\tools\tests\p46_runs.ps1", "-Tag", $run, "-Scenes", $Scene,
        "-Repeats", "1", "-Seconds", "$Seconds", "-OutRoot", $out, "-ExtraArgs", "--press $press", "-Env", $envs)
    if ($Validate) { $args46 += "-Validate" }
    if ($Exe) { $args46 += @("-Exe", $Exe) }
    powershell @args46 | Select-Object -Last 2
    $dirs += $cap
}
py -I -X utf8 "$root\tools\tests\cp_image_compare.py" @dirs --out "$out\compare.md"
