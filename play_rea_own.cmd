@echo off
rem play_rea.cmd with SecondSonPC's own GCN translator for the compute, vertex and pixel programs
rem (SSPC_OWN_SHADERS=cs,vs,ps; the rect / quad lists' auxiliary tessellation stages are its own too; a program it
rem does not translate stays the bridge's, the reason in logs\SecondSonPC.log). Its shader cache is a folder of its
rem own (...\native_own); the main cache (...\native) is not touched. docs\OWN_SHADER_TRANSLATOR.md
rem Each start first seeds that cache from the main one when the own translator (build\bin\own_translator_generation.txt)
rem or its stages changed (tools\seed_own_shader_cache.py): the records the bridge made for these stages are left out,
rem so the preparation window makes them with the own translator before the game.
setlocal
cd /d "%~dp0"
set SSPC_OWN_SHADERS=cs,vs,ps
python -I "%~dp0tools\seed_own_shader_cache.py" --stages %SSPC_OWN_SHADERS%
if errorlevel 1 (
    echo play_rea_own: the own translator's shader cache was not seeded ^(tools\seed_own_shader_cache.py^) - not started,
    echo so that no module of an earlier own translator is used. Is Python on PATH?
    endlocal
    exit /b 1
)
call "%~dp0play_rea.cmd" %*
endlocal
