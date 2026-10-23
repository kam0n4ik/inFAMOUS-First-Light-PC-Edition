@echo off
rem play.cmd plus two experiments that are off for players until they are confirmed (docs/R9_REA3_FIX.md):
rem   REA-3 - the game's second async compute wait of a frame done by the GPU,
rem   AQ    - the game's compute on a Vulkan queue of its own, beside the graphics.
rem R9 A/B on a save: +7 % FPS (REA-3), +11 % (REA-3 + AQ), but frame pacing is less even than with play.cmd
rem (more frames over 50 ms).
set SSPC_PLAY_EXPERIMENTAL=1
call "%~dp0play.cmd" %*
