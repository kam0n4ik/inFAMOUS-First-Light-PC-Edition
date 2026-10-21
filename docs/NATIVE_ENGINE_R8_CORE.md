# R8: Native CP Optimization & Render Independence — ядро, владение, интерфейсы

Фаза R8 (`feature/native-engine-r8`, worktree `E:\SecondSonPC-r7-core`). Адреса — смещения eboot CUSA00004 (база 0).
Статусы — ANALYZED → GENERATED → OFFLINE_VERIFIED → GAME_VERIFIED → PERFORMANCE_VERIFIED; одна ступень не
подразумевает следующую. Метки: [A] анализ кода, [E] измерение, [?] не разобрано.

Цель R8 — чтобы собственные компоненты R5–R7 начали **убирать работу потока 1 (CP + фронтенд шейдеров)**, а не только
исполнять участки движка своим кодом. Приоритет: корректность → сокращение реальной работы CP → FPS → нативная
независимость. Всё новое — за флагами, по умолчанию выключено; пресет стенда (`SSPC_CITY_PERFORMANCE`, с
`SSPC_R6_CP_POLL_TRIVIAL`) не меняется.

## 1. Исходная точка (R8.0)

| Что | Значение |
|---|---|
| база | `main` @ `2411c16` (R7 влит: `25eec6b`); код `main` = `feature/r7-native-core` `2e27365` (отличия — только docs) |
| R8.0 | этот коммит: точки расширения R8 (§4), владение (§3), интерфейсы (§5), без изменения поведения |
| шейдерный кэш | `src/runtime/gpu/shader/*`, `gcn/*`, `shader_translator_host.cpp`, `shader_bridge_hooks.h` не отличаются от `main` |
| базовые замеры | §9 (пресет стенда на сборке R8.0) |

Что известно до R8 (не повторять, опираться):
- Поток 1 в городе ≈ 30 мс при кадре ≈ 31 мс — он ограничивает кадр (R6-P §2, R7-R §7.1, R7 RESULTS §2). Поток 2
  (запись Vulkan) ≈ 24 мс, GPU ≈ 17 мс.
- Сэмплер R6-P (до POLL_TRIVIAL, доли потока 1): `PrepareDraw` 44 % (из них `PrepareStage` 32 %: обход SRT 13 %,
  `DecodeResources` 6 %, поиск программы 4 %, ключ 3 %), `CopyStage` 9,3 %, `Drain` 8,7 % (`GdsOnTimeline` 7,2 %),
  `JoinRun` 8,3 %, `SetRegs`/`RegisterFile::Write` ≈ 10 %, `Pm4::Decode` 5,7 %, `ShaderFrontend::Invalidate` 3 %.
- R7-R: фронтенд на потоке 1 — 62,6 с за 150 с прогона; из 47,1 M draw 30,2 M идут сериями Phase 5/6 «только своими
  привязками», ≈ 16,8 M — через `PrepareStage`; промахи памятки стадий P4.7: «other user data» 13,2 M (новый снимок CUE
  на каждый commit), «other inputs» 21,9 M. Версия привязки Native Resource State сохраняется у 41,7 % снимков PS и
  0,13 % VS.
- Отвергнуто замерами (без новой гипотезы не повторять): P5.3 hybrid (−4,4 FPS), R6 CUE dedup (+0,00), `PREFETCH=skip`
  (+0,05 поверх T), пул векторов серии (+0,37 в шуме), R7 C3 G-buffer (−1,57), R7-R кэш снимков на стороне CUE, R7-S
  dedup теней (+0,44 в шуме). Не подтверждено: `SSPC_CP_ASYNC_WRITES=gds` поверх T **не мерился**.

## 2. Ветки и worktree

Новые worktree с полной сборкой не создаются (на E: ≈ 20 ГБ): используются существующие worktree R6/R7 с их
build-каталогами, переключённые на ветки R8 от коммита R8.0 (сборка инкрементальная).

| Направление | Ветка | Worktree | Каталог экспериментов | Ghidra (своя копия) |
|---|---|---|---|---|
| R8-Core (Lead: архитектура, интеграция, итоговые A/B) | `feature/native-engine-r8` | `E:\SecondSonPC-r7-core` | `E:\research\r8\core` | `E:\research\r7\core\ghidra` |
| A1 CP Profiler | `feature/r8-cp-profile` | `E:\SecondSonPC-r6-cp` | `E:\research\r8\profile` | не нужна |
| A2 Native CP | `feature/r8-native-cp` | `E:\SecondSonPC-r7-resources` | `E:\research\r8\cp` | `E:\research\r7\resources\ghidra` |
| A3 Native Render Layer | `feature/r8-render-layer` | `E:\SecondSonPC-r7-gbuffer` | `E:\research\r8\render` | `E:\research\r7\gbuffer\ghidra` |
| A4 REA / Recompiler | `feature/r8-recomp` | `E:\SecondSonPC-r7-shadows` | `E:\research\r8\recomp` | `E:\research\r7\shadows\ghidra` |
| A5 Verification | `feature/r8-verify` | `E:\SecondSonPC-r6-gbuffer` | `E:\research\r8\verify` | не нужна (при нужде — только чтение MCP) |

