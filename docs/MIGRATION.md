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
| Phase 1 M2 (2026-10-08, 3d43006) | 33 | 2 | 362 | 18 | 2 | 147 | FileSystem: +21 (17 функций файлов libkernel, `_read`/`_write`, 2 DiscMap); `sceKernelOpen`/`sceKernelStat` из OBSERVED → NATIVE, `sceKernelTruncate` из COMMON_STUB → NATIVE; наблюдатели `sceSaveDataMount2`/`sceSaveDataUmount` — новые OBSERVED |
| Phase 1 M3 (2026-10-08) | 54 | 2 | 341 | 18 | 2 | 147 | +4 SystemService (`GetStatus`, `ReceiveEvent`, `HideSplashScreen`, `GetDisplaySafeAreaInfo`; `ParamGetInt`/`ParamGetString` — NATIVE с Phase 0), +5 UserService (все импорты), +10 PlayGo (все), +2 AppContent (все) |

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

## Phase 1 — milestone 2: свой VFS/FileSystem — **достигнут (проверка в пределах до GPU-сбоя)**

**Что сделано.** `src\runtime\filesystem\`: `Vfs` (`/app0`, `/hostapp` — только чтение; `/temp0`,
`/temp` — `userdata\temp0\<id>`, очищается при старте; `/download0`; `/data`; `/savedata0` — зеркало
монтирования SaveData, пока SaveData — BRIDGE), файловые функции libkernel, каталоги PFS (`/app0`) и
обычные, DiscMap. От shadPS4 через `FsHost` (`src\bridge\shadps4\fs_host.cpp`) остаются: номера
дескрипторов (общая таблица с очередями событий и сокетами), guest errno (`__Error`, Kernel —
Phase 2), инвалидация GPU-страниц перед чтением (Memory/GPU), хост-папка `/savedata0`, дескрипторы,
которые открывали не мы (std, `/dev`, сокеты).

**Проверка.**

| Критерий | Результат |
|---|---|
| самотест `--selftest` (NATIVE и BRIDGE на одних и тех же вызовах, см. ниже) | 71 проверка FS, 0 расхождений, 4 известных отличия |
| порядок открытия файлов внутри каждого гостевого потока при том же состоянии сохранений (`vram_oracle` ↔ `m3_sspc`) | Game:Main 4/4, GameState 3/3, SceFiosIO 45/45 — **идентичен** |
| набор открытых файлов | 52/52, разница 0 |
| глобальный порядок | первые 22 одинаковы; дальше `dis.dat` открывается на другом потоке — зависит от планировщика |
| состояние сохранений меняет маршрут игры | со старыми сохранениями (`m1_oracle`) игра грузит уровень (`*.xpps`), с новыми — вступительный ролик (`v_vor_opening_no_text.bsf`); сравнивать только при одинаковых сохранениях |
| `--bridge fs` (FS обратно в BRIDGE) | работает, контрольный путь |

Оговорка: из-за сбоя GPU (SSPC-001, ~17,6 с) все игровые сравнения ограничены первыми ~17 с.
Полный игровой прогон ≥150 с на текущем окружении не проходит ни на нашем пути, ни на эталонном.

## Phase 1 — milestone 3: язык, SystemService, UserService, PlayGo, AppContent — **NATIVE; runtime-milestone не закрыт до стабильного прогона**

**Что сделано.**
- `src\runtime\system\system_service.cpp`: `GetStatus`, `ReceiveEvent` (своя очередь событий,
  `PushEvent` для своих подсистем), `HideSplashScreen` (своей заставки нет; у oracle она выключена),
  `GetDisplaySafeAreaInfo` (ratio 1.0). `ParamGetInt`/`ParamGetString` — NATIVE с Phase 0.
- `src\runtime\user\user_service.cpp`: пользователи слотов 1–4 = id 1000–1003 (как у oracle;
  сохранения лежат в `userdata\home\<id>`, BRIDGE-Pad ищет контроллеры по тем же id), очередь
  Login/Logout под мьютексом. Логины приходят от слоя ввода (BRIDGE до Phase 5) через патч `0003`
  (`SetUserServiceEventSink`) и `src\bridge\shadps4\user_host.cpp`; взятое игрой событие передаётся
  NpManager (BRIDGE), как в oracle. На пути `--loader shadps4` и при `--bridge user` приёмник не
  ставится — работает исходная очередь shadPS4.
- `src\runtime\playgo\playgo.cpp`: пакет установлен полностью (все чанки LocalFast, прогресс =
  размер), разбор `/app0/sce_sys/playgo-chunk.dat` (71 чанк), маска языков от системного языка.
- `src\runtime\app_content\app_content.cpp`: дополнительного контента нет (как у oracle без папки
  add-on и лицензий): `Initialize` (BUSY при повторе для SDK ≥ 1.50), `GetAddcontInfo` → нет права.
- Язык полностью NATIVE: `ParamGetInt(Lang)` = 8 (ru-RU) из `SecondSonPC.toml`; маску PlayGo игра
  выставляет сама (`0x4080000000000000`: английский + русский). `EmulatorSettings.SetConsoleLanguage`
  в bootstrap остаётся только для внутренних пользователей shadPS4 (трофеи, SaveData — BRIDGE).

**Проверка.**

| Критерий | Результат |
|---|---|
| самотест `--selftest` | PlayGo 95, AppContent 4, SystemService 18 (5 известных отличий), UserService 6 + 2 проверки событий — **0 расхождений** |
| порядок импорт-вызовов (`m3co_oracle` ↔ `m3co_sspc`, `--call-order 400000`) | первые 1933 идентичны (как в M1; дальше стартует 2-й гостевой поток); все вызовы PlayGo/SystemService в этом префиксе на тех же местах |
| число вызовов M3-функций за 400 000 импорт-вызовов | как у oracle: `scePlayGoGetProgress` 71, `sceAppContentGetAddcontInfo` 16, `sceUserServiceGetEvent` 15, `sceSystemServiceGetStatus` 14, `ParamGetInt` 2, `scePlayGoGetLocus` 2, `sceUserServiceInitialize`/`GetLoginUserIdList`/`GetInitialUser`/`ParamGetString` по 1 |
| в игре (`m3_sspc`) | PlayGo: 71 чанк; событие Login user 1000 взято игрой; AppContent инициализирован; 52 открытия файлов |
| точка сбоя GPU (оба пути) | 17,6–17,7 с, `ErrorOutOfDeviceMemory`, ~560–570 кадров к 10 с — одинаково |

### Самотест подсистем (`SecondSonPC.exe --selftest`)
`src\bridge\shadps4\selftest.cpp`. Игра не запускается. Каждая NATIVE-подсистема прогоняется рядом
с BRIDGE-реализацией shadPS4 на одинаковых входах; функции берутся из таблиц привязок по NID — ровно
те адреса, что получает игра. FS работает в двух временных папках (`userdata\selftest\{native,bridge}`
→ `/sspctest`); `/app0` на время теста — копия `sce_sys` только для чтения (запись в папку игры
исключена даже при ошибке). Отчёт `logs\selftest.txt`, код выхода 0 = всё совпало.
Последний прогон: **196 проверок, 0 расхождений**, 9 известных отличий:

| Отличие | Почему |
|---|---|
| `write` в файл, открытый на чтение: NATIVE EBADF, oracle −1 без errno | `File::Write` shadPS4 не выставляет errno (код = прежнее значение errno; тест обнуляет его перед вызовом и получает 0x80020000); EBADF — поведение FreeBSD |
| `sceKernelTruncate` (3 строки) | в oracle — COMMON_STUB (возврат 0, файл не меняется); у нас реализован |
| `ParamGetInt` TimeZone/Summertime и неизвестные id; `ParamGetString(SystemName)` | NATIVE уже в Phase 0 (oracle использовал наши); у shadPS4 константы +120/1, 0 для неизвестных id, буфер не пишется. Игра запрашивает только `Lang` |

### Какие компоненты shadPS4 исключены из пути `sspc` (M1–M3)
- загрузка: `Core::Emulator`, `Core::Linker`, `Core::Module`, `Core::Loader::Elf`, `SymbolsResolver`
  для разрешения импортов, aerolib (M1);
- файловая система игры: файловые функции `kernel/file_system.cpp`, `Core::FileSys::MntPoints` для
  путей игры, `Core::Directories::{Pfs,Normal}Directory` (M2);
- `libSceSysmodule` (M1); `libSceSystemService` (все 6 импортов), `libSceUserService` (все 5),
  `libScePlayGo` (все 10), `libSceAppContentUtil` (все 2) (M3).

## Открытые пункты (до финала Phase 1)

1. **Старт BRIDGE-библиотек.** `RegisterLib` shadPS4 не только регистрирует символы, но и запускает
   библиотеки (GnmDriver: Liverpool + Vulkan presenter/rasterizer; VideoOut: драйвер; Kernel:
   служебный поток, часы, семафоры, AIO; Pad: контроллеры; NpManager: обработчик). Состояние
   `static` в их `.cpp`, извне недоступно. Временно путь `sspc` вызывает `InitHLELibs` в
   выбрасываемую таблицу (`StartBridgeLibraries` в `bootstrap.cpp`) — для разрешения она не
   используется. Убрать: патч `0003`, выделяющий старт библиотек из регистрации.
2. EmulatorSettings/UserSettings/config.json ядра ещё загружаются (общая часть bootstrap); от них
   зависят BRIDGE-подсистемы (GPU, Input, SaveData). `UserManagement` нужен BRIDGE-вводу для логина.
3. `RunGame` ещё монтирует пути в `MntPoints` и читает `param.sfo` через `PSF` shadPS4 — для
   BRIDGE-библиотек (SaveData, трофеи) и заполнения `ElfInfo`. Перевести чтение `param.sfo` на свой
   код, `MntPoints` оставить только для BRIDGE-SaveData.
4. **Сбой GPU (SSPC-001, `docs\KNOWN_ISSUES.md`)**: без стабильного игрового прогона финальный
   критерий Phase 1 (≥150 с вступления на русском) не проверить. Сбой одинаков на обоих путях.
