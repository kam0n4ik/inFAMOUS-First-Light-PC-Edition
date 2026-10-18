@echo off
rem play_rea.cmd with every Phase 2-3 native subsystem of SecondSonPC instead of the shadPS4 bridge
rem (kernel, threads, memory, exceptions, CPU patcher, errno, clock, exit) - before they become the default
cd /d "%~dp0"
call "%~dp0play_rea.cmd" --native cpu --native exceptions --native threads --native memory --memory-commit on-demand --native errno --native clock --native kernel-memory --native exit %*
