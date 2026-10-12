@echo off
rem SecondSonPC with its own Vulkan backend and the P4.10 parallel command processor (experimental, off by default):
rem the PM4 command processor and the shader frontend on one thread, the Vulkan recording on another
rem (docs\PHASE4_P410_PARALLEL_COMMAND_PROCESSING.md). Otherwise the same as play_native.cmd.
rem   play_parallel.cmd <flags...>  - further SecondSonPC.exe flags are passed as they are
setlocal
set SSPC_P410_PARALLEL_CP=1
call "%~dp0play_native.cmd" %*
endlocal
