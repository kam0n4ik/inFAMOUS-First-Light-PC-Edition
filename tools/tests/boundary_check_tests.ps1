# Tests of the SecondSonPC <-> bridge boundary check (cmake\sspc_boundary_check.cmake, P3.6.6) on small fixture
# trees in E:\SecondSonPC\.tmp\boundary_fixture: a clean tree passes; each kind of violation fails with its message;
# what only looks like a violation (a comment, a string, an own header) passes.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\boundary_check_tests.ps1
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
. "$root\tools\env.ps1" *> $null
$base = "$root\.tmp\boundary_fixture"
$script = "$root\cmake\sspc_boundary_check.cmake"
$passed = 0
$failed = 0

function Write-Fixture([string]$dir, [hashtable]$files) {
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    foreach ($rel in $files.Keys) {
        $path = Join-Path $dir $rel
        New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
        [System.IO.File]::WriteAllText($path, $files[$rel])
    }
    foreach ($sub in 'src', 'shad', 'patches') { New-Item -ItemType Directory -Force (Join-Path $dir $sub) | Out-Null }
    if (-not (Test-Path "$dir\allow.txt")) { [System.IO.File]::WriteAllText("$dir\allow.txt", "") }
}

function Test-Case([string]$name, [hashtable]$files, [bool]$expectPass, [string]$expectText) {
    $dir = Join-Path $base $name
    Write-Fixture $dir $files
    $ErrorActionPreference = "Continue" # cmake's messages on stderr are output here, not errors
    $out = & cmake "-DSSPC_SRC=$dir/src" "-DSSPC_SHAD=$dir/shad" "-DSSPC_PATCHES=$dir/patches" `
        "-DSSPC_ALLOWLIST=$dir/allow.txt" "-DSSPC_REPORT=$dir/report.txt" -P $script 2>&1 | Out-String
    $ok = ($LASTEXITCODE -eq 0) -eq $expectPass -and ($expectText -eq "" -or $out.Contains($expectText))
    if ($ok) { $script:passed++; Write-Output "PASS  $name" }
    else { $script:failed++; Write-Output "FAIL  $name (exit ${LASTEXITCODE}, expected pass $expectPass, text '$expectText')"; Write-Output $out }
}

$own = @{ 'src/runtime/a.h' = "#pragma once`n"; 'src/runtime/a.cpp' = "#include `"runtime/a.h`"`n#include <vector>`n" }

Test-Case "clean" $own $true "own runtime 2 files clean"

Test-Case "own-includes-bridge-header" (@{ 'src/runtime/a.cpp' = "#include `"core/memory.h`"`n" }) $false `
    "includes core/memory.h, which is not a SecondSonPC header"

Test-Case "own-includes-adapter-layer" (@{ 'src/bridge/shadps4/x.h' = "#pragma once`n";
        'src/runtime/a.cpp' = "#include `"bridge/shadps4/x.h`"`n" }) $false "includes the adapter layer's bridge/shadps4/x.h"

Test-Case "own-declares-bridge-namespace" (@{ 'src/runtime/a.cpp' = "namespace Libraries::Kernel {`nint f();`n}`n" }) $false `
    "names the bridge's namespace"

Test-Case "own-calls-bridge-symbol" (@{ 'src/runtime/a.cpp' = "void g() { Core::GetGuestVirtualMemory(); }`n" }) $false `
    "names the bridge's namespace: void g() { Core::GetGuestVirtualMemory()"

Test-Case "own-mentions-in-comment-and-string" (@{ 'src/runtime/a.cpp' = "// the bridge's Core::Linker`nconst char* s = `"Libraries::Kernel`"; /* Common::Log */`nint SecondSon_Core = 0;`n" }) $true ""

Test-Case "adapter-without-allowlist-line" (@{ 'src/bridge/shadps4/x.cpp' = "#include `"core/memory.h`"`n" }) $false `
    "has no line in the allowlist"

Test-Case "adapter-new-dependency" (@{ 'src/bridge/shadps4/x.cpp' = "#include `"core/memory.h`"`n#include `"core/tls.h`"`n";
        'allow.txt' = "src/bridge/shadps4/x.cpp | memory | a reason | core/memory.h`n" }) $false `
    "new direct dependency on the bridge's core/tls.h"

Test-Case "adapter-allowlisted" (@{ 'src/bridge/shadps4/x.cpp' = "#include `"core/memory.h`"`n";
        'allow.txt' = "src/bridge/shadps4/x.cpp | memory | a reason | core/memory.h core/gone.h`n" }) $true ""

Test-Case "allowlist-without-reason" (@{ 'src/bridge/shadps4/x.cpp' = "#include `"core/memory.h`"`n";
        'allow.txt' = "src/bridge/shadps4/x.cpp | memory |  | core/memory.h`n" }) $false "has no owner or no reason"

Test-Case "patch-includes-sspc-header" (@{ 'src/runtime/k.h' = "#pragma once`n";
        'patches/0001-x.patch' = "--- a/src/core/x.cpp`n+++ b/src/core/x.cpp`n@@ -1 +1,2 @@`n+#include `"runtime/k.h`"`n" }) $false `
    "the bridge includes SecondSonPC's runtime/k.h"

Test-Case "patch-includes-allowed-header" (@{ 'src/bridge/shadps4/d.h' = "#pragma once`n";
        'patches/0001-x.patch' = "+#include `"bridge/shadps4/d.h`"`n";
        'allow.txt' = "bridge -> bridge/shadps4/d.h | diagnostics | a reason`n" }) $true ""

# The report of a passing tree lists the stale allowlist entry.
$report = Get-Content "$base\adapter-allowlisted\report.txt" -Raw
if ($report.Contains("stale allowlist entries (no longer included): core/gone.h")) { $passed++; Write-Output "PASS  stale entry reported" }
else { $failed++; Write-Output "FAIL  stale entry not reported"; Write-Output $report }

Write-Output "boundary check tests: $passed passed, $failed failed"
exit $(if ($failed -eq 0) { 0 } else { 1 })
