inFAMOUS: Second Son - PC Edition (SecondSonPC)
https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition

1. Copy everything of this folder into the game's folder - the one with eboot.bin, sce_module and sce_sys
   (your own decrypted copy of CUSA00004, update 01.07). Nothing of the game's own files is replaced.
2. Install the Microsoft Visual C++ 2015-2022 Redistributable (x64) if it is not installed yet:
   https://aka.ms/vs/17/release/vc_redist.x64.exe
3. Start SecondSonPC.exe: the launcher opens (settings, launch profile), press "Играть" (Play).
   The first start prepares the game's shaders in a console window (a few minutes, once);
   later starts take a few seconds.

SecondSonPC.exe        the launcher, then the game with the settings and the profile chosen in it
play.cmd               the game at once, without the launcher, with every improvement that is on for players
play_experimental.cmd  play.cmd plus faster experiments (REA-3, AQ, MAIN_FREE); Neon Dash is less even
SecondSonPC.toml       defaults (window, language, keys); your changes from the launcher and the game are kept in
                       %LOCALAPPDATA%\SecondSonPC\settings.toml
userdata\              saves (copied to userdata\savedata_backups\ before every start)
cache\shaders\         the prepared shaders (safe to delete: they are prepared again)
logs\                  SecondSonPC.log - attach it to a bug report

PC settings: in the launcher, or in the game: Esc (Options) -> Settings -> "Additional settings".
Experiments: the last page of that window turns every switch of play.cmd and every experiment on or off;
             a change applies from the next start, "Reset this page" goes back to the launcher's values.
Requirements, known issues, benchmarks: README.md on the project page.