Имена каталогов worktree остались от R6/R7 (переименование сломало бы build-каталоги CMake). Ни одна ветка не
сливается в `main` и не публикуется (push) без разрешения пользователя. Интеграция — в `feature/native-engine-r8`,
только через Lead, только сборкой и тестами без ошибок.

## 3. Владение файлами

| Файл / каталог | Владелец | Что |
|---|---|---|
| `cmake/r8/<core\|profile\|cp\|render\|recomp\|verify>.cmake` | владелец подсистемы | `SSPC_R8_<X>_SOURCES` (в `sspc_runtime`), `SSPC_R8_<X>_TEST_SOURCES` (в `sspc_native_engine_tests`) |
| `tools/native_engine_tests/r8_<x>_tests.cpp` | владелец подсистемы | `R8<X>Tests(...)` — вызывается из `main.cpp` до установки переходов на образ |
| `src/native_engine/r8/r8_{core,render,recomp}.h` → `SSPC_R8_*_SPECS` | Lead / A3 / A4 | `#include` реконструкций и `&Reconstructed::kXxxSpec,` |
| `src/runtime/gpu/r8/cp_profile.{h,cpp}` (новые) | **A1** | профайлер CP (§5.1) |
| `src/runtime/gpu/r8/cp_native_*.{h,cpp}` (новые) | **A2** | потребитель нативного состояния в CP (§5.2) |
| `src/runtime/gpu/parallel_cp.{h,cpp}`, `command_processor.{h,cpp}`, `cp_r6_opt.*`, `cp_registers.*` | **A2** (логика) | A1 добавляет только однострочные маркеры профайлера отдельным коммитом `[shared] profiler markers`; остальные — через A2 или `[shared]` |
| `src/engine/render/native_render_layer.{h,cpp}`, `native_draw.*`, `native_render_ops.*`, `draw_runs.*`, `native_state.*` | **A3** | контракт и рабочий путь слоя (§5.3); новое поле `NativeDrawCommand`, нужное A2, — по просьбе A2 |
| `src/native_engine/r8/render/*`, `src/native_engine/r7/gbuffer/draw_command.*`, `r7/shadows/shadow_draw_command.*` | **A3** | построение команд |
| `src/runtime/gpu/vulkan/vk_executor.*` | **A3** | A2 — только `[shared]` |
| `src/native_engine/reconstructed/r8_*.{h,cpp}`, `src/native_engine/r8/recomp/*`, `tools/native-reconstruction/recomp/*` (вкл. `pcode2c.py`) | **A4** | реконструкции и рекомпилятор |
| `src/native_engine/r7/resources/resource_state.{h,cpp}`, `reconstructed/cue_set_buffers.*`, `cue_commit.*` | **A2** | A4 отдаёт реконструкции новых наблюдаемых функций (`0x16fb0` и др.) как отдельные файлы, A2 подключает |
| `src/native_engine/native_engine_verify.{h,cpp}`, `src/runtime/gpu/r8/r8_verify.{h,cpp}` (новые), `src/runtime/gpu/vulkan/vk_capture.*`, `tools/r8/*` (новые) | **A5** | общие средства сверки (§5.5), сравнение кадров |
| `CMakeLists.txt`, `src/runtime/gpu/cp_mode.cpp`, `native_function_registry.*`, `native_engine_install.cpp`, `native_dispatch.h`, `guest_code_clone.*`, `native_measure.*`, `engine/guest_hooks.*`, `engine/guest_probe.*`, `tools/native_engine_tests/main.cpp`, `tools/native-reconstruction/run_native_engine.ps1`, `docs/NATIVE_ENGINE_R8_{CORE,RESULTS}.md` | **Lead** | агенту нужно — маленький коммит `[shared] ...` в своей ветке с описанием в отчёте; Lead переносит его первым |

