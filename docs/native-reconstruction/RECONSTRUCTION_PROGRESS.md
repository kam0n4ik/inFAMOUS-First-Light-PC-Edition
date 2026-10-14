# Прогресс реконструкции

Обновлено: 2026-10-14 (этап 1). Ветка `feature/native-engine-reconstruction`.

## 1. Изученность архитектуры движка (что разобрано, а не «процент игры»)

| Подсистема | Изучено | Подтверждено |
|---|---|---|
| Платформенные импорты | 908/908 слотов именованы, 460 PLT-заглушек размечены в Ghidra | анализ [A] |
| Потоки движка | таблица всех 16 потоков (имя, стек, приоритет, аффинность), два пути создания (WWS, Wwise) | анализ [A] |
| Job System (очередь движка) | 13 функций разобраны (Init, Worker, NextJob, TryNextJob, WaitBatch, Submit, KickGroup, Release, ResetGroup, Shutdown, delay, итераторы, объект потока) | анализ [A] + игра/тесты [E] |
| Havok hkTaskScheduler | найден по строкам | — |
| Memory System | 3 функции поточных арен видны в коде воркера | гипотеза [H] |
| Render Pipeline (гостевая сторона) | P5.2B: цепочка «модель → экземпляр рендера (0x80) → 41 проход → контекст из пула (254) → материал → тень ресурсов / раскладка → draw → SubmitFrame», ~90 функций (RENDER_ENGINE_ARCHITECTURE.md) | анализ [A]; P5.3A: сброс слотов (`BeginFrame 0x1f0be0`), 41 проход, зависимости — измерены (RENDER_ENGINE_PASSES.md) |
| Native Render Interface | P5.3B: draw движка из его объектов, маркер в DCB, сверка в CP (shadow, `SSPC_P53_NATIVE_DRAW=shadow`); P5.3C: нативные commit CUE + draw выбранных проходов (`apply:1,9` проверено), без своего PM4 (NATIVE_RENDER_INTERFACE.md) | эксперимент [E], выкл. по умолчанию; состояние прохода (цели, блендинг, viewport) — ещё PM4 |
| Streaming / Resource / World | — | — |

## 2. Фактически проверенная нативизация

| Подсистема | Статус | Почему |
|---|---|---|
| Job System движка | **HYBRID** | алгоритм — гостевой x86-64, исполняется напрямую; 3 функции очереди можно заменить C++-реконструкцией (`SSPC_NATIVE_JOBS=1`, выкл. по умолчанию; эквивалентность — 68 тестов + A/B); mutex/cond — NATIVE; создание потоков по умолчанию — BRIDGE |
| Ядро: синхронизация | NATIVE | (раньше, P2–P3) |
| Ядро: потоки | BRIDGE (NATIVE за `--native threads`) | без изменений |
| Файлы, ввод, системные параметры | NATIVE | без изменений |
| GPU | BRIDGE по умолчанию / HYBRID за `--gpu-backend sspc` | без изменений; шейдерный компилятор моста |
| Аудио, видеодекодер, сеть, NP, сохранения | BRIDGE | без изменений |

Ни одна подсистема в этом этапе не получила статус NATIVE.

## 3. Код этапа

| Файл | Что |
|---|---|
| `src/engine/guest_hooks.{h,cpp}` | проверка хеша тел гостевых функций, установка `jmp [rip]` |
| `src/engine/job_system.{h,cpp}` | реконструкция NextJob/TryNextJob/WaitBatch, политики ожидания, счётчики, установка по `SSPC_NATIVE_JOBS` |
| `src/bridge/shadps4/bootstrap.cpp` | вызов `Engine::Jobs::InstallFromEnvironment` между загрузкой и стартом eboot |
| `src/base/log.{h,cpp}` | категория лога `Engine` |
| `CMakeLists.txt` | `src/engine/*` в `sspc_runtime`; цель `sspc_engine_tests` |
| `tools/engine_tests/main.cpp` | 68 проверок + `--bench` |
| `tools/native-reconstruction/**` | скрипты Ghidra/ELF (BINARY_ANALYSIS.md §4) |

Переключатели: `SSPC_NATIVE_JOBS=1`, `SSPC_NATIVE_JOBS_WAIT=guest|pause|yield`, `SSPC_NATIVE_JOBS_STATS=<с>`.

## 4. Проверки

- `sspc_engine_tests`: 68/0 PASS.
- Игра: 7 прогонов по 90 с (пляж ×5, гавань ×2), ошибок 0; A/B без различий вне разброса (PERFORMANCE_BASELINE.md).
- Полный `--selftest` SecondSonPC: см. отчёт этапа (запуск после сборки).
