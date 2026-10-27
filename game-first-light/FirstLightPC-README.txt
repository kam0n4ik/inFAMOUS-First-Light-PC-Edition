inFAMOUS: First Light - PC Edition (FirstLightPC)
https://github.com/kam0n4ik/inFAMOUS-First-Light-PC-Edition

An early build: the game reaches gameplay; see README.md on the project page for what is checked and what is not.

1. Copy everything of this folder into the game's folder - the one with eboot.bin, sce_module and sce_sys
   (your own decrypted copy of CUSA00897). Nothing of the game's own files is replaced.
2. Install the Microsoft Visual C++ 2015-2022 Redistributable (x64) if it is not installed yet:
   https://aka.ms/vs/17/release/vc_redist.x64.exe
3. Start play.cmd (or FirstLightPC.exe: the launcher opens, press "Играть" - Play).
   The first start prepares the game's shaders in a console window (a few minutes, once);
   later starts take a few seconds.

FirstLightPC.exe       the launcher, then the game with the settings and the profile chosen in it
play.cmd               the game at once, without the launcher, with every improvement that is on for players
play_safe.cmd          the game without the performance switches (to compare when something looks wrong)
FirstLightPC.toml      defaults (window, language, keys); your changes from the launcher and the game are kept in
                       %LOCALAPPDATA%\FirstLightPC\settings.toml
userdata\              saves (copied to userdata\savedata_backups\ before every start)
cache\shaders\         the prepared shaders (safe to delete: they are prepared again)
logs\                  FirstLightPC.log - attach it to a bug report

PC settings: F1 in the game (resolution, FOV, frame rate limit up to unlimited, anti-aliasing, mouse, keys).
Cutscenes are videos recorded at 30 frames per second, as on PS4: the frame rate limit does not change them.