**Нельзя трогать никому:** `src/runtime/gpu/shader/*`, `src/runtime/gpu/gcn/*`,
`src/bridge/shadps4/shader_translator_host.cpp`, `shader_bridge_hooks.h` — их содержимое задаёт поколение шейдерного
кэша (`CMakeLists.txt`, `SSPC_NATIVE_SHADER_GENERATION`); игровые прогоны стенда используют кэш пользователя
`E:\SecondSonPC\cache\shaders`, и новое поколение вытеснило бы его подготовленный кэш (хранится 4 поколения).
Потребитель нативного состояния строится **снаружи** фронтенда (вокруг вызовов `PrepareDraw`/`PrepareStage` в
`parallel_cp.cpp`). Если доказано, что без правки `shader/*` выигрыша нет, — только предложение с замерами Lead'у.

Новые файлы подсистемы — в своих каталогах. Флаги — `SSPC_R8_<X>_...`, читаются один раз, по умолчанию выкл.

## 4. Точки расширения R8.0

- `cmake/r8/*.cmake` — пустые списки, подключены в `CMakeLists.txt` после R7.
- `src/native_engine/r8/r8_{core,render,recomp}.h` — `SSPC_R8_{CORE,RENDER,RECOMP}_SPECS` в реестре после R7;
  `kMaxFunctions` 48 → 64.
- `tools/native_engine_tests/r8_<x>_tests.cpp` — `R8<X>Tests(...)`, вызываются из `main.cpp` после R7.
- `src/runtime/gpu/r8/` — каталог CP-компонентов R8 (A1, A2, A5).

## 5. Интерфейсы (согласованы до реализации; изменения — через Lead)

### 5.1 Профайлер CP (A1) — `src/runtime/gpu/r8/cp_profile.h`

- `SSPC_R8_CP_PROFILE=1` (выкл. по умолчанию). Когда выключен — одна загрузка статического `bool` и предсказуемая ветка
  на маркер; ни одной записи в общую память.
- Маркер — RAII на `__rdtsc()` с накоплением в thread_local счётчики секции (тики, вызовы), сброс в общий итог раз в
  кадр (флип) без блокировок на горячем пути. Секции — перечисление, не строки.
- Отчёт — раздел `R8 CP profile` в `gpu_native.txt` (мс/кадр и доля потока по секциям, вычисления / ожидания /
  синхронизация раздельно) и, по флагу, CSV по кадрам. Накладные расходы профайлера — измерены и указаны в отчёте
  (прогон с флагом против прогона без него).
- Потребители: A2 и Lead меряют им каждую оптимизацию (секции до/после).

### 5.2 Нативное состояние в CP (A2) — `src/runtime/gpu/r8/cp_native_*.h`

Правила (обязательны для любого механизма пропуска работы):
1. **Единственный источник истины — то, что видит CP** (регистры, PM4, память гостя). Нативное знание (версии
   Native Resource State, описание draw от движка) — только подсказка «можно не пересчитывать», которую CP обязан
   уметь проверить (режим verify: полный путь + сравнение) и которая при любой неполноте → полный путь.
2. Канал движок → CP упорядочен самим DCB (токен/NOP в потоке контекста) или таблицей, запись в которую
   происходит-раньше отправки кадра (`SubmitFrame`) и снабжена эпохой кадра/арены снимков; указатель на временный
   объект движка в очереди не хранится.
3. Ключ переиспользования включает всё, что читает подготовка: программу (адрес + `RSRC1/2`), user data стадии
   (16 dword), версию ресурсов (`NativeStageResources::version`, 0 = нельзя), прямые SGPR (`0x16fb0` в версию не
   входит — сравнивать сами значения), поколение памяти фронтенда для памяти под дескрипторами.
4. Инвалидация: смена версии / программы / поколения памяти / сброс CUE (`0x13d20`, `0x12390`) / новая эпоха кадра.
5. Отказ в переиспользовании — на границе draw; посреди частично подготовленного draw путь не переключается.

### 5.3 Native Render Layer (A3) — `src/engine/render/native_render_layer.h`

- `NativeDrawCommand` — владелец A3; новые поля только с доказанной необходимостью (их потребитель указан).
- `R7::Resources::Fill(cue, cmd.resources)` — вызывается при построении команды (A3); данные копируются по значению,
  в очереди нет указателей на временные объекты.
- Диспетчеризация `0x14ad0`: **один** механизм (вход C3 и `cue_commit` реестра не могут оба обработать один
  commit) — A3 решает и доказывает тестом (§NR4 ТЗ).
- `Submit` возвращает `Native` только если путь игры для этого draw не выполняется; двойного draw/commit нет.

### 5.4 Реконструкции (A4)

