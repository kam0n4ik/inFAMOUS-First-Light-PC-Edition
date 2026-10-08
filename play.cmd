@echo off
rem SecondSonPC manual test session (docs\MANUAL_TEST_PLAN.md).
rem   play.cmd            - own SecondSonPC loader (default)
rem   play.cmd shadps4    - control path (shadPS4 loader, for comparison only)
set LOADER=%1
if "%LOADER%"=="" set LOADER=sspc
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\manual_run.ps1" -Loader %LOADER%
pause
