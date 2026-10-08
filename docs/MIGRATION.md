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
| Phase 2 P2.0 (2026-10-08, `e7ee32b`) | 54 | 2 | 341 | 18 | 2 | 147 | конфигурация (не импорты): путь `sspc` не читает `config.json`/`users.json`/`keys.json` |
| Phase 2 P2.1 (2026-10-08) | 61 | 2 | 334 | 18 | 2 | 147 | +7 `libScePad` (все импорты): клавиатура/мышь + реальные контроллеры |
| Phase 2 P2.2 (2026-10-08) | 71 | 2 | 324 | 18 | 2 | 147 | +10 libkernel «время»: `clock_gettime` ×2, `sceKernelClockGettime`, `gettimeofday` ×2, `sceKernelGettimeofday`, `nanosleep`, `sceKernelUsleep`, `sceKernelGetTscFrequency`, `sceKernelIsNeoMode` |

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

## Phase 1 — финальный этап: SSPC-001, жизненный цикл BRIDGE-библиотек, свой param.sfo (2026-10-08)

Счётчики привязок не меняются (это инфраструктура старта, а не импорты): NATIVE 54, OBSERVED 2,
BRIDGE 341, COMMON_STUB 18, UNRESOLVED 2, LLE 147.

**SSPC-001 устранён** (`docs\KNOWN_ISSUES.md`): исчерпание системного commit при загрузке уровня;
патч `0005` (загрузка в арену порциями ≤ 64 МиБ), диагностика — патч `0004` +
`src\bridge\shadps4\gpu_alloc_diag.cpp`. Коммиты `439010a`, `ef90f92`.

**Жизненный цикл BRIDGE-библиотек (`StartBridgeLibraries`, `bootstrap.cpp`).** Путь `sspc` больше
не вызывает `InitHLELibs`. У shadPS4 нет отдельной точки старта библиотеки — `RegisterLib`
регистрирует символы и одновременно стартует библиотеку (состояние `static` в её `.cpp`). Поэтому:
- регистрация функций — не нужна: импорты разрешает своя статическая таблица (с M1);
- старт — явный список библиотек, у которых `RegisterLib` что-то запускает, в порядке `InitHLELibs`,
  и только если у библиотеки остался хотя бы один BRIDGE-импорт (символы уходят в выбрасываемую
  таблицу, в лог — что и почему стартовало):

  | Модуль | Что стартует `RegisterLib` |
  |---|---|
  | libkernel | служебный поток, часы процесса, очереди AIO, таблица семафоров, сетевые хуки ядра |
  | libSceGnmDriver | Liverpool (командный процессор), Vulkan presenter |
  | libSceVideoOut | драйвер VideoOut |
  | libScePad | SDL-контроллеры (они же логинят пользователей) |
  | libSceNpManager | колбэк состояния NP, версия SDK |

- `--selftest`: дополнительно SystemService, UserService, AppContent, PlayGo — их shadPS4-сторона в
  сравнении читает глобальные из `RegisterLib` (версия SDK);
- больше не регистрируются и не стартуют на пути `sspc`: ~65 библиотек, которые игра не импортирует
  (Camera, Hmd, Usbd, Audio3d, Fiber, Ime, …), и библиотеки, которые SecondSonPC реализует сам
  (SystemService, UserService, AppContent, PlayGo, Sysmodule); поиск LLE-модулей в
  `sys_modules\<serial>` (часть `InitHLELibs`) — тоже. Библиотека без побочных эффектов старта
  (AudioOut, Ajm, Http, Net, NetCtl, NpTrophy, SaveData, Videodec, диалоги) работает и без
  `RegisterLib`. Проверено: в логе моста нет «Initializing HLE libraries», 5 библиотек стартовали.
- Остаётся: те 5 `RegisterLib` всё ещё регистрируют символы (лишняя работа, без влияния). Полностью
  уйдёт, когда каждая из 5 подсистем станет NATIVE (Kernel — Phase 2, Pad — Phase 3, Gnm/VideoOut —
  Phase 4) или патчем, выделяющим их старт, если понадобится раньше.
