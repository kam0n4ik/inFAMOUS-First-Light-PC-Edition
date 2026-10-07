# Isolated reference run of the stock shadPS4 build (portable mode, own user\ dir).
# Never touches %APPDATA%\shadPS4. Stops only the process it started.
param(
    [int]$Lang = 1,
    [int]$Seconds = 100,
    [string]$Tag = "lang1"
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC\runtime\shadps4-ref"
$cfgPath = Join-Path $root "user\config.json"
# Targeted edit: PS 5.1 ConvertTo-Json mangles arrays, so only the one field is replaced.
$text = [IO.File]::ReadAllText($cfgPath)
$text = [regex]::Replace($text, '"console_language":\s*\d+', "`"console_language`": $Lang")
[IO.File]::WriteAllText($cfgPath, $text, (New-Object Text.UTF8Encoding($false)))

$log = Join-Path $root "user\log\shad_log.txt"
if (Test-Path $log) { Remove-Item $log -Force }

$p = Start-Process -FilePath (Join-Path $root "shadPS4.exe") `
    -ArgumentList @("-g", "E:\shdps4\CUSA00004\eboot.bin") `
    -WorkingDirectory $root -PassThru
"started pid=$($p.Id) lang=$Lang"
try { Wait-Process -Id $p.Id -Timeout $Seconds -ErrorAction Stop; "exited by itself code=$($p.ExitCode)" }
catch { Stop-Process -Id $p.Id -Force; "stopped after $Seconds s" }

New-Item -ItemType Directory -Force "E:\SecondSonPC\logs\reference" | Out-Null
$dst = "E:\SecondSonPC\logs\reference\trace_$Tag.txt"
Copy-Item $log $dst -Force
"log -> $dst ($((Get-Item $dst).Length) bytes)"
