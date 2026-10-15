@echo off
rem SecondSonPC manual test session (docs\MANUAL_TEST_PLAN.md).
rem   play.cmd            - own SecondSonPC loader (default)
rem   play.cmd shadps4    - control path (shadPS4 loader, for comparison only)
rem   play.cmd dmem       - own loader + EXPERIMENTAL native direct memory (--native kernel-dmem)
rem   play.cmd full       - own loader + every EXPERIMENTAL native subsystem of Phase 3 (P2.3-P3.6: memory,
rem                         threads, CPU patcher, exceptions, errno, clock, kernel memory, exit)
rem   play.cmd gpu        - own loader + Phase 4 diagnostics: SecondSonPC's command processor (P4.2) and shader
rem                         frontend (P4.3) beside the bridge's, frame pacing; about half the FPS (diagnostic)
rem   play.cmd all        - full + gpu
rem   play.cmd <mode> <flags...>  - further SecondSonPC.exe flags are passed as they are
set MODE=%~1
if "%MODE%"=="" set MODE=sspc
set SSPC_EXTRA=
:args
shift
if "%~1"=="" goto run
set SSPC_EXTRA=%SSPC_EXTRA% %1
goto args
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\manual_run.ps1" -Mode %MODE%
pause