- Известный разрыв: BRIDGE `NpHandler` кладёт событие приглашения в сессию (`np_handler.cpp:686`) в
  очередь SystemService **shadPS4**, а на пути `sspc` игра читает очередь SystemService SecondSonPC.
  Возникает только с онлайн-сервисом shadNet (выключен) — оставлено до Phase 3 (NP офлайн).

**Свой param.sfo (`src\runtime\psf\param_sfo.{h,cpp}`).** Разбор PSF (версии 1.0/1.1; форматы
текст 0x0204, целое 0x0404, двоичный 0x0004 — как у shadPS4; полная проверка границ). Путь `sspc`
читает `param.sfo`, `pic1.png`, `icon0.png` с хоста напрямую (`[Game] path\sce_sys`), без
`MntPoints` и без `PSF` shadPS4; путь `shadps4` — без изменений. Синглтон `PSF` shadPS4 на пути
`sspc` наполняется из того же файла по пути на хосте только для BRIDGE-SaveData
(`INSTALL_DIR_SAVEDATA`). `MntPoints` на пути `sspc`: только `/app0` и `/hostapp` — их читает
BRIDGE-SaveData (`/app0/sce_sys/save_data.png`) и таблица трофеев; `/data`, `/temp0`, `/temp`,
`/download0` и шрифты больше не монтируются (все файловые функции libkernel — NATIVE, libSceFont игра
не импортирует). Самотест: 26 значений `param.sfo` совпадают с `PSF` shadPS4, 24 проверки «чужой
формат → пусто», 5 повреждённых образов отвергнуты (у `PSF` shadPS4 проверок границ нет).

**Проверки этапа.**

| Проверка | Результат |
|---|---|
| `--selftest` | **251 проверка, 0 расхождений** (196 + 55 ParamSfo), 9 известных отличий |
| 150-с прогоны после 0005 (`f2_*`) | 4/4: 2 × `shadps4`, 2 × `sspc`; штатное закрытие, игра жива до конца (~8700 кадров), пик VRAM 4200 МиБ, пик commit 29,6–29,8 ГБ, 275 пайплайнов |
| 150-с прогоны после 5.1 + 5.3 (`m4_*`) | `sspc` 2/2 (8648 и 8708 кадров, commit 30,1–30,3 ГБ, 275/277 пайплайнов); `shadps4` — см. сводку `logs\runs\m4_summary.jsonl` |
| Язык | 8 русских файлов локализации открываются (`lang_russian_*`, `russian_*.psarc`); на снимке экрана титульный экран «✕ Продолжить» и вступительный текст — на русском |

Оговорка о прогонах: без нажатий игра доходит до титульного экрана «✕ Продолжить» и ждёт; критерий
«≥ 150 с вступления» требует нажатия ✕. Для этого добавлен `--press key@сек,…` (нажатия кладутся в
очередь событий окна, фокус не нужен; в раскладке моста по умолчанию ✕ = `n`).

**Вступление (`intro_*`, 210 с, `--press n@45`).** Оба пути: титульный экран → ✕ на 45 с →
вступительный текст → ролик → первая игровая сцена (обучение граффити «Держите беспроводной
контроллер боком» — на снимках `logs\runs\intro_{sspc,shadps4}_1\screen.png` один и тот же кадр).
| Путь | Длительность | Кадров | Сбой | Пик VRAM | Пик commit | Пайплайнов |
|---|---|---|---|---|---|---|
| `sspc` | 209,8 с, штатно | 12 246 | нет | 4275 МиБ | 30 155 / 32 655 МиБ | 453 |
| `shadps4` | 209,8 с, штатно | 12 288 | нет | 4122 МиБ | 30 075 / 32 655 МиБ | 393 |

Вступление на русском ≥ 150 с на собственном загрузчике проходит (165 с после ✕); оставшееся
условие финала Phase 1 — конфигурация (п. 2 ниже). Разница в числе пайплайнов — компиляция по мере
появления эффектов в разные моменты (кадры совпадают), отдельно не исследовалась.

