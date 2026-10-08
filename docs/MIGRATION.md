# SecondSonPC — журнал миграции (Phase 1+)

Как читать счётчики. Импорты игры (eboot.bin + libc.prx + libSceFios2.prx) — 564 уникальных ключа
(nid, library, module): **417** обслуживает рантайм, **147** — сами модули игры (LLE). Каждый из 417
имеет ровно одного владельца в статической таблице привязок (`src\loader\generated\import_table.inc`,
генератор `tools\gen_bindings.py`); при каждом запуске `--loader sspc` таблица печатается в
`SecondSonPC.log` строкой `binding table: …`.

| Владелец | Смысл |
|---|---|
| NATIVE | реализация SecondSonPC |
| OBSERVED | наблюдатель SecondSonPC перед BRIDGE-реализацией |
| BRIDGE | временная функция shadPS4, адрес взят по C++-имени (`src\bridge\shadps4\generated\bridge_exports.cpp`) |
| COMMON_STUB | явная заглушка SecondSonPC: лог первого вызова, возврат 0 (то, что в Phase 0 делала общая заглушка aerolib) |
| UNRESOLVED | объект, которого никто не предоставляет: слот релокации не трогается (как в Phase 0) |
| LLE | экспорт модуля игры (libc / libSceFios2) |

В Phase 0 (`docs\PHASE0.md`) графа COMMON_STUB = 21 включала 2 неразрешённых объекта
(`Need_sceLibcInternal`, `_Stdout`); с Phase 1 они считаются отдельно (UNRESOLVED), 19 + 2 = 21.

## Счётчики по milestone

| Точка | NATIVE | OBSERVED | BRIDGE | COMMON_STUB | UNRESOLVED | LLE | Что изменилось |
|---|---:|---:|---:|---:|---:|---:|---|
| Phase 0 (f404c5c) | 2 | 2 | 392 | 19 | 2 | 147 | исходное состояние |
| Phase 1 M1 (2026-10-08) | 12 | 2 | 382 | 19 | 2 | 147 | свой Loader: +8 rtld/процесс (`__tls_get_addr`, `_sceKernelRtldSetApplicationHeapAPI`, `sceKernelGetProcParam`, `sceKernelGetModuleInfoForUnwind`, `sceKernelGetModuleInfoFromAddr`, `__stack_chk_fail`, `__stack_chk_guard`, `__progname`), +2 Sysmodule (`sceSysmoduleLoadModule`, `sceSysmoduleUnloadModule`) |

## Phase 1 — milestone 1: свой Loader — **достигнут**

**Что сделано.** `--loader sspc` (с M1 — путь по умолчанию):
- `src\loader\` — SELF/ELF (`image_file`), модуль: маппинг сегментов, PT_DYNAMIC/DT_SCE_*, экспорты,
  TLS-образ, proc param, `.eh_frame_hdr` (`module`, `eh_frame`); линкер: проходы релокаций с битами
  «разрешено» (как у oracle: заглушки переразрешаются на следующих проходах), разрешение
  таблица → экспорты LLE-модулей → заглушка, TLS/DTV и `__tls_get_addr`, heap API libc, старт
  `Game:Main`, вход в eboot по ABI процесса PS4 (`linker`); rtld-функции libkernel (`rtld.cpp`).
- `src\loader\bindings.*` + `generated\import_table.inc` — статическая таблица 564 импортов
  (`tools\gen_bindings.py`); BRIDGE-провайдеры — `src\bridge\shadps4\generated\bridge_exports.cpp`:
  392 функции shadPS4 по C++-имени через `HOST_CALL` (тот же адрес, что регистрировал Phase 0).
- `src\runtime\sysmodule\` — нативный Sysmodule (фронтенд загрузчика: preload `libSceFios2.prx`,
  `libc.prx`; таблица модулей перенесена данными). Перенесён в M1, а не в M3: preload shadPS4
  грузит модули через `Core::Linker`, без своего Sysmodule наш Loader не стартует.
- Bridge: `loader_host.cpp` (память/CPU-патчи/потоки shadPS4 за интерфейсом `Loader::Host`),
  `rtld_tcb.cpp` (заменяет при сборке `kernel\threads\tcb.cpp` shadPS4: TCB/TLS потоков — от нашего
  Loader; в пути Phase 0 — исходный код).
- В пути `sspc` не используются: `Core::Emulator`, `Core::Linker`, `Core::Module`, `Core::Loader::Elf`,
  `SymbolsResolver` для разрешения, aerolib. Объект `Core::Linker` не создаётся.

**Проверка (один и тот же бинарник, 60 с, `--call-order 300000 --reloc-snapshot`).**
Прогоны `logs\runs\m1_oracle2` (`--loader shadps4`) и `logs\runs\m1_sspc` (`--loader sspc`),
сравнение `python -I tools\compare_loader.py logs\runs\m1_oracle2 logs\runs\m1_sspc` → `result: OK`.

| Критерий | oracle | sspc |
|---|---|---|
| `libSceFios2.prx` / `libc.prx` | 0x80000000 / 0x8098c000 | 0x80000000 / 0x8098c000 |
| `eboot.bin` | 0xa290000 | 0xa460000 (база плавает и между прогонами oracle: 0xa250000, 0xa350000, 0xa290000) |
| слоты релокаций (98 139 + 3 заголовка) | — | 98 102 идентичны; 21 «общая заглушка → COMMON_STUB»; 18 «владелец → NATIVE» (план M1); 1 заголовок с базой |
| порядок импорт-вызовов | — | первые 1933 идентичны, расхождение после старта 2-го гостевого потока; два прогона oracle расходятся в той же точке (1944) |
| состав первых 300 000 вызовов | — | разница 40 (oracle↔oracle: 80) |
| функций вызвано | 171 | 171 + `vprintf` (COMMON_STUB, в oracle не считался) |
| языковые файлы / ошибки | 8 / 0 | 8 / 0 |
| пайплайнов (строк в логе) / кадров (`sceGnmSubmitAndFlipCommandBuffers`) | 882 / 3275 | 882 / 3276 |

Найдено по ходу: при выносе пути Phase 0 в отдельную функцию вектор аргументов, который
`Core::Linker::Execute` захватывает по ссылке, стал локальным → падение в `linker.cpp:211`
(исправлено: `static`).

## Открытые пункты (до финала Phase 1)

1. **Старт BRIDGE-библиотек.** `RegisterLib` shadPS4 не только регистрирует символы, но и запускает
   библиотеки (GnmDriver: Liverpool + Vulkan presenter/rasterizer; VideoOut: драйвер; Kernel:
   служебный поток, часы, семафоры, AIO; Pad: контроллеры; NpManager: обработчик). Состояние
   `static` в их `.cpp`, извне недоступно. Временно путь `sspc` вызывает `InitHLELibs` в
   выбрасываемую таблицу (`StartBridgeLibraries` в `bootstrap.cpp`) — для разрешения она не
   используется. Убрать: патч `0003`, выделяющий старт библиотек из регистрации.
2. EmulatorSettings/UserSettings/config.json ядра ещё загружаются (общая часть bootstrap).
3. Файлы, система, пользователи, PlayGo, AppContent — BRIDGE до M2/M3.
