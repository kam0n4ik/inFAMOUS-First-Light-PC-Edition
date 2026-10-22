@echo off
rem FrameBreak: the game with every measured improvement - the Native CP preset (early frame submission, shader code
rem trust, register journal of changes, SRT walks by lines; docs/NATIVE_CP.md, docs/GPU_OPTIMIZATION.md) and the
rem frame rate limit from the settings ([Display] frameRateLimit; docs/FRAMEBREAK.md).
rem (play.cmd finds tools\manual_run.ps1 from the current directory once it shifted its arguments)
cd /d "%~dp0"
set SSPC_CP_MODE=native
rem the city's measured CPU optimizations of the draw path (docs/CITY_PERFORMANCE.md, cp_mode.cpp)
set SSPC_CITY_PERFORMANCE=1
rem the measurement stand's other switches (E:\research\r6\cp\run_engine_extra.ps1; city A/B 10-22: 24.8 -> 31.9 FPS with the preset)
set SSPC_REA_MERGE_RUNS=1
set SSPC_CP_ROUND_NODRAIN=1
set SSPC_P411_ASYNC_ACQUIRE=1
set SSPC_ASYNC_PIPELINES=1
call "%~dp0play_native.cmd" %*