## Карта зависимостей от EmulatorSettings — **закрыто в P2.0** (см. ниже «Phase 2 — P2.0»)

Исходная оценка (до P2.0):

Мост читает ~70 разных настроек `EmulatorSettings` (`runtime\sspc\user\config.json`, плюс то, что
выставляет `bootstrap.cpp` из `SecondSonPC.toml`). По подсистемам (число разных геттеров):
`video_core` 23 (present mode, пайплайн-кэш, readbacks, FSR/RCAS, маркеры и validation Vulkan,
HDR, внутреннее разрешение…), NP 14 (shadNet, сеть, трофеи), аудио 7 (бэкенд, устройства,
громкость), память 3 (`neo_mode`, extra dmem/fmem), ввод 3, ядро 2, SaveData 2 (домашняя папка),
Pad 2. Уже задаются из `SecondSonPC.toml`: язык, домашняя папка (сохранения), окно, полноэкранный
режим, vblank, сеть выкл., фильтр лога.
Порядок переноса (без копирования `EmulatorSettings` целиком):
1. Параметры, определяющие эмулируемую консоль и влияющие на поведение игры — `neo_mode`,
   `dev_kit_mode`, `extra_dmem/fmem`, язык, имя системы — секция `[Console]` в `SecondSonPC.toml`,
   выставляются SecondSonPC явно (сейчас в `config.json`: все выключены/0 — совпадают с умолчаниями).
2. Ввод — с Phase 3 (свой бэкенд ввода), аудио — Phase 3, SaveData — Phase 3, GPU — Phase 4
   (свой Vulkan-бэкенд, секция `[Graphics]`).
3. После этого `config.json` перестаёт загружаться; `EmulatorSettings` получает значения только от
   SecondSonPC до удаления моста (Phase 5).

## Открытые пункты Phase 1 — **все закрыты** (2026-10-08)

1. ~~Старт BRIDGE-библиотек~~ — сделано (выше); остаточная регистрация символов 4 библиотек
   (libkernel, Gnm, VideoOut, NpManager) — до их NATIVE. `libScePad` с P2.1 не стартует вовсе:
   реальные контроллеры открывает `StartControllers` (то, что делал его `RegisterLib`).
2. ~~EmulatorSettings/UserSettings/config.json~~ — **сделано в P2.0** (ниже).
3. ~~param.sfo через PSF/MntPoints~~ — сделано; `MntPoints` на `sspc` только для BRIDGE-SaveData и
   трофеев.
4. ~~SSPC-001~~ — устранён. Редкий SSPC-002 (`docs\KNOWN_ISSUES.md`) — наблюдать.
5. Финальный критерий Phase 1 — «вступление на русском ≥ 150 с без общей инфраструктуры
   загрузчика/конфига/ФС/системных сервисов shadPS4»: загрузчик, ФС, системные сервисы и
   конфигурация — свои; прогон `p20_sspc_1` (210 с, 12 302 кадра, 8 русских файлов) — **Phase 1
   закрыта**.

## Phase 2 — P2.0: своя конфигурация (2026-10-08, `e7ee32b`)

**Что сделано.** `SecondSonPC.toml` — единственная конфигурация пути `sspc`.
- Новые секции: `[Console]` (`neoMode`, `devKit`, `extraDirectMemoryMiB`, `extraFlexibleMemoryMiB`),
  `[Audio]` (`volume`, `device`), `[Debug]` (`vulkanValidation`, `vulkanCrashDiagnostic`); в
  `[Display]` — `presentMode`, `fullscreenMode`, `showFps`. Ошибочная таблица/ключ — ошибка запуска.
- `ApplyBridgeSettings` (`bootstrap.cpp`) выставляет мосту все значения, которые решает SecondSonPC;
  на пути `sspc` `config.json`, `custom_configs\<id>.json`, `users.json`, `keys.json` не читаются
  (и не пишутся): пользователи моста — четыре стандартных в памяти (id 1000–1003, как у
  UserService), ключ трофеев пуст (как и был в `keys.json`). Путь `shadps4` грузит файлы как в
  Phase 0, затем получает те же значения из toml.

