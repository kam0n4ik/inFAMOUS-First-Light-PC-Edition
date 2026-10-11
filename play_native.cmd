@echo off
rem SecondSonPC with its own Vulkan backend (--gpu-backend sspc): the P4.7 foliage optimizations are on by default
rem there (docs\PHASE4_FOLIAGE_OPTIMIZATION.md). Same session as play.cmd (save backup, logs\manual\...).
rem   play_native.cmd <flags...>  - further SecondSonPC.exe flags are passed as they are
call "%~dp0play.cmd" sspc --gpu-backend sspc %*
