# Архитектура inFAMOUS: First Light — PC Edition

## 1. Принцип: один рантайм — два продукта

FirstLightPC не копирует и не переименовывает SecondSonPC. Обе игры собираются из **одного** исходного кода рантайма.
Продукт выбирается при конфигурации CMake.

```
E:\SecondSonPC                       эталон (git, main); не трогаем: в нём играют
  third_party\shadPS4                shadPS4 0fe263a + 34 патча (общий)
  third_party\upscalers              NGX/FidelityFX SDK (общий)

E:\inFAMOUS-First-Light-PC-Edition   этот проект (свой git, приватный репозиторий)
  eboot.bin, sce_sys, sce_module, art   данные игры (в git не входят, только чтение)
  core\        git worktree репозитория SecondSonPC, ветка feature/first-light
    third_party\shadPS4   -> junction на E:\SecondSonPC\third_party\shadPS4
    third_party\upscalers -> junction на E:\SecondSonPC\third_party\upscalers
  build\       своя папка сборки (SSPC_PRODUCT=FirstLight) -> build\bin\FirstLightPC.exe
  FirstLightPC.toml   конфигурация (корень ищется по этому файлу)
  play.cmd, play_safe.cmd, build.ps1
  tools\       ps4elf.py, gen_import_table.py
  docs\        эти документы
  userdata\    сохранения FL (создаётся при запуске; в git не входит)
  cache\       кэш шейдеров FL (в git не входит)
  logs\        логи (в git не входит)
```

**Почему worktree, а не копия.**
- Исправление общего ядра сделано один раз и видно обеим играм: ветку `feature/first-light` можно слить в `main` SS.
- Гигабайты не копируются: shadPS4 и SDK апскейлеров подключены junction-ссылками.
- Рабочая копия `E:\SecondSonPC` и её `build\bin` не меняются, поэтому поколение кэша шейдеров у игрока не сбрасывается.

## 2. Продукт как параметр сборки

| Что | Где | SecondSon (по умолчанию) | FirstLight |
|---|---|---|---|
| Выбор | `core/CMakeLists.txt`: `SSPC_PRODUCT` (CACHE STRING) | `SecondSon` | `FirstLight` |
| Имя exe | `SSPC_PRODUCT_EXE` → `OUTPUT_NAME` в `cmake/shadps4_bootstrap.cmake` | SecondSonPC.exe | FirstLightPC.exe |
| Определение компилятора | `add_compile_definitions(SSPC_PRODUCT_FIRST_LIGHT=1)` | нет | есть |
| Идентичность | `core/src/app/product.h` → `kProduct` | SecondSonPC, «inFAMOUS: Second Son — PC Edition», «SECOND SON», CUSA00004 | FirstLightPC, «inFAMOUS: First Light — PC Edition», «FIRST LIGHT», CUSA00897 |
| Конфигурация | `ProductFile(".toml")` | SecondSonPC.toml | FirstLightPC.toml |
| Лог | `ProductFile(".log")` | logs\SecondSonPC.log | logs\FirstLightPC.log |
| Пользовательские настройки | `%LOCALAPPDATA%\<name>\settings.toml` (`settings/user_store.cpp`) | …\SecondSonPC\ | …\FirstLightPC\ |
| Профиль лаунчера | `%LOCALAPPDATA%\<name>\launcher.toml` | …\SecondSonPC\ | …\FirstLightPC\ |
| Дамп падения | `logs\crash\<name>_<дата>_<pid>.dmp` (`base/crash.cpp`) | SecondSonPC_… | FirstLightPC_… |
| Таблица импортов | `loader/bindings.cpp` | `generated/import_table.inc` | `generated/import_table_cusa00897.inc` |

Без определения сборка SS получает ту же таблицу импортов, те же имена и тексты: все ветки `#if` для FL выключены.
Для SS в ветке меняются только общие места:
- **исправление порядка проверки хуков** (`guest_hooks.cpp`). На SS хеши совпадают, поэтому поведение прежнее.
- **Правило «без переключателя размера рендера нет апскейлера».** На SS переключатель ставится.
- **Нативная `sceSaveDataTransferringMount` и экспорты моста FL.** SS их не импортирует, так что они не привязываются.
  Проверка согласованности относит их к «импорту другой игры».

