@echo off
rem play_rea.cmd with the bridge's (shadPS4's) shader recompiler instead of SecondSonPC's own GCN translator: the
rem reference for an A/B of a picture (SSPC_OWN_SHADERS=bridge; its shader cache is the main one, ...\native).
setlocal
cd /d "%~dp0"
set SSPC_OWN_SHADERS=bridge
call "%~dp0play_rea.cmd" %*
endlocal
