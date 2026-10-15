@echo off
rem SecondSonPC with its own Vulkan backend (--gpu-backend sspc) and the upscaling / frame generation build
rem (docs\UPSCALING_FRAME_GENERATION.md). Settings: F1 -> "Upscaling & frame generation" (saved to
rem %LOCALAPPDATA%\SecondSonPC\settings.toml; technology, quality and frame generation apply after a restart).
rem   play_upscale.cmd                 - as chosen in F1
rem   play_upscale.cmd dlss            - DLSS Quality for this run
rem   play_upscale.cmd fsr             - FSR 3.1 Quality for this run
rem   play_upscale.cmd fsr fg          - FSR 3.1 Quality + FSR frame generation for this run
rem   play_upscale.cmd dlss fg         - DLSS Quality + FSR frame generation for this run
if not "%~1"=="" set SSPC_UPSCALER=%~1
if "%~2"=="fg" set SSPC_FRAME_GEN=fsr
cd /d E:\SecondSonPC
"E:\SecondSonPC\build\upscale\SecondSonPC.exe" --gpu-backend sspc
