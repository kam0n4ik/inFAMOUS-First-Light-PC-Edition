# Reproducible E:-only toolchain for SecondSonPC.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\setup_toolchain.ps1
# - LLVM 20.1.8 (clang-cl, lld-link)  -> E:\SecondSonPC\toolchain\LLVM      (NSIS installer, /S /D=...)
# - Windows SDK 10.0.26100 (NuGet: Microsoft.Windows.SDK.CPP, .CPP.x64, .BuildTools)
#                                     -> E:\SecondSonPC\toolchain\winsdk    (extracted, nothing installed)
# - MSVC STL/CRT 14.44, CMake 3.31.6, Ninja 1.12.1 come from the existing E:\BuildTools.
# The installed Windows Kits root (C:\Program Files (x86)\Windows Kits\10) is intentionally NOT used.
$ErrorActionPreference = "Stop"
$tc = "E:\SecondSonPC\toolchain"
$dl = "$tc\downloads"
New-Item -ItemType Directory -Force $dl | Out-Null

$sdkVer = "10.0.26100.9169"
$items = @(
    @{ File = "LLVM-20.1.8-win64.exe"
       Url  = "https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/LLVM-20.1.8-win64.exe"
       Sha  = "3197846a2b19063687dd56e93e34cd941e3548d907f23a6131571321bdf9fe7b" },
    @{ File = "microsoft.windows.sdk.cpp.$sdkVer.nupkg"
       Url  = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/$sdkVer/microsoft.windows.sdk.cpp.$sdkVer.nupkg"
       Sha  = "475269434dcd808a67853773272f972c3229c0e10c3ddc821290e70cc0f6904d"; Dir = "cpp" },
    @{ File = "microsoft.windows.sdk.cpp.x64.$sdkVer.nupkg"
       Url  = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp.x64/$sdkVer/microsoft.windows.sdk.cpp.x64.$sdkVer.nupkg"
       Sha  = "df6226a051e320942abfbd57848b43d18772996ecd66beadad240f2a56ed2f7b"; Dir = "cpp.x64" },
    @{ File = "microsoft.windows.sdk.buildtools.$sdkVer.nupkg"
       Url  = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.buildtools/$sdkVer/microsoft.windows.sdk.buildtools.$sdkVer.nupkg"
       Sha  = "6000c971fc9155052a8359779b30b6682c39e091664d83b3852c4702ab6d238e"; Dir = "buildtools" }
)

foreach ($it in $items) {
    $path = Join-Path $dl $it.File
    if (-not (Test-Path $path)) { Invoke-WebRequest -UseBasicParsing -Uri $it.Url -OutFile $path }
    $sha = (Get-FileHash $path -Algorithm SHA256).Hash.ToLower()
    if ($sha -ne $it.Sha) { throw "SHA-256 mismatch for $($it.File): $sha" }
    if ($it.Dir) {
        $dst = "$tc\winsdk\$($it.Dir)"
        if (-not (Test-Path $dst)) { Expand-Archive -LiteralPath $path -DestinationPath $dst }
    }
}

if (-not (Test-Path "$tc\LLVM\bin\clang-cl.exe")) {
    $p = Start-Process -FilePath (Join-Path $dl "LLVM-20.1.8-win64.exe") -ArgumentList "/S", "/D=$tc\LLVM" -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "LLVM installer failed: $($p.ExitCode)" }
}
& "$tc\LLVM\bin\clang-cl.exe" --version
