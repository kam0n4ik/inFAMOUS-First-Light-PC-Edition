# Архитектура: игра → движок → PS4-зависимости → слой совместимости SecondSonPC → Windows

Дата: 2026-10-14. [A] анализ, [E] эксперимент, [H] гипотеза, [?] неизвестно.

## 1. Главный факт

PS4 — x86-64 (Jaguar). Код игры (`eboot.bin`, 12,8 МБ x86-64) **уже исполняется прямо на CPU ПК**: загрузчик
SecondSonPC отображает сегменты, связывает импорты и прыгает в точку входа (`src/loader/linker.cpp` `RunMainEntry`);
гостевой адрес = адрес хоста [A][E]. Правится только то, что на Windows устроено иначе: доступ к TCB через `fs:`
(3 616 мест, `src/runtime/cpu/fs_patcher` / патчер моста), SSE4a на Intel (`sse4a.cpp`) [A].

Поэтому «нативизация» здесь — не перекомпиляция игры, а:
1. замена **платформенных библиотек PS4**, которые вызывает игра (libkernel, Gnm, VideoOut, AudioOut…), — этим заняты
   фазы 2–4 SecondSonPC;
2. замена **алгоритмов самого движка** своими C++-реализациями там, где это даёт измеримую пользу или убирает
   PS4-специфику (GPU-команды, синхронизация), — этим занята эта ветка; первый механизм — `src/engine/guest_hooks`.

Декомпилировать и переписывать CPU-код игры «ради нативизации» не нужно: он и так нативный.

## 2. Слои

```
inFAMOUS Second Son (геймплей, мир, ИИ, UI)                 гостевой x86-64, исполняется напрямую
        │
Движок Sucker Punch (eboot)                                 гостевой x86-64
  ├─ потоки: Main, GameState(Sys), Synergy, Flip, Movie*, SppPlayGo, Http, Trophy, JobWorker1..5   [A]
  ├─ JobManager 0x1d1bfe0 (очередь задач, группы-зависимости)                                      [A]
  ├─ память: поточные арены (0x5b9ec0/0x5ba0f0, TLS fs:-0x20), аллокатор 0x5ba700                   [A; назначение H]
  ├─ рендер: формирует DCB/CCB → sceGnm* (85 импортов), флип через VideoOut                         [A]
  ├─ стриминг: .xpps/.psarc через libSceFios2 (LLE) → файлы libkernel                               [A]
  └─ Havok (физика/анимация, hkTaskScheduler), Wwise (звук, AK::*), Sony WWS (потоки)               [A]
        │
PS4-зависимости (импорты eboot: 908 слотов, 417 уникальных функций)
  libc (542 слота) → libc.prx (LLE) │ libkernel/Posix │ Gnm/VideoOut │ AudioOut/Ajm │ Fios2 (LLE) │
  Net/Http/Ssl/Np* │ SaveData/диалоги │ Pad/UserService/SystemService │ PlayGo │ Videodec
        │
SecondSonPC Compatibility Layer
  loader (src/loader)   свой загрузчик/линкер, привязки NATIVE/BRIDGE/LLE/STUB (bindings.cpp)
  runtime (src/runtime) NATIVE: mutex/rwlock/sema/eventflag/util ядра, fs, pad, playgo, system, user, sysmodule;
                        GPU: свой Gnm-драйвер + PM4 CP + Vulkan (--gpu-backend sspc; Parallel CP P4.10 по флагу)
  engine (src/engine)   НОВОЕ: реконструированные функции движка вместо гостевых (SSPC_NATIVE_JOBS)
  bridge (src/bridge/shadps4 → third_party/shadPS4)
                        BRIDGE: потоки*, память*, аудио, видеодекодер, сеть, NP, сохранения, диалоги, GPU по умолчанию
                        (* — есть NATIVE за --native threads|memory)
        │
Windows: потоки Win32, WaitOnAddress/SRW, VirtualAlloc2/placeholder-секции, Vulkan (NVIDIA RTX 2060), SDL3 окно/ввод
```

Счёт привязок в запуске по умолчанию: **NATIVE 120, BRIDGE 275, OBSERVED 2, COMMON_STUB 18, UNRESOLVED 2, LLE 147**
(`logs\SecondSonPC.log`, строка «binding table») [E]. Разбивка по подсистемам — PS4_DEPENDENCIES.md.

## 3. Подсистемы движка: что установлено

| Подсистема | Установлено | Источник | Статус знания |
|---|---|---|---|
| Engine Core / главный цикл | поток Main (стек 4 МиБ, prio 700); главный поток ждёт EOP предыдущего кадра (1 кадр в полёте); `delay()` ≤500 мкс спином | P4.4/P4.9 + эта фаза | частично [A][E] |
| Job System | JobManager, 5 воркеров, одна FIFO, группы с зависимостями, ожидание с помощью | JOB_SYSTEM_RECONSTRUCTION.md | **разобрана** [A][E] |
| Memory System | поточные арены 2 МиБ у воркеров; буфер JobManager 1 МиБ; аллокатор `0x5ba700(size, align)` | эта фаза | гипотезы [H] |
| Render Pipeline | гостевая сторона: DCB/CCB, 85 функций Gnm, флип через VideoOut; GPU-сторона полностью в SecondSonPC (P4.x) | P4.1–P4.14 | GPU-сторона [E]; гостевая [?] |
| Streaming | файлы `.xpps` (KCAP), `.psarc`; через Fios2 (LLE) → open/read libkernel (NATIVE fs) | P4.14, логи Filesystem | частично [E] |
| World System | — | — | [?] |
| Resource System | шейдеры в `.xpps` (футер OrbShdr, 10 374 программы) | P4.14 | шейдеры [E]; остальное [?] |
| Platform Layer | 417 импортов, классы привязок | PS4_DEPENDENCIES.md | [A][E] |

## 4. Где возможна замена гостевого кода нативным

Механизм `src/engine/guest_hooks` (проверка хеша + `jmp` на SysV-функцию) годится для функций, которые не трогают
`fs:` (иначе нужна та же трансляция TCB), имеют ≥14 байт без переходов внутрь начала и чьи данные мы умеем разметить.
Кандидаты по пользе — NATIVE_MIGRATION_ROADMAP.md.
