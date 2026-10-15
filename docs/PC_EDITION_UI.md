# PC Edition: settings in the game's own menu, the mouse in its menus

Stage 1 of the PC Edition (2026-10-16). The PC settings (display, graphics, DLSS / FSR / frame generation, sound,
keyboard, controller, interface, shaders) are a page of the game's own Settings screen (Esc / Options during play),
drawn as the game draws that screen; the F1 menu is gone (F1 only with `SSPC_DEV_F1=1`, for development).

## Why not a new entry in the game's menu itself

The game's menus are compiled bytecode of Sucker Punch's own script VM (`art/cache/core_common.sprig.xpps`, names by
hash h·101+c). Adding an entry there means reversing that VM: not done. What is done instead, from the most to the
least "native":

1. **The item is the game's.** The Settings list ends with "HDR Rendering — Unavailable" (`u_options_opt_hdr`,
   `u_options_val_hdr_unavailable` of `lang_<language>_text_p.xpps`), an option a PC never has. The file system layer
   replaces these two strings as the game reads the table (`src/runtime/filesystem/content_patch.*`; the files on disk
   are unchanged): the game itself shows **"Дополнительные настройки — Нажмите ⊗" / "Additional Settings — Press ⊗"**
   in its own list and font (⊗ is the game font's glyph U+F08E, as in `u_button_press_yes`). Log:
   `lang_russian_text_p.xpps: 2 of the game's strings replaced as read`.
2. **The page is ours, drawn as the game's.** When that item is chosen (cross: its key, the controller's, a click),
   `SettingsMenu` opens over the game's Settings panel (`MenuEnv::game_panel`) and draws, from measurements of the
   game's 1920×1080 frame: the panel 315,92 742×895 with its white frame, corner square, top tab and grid; the yellow
   title; one list of every section (white slab capitals), items and values (condensed sans, 35 px rows, the chosen
   one yellow), ten dashes for sliders, the grey scroll bar. The game's screen stays under it (its controller picture,
   "○ Back"). Faces: Roboto Slab (Apache 2.0) and Roboto Condensed (OFL), `assets/fonts`, narrowed to the game's
   proportions (84 % / 92 %); without them Bahnschrift / Segoe UI.

Which screen is open is known from probes on the game's pausing-screen functions (`src/engine/ui/game_menu.*`:
`0x27db10` opened, `0x27e210` closed; the Settings screen is kind 7).

## The mouse in the game's menus

The game has no cursor and SecondSonPC has no list of its widgets. While its Settings screen is in front:

- the native backend copies two narrow strips of every presented frame to host memory (the item column x 410..538
  and the scroll bar x 1028..1036, y 180..1000; `src/runtime/gpu/menu_focus.*`, `Executor::MenuFocusProbe`): the
  chosen item is the yellow row, the list's end is shown when the grey thumb is at the bottom;
- pointing at an item (the pointer at rest 90 ms) moves the game's own focus there with d-pad presses (at most 6 at
  once, then the frame is read again); a click on an item's name presses ✕ there, on its value ← / → (a choice, a
  volume bar); the wheel is ↑ / ↓; the right button is ○ (back);
- the cursor is free in every pausing screen and is captured again when the game runs.

Presses are queued in `KeyboardMouse::MenuPress`: each lasts one controller reading with a released reading after it.
The game's code and data stay unchanged. Limits: the frame reading needs the native backend (`--gpu-backend sspc`,
now the default); with the bridge a click presses ✕ on the chosen item. Other pausing screens (map, pause) get the
wheel, ○ and a free cursor, not pointing.

## Settings, storage

One store: `%LOCALAPPDATA%\SecondSonPC\settings.toml` over `SecondSonPC.toml` (no second file). Display changes ask
for confirmation and are restored after 15 s. Settings that apply after a restart carry "*" and say so. Unavailable
settings say why ("Недоступно" with the reason), none is shown working when it is not.

## Other changes of the stage

- `--gpu-backend sspc` is the default; the shadPS4 bridge's presenter only with `--gpu-backend bridge` (older
  measurement scripts that relied on the bridge default must pass it).
- F1 is no longer a reserved key (F10 FPS, F11 full screen stay); the controller shortcut (PS / L3+R3) is off by
  default.
- The input log records every press and release sent to the game (`keyboard/mouse buttons: +L2 ... +cross`).

## Checks

- `sspc_settings_tests`: every check and shot passes, including the game page (`ru_1080_page*.png`,
  `en_900_page.png`).
- In the game (2026-10-16, user): the item, the page, the mouse, saving — "всё работает"; log: the strings replaced,
  the page open 3.5 min (986 renders, slowest 20 ms), settings.toml written.

# Stage 2: the window title

The game window is titled exactly **`inFAMOUS: Second Son — PC Edition`** (UTF-8, the dash is U+2014 EM DASH).

- One definition: `SecondSon::App::kWindowTitle` in `src/app/product.h` (a `static_assert` checks its bytes).
- The only path that creates the game window, `src/bridge/shadps4/bootstrap.cpp`, uses it; full screen (F11), a new
  swapchain and a resize keep the same window, so the title never changes while the game runs.
- After the window is made, the title is read back through SDL and Win32 (`GetWindowTextW`) and compared with the
  definition; log: `game window title verified (SDL and Win32, 33 UTF-16 units)`, an error otherwise.
- Not changed: save paths, the title id CUSA00004, internal names. The debugging path `--stock-bootstrap` (shadPS4's own
  start, kept for comparisons only) keeps shadPS4's title: it is not the PC Edition.
- Checked in the game (2026-10-16): windowed 1296×759 and full screen 1920×1080 after F11 — the title exact, with the
  dash; the user confirmed.
