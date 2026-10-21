# R7-Core: Native Render Layer, общая инфраструктура R7

Фаза R7 (`feature/r7-native-core`, worktree `E:\SecondSonPC-r7-core`). Адреса — смещения eboot CUSA00004 (база 0).
Статусы — строго по шкале ANALYZED → GENERATED → OFFLINE_VERIFIED → GAME_VERIFIED → PERFORMANCE_VERIFIED; одна
ступень не подразумевает следующую. Метки: [A] анализ кода, [E] измерение, [?] не разобрано.

## 1. Исходная точка (R7.0)

| Что | Значение |
|---|---|
| база | `main` @ `6167315`: R6 (`feature/r6-core` @ `8b0f199`, fast-forward) + `SSPC_R6_CP_POLL_TRIVIAL=1` в пресете `SSPC_CITY_PERFORMANCE` |
| R7.0 | этот коммит: точки расширения R7 (§3), контракт Native Render Layer (§4), без изменения поведения |
| шейдерный кэш | `src/runtime/gpu/shader/*`, `gcn/*`, `shader_translator_host.cpp`, `shader_bridge_hooks.h` не отличаются от `main` |

## 2. Ветки и worktree

| Направление | Ветка | Worktree | Каталог экспериментов |
|---|---|---|---|
| R7-Core (Native Render Layer, интеграция, итоговые A/B) | `feature/r7-native-core` | `E:\SecondSonPC-r7-core` | `E:\research\r7\core` |
| R7-G (нативный G-buffer / Geometry Engine) | `feature/r7-native-gbuffer` | `E:\SecondSonPC-r7-gbuffer` | `E:\research\r7\gbuffer` |
| R7-S (нативный Shadow Engine) | `feature/r7-native-shadows` | `E:\SecondSonPC-r7-shadows` | `E:\research\r7\shadows` |
| R7-R (Resource / Material / CUE) | `feature/r7-native-resources` | `E:\SecondSonPC-r7-resources` | `E:\research\r7\resources` |

Ветки агентов — от коммита R7.0. Ни одна не сливается в `main` и не публикуется (push) без разрешения пользователя.
`third_party\shadPS4` и `third_party\upscalers` в worktree — junction на `E:\SecondSonPC` (только чтение), `runtime\` —
копия. У каждого агента своя копия Ghidra-проекта в `E:\research\r7\<x>\ghidra` (`SSPC_GHIDRA_PROJECT_DIR`).

## 3. Владение общими файлами

| Файл | Владелец | Что туда добавляют |
|---|---|---|
| `src/native_engine/r7/r7_gbuffer.h` → `SSPC_R7_GBUFFER_SPECS` | R7-G | `#include` своих реконструкций и `&Reconstructed::kXxxSpec,` |
| `src/native_engine/r7/r7_shadows.h` → `SSPC_R7_SHADOWS_SPECS` | R7-S | то же |
| `src/native_engine/r7/r7_resources.h` → `SSPC_R7_RESOURCES_SPECS` | R7-R | то же |
| `src/native_engine/r7/r7_core.h` → `SSPC_R7_CORE_SPECS` | R7-Core | то же |
| `cmake/r7/<подсистема>.cmake` | владелец подсистемы | `SSPC_R7_<X>_SOURCES` (в `sspc_runtime`), `SSPC_R7_<X>_TEST_SOURCES` (в `sspc_native_engine_tests`), свойства файлов |
| `tools/native_engine_tests/r7_<подсистема>_tests.cpp` | владелец подсистемы | `R7<X>Tests(...)` — вызывается из `main.cpp` до установки переходов на образ |
| `src/engine/render/native_render_layer.{h,cpp}` | R7-Core | контракт §4 и его реализация |

Новые файлы подсистемы — в своих каталогах: `src/native_engine/reconstructed/` (реконструкции функций eboot, имена
с префиксом подсистемы), `src/native_engine/r7/<подсистема>/` (нативные компоненты движка R7).

Только R7-Core правит: `native_function_registry.{h,cpp}`, `native_engine_install.cpp`, `native_dispatch.h`,
`guest_code_clone.*`, `native_engine_verify.*`, `native_measure.*`, `engine/guest_hooks.*`, `engine/guest_probe.*`,
`engine/render/native_draw.*`, `engine/render/native_render_ops.*`, `engine/render/draw_runs.*`,
`runtime/gpu/command_processor.*`, `runtime/gpu/parallel_cp.*`, `runtime/gpu/cp_mode.cpp`, `CMakeLists.txt`,
`tools/native_engine_tests/main.cpp`, `tools/native-reconstruction/recomp/pcode2c.py`,
`tools/native-reconstruction/run_native_engine.ps1`. Агенту, которому нужно изменить один из них, — отдельный
маленький коммит в своей ветке с пометкой `[shared]` в заголовке и описанием в своём отчёте; R7-Core переносит его
первым при интеграции (или делает сам и сообщает хеш).

Нельзя трогать никому: `src/runtime/gpu/shader/*`, `src/runtime/gpu/gcn/*`,
`src/bridge/shadps4/shader_translator_host.cpp`, `shader_bridge_hooks.h` — их содержимое задаёт поколение шейдерного
кэша; любое изменение заставит игру пользователя заново готовить все шейдеры.

## 4. Native Render Layer: контракт (`src/engine/render/native_render_layer.h`)

