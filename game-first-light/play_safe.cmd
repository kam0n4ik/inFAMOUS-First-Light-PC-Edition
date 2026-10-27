@echo off
rem play.cmd without its performance switches: the runtime's conservative defaults, for comparing when something
rem looks wrong with play.cmd. Arguments are passed on to FirstLightPC.exe.
cd /d "%~dp0"
if not exist "%~dp0eboot.bin" (
    echo eboot.bin is not in this folder - put FirstLightPC into the game's folder ^(README.md, Installation^).
    pause
    exit /b 1
)
"%SystemRoot%\System32\tasklist.exe" /fi "imagename eq FirstLightPC.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "FirstLightPC.exe" >nul
if not errorlevel 1 (
    echo FirstLightPC.exe is already running.
    pause
    exit /b 1
)
set SSPC_R10_AJM=native
set SSPC_R10_SYSRT=native
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
if exist "%~dp0userdata\home" if defined STAMP xcopy /e /i /q /y "%~dp0userdata\home" "%~dp0userdata\savedata_backups\%STAMP%" >nul
start "" /wait /d "%~dp0" "%~dp0FirstLightPC.exe" --no-launcher %*
