# Configure + build SecondSonPC.exe (E:\SecondSonPC\build\bin\SecondSonPC.exe).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\build.ps1 [-Config RelWithDebInfo] [-Fresh]
param(
    [string]$Config = "RelWithDebInfo",
    [switch]$Fresh
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"

$src = "E:\SecondSonPC"
$bld = "E:\SecondSonPC\build"
$log = "E:\SecondSonPC\logs\build.log"
$jobs = [Environment]::ProcessorCount
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
# Windows PowerShell 5.1 turns native stderr into errors; run the tools through cmd and log to a file.
$ErrorActionPreference = "Continue"

if ($Fresh -or -not (Test-Path "$bld\build.ninja")) {
    cmd /c "cmake -S $src -B $bld -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER_TYPE=LLD -DCMAKE_RC_COMPILER=rc.exe -DCMAKE_MT=mt.exe > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE), see $log" }
}
cmd /c "cmake --build $bld --target shadps4 --parallel $jobs >> `"$log`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE), see $log" }
Get-Item "$bld\bin\SecondSonPC.exe" | Select-Object FullName, Length, LastWriteTime