```
Game engine  →  Native Render Layer  →  существующий backend (CommandProcessor → Executor → Vulkan)
```

Второго GPU-бэкенда и второго командного процессора нет. Нативная операция — это операция прямого потока P5.3
(`native_render_ops.h`: `StreamOp::Registers / State / Draw`, токен серии в DCB контекста там, где были бы пакеты
игры). CP исполняет её через тот же вход, что PM4-draw (`Draw → BuildDraw → подготовка стадий фронтенда →
Executor`): конвейеры, дескрипторы, память, барьеры и отправка — существующие; порядок относительно пакетов игры
(барьеры, метки, другие контексты) — место токена. Всё, что слой не умеет передать, идёт путём игры без изменений.

| Структура | Кто заполняет | Что это |
|---|---|---|
| `NativeRenderContext` | R7-Core | контекст движка R, его CUE (`R+0x128`), DCB (`R+8`), проход/sub |
| `NativeDrawState` | R7-G / R7-S | регистры VS/PS (как `DrawOp`), prim/index type, `NUM_INSTANCES`, проход |
| `NativeResourceBindings` | R7-R | ресурсы стадий VS/PS так, как их держит CUE: тень, размер копии, снимок, форма таблицы, прямые SGPR, **версия** (меняется при любой смене ресурсов стадии; 0 — не отслеживается, переиспользование запрещено) |
| `NativeDrawCommand` | R7-G / R7-S | draw как его задумал движок: функция движка, indexed/count/base, метка серий CITY-OPT (`run_tag`), состояние, ресурсы |
| `Submit(ctx, cmd)` | R7-Core | `Native` — команда в нативном потоке, путь игры не выполнять; `Fallback`/`Off` — ничего не записано, выполнить путь игры |
| `StateKey(cmd)` | R7-Core | ключ совместимого состояния для переиспользования подготовленного draw (0 — нельзя) |

Источники полей заданы по коду P5.3 (`native_draw.cpp`: `BuildDrawOp`, `LayoutShape`, `NativeCommit`) и R5/R6
(`NATIVE_RECOMP_R5_D.md`, `NATIVE_RENDER_R6_CORE.md` §5.1). Новое поле — только через R7-Core (`[shared]`).

## 5. Стенд: сборки, игровые прогоны, A/B (как в R6, `NATIVE_RENDER_R6_CORE.md` §4)

- **Сборка** — только `E:\research\sspc_build.ps1 -Worktree <свой> -Owner r7-<x> -Log E:\research\r7\<x>\build.log
  -Target shadps4|sspc_native_engine_tests [-Configure]` (замок `sspc_build.lock`; ждёт `sspc_bench.lock` и
  `sspc_game.lock`). Цель `shadps4` = `SecondSonPC.exe`. На ПК нет файла подкачки: одна сборка за раз, `-Jobs 3..4`
  (первая сборка нового worktree — полная, до ~40 мин; её можно `-Jobs 8`, если никто не играет).
- **Игра** — только `tools\native-reconstruction\run_native_engine.ps1 -Worktree <свой> -Out E:\research\r7\<x>\runs
  -LogsDir E:\research\r7\<x>\logs -Owner r7-<x> -Scene city|beach|harbor|village -Tag <тег>`. Скрипт ставит пресет
  стенда (`SSPC_CP_MODE=native`, `REA_MERGE_RUNS`, `CP_ROUND_NODRAIN`, `P411_ASYNC_ACQUIRE`, `ASYNC_PIPELINES`,
  `SSPC_CITY_PERFORMANCE=1` — с R7.0 это включает и `SSPC_R6_CP_POLL_TRIVIAL=1`), ждёт чужих замков и тишины, берёт
  `sspc_game.lock`. Окна замера: город 70…148 с (150 с прогон), пляж/гавань/деревня 62…88 с (90 с).
- **Серия FPS (A/B)** — `sspc_bench.lock` на всю серию (образец — `E:\research\r6\core\series.ps1`), ABBA, ≥3 пары, одна
  копия exe на серию. Пока существует чужой `sspc_bench.lock`, никто не запускает тяжёлую CPU-работу (офлайн-тесты с
  большим `--cases`, `--lift-check`, генерацию/компиляцию recomp, headless Ghidra). Финальные A/B — у R7-Core.
- **Окружение прогона** — `env_effective.txt` в каталоге прогона; по нему проверять, что прогон — заявленная
  конфигурация. База сравнения R7 — **пресет стенда (Phase 6 + POLL_TRIVIAL)**.
- Зависший замок: удалять чужой — только убедившись, что владельца нет, и записав это в отчёт.
- Полный `SecondSonPC.exe --selftest` не запускать (стресс стеков исчерпывает commit).
- Сгенерированный C++ и DLL рекомпилятора (производные eboot) — только в `E:\research\r7\<x>\recomp*`, не в Git.

## 6. План R7-Core

- C1 — контракт §4 (сделано в R7.0).
- C2 — минимальный контракт передачи draw в backend: что именно нужно CP/Executor от нативного draw (поток P5.3).
- C3 — прямой нативный путь для одного типа draw: G-buffer `0x1a1770` (проход 1) совместимо с сериями Phase 6.
- C4 — проверка эквивалентности (сверка P5.3 `kVerify`, состояние CP, изображения).
- C5 — замеры стоимости участков и A/B против пресета стенда.
