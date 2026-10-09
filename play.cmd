@echo off
rem SecondSonPC manual test session (docs\MANUAL_TEST_PLAN.md).
rem   play.cmd            - own SecondSonPC loader (default)
rem   play.cmd shadps4    - control path (shadPS4 loader, for comparison only)
rem   play.cmd dmem       - own loader + EXPERIMENTAL native direct memory (--native kernel-dmem)
set LOADER=%1
set EXTRA=
if "%LOADER%"=="" set LOADER=sspc
if /i "%LOADER%"=="dmem" (
    set LOADER=sspc
    set EXTRA=-NativeMemory
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\manual_run.ps1" -Loader %LOADER% %EXTRA%
pause
