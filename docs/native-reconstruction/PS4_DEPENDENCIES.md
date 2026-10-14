# Карта PS4-зависимостей

Дата: 2026-10-14. Источники: `logs\SecondSonPC.log` (таблица привязок запуска по умолчанию), `src/loader/bindings.cpp`,
`docs/PHASE2_NATIVE_MIGRATION_MATRIX.md`, `docs/PHASE3_BRIDGE_DEPENDENCIES.md`, слоты импорта eboot
(BINARY_ANALYSIS.md). Разбивка по подсистемам ниже — сопоставление имён импортов с логом, а не отчёт, который
генерирует проект.

## 1. Классы привязки (bindings.cpp)

| Класс | Смысл |
|---|---|
| NATIVE | своя реализация SecondSonPC, без моста |
| BRIDGE | функция shadPS4 (third_party) |
| OBSERVED | своя обёртка поверх BRIDGE |
| LLE | гостевой код из PRX игры (`libc.prx`, `libSceFios2.prx`) — уже x86-64, исполняется напрямую |
| COMMON_STUB | заглушка «вернуть 0» |
| UNRESOLVED | объект без поставщика |

Запуск по умолчанию: **NATIVE 120 / BRIDGE 275 / OBSERVED 2 / COMMON_STUB 18 / UNRESOLVED 2; LLE 147** [E].

## 2. По подсистемам (запуск по умолчанию)

| Подсистема | NATIVE | BRIDGE | Сложность замены |
|---|---|---|---|
| Ядро: mutex/rwlock/sema/eventflag/util | 45 | — | сделано |
| Ядро: потоки | — | 30 | **NATIVE готов за `--native threads`** (не по умолчанию: нет длительной проверки) |
| Ядро: память (direct/flexible, mmap) | частично | да | NATIVE за `--native memory|kernel-memory` (экспериментально) |
| Ядро: часы, сигналы, `__error`, exit | частично | да | NATIVE за флагами P3.5/P3.6 |
| Файлы (libkernel fs) | 21 | — | сделано |
| GPU: Gnm 85 + VideoOut 13 | 0 по умолчанию | 98 | **NATIVE за `--gpu-backend sspc`** (свой CP + Vulkan; трансляция GCN→SPIR-V пока через адаптер к компилятору shadPS4) |
| AudioOut 6 + Ajm 11 | 0 | 17 | средняя: вывод — WASAPI; Ajm — декодеры AT9/MP3 |
| Videodec 6 | 0 | 6 | средняя (H.264 — Media Foundation) |
| Pad 7 | 7 | — | сделано (геймпады пока через SDL моста) |
| PlayGo 10, System 6, User 5, AppContent 2, Sysmodule 2 | 25 | — | сделано |
| Net 30 / Http 18 / Ssl 2 / NetCtl 6 | 2 | ~54 | низкая польза (онлайн-функции игры закрыты) |
| NP (PSN) 14, Trophy 7 | 0 | 21 | низкая польза; можно заглушить |
| SaveData 8 + диалоги | 0 (+2 OBSERVED) | ~13 | средняя (файлы Windows) |
| libc (542 слота) | — | — | LLE: `libc.prx` игры, уже нативный x86-64 |
| Fios2 (42) | — | — | LLE: `libSceFios2.prx` игры поверх NATIVE fs |

## 3. Зависимости системы задач движка (JOB_SYSTEM_RECONSTRUCTION.md)

| Вызов | Класс по умолчанию | Примечание |
|---|---|---|
| `scePthreadMutexInit/Lock/Unlock`, `scePthreadCondInit/Wait/Broadcast` | **NATIVE** (kernel-mutex) | гостевые объекты; WaitOnAddress / свой WaitList |
| `scePthreadMutexattr*` (группы, тип 4) | NATIVE | |
| `scePthreadCreate`, `scePthreadAttr*` (через WWS `0x895f60`) | **BRIDGE** (NATIVE за `--native threads`) | аффинность не задаётся (0); приоритет 700 сохраняется, но к потокам Windows не применяется |
| `sceKernelUsleep` (delay >500 мкс, только при выходе) | по группе | |
| частота TSC (глобал `0x7f91f88`) | — | из `sceKernelGetTscFrequency` при старте [H] |
| TLS `fs:-0x30`, `fs:-0x20`, `fs:-0x14` | патч `fs:` (TCB-слот NATIVE) | статический TLS eboot |
| `rdtsc` | — | исполняется напрямую |

Вывод: у системы задач **нет** зависимостей, требующих эмуляции, кроме создания потоков (NATIVE-вариант есть) и
`fs:`-TLS (решено слоем CPU). Её статус — HYBRID: алгоритм уже нативный (гостевой x86-64 или наш C++), создание потоков
по умолчанию через мост.

## 4. Что переносится просто, а что сложно

- **Просто / почти сделано:** всё ядро (включить NATIVE-потоки и память по умолчанию после soak-тестов), файлы,
  ввод, системные параметры.
- **Средне:** аудио (WASAPI + декодер AT9), видео (Media Foundation), сохранения.
- **Сложно:** GPU. NATIVE-путь уже есть, но гостевой движок формирует команды PS4 (PM4). Нативный рендер без PM4
  требует реконструкции гостевого рендер-пайплайна (см. NATIVE_MIGRATION_ROADMAP.md), а шейдеры — это GCN-байткод игры.
