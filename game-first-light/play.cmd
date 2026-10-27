@echo off
rem inFAMOUS: First Light - PC Edition. Lives in the game's folder (the one with eboot.bin) next to FirstLightPC.exe.
rem Starts the game with Second Son's measured improvements that do not depend on Second Son's engine hooks (the
rem native command processor preset, the PM4-level batching of the city preset, the second Vulkan recording thread
rem and its batching, F1, F3, R10's own audio and system libraries). The parts that are Second Son's engine hooks
rem (CITY_ENGINE_OPT, G-buffer runs, early submit) are off: on First Light they would only log "another game version".
rem Saves: userdata\ here, copied to userdata\savedata_backups\<date_time> before every start.
rem Arguments are passed on to FirstLightPC.exe.
cd /d "%~dp0"
if not exist "%~dp0eboot.bin" (
    echo eboot.bin is not in this folder - put FirstLightPC into the game's folder ^(README.md, Installation^).
    pause
    exit /b 1
)
rem Windows' own find by its path: with Git's Unix tools in PATH a bare "find" is the Unix one
"%SystemRoot%\System32\tasklist.exe" /fi "imagename eq FirstLightPC.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "FirstLightPC.exe" >nul
if not errorlevel 1 (
    echo FirstLightPC.exe is already running.
    pause
    exit /b 1
)
set SSPC_CP_MODE=native
set SSPC_CITY_PERFORMANCE=1
set SSPC_CITY_ENGINE_OPT=0
set SSPC_CITY_ENGINE_GBUFFER_RUNS=0
set SSPC_P54_EARLY_SUBMIT=0
set SSPC_REA_MERGE_RUNS=1
set SSPC_CP_ROUND_NODRAIN=1
set SSPC_P411_ASYNC_ACQUIRE=1
set SSPC_ASYNC_PIPELINES=1
set SSPC_P53_FRONTEND_THREAD=1
set SSPC_CP_ASYNC_WRITES=1
set SSPC_CP_ASYNC_FLIP=1
set SSPC_VK_EMIT_THREAD=1
set SSPC_VK_EMIT_BATCH=1
set SSPC_CP_RING=128
set SSPC_R9_PCACHE=1
set SSPC_R9_HEAP_WINDOW=1
set SSPC_R10_AJM=native
set SSPC_R10_SYSRT=native
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
if exist "%~dp0userdata\home" if defined STAMP xcopy /e /i /q /y "%~dp0userdata\home" "%~dp0userdata\savedata_backups\%STAMP%" >nul
start "" /wait /d "%~dp0" "%~dp0FirstLightPC.exe" --no-launcher %*
