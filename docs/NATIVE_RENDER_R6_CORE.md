# R6-Core: Native Render Bridge, общая инфраструктура R6

Фаза R6 (`feature/r6-core`, worktree `E:\SecondSonPC-native-r5`). Адреса — смещения eboot CUSA00004 (база 0).
Статусы — строго по шкале ANALYZED → GENERATED → OFFLINE_VERIFIED → GAME_VERIFIED → PERFORMANCE_VERIFIED; одна
ступень не подразумевает следующую. Метки: [A] анализ кода, [E] измерение, [?] не разобрано.

## 1. Исходная точка (R6.0) [E]

| Что | Значение |
|---|---|
| база R5 | `feature/native-engine-r5` @ `f08729a` (R5-D `bbf1dc6`, R5-E/F влиты `346a403…4fc5ef5`); `main` @ `6520f5d` — предок |
| база R6 | `feature/r6-core` — коммит R6.0 поверх `f08729a` (этот документ, §3) |
| офлайн | `sspc_native_engine_tests --eboot … --recomp E:\research\native-r5\recomp7\recomp.dll`: **228 проверок, 0 ошибок** |
| шейдерный кэш | `src/runtime/gpu/shader/*`, `gcn/*` не отличаются от `main` → поколение кэша то же, что у `E:\SecondSonPC\cache\shaders` |

## 2. Ветки и worktree

| Направление | Ветка | Worktree | Каталог экспериментов |
|---|---|---|---|
| R6-Core (bridge, интеграция, A/B) | `feature/r6-core` | `E:\SecondSonPC-native-r5` | `E:\research\r6\core` |
| R6-G (G-buffer) | `feature/r6-gbuffer` | `E:\SecondSonPC-r6-gbuffer` | `E:\research\r6\gbuffer` |
| R6-S (тени) | `feature/r6-shadows` | `E:\SecondSonPC-r6-shadows` | `E:\research\r6\shadows` |
| R6-P (CPU / CP) | `feature/r6-cp-optimization` | `E:\SecondSonPC-r6-cp` | `E:\research\r6\cp` |

Все три ветки агентов — от коммита R6.0. Ни одна не сливается в `main` и не публикуется (push) без разрешения
пользователя. `third_party\shadPS4` и `third_party\upscalers` в worktree агентов — junction на `E:\SecondSonPC`
(только чтение), `runtime\` — копия.

## 3. Владение общими файлами

Чтобы четыре ветки не правили одни и те же строки, в R6.0 сделаны точки расширения — у каждой один владелец:

| Файл | Владелец | Что туда добавляют |
|---|---|---|
| `src/native_engine/r6/r6_gbuffer.h` → `SSPC_R6_GBUFFER_SPECS` | R6-G | `#include` своих реконструкций и `&Reconstructed::kXxxSpec,` |
| `src/native_engine/r6/r6_shadows.h` → `SSPC_R6_SHADOWS_SPECS` | R6-S | то же |
| `src/native_engine/r6/r6_core.h` → `SSPC_R6_CORE_SPECS` | R6-Core | то же |
| `cmake/r6/<подсистема>.cmake` | владелец подсистемы | `SSPC_R6_<X>_SOURCES` (в `sspc_runtime`), `SSPC_R6_<X>_TEST_SOURCES` (в `sspc_native_engine_tests`), свойства файлов (`/arch:AVX`) |
| `tools/native_engine_tests/r6_<подсистема>_tests.cpp` | владелец подсистемы | `R6<X>Tests(image_base, image_size, recomp_module, recomp_dll, cases, checks)` — вызывается из `main.cpp` до установки переходов на образ |
| `cmake/r6/cp.cmake` | R6-P | исходники оптимизаций CP (`SSPC_R6_CP_SOURCES`) и, при желании, своя тестовая цель |

Только R6-Core правит: `native_function_registry.{h,cpp}`, `native_engine_install.cpp`, `native_dispatch.h`,
`guest_code_clone.*`, `native_engine_verify.*`, `native_measure.*`, `engine/guest_hooks.*`, `engine/guest_probe.*`,
`CMakeLists.txt`, `tools/native_engine_tests/main.cpp`, `tools/native-reconstruction/recomp/pcode2c.py`,
`tools/native-reconstruction/run_native_engine.ps1`. Агенту, которому нужно изменить один из них, — отдельный
маленький коммит в своей ветке с пометкой `[shared]` в заголовке и описанием в своём отчёте; R6-Core переносит его
первым при интеграции (или делает сам и сообщает хеш).

`kMaxFunctions` поднят до 48 (было 16 при 11 занятых).

Нельзя трогать никому: `src/runtime/gpu/shader/*`, `src/runtime/gpu/gcn/*`, `src/bridge/shadps4/shader_translator_host.cpp`,
`shader_bridge_hooks.h` — их содержимое задаёт поколение шейдерного кэша; любое изменение заставит игру пользователя
заново готовить все шейдеры.

## 4. Стенд: сборки, игровые прогоны, A/B

- **Сборка** — только `E:\research\sspc_build.ps1 -Worktree <свой> -Owner <r6-x> -Log E:\research\r6\<x>\build.log`
  (ждёт, пока нет `sspc_bench.lock` и `sspc_game.lock`; берёт `sspc_build.lock` атомарно через `mkdir`; освобождает в
  `finally`). Цели: `shadps4` (это `SecondSonPC.exe`, OUTPUT_NAME), `sspc_native_engine_tests`. Никаких `cmake --build` в обход. На ПК нет файла подкачки: одна сборка за раз, `-Jobs 3..4`.
- **Игра** — только `tools\native-reconstruction\run_native_engine.ps1 -Worktree <свой> -Out E:\research\r6\<x>\runs
  -LogsDir E:\research\r6\<x>\logs -Owner r6-<x> -Scene city|beach|harbor|village -Tag <тег>`. Скрипт ждёт, пока нет
  чужого `sspc_bench.lock`, `sspc_game.lock`, `sspc_build.lock` и процесса `SecondSonPC`, затем 300 с тишины, берёт
  `sspc_game.lock` через `mkdir` (атомарно), после — повторно проверяет серию и процесс; освобождает в `finally`.
  Окна замера: город 70…148 с (150 с прогон), пляж/гавань/деревня 62…88 с (90 с прогон).
- **Серия FPS (A/B)** — держит `E:\research\sspc_bench.lock` (каталог, `owner.txt` = `<владелец> …`) всё время серии,
  чтобы ни сборка, ни чужой прогон не встали между парами. Финальные A/B — у R6-Core; агенты делают свои локальные
  A/B той же схемой (ABBA, ≥3 пары, одна копия exe на серию) и берут bench-замок только на время своей серии.
- **Зависший замок.** `finally` не выполняется, если оболочку убили: замок остаётся. `run_native_engine.ps1` теперь
  один раз пишет `STALE? …`, если `sspc_game.lock` старше 40 мин и игры нет. Удалять чужой замок — только убедившись,
  что владельца нет (нет его процесса PowerShell/игры), и записав это в отчёт.
- Полный `SecondSonPC.exe --selftest` не запускать (стресс стеков исчерпывает commit, падал весь ПК).
- Сгенерированный C++ и DLL рекомпилятора (производные eboot) — только в `E:\research\r6\<x>\recomp*`, не в Git.

## 5. Native Render Bridge

(R6-Core, заполняется по мере работы.)
