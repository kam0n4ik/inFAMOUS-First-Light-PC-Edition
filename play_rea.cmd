@echo off
rem REA (docs/REA_ENGINE_REVERSE.md): play_unlimited.cmd plus the combination measured faster in every A/B pair
rem (logs/p53/rea5_ab1) and clean under Vulkan + Synchronization Validation:
rem   SSPC_REA_MERGE_RUNS=1      the 542 MiB vertex heap's constants in video memory (GPU -1.5..-2 ms)
rem   SSPC_CP_ROUND_NODRAIN=1    the command processor's round end without waiting for the recording thread
rem   SSPC_P411_ASYNC_ACQUIRE=1  met memory waits without waiting for the recording thread
rem (NOT SSPC_REA_DEFER_WAIT_B: the game then reuses its compute command memory before the processor read it -
rem  "type-0 packet on queue 1", the particles' queue stops and the game hangs; 2026-10-18 hand play)
cd /d "%~dp0"
rem (cleared: a console that ran an earlier version of this file keeps the variable)
set SSPC_REA_DEFER_WAIT_B=
set SSPC_CP_MODE=native
set SSPC_REA_MERGE_RUNS=1
set SSPC_CP_ROUND_NODRAIN=1
set SSPC_P411_ASYNC_ACQUIRE=1
rem new pipelines (graphics and compute) built on 3 background threads; their draws/dispatches skipped until ready
set SSPC_ASYNC_PIPELINES=2
call "%~dp0play_native.cmd" %*
