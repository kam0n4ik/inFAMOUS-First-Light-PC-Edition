@echo off
rem play_rea.cmd with SecondSonPC's own GCN translator for the compute programs (SSPC_OWN_SHADERS=cs; the graphics
rem programs, and each compute program it does not translate yet, stay the bridge's). Its shader cache is a folder of
rem its own (...\native_own); the main cache (...\native) is not touched.
rem docs\OWN_SHADER_TRANSLATOR.md
rem The first start seeds it from the main cache (tools\seed_own_shader_cache.py): the graphics modules and pipelines
rem prepared (the bridge's in both), the compute ones left to the own translator.
setlocal
cd /d "%~dp0"
python -I "%~dp0tools\seed_own_shader_cache.py"
set SSPC_OWN_SHADERS=cs
call "%~dp0play_rea.cmd" %*
endlocal