Определение компилятора задано глобально, поэтому его видят и исходники shadPS4. Из-за этого первая сборка FL полная
(~10 мин), но она идёт в своей папке `build\` и сборку SS не затрагивает.

## 3. Изоляция данных пользователя

| Данные | Путь FL | Пересечение с SS |
|---|---|---|
| Сохранения | `userdata\home\<user>\savedata\CUSA00897\<dir>` | нет: другой корень (папка FL) и другой серийный номер |
| Бэкап сохранений перед запуском | `userdata\savedata_backups\<дата_время>` (`play.cmd`, лаунчер) | нет |
| Трофеи | `userdata\home\trophies\CUSA00897.txt` | нет |
| Кэш шейдеров | `cache\shaders\CUSA00897\native_own\<generation>` | нет |
| Настройки ПК | `%LOCALAPPDATA%\FirstLightPC\settings.toml` | нет |
| Сохранение SS для переноса | `sceSaveDataTransferringMount` читает `userdata\home\<user>\savedata\CUSA00004\<dir>` **в папке FL**, только для чтения | Сохранения SS из `E:\SecondSonPC` не читаются и не меняются. Чтобы перенести, скопируйте их в эту папку вручную |

## 4. Привязка импортов

```
tools\gen_import_table.py eboot.bin <таблица SS> <aerolib.inl> core\src\loader\generated\import_table_cusa00897.inc
```

- Строки eboot строятся заново из импортов FL. Строки libc и Fios2 берутся из таблицы SS: PRX у игр одинаковые.
- Новые строки получают `Phase0::Hle`, если NID есть в shadPS4 (aerolib), иначе `Phase0::CommonStub`.
- **Проверка генератора:** на eboot SS он воспроизводит таблицу SS один в один (564 строки, `diff` пуст).
- `bindings.cpp` знает обе таблицы. Экспорт, который импортирует только другая игра, теперь не ошибка, а запись в лог
  уровня Debug («only the other game imports it»). Сборка SS проверяет согласованность так же строго, как раньше.
- HLE-функции shadPS4 для импортов только FL (NpScore, NpTus, NpWebApi, Rtc) перечислены в
  `core/src/bridge/shadps4/bridge_exports_first_light.cpp`. `AllBridgeExports()` складывает сгенерированный список SS и
  этот список. Файл занесён в список разрешённых для проверки границы (`cmake/sspc_boundary_allowlist.txt`).

## 5. Решения о корректности (без фиктивных успехов)

| Решение | Обоснование |
|---|---|
| NpScore/Tus/WebApi → HLE shadPS4, ответ `ORBIS_NP_ERROR_SIGNED_OUT` | PSN нет. Игра получает настоящий отказ, который умеет обрабатывать, а не нули |
| `sceSaveDataTransferringMount` — нативная реализация, только чтение | Проверка `title_id` (без `/ \ . :`), монтирование только для чтения, ошибка «не найдено», если сохранения нет |
| libSceJson2 → COMMON_STUB (возвращает 0, первый вызов пишется в лог) | Реализации нет нигде. Если вызовы пойдут на загрузке, их видно в `calls.json` и логе; тогда нужна HLE (TODO) |
| Хуки SS на FL | Проверяются хешем и отключаются сами. Исправлен порядок проверки (`guest_hooks.cpp`) |
| Апскейлер и генерация кадров | Без установленного переключателя размера рендера выключаются (общее правило, `settings_host.cpp`) |
| `SSPC_R12_LIGHT_LIST_KEEP` | В сборке FL игнорируется: ключ — GPU-адрес шейдера SS |
| Чтение камеры SS для генерации кадров | В сборке FL вырезано |

## 6. Запуск

- `play.cmd`: стандартный пресет SS без хуков SS (NATIVE_GBUFFER, R11_EARLY_SUBMIT, CITY_ENGINE_OPT,
  CITY_ENGINE_GBUFFER_RUNS, P54_EARLY_SUBMIT), плюс R10_AJM и R10_SYSRT = native. Затем бэкап сохранений и
  `build\bin\FirstLightPC.exe --no-launcher`.
- `play_safe.cmd`: CP_MODE=parallel без пресета. Это база для сравнения, если что-то рисуется не так.
- `FirstLightPC.exe` без аргументов открывает лаунчер (Direct2D) с брендом «FIRST LIGHT» и картинкой из
  `sce_sys\pic0.png` игры. Лаунчер уже есть; его пресет и страница «Эксперименты» пока описывают SS (TODO).
- Логи: `logs\FirstLightPC.log`, `logs\calls.json` (счётчик вызовов импортов, `[Trace] importCounter = true`),
  `logs\bridge_shadps4.log`, `logs\crash\`.

## 7. Сборка

`powershell -ExecutionPolicy Bypass -File build.ps1 [-Config RelWithDebInfo] [-Fresh]`:
- тулчейн SS: clang-cl и LLD из `E:\SecondSonPC\toolchain`, CMake и Ninja из `E:\BuildTools`, Windows SDK 10.0.26100;
- общая очередь сборок `E:\research\r10\build.lock`, пониженный приоритет;
- лог `build\build.log`.

Проверка SS из ветки: отдельная папка сборки с `-DSSPC_PRODUCT=SecondSon`. Не в `E:\SecondSonPC\build`.
