@echo off
rem play_rea.cmd with SecondSonPC's own GCN translator for compute, vertex and pixel programs - since 2026-10-19 the
rem default, so this is play_rea.cmd itself (kept for its name). play_rea_bridge.cmd: the bridge's translator, the
rem reference for an A/B. The own translator's cache (...\native_own) is seeded from the main one by
rem tools\manual_run.ps1 before each start (tools\seed_own_shader_cache.py). docs\OWN_SHADER_TRANSLATOR.md
setlocal
cd /d "%~dp0"
set SSPC_OWN_SHADERS=
call "%~dp0play_rea.cmd" %*
endlocal
