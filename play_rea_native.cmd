@echo off
rem Since 2026-10-18 play_rea.cmd itself runs on SecondSonPC's own kernel, threads, memory, exceptions, CPU patcher,
rem errno, clock and exit (the default with the own loader); this file is kept for old shortcuts.
rem The bridge's instead: play_rea.cmd --legacy-bridge-kernel
cd /d "%~dp0"
call "%~dp0play_rea.cmd" %*
