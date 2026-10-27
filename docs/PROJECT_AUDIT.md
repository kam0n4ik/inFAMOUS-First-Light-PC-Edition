# Аудит SecondSonPC (основа для First Light)

Эталон: `E:\SecondSonPC`, ветка `main`, HEAD `630fcc9`. Читали только код, эталон не собирали и не меняли.
Аудит вели три агента (ядро, GPU, движок с мостом) и сверка вручную. Пути ниже даны относительно корня репозитория
SecondSonPC. В проекте First Light тот же код лежит в `core\` (ветка `feature/first-light`).

Обозначения в столбце «Для FL»:
- **У** — универсально, работает для любой PS4-игры на этом рантайме.
- **И** — универсально, но проверено только на контенте Second Son.
- **SS** — специфично для Second Son: привязано к адресам или хешам его eboot.

## 1. Карта подсистем

| # | Подсистема | Где | Что делает | Для FL |
|---|---|---|---|---|
| 1 | Запуск | `src/app/main.cpp`, `paths.cpp`, `config.cpp`, `bridge/shadps4/early_init.cpp` | Ищет корень по `<Product>.toml`, строго разбирает TOML, читает флаги «Экспериментов», включает нативные группы по умолчанию (`main.cpp:569-612`), затем `RunGame` | У. Имена файлов вынесены в `app/product.h` |
| 2 | Загрузчик ELF/SELF | `src/loader/image_file.cpp`, `module.cpp`, `linker.cpp`, `rtld.cpp` | Разбор SELF, релокации, TLS, PRX `libc`/`libSceFios2` в режиме LLE | У |
| 3 | Таблица привязок импортов | `src/loader/bindings.cpp`, `generated/import_table.inc` | Каждый NID получает ровно одного владельца: NATIVE / OBSERVED / BRIDGE / COMMON_STUB / UNRESOLVED | **Своя таблица на игру.** Для FL сгенерирована `import_table_cusa00897.inc` |
| 4 | Ядро (kernel) | `src/runtime/kernel/*` | Потоки, синхронизация, equeue, часы, сигналы, errno. По умолчанию нативно с 2026-10-18 | У |
| 5 | Память | `src/runtime/memory/*` | Адресное пространство гостя, VMM, direct/flexible memory | У |
| 6 | CPU | `src/runtime/cpu/fs_patcher.*`, `sse4a.cpp`, `exceptions.cpp` | Патчи `fs:`, эмуляция SSE4a, фикс red zone для Intel (`vzeroupper`), диспетчер исключений | У |
| 7 | Файловая система | `src/runtime/filesystem/*`, `bootstrap.cpp:688-706` | `/app0` только для чтения, `/temp0`, `/download0`, `/data`, `/savedata0`. Подпапки по серийному номеру из param.sfo | У |
| 8 | SaveData | `src/runtime/savedata/savedata.cpp` | 8 нативных функций; сохранения в `userdata\home\<user>\savedata\<title>\<dir>` | У. Для FL добавлен `sceSaveDataTransferringMount` |
| 9 | Gnm-драйвер | `src/runtime/gpu/gnm/gnm_driver.cpp` | 85 экспортов libSceGnmDriver. Последовательность init зависит от версии SDK | У. FL импортирует **те же 85 NID** (замер) |
| 10 | Командный процессор PM4 | `src/runtime/gpu/command_processor.cpp`, `parallel_cp.cpp`, `native_backend.cpp` | Потоки T1 (разбор) / фронтенд / T2 (запись) / эмиттер. Режимы legacy, parallel, native | И. Неподдержанный пакет PM4 даёт CpError и останавливает очередь |
| 11 | Vulkan-бэкенд | `src/runtime/gpu/vulkan/*` | Исполнитель, кэши буферов и изображений, импорт памяти гостя окнами, timeline-синхронизация | И |
| 12 | Шейдеры | `src/runtime/gpu/gcn/*`, `shader/*`, `bridge/shadps4/shader_translator_host.cpp` | Свой транслятор GCN→SPIR-V (cs/vs/ps); резерв — рекомпилятор shadPS4 | И. Только VS+PS: ES/GS/LS/HS не рисуются (`shader_frontend.cpp:1637`) |
| 13 | Кэш шейдеров и предсказание | `shader/shader_disk_cache.cpp`, `static/*`, `vk_precompile.cpp` | Кэш на диске `<dir>\<serial>\native[_own]\<generation>`; статическое предсказание по KCAP-пакам `.xpps` | У для кэша (свой каталог на серийный номер). **SS** для шаблонов `data\static_templates.txt` |
| 14 | Презентер и VideoOut | `native_videoout.cpp`, `vk_presenter.cpp` | vblank 59,94 Гц или свободный темп, ограничитель FPS, mailbox/fifo/immediate | И. Допущение «дельта кадра по TSC с ограничением 1/30 с» проверено только на SS |
| 15 | Апскейлеры | `src/runtime/gpu/upscale/*`, `vk_executor_upscale.cpp` | DLSS (через `sspc_dlss.dll`), FSR 3.1, генерация кадров | **SS**: размер сцены, TAA и камера берутся из хуков SS. На FL отключено (§3) |
| 16 | Звук | `src/runtime/audio/ajm.cpp`, AudioOut | Свой AJM, только ATRAC9 (`SSPC_R10_AJM=native`); AudioOut | И. Тот же Wwise |
| 17 | Ввод | `src/runtime/pad/pad.cpp`, `src/input/*` | Виртуальный DualShock 4 с клавиатуры и мыши, гироскоп, тачпад; SDL-геймпады | У. Прямая камера мышью — хук SS |
| 18 | Настройки ПК и меню | `src/settings/*`, `src/ui/*` | Реестр настроек, `%LOCALAPPDATA%\<Product>\settings.toml`, меню F1 и оверлей | У. Механика общая; часть пунктов работает только через хуки SS (§2) |
| 19 | Лаунчер | `src/launcher/*` | Окно Direct2D, профили запуска, бэкап сохранений перед стартом | У. Пресет `launch.cpp:22-50` взят с SS |
| 20 | NP, трофеи, DLC | `src/runtime/np/*`, `app_content/*` | Эмуляция входа в PSN, офлайн Http/Ssl/NetCtl, трофеи из `trophy00.trp`, DLC по правилу `UNLOCK*` | У. Таблица текстов трофеев — **SS** (48 ключей) |
| 21 | Хуки движка | `src/engine/*`, `bootstrap.cpp:872-960` | ~30 установщиков: меню, камера, разрешение, FOV, TAA, частицы, отражения, тени и т. д. | **SS**. На FL замер: **0 из 95** закреплённых функций совпадают, все отключаются сами |
| 22 | Нативный движок | `src/native_engine/*` | 18 реконструкций функций SS, закреплённых хешами | **SS**. По умолчанию не ставится ничего |
| 23 | Логи и диагностика | `src/base/log.*`, `crash.*`, `src/trace/*` | `<Product>.log`, minidump, `calls.json`, журнал фризов | У |
| 24 | Сборка | `CMakeLists.txt`, `cmake/shadps4_bootstrap.cmake`, `build.ps1` | Цель `shadps4` из shadPS4 `0fe263a` + 34 патча + своя статическая библиотека `sspc_runtime` | У |

## 2. Изменения относительно shadPS4

- **Закрепление.** shadPS4 закреплён на коммите `0fe263a4760dfbfa973366890061749b4af0de97`. 34 патча из
  `patches/shadps4/*.patch` CMake проверяет как одну цепочку (`cmake/shadps4_bootstrap.cmake:14-51`).
- **Ни один патч не содержит адресов или идентификаторов Second Son.** Список (подробности в `docs/PATCHES.md`
  эталона):
  - сборка: 0001;
  - стыковка со своим рантаймом: 0002, 0003, 0006, 0007, 0008, 0009, 0012, 0021–0026, 0030, 0032, 0033;
  - исправления ошибок: 0010, 0011, 0016;
  - производительность моста: 0005, 0015, 0018–0020;
  - кэш шейдеров моста: 0013;
  - диагностика: 0004, 0014, 0017, 0027–0029, 0031, 0034.
- **Что осталось от shadPS4 в рабочем пути** (`--gpu-backend sspc`, `--loader sspc`):
  - libkernel моста (сервисный поток, AIO);
  - окно SDL и геймпады SDL;
  - разбор PSF;
  - 38 импортов BRIDGE: по логу SS в игре они не вызываются;
  - рекомпилятор шейдеров как резерв.
  
  Загрузчик, Gnm, PM4, Vulkan, AudioOut, Pad, SaveData, NP и трофеи — свои.
- **Граница с мостом проверяется при каждой сборке** (`cmake/sspc_boundary_check.cmake`). Файл моста может включать
  заголовки shadPS4 только из списка в `cmake/sspc_boundary_allowlist.txt`. Для First Light туда добавлена строка
  `bridge_exports_first_light.cpp`.

## 3. Что специфично для Second Son и как это ведёт себя на чужом eboot

Как проверяются хуки (`engine/guest_hooks.cpp`):
- FNV-1a 64 считается по всему телу функции, по нетронутой копии, которую загрузчик сохранил до патчера CPU.
- Если хеш не совпал, пишется «another game version», и хук не ставится.
- **[замер]** На eboot FL ни одна из 95 закреплённых функций не совпадает по своему RVA. 10 тел встречаются побайтно
  в другом месте; это в основном мелкие функции.
- Код установки разрешения есть и в FL, сдвинутый на +0x46a90: RVA ≈ 0x2369d7 / 0x2369e5 / 0x236e88. Значит, перенос
  разрешения сводится к новой таблице мест.

Что было опасно и что сделано:

| Место | Риск на FL | Что сделано |
|---|---|---|
| `VerifyGuestFunction` вызывал `Cpu::RestoreRedZoneEntries` **до** сравнения хеша | При неудачной проверке на чужом eboot возвращались исходные байты входа функции, которую патчер CPU исправил для Intel red zone. Это ошибка общего ядра | **Исправлено в корне** (`core/src/engine/guest_hooks.cpp`): сначала хеш нетронутой копии, восстановление только при совпадении. На SS поведение не меняется: там хеш совпадает |
| `SSPC_R12_LIGHT_LIST_KEEP` выбирает шейдер по GPU-адресу SS `0x2edafa400`, без хеша содержимого | Неверное срабатывание: результаты чужого compute-шейдера не попадут к CPU | Для FL флаг игнорируется с предупреждением (`vk_buffer_cache.cpp`, `#if SSPC_PRODUCT_FIRST_LIGHT`) |
| Генерация кадров читает `*(base+0x7f2d2b8)` — камеру SS — без проверки | Адрес попадает в bss FL: прочитается чужой указатель, затем по нему пойдёт обращение | В сборке FL чтение вырезано (`vk_executor_upscale.cpp`). Кроме того, общее правило ниже |
| Апскейлер без хука переключения размера рендера | Сцена только растягивается; T2x игры не выключается | **Общее правило** (`settings_host.cpp`, `render_resolution.cpp: RenderSwitchInstalled`): без установленного переключателя апскейлер и генерация кадров выключаются для любой игры и версии |
| `SSPC_FB_CLOCK_PROBE`, `SSPC_MOUSE_PROBE` читают фиксированные RVA без проверки | Возможно нарушение доступа | По умолчанию выключены; в `play.cmd` FL не включаются. Занесено в TODO |
| Шаблоны предсказания шейдеров `data\static_templates.txt` сняты с SS | Минуты лишней работы, раздутый кэш | `predictAll = false` в `FirstLightPC.toml`; в каталоге FL нет `data\` |
| Тексты трофеев: 48 ключей SS; платиной считается id 0 | Уведомления покажут «Трофей N»; правило про платину не сверено | TODO |
| Пресет лаунчера и страница «Эксперименты» описывают адреса SS | Вводят в заблуждение | `play.cmd` FL без хуковых флагов SS; страница — TODO |

## 4. Импорты: главное препятствие (замер)

`tools\ps4elf.py` сравнивает eboot по NID:

| | SS (`E:\shdps4\CUSA00004\eboot.bin`) | FL (`eboot.bin` этой папки) |
|---|---|---|
| Импортов | 466 | 510 |
| Общих | 464 | 464 |
| Только у этой игры | 2 (`localeconv`, `malloc`: строки LLE libc) | 46 |

46 импортов есть только у FL:

| Библиотека | Сколько | Как решено |
|---|---|---|
| libSceNpScore | 8 | HLE shadPS4 через `bridge_exports_first_light.cpp`: без входа в PSN возвращает `ORBIS_NP_ERROR_SIGNED_OUT` |
| libSceNpTus | 8 | то же |
| libSceNpWebApi | 10 | то же |
| libSceRtc | 4 | HLE shadPS4, функции полные |
| libSceSaveData | 1 (`sceSaveDataTransferringMount`) | Своя нативная реализация в `savedata.cpp`: только чтение сохранений другого тайтла |
| libSceJson2 | 15 | В shadPS4 нет реализации; пока COMMON_STUB (возвращает 0). Наблюдаем на запуске |

Без этих правок все 46 импортов получили бы `PendingImportStub` и молча возвращали бы 0 (`linker.cpp:316-328`).

## 5. Флаги и прошлые A/B-замеры (что включать)

- **Стандартный пресет SS** (`play.cmd` = лаунчер «Стандарт»): CP_MODE=native, CITY_PERFORMANCE, REA_MERGE_RUNS,
  CP_ROUND_NODRAIN, P411_ASYNC_ACQUIRE, ASYNC_PIPELINES, P53_FRONTEND_THREAD, CP_ASYNC_WRITES, CP_ASYNC_FLIP,
  VK_EMIT_THREAD + VK_EMIT_BATCH (город 39,1 → 41,5 FPS), CP_RING=128, R9_PCACHE, R9_HEAP_WINDOW, NATIVE_GBUFFER,
  R11_EARLY_SUBMIT=256, R10_AJM/SYSRT=native.
- **Что из него взято для FL:**
  - всё общее;
  - **без** NATIVE_GBUFFER: это хук SS;
  - **без** R11_EARLY_SUBMIT: в R11 был один TDR с неустановленной причиной; включать после отдельной проверки на FL;
  - части CITY_PERFORMANCE и native-режима, которые являются хуками движка (CITY_ENGINE_OPT, CITY_ENGINE_GBUFFER_RUNS,
    P54_EARLY_SUBMIT), явно выставлены в 0.
- **Экспериментальный профиль SS остаётся выключенным:** REA_DEFER_WAIT_B, FB_ASYNC_COMPUTE, R10_AQ_MAIN_FREE,
  R11_AQ_IMAGES, R11_AQ_ORDER_EPOCH. Причины:
  - у линии AQ в прошлом была потеря устройства и зависание «128 fences held»;
  - на Neon Dash 1 % low хуже: 38,9/18,9 → 38,1/16,3;
  - REA-3 — хук SS.
- **Консервативная база первого запуска:** `play_safe.cmd` = CP_MODE=parallel без всего пресета.

## 6. Сборка (замер)

- Тулчейн: clang-cl из `E:\SecondSonPC\toolchain\LLVM`, LLD, Windows SDK 10.0.26100, CMake и Ninja из
  `E:\BuildTools`.
- Полная сборка SS ≈ 563 с (2787 шагов). Инкрементальная сборка `sspc_runtime` ≈ 30 с, размер exe 72,7 МБ.
- В дереве нет целей CMake для тестов: `sspc_settings_tests` и другие лежат в теге `archive/tests-20261023`. Самопроверки
  встроены в exe (`--selftest`, `--fault-test` и т. д.).
