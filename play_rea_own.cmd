@echo off
rem play_rea.cmd with SecondSonPC's own GCN translator for the compute programs (SSPC_OWN_SHADERS=cs; the graphics
rem programs, and each compute program it does not translate yet, stay the bridge's). Its shader cache is a folder of
rem its own (...\native_own): the first start prepares the shaders once, the main cache (...\native) is not touched.
rem docs\OWN_SHADER_TRANSLATOR.md
setlocal
cd /d "%~dp0"
set SSPC_OWN_SHADERS=cs
call "%~dp0play_rea.cmd" %*
endlocal