- Заявки на функции — от A2/A3/A5 через Lead (`E:\research\r8\recomp\REQUESTS.md`). Каждая реконструкция: спек в
  `SSPC_R8_RECOMP_SPECS` (или файл, который подключает владелец потребителя), офлайн-сверка с кодом eboot
  (`R8RecompTests`), `--lift-check` для рекомпилированных, проверка арности callee из их кода (урок R7-G).

### 5.5 Сверка (A5) — `src/runtime/gpu/r8/r8_verify.h`

- `SSPC_R8_VERIFY=<список участков>|all` — включает режим verify у участков R8 (каждый участок регистрирует имя).
- Общий отчёт о первом расхождении: участок, номер кадра/draw, ключ, поле, ожидаемое/фактическое; ограничение по
  числу записей на участок (кольцо, без исчерпания чужих), счётчики сверок/расхождений в `gpu_native.txt`.
- Сравнение кадров: детерминированный захват (одинаковая сцена/камера/ввод), `tools/r8/frame_compare.py`
  (точное совпадение, MAE, SSIM, тепловая карта, области).

## 6. Флаги R8 (все по умолчанию выключены; таблица пополняется владельцами)

| Флаг | Владелец | Что |
|---|---|---|
| `SSPC_R8_CP_PROFILE` | A1 | профайлер CP |
| `SSPC_R8_CP_*` | A2 | оптимизации CP на нативном состоянии (имена — в `NATIVE_ENGINE_R8_CP_OPTIMIZATION.md`) |
| `SSPC_R8_RENDER_*` | A3 | рабочий путь Native Render Layer |
| `SSPC_R8_VERIFY` | A5 | режимы сверки |

Без флагов R8 игра идёт ровно как `main` (пресет стенда). Пресет `SSPC_CITY_PERFORMANCE` R8 не трогает; включение
чего-либо в пресет — решение пользователя по итоговым A/B.

## 7. Стенд (как R7 §5)

- **Сборка** — только `powershell -File E:\research\sspc_build.ps1 -Worktree <свой> -Owner r8-<x> -Log
  E:\research\r8\<x>\build.log -Target shadps4|sspc_native_engine_tests [-Configure] [-Jobs N]` (замок
  `E:\research\sspc_build.lock`, ждёт `sspc_bench.lock` и `sspc_game.lock`). Цель `shadps4` = `SecondSonPC.exe`. Файла
  подкачки нет: одна сборка за раз, `-Jobs 4` (если никто не играет — до 8).
- **Игра** — только `powershell -File E:\research\r6\cp\run_engine_extra.ps1 -Worktree <свой> -Out
  E:\research\r8\<x>\runs -LogsDir E:\research\r8\<x>\logs -Owner r8-<x> -Scene city|beach|harbor|village -Tag <тег>
  -Env "..." [-MoreArgs "--gpu-native-validate"] -QuietSeconds 60`. Пресет стенда + заявка в `sspc_game_wait`; город
  150 с (окно 70…148 с), остальные 90 с (окно 62…88 с). Прогон долгий — в фоне, ждать уведомления.
- **Серия A/B** — `E:\research\r8\series.ps1` (держит `sspc_bench.lock` всю серию, чередует порядок по раундам), ≥ 3
  пары; пока существует чужой `sspc_bench.lock` — никакой тяжёлой CPU-работы. Сводка —
  `py -I -X utf8 <worktree>\tools\native-reconstruction\native_engine_ab.py <runs> <A,...> <B,...> [--from --to]`.
  Холодный первый прогон после новой сборки (перетрансляция модулей) в A/B не входит.
- **Окружение прогона** — `env_effective.txt`; проверка «waits that waited: processor memory reads» = 0 (POLL_TRIVIAL
  действует).
- Полный `SecondSonPC.exe --selftest` не запускать. Сгенерированный C++ и DLL рекомпилятора — только в
  `E:\research\r8\<x>\recomp*`, не в Git. Диск E: ≈ 20 ГБ: копии сохранений в каталогах прогонов (`save\`) удалять
  после разбора прогона.

## 8. План и статус

| Этап | Что | Статус |
|---|---|---|
| R8-A | профиль CP (A1), анализ слоя (A3), недостающие реконструкции (A4), стенд сверки (A5), базовые замеры (Lead) | в работе |
| R8-B | Native Resource State → CP: первый измеримый механизм (A2) | — |
| R8-C | NativeDrawCommand + Fill + единая диспетчеризация `0x14ad0` + рабочий submit (A3) | — |
| R8-D | быстрый путь CP по нативному описанию draw (A2 + A3 + A4) | — |
| R8-E | слияние, сверки, 4 сцены, отключаемость, `NATIVE_ENGINE_R8_RESULTS.md` (Lead) | — |