**Разделение ~95 геттеров `EmulatorSettings` (по фактическим вызовам в коде моста):**

| Группа | Геттеры | Решение |
|---|---|---|
| Консоль (влияет на игру) | `IsNeo`, `IsDevKit`, `Get{ExtraDmem,ExtraFmem}InMBytes`, `GetConsoleLanguage`, `GetHomeDir` | `[Console]`, `[Language]`, `[Paths]` |
| Вывод | `IsFullScreen`, `GetFullScreenMode`, `GetWindow{Width,Height}`, `GetVblankFrequency`, `GetPresentMode`, `IsShowFpsCounter` | `[Display]` |
| Аудио | `GetVolumeSlider`, `GetAudioBackend`, `GetSDLMainOutputDevice` | `[Audio]` (бэкенд — SDL) |
| Сеть/NP | `IsConnectedToNetwork`, `IsShadNetEnabled` | всегда выкл. (офлайн; NP — Phase 3) |
| Диагностика Vulkan моста | `IsVkValidationEnabled`, `IsVkCrashDiagnosticEnabled` | `[Debug]` |
| Внутренности моста | readbacks, DMA, copy GPU buffers, null GPU, patch/dump shaders, inline fetch, userfaultfd, pipeline cache, маркеры Vulkan, остальная валидация, RCAS/FSR/HDR, internal resolution, `GetGpuId`, детали лога | встроенные значения shadPS4 (= нынешний `config.json`) |
| Не используется игрой | камера, IME, USB, мышь/клавиатура PS4, special pad, трофейные попапы, big picture, Discord, каталоги установки/модулей/шрифтов/аддонов, UPnP/P2P/https, `debug_dump`, red zone | по умолчанию |

**Остаток.** Раскладка **геймпадов** моста (`input_config`, читает конструктор окна) — до своего
бэкенда контроллеров (Phase 3); клавиатуру она с P2.1 не обслуживает.

**Проверки.** `--selftest` 251/0; прогоны `p20_*` (210 с): `sspc` 12 302 кадра, `shadps4` 12 304,
штатное закрытие, VRAM 4122/4121 МиБ, commit 29 637/29 606 МиБ; файлы конфигурации моста не
изменены (время изменения до/после совпадает).

## Phase 2 — P2.1: ввод (2026-10-08)

`docs\INPUT_MAPPING.md`. NATIVE `libScePad` (7/7): `src\runtime\pad\pad.cpp`; клавиатура/мышь:
`src\input\`; мост: `src\bridge\shadps4\input_host.cpp` (реальные контроллеры через
`Input::GameControllers`, часы процесса, перехват событий окна — патч `0006`). Самотест: `Pad`
23 проверки (NATIVE = BRIDGE побайтно, кроме метки времени), `Input` 30 проверок. Игровые прогоны
и снимки — `docs\PHASE2_TEST_RESULTS.md`.

## Phase 2 — P2.2: libkernel, время (2026-10-08)

Аудит — `docs\generated\KERNEL_AUDIT.md`; план — `docs\PHASE2_PLAN.md`. NATIVE (группа `kernel`,
`src\runtime\kernel\time.cpp`): часы, сон, частота TSC, `sceKernelIsNeoMode` — 10 импортов. От моста:
guest errno и часы процесса (`Kernel::Host`, `src\bridge\shadps4\kernel_host.cpp`). Самотест
`KernelTime` 18 проверок.

Следующие milestones — `docs\PHASE2_PLAN.md`: P2.2 дальше (семафоры → event flags →
mutex+cond+rwlock), P2.3 память (адаптер `MemoryForGpu` → свой `AddressSpace`/`MemoryManager`),
P2.4 потоки и свой VEH/патчер TCB; выход Phase 2 — все функции libkernel NATIVE и 30 минут игры
без падений.
