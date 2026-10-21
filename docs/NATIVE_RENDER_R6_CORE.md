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
  `finally`). Никаких `cmake --build` в обход. На ПК нет файла подкачки: одна сборка за раз, `-Jobs 3..4`.
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

### 5.1 B1: путь draw сегодня [A — по коду и документам, ссылки ниже]

| Шаг | Где | Что происходит | Источник |
|---|---|---|---|
| 1. состояние CUE | `0x16aa0/0x16b70/0x169d0/0x16d10/0x16eb0`, материал `0x448640` | дескрипторы → тень стадии `CUE+0x90+stage·0x1800` (или прямой `SET_SH_REG` при бите 15), флаг `CUE+0xC1D0+stage` | NATIVE_RECOMP_R5_D.md §2 |
| 2. commit | `0x14ad0` | для грязной стадии: снимок `0x123d0` в кольце кадра, `memcpy` тени, указатель в user data (`0x626400`), прямые SGPR (`0x626520`), предвыборка кода (`0x623630`, `CP_DMA`), привязка шейдера Gnm (`0x625100…0x625aa0`), `SPI_PS_INPUT_CNTL` (`0x638540`) | NATIVE_RECOMP_R5_D.md §5 |
| 3. draw | `drawIndex 0x623c60` / `drawIndexAuto 0x623a40` | пакет `DRAW_INDEX_2`/`DRAW_INDEX_AUTO` + NOP из 4 dword (тело не читает железо) | NATIVE_RENDER_INTERFACE.md §2, §7.2 |
| 4. разбор PM4 | `CommandProcessor` (поток 1) | `SET_*_REG` → `RegisterFile`, `DMA_DATA`, `ACQUIRE_MEM`, draw → `BuildDraw` → `DrawDescriptor` | command_processor.cpp |
| 5. подготовка draw | фронтенд шейдеров (поток 1) | поиск программы/раскладки, обход SRT по указателю user data, `DecodeResources`, копии стадий; Phase 5/6 — серии, GEOFAST | CITY_ENGINE_RESOURCES.md §4 |
| 6. запись Vulkan | поток 2 (Recorder/Executor) | конвейер, дескрипторы, барьеры, `vkCmdDraw*` | NATIVE_CP.md |

**Повторные преобразования** [A + E из Phase 6]: (а) таблица ресурсов копируется в новый блок кольца на каждом
commit, поэтому указатель user data всегда новый и обход SRT повторяется даже при том же содержимом; (б) значения,
которые движок уже держит структурировано (регистры шейдеров в объектах `CUE+0xC0A8/0xC0B0`, тени стадий, параметры
draw), сериализуются в PM4 и снова разбираются в `RegisterFile`; (в) по профилю Phase 6 поток 1 ≈ 29–30 мс на кадр в
городе, из них полные подготовки ≈ 11–12 мс, остальное — разбор PM4, запись регистров, журнал, записи очереди.

### 5.2 Что уже есть: P5.3 — мост движок → CP без PM4 своих операций [A]

`docs/native-reconstruction/NATIVE_RENDER_INTERFACE.md`. За флагами, по умолчанию выключено:

- `SSPC_P53_NATIVE_DRAW=apply:<проходы>` — commit `0x14ad0` (`NativeCommit`, ручная реконструкция в
  `src/engine/render/native_draw.cpp`) и draw выполняются без PM4: записи регистров (`OpKind::Registers`) и `DrawOp`
  уходят в CP, который исполняет их через тот же `Draw()`/`BuildDraw`/Executor, что и пакет;
- `SSPC_P53_NATIVE_STATE=apply` — состояние прохода (SetRenderTargets, TransitionTo, prim/index, 15 установщиков);
- `SSPC_P53_DIRECT=1` — операции в собственных блоках памяти, в DCB один токен на серию (порядок = место токена);
- `SSPC_P53_NATIVE_FRONTEND` — подготовки стадий по описанию движка (форма таблицы), общий кэш;
- `--render-mode hybrid` = apply:1,9 + state apply + describe + frontend all, без проверок.

Проверено ранее [E]: пляж/гавань, verify 0 расхождений (до 10 млн draw), Vulkan Validation 0, изображения в паузе
побитово (кроме G-буфера 0/2 в гавани). **Не проверено:** город, пресет Phase 6 (нативные draw идут через
`Draw(packet=false)` — метки серий Phase 6 на них не действуют, т.е. для прохода 1 это альтернатива сериям), текущий
Native CP, и производительность в чередующемся A/B — ни разу.

### 5.3 B4: P5.3 hybrid в городе поверх пресета Phase 6 — двухпутевая сверка [E]

Прогон `E:\research\r6\core\runs\r6c_hyb_verify_city` (exe `feature/r6-core` @ `5752e0f`, стенд: пресет
`SSPC_CITY_PERFORMANCE=1` + Native CP; `SSPC_RENDER_MODE=hybrid;SSPC_P53_NATIVE_VERIFY=1`, город 150 с). Числа —
последняя строка журнала:

| Что | Значение |
|---|---|
| draw движка, описанных и сверенных с PM4-путём (shadow, все проходы) | 27 288 883 описано, **27 272 196 сверено — 0 расхождений**, foreign / no space / malformed / orphaned 0, shaders unknown 0, таблиц по содержимому 40,3 млн |
| нативные draw (проходы 1, 9) | 5 421 613, draw fallbacks 0; CP исполнил 5 419 128 (разница — кадры в полёте) |
| нативные commit | 4 341 661; откаты commit: stage mode 0, alt shader 1 079 952 (20 %), missing 0 |
| нативное состояние | SetRenderTargets 13 264, TransitionTo 27 385, prim/index по 5,42 млн, установщики 15,3 млн; fallbacks 0; сверка блока и размера PM4 — 0 / 0; CP: 77,4 млн операций сверено — **0 расхождений** |
| журнал | 0 строк Error/Critical; закрытие тестом, exit 0 |
| кадры (окно 70…148 с) | 18,9 FPS, CP 52,5 мс — с проверочным маркером на каждом draw (68–135 млн маркеров), как показатель скорости не годится |

Статус моста P5.3 hybrid в городе: **GAME_VERIFIED** (verify, один прогон, одна сцена). Изображение отдельно не
сравнивалось (скриншоты стенда ненадёжны); сверка покрывает регистры шейдеров, таблицы ресурсов по содержимому и
параметры draw, и состояние прохода — то, что потребляет Vulkan-исполнитель.

Вывод B1: мост R6 строится на P5.3, а не с нуля. Первое — измерить его в городе поверх пресета (корректность, затем
A/B), найти, что съедает выигрыш, и только потом расширять.
