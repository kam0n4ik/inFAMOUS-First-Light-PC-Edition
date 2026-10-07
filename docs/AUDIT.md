# SecondSonPC — AUDIT (Этап 1)

Дата: 2026-10-07. Статус: аудит завершён. Кода рантайма ещё нет.
Сгенерированные данные: `E:\SecondSonPC\docs\generated\IMPORTS.md`, `imports.csv`, `prx_imports.csv`, `summary.json`.

## 0. Коротко

1. **Почему игра на английском (корневая причина).** Причина не в заглушке `sceSystemServiceParamGetString`. Игра берёт язык из `sceSystemServiceParamGetInt(1 /*Lang*/)`. Установленная сборка shadPS4 `0fe263a` перешла на `config.json` (там `console_language: 1`) и **игнорирует** `custom_configs\CUSA00004.toml` (`consoleLanguage = 8`); в логе стоит `Game-specific config used: false`.
2. **Эксперимент подтвердил.** Стоковый shadPS4 без единой правки кода с `console_language = 8` сам открывает `lang_russian_*.xpps` и `russian_*.psarc`, ничего переименовывать не нужно (раздел 3.4).
3. **`sceSystemServiceParamGetString` вызывается с `(6 /*SystemName*/, buf, 65)`.** Заглушка shadPS4 не пишет в буфер, и игра читает неинициализированный стек. Это баг, но к языку он отношения не имеет.
4. **Папка игры уже изменена.** Все 4 файла `english_*.psarc` — побайтные копии `russian_*.psarc`; оригиналы лежат в `_LANG_BACKUP_2026-10-07_22-58-16`. См. раздел 2.
5. **Состав импортов.** eboot.bin импортирует 466 символов из 26 модулей (+3 библиотеки объявлены, но символов из них нет: `libSceMouse`, `libSceGameLiveStreaming`, `libSceNpCommon`). 142 из них закрывают LLE-модули самой игры (`sce_module\libc.prx`, `libSceFios2.prx`). **Рантайм обязан предоставить 417 уникальных функций** (eboot + prx). Это целевая таблица привязок для миграции.
6. **Покрытие shadPS4.** Все 466 импортов разрешаются. Неразрешённых нет: 270 реализованы, 7 частично, 28 STUBBED, 2 DUMMY, 15 тривиальных return. Блокеров запуска нет: игра в shadPS4 идёт 30+ минут.
7. **Целевая архитектура:** собственный Loader + Runtime SecondSonPC. shadPS4 используется только как временный bootstrap (Phase 0), донор кода и oracle. План полного удаления зависимости — разделы 10–13.

## 1. Исходные данные

| Что | Значение |
|---|---|
| Игра | inFAMOUS™ Second Son, `CUSA00004`, App 01.07, маркер `++ retail 0212.1422 i3_eu ship ++` (EU) |
| Путь | `E:\shdps4\CUSA00004` |
| eboot.bin | SELF (fake-signed, сегменты расшифрованы), 17 079 995 байт, SHA-256 `2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab` |
| Оригинальное имя | `d:/projects/iss/code/bin/ne_ship.elf` (движок iSS, User-Agent `iSS Engine`) |
| SDK | eboot `0x4008061`, param.sfo `0x4008000` |
| LLE-модули игры | `sce_module\libc.prx`, `sce_module\libSceFios2.prx` (SELF) |
| Эталонный shadPS4 | Pre-release `0fe263a4760dfbfa973366890061749b4af0de97` (v0.19.1 WIP, 2026-10-07), `C:\Users\kamon\AppData\Roaming\shadPS4\versions\Pre-release-shadPS4-2026-10-07-0fe263a4760dfbfa973366890061749b4af0de97\shadPS4.exe` |
| Исходники shadPS4 | `E:\SecondSonPC\third_party\shadPS4` (тот же коммит, только для чтения, сабмодули не загружены) |
| ПК | Ryzen 5 5500 (6C/12T, SSE4a есть), 32 ГБ, RTX 2060 (Vulkan 1.4.329, драйвер 596.21), Windows 10 Pro |

## 2. Состояние папки игры: отклонения от оригинала

| Файл в `art\cache\psarc` | SHA-256 сейчас | = russian? | Оригинал в `_LANG_BACKUP_2026-10-07_22-58-16\psarc` |
|---|---|---|---|
| english_a.psarc (152 383 092) | c247474d… | **да** | 163 264 452, b3c8f38f… |
| english_a_p.psarc (10 803 449) | 510b322c… | **да** | 10 639 609, c19a5c66… |
| english_b.psarc (189 346 045) | fc9ed020… | **да** | 180 774 780, 373055af… |
| english_b_p.psarc (409 743) | 880058b7… | **да** | 115, 98a2f5d8… |

`lang_english_text.xpps` и `lang_english_audio.xpps` совпадают с бэкапом, то есть оригинальные.

**Вывод.** Кто-то уже маскировал проблему переименованием файлов. Чтобы проверять реализацию честно, папку `art\cache\psarc` нужно вернуть в оригинальное состояние, скопировав 4 файла из `_LANG_BACKUP`. **Не сделано: ждёт подтверждения пользователя.** Иначе «английский» режим будет грузить русские psarc.

## 3. Язык: корневая причина и спецификация

### 3.1 Конфигурация shadPS4
- `src\core\emulator_settings.cpp:432`: per-game оверрайды читаются **только** из `custom_configs\<serial>.json`.
- `src\core\emulator_settings.cpp:657-661`: TOML-ключ `[Settings] consoleLanguage` переносится один раз и только из глобального `config.toml`, когда `config.json` ещё не существует.
- Сейчас `C:\Users\kamon\AppData\Roaming\shadPS4\config.json` → `"console_language": 1`, а файла `custom_configs\CUSA00004.json` нет. Поэтому `ParamGetInt(Lang)` возвращает 1 (English US).

### 3.2 Как игра выбирает язык (дизассемблер, адреса относительно базы модуля; shadPS4 грузит eboot по `0x55e0000`)
```
0x15d6b9  lea  rsi,[rbp-0x9c] ; mov edi,1          ; call sceSystemServiceParamGetInt   (Lang)
0x15d6ca  mov  eax,[rbp-0x9c] ; cmp eax,0x16 ; ja skip ; mov [r15],eax   ; принимаются id 0..22
0x15d6db  call 0x15d2b0                                                   ; применение настроек
0x15d6e0  lea  rsi,[rbp-0x70] ; mov edi,6 ; mov edx,0x41 ; call sceSystemServiceParamGetString
0x15d6f3  cmp  byte [rbp-0x70],0 ; je skip ; call 0x60d8d0(r15+0x70, buf)  ; strlen + копия строки
```
- Таблица имён по адресу `0xd7b500`: `char*[23]`, индекс = id языка PS4:
  `0 japanese, 1 english, 2 french, 3 spanish, 4 german, 5 italian, 6 dutch, 7 portuguese, 8 russian, 9 korean, 10 chinese, 11 chinese_s, 12 finnish, 13 swedish, 14 danish, 15 norwegian, 16 polish, 17 brazilian, 18 british, 19 turkish, 20 latino, 21 arabic, 22 quebecois`.
- Формат имени файла: `lang_%s_%s%s.xpp` (`text`/`audio`, суффикс `_p`), psarc: `<lang>_a/_b(_p).psarc`.
- Второе место вызова `0x3c299c`: `ParamGetInt(1)` → маска `scePlayGoSetLanguageMask(h, (1 << (63 - lang)) | 0x4000000000000000)`.
- **Russian = id 8.** Языковые файлы в этом EU-дампе: text+audio для english, dutch, french, german, italian, polish, portuguese, **russian**, spanish, turkish. Только text: british, danish, finnish, norwegian, swedish. Нет: japanese, korean, chinese*, brazilian, latino, arabic, quebecois.

### 3.3 `sceSystemServiceParamGetString`
- NID `SsC-m-S9JTA`, lib/mod `libSceSystemService`.
- Реальная сигнатура (подтверждена регистрами в месте вызова): `s32 sceSystemServiceParamGetString(s32 paramId, char* buf, size_t bufSize)`.
- Игра передаёт `paramId = 6` (`SCE_SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME`, имя консоли) и `bufSize = 0x41` (65: 64 символа + NUL).
- shadPS4 `src\core\libraries\system\systemservice.cpp:1979`: `int sceSystemServiceParamGetString()` без аргументов, ничего не пишет, возвращает `ORBIS_OK`.
- Между прологом функции `0x15d480` и вызовом буфер `[rbp-0x70]` не инициализируется. Значит, игра делает `strlen` по стековому мусору. Это **неопределённое поведение**, и оно должно быть исправлено.
- Спецификация для SecondSonPC (SystemService):
  - `paramId == 6`: записать NUL-terminated UTF-8 имя из конфига (по умолчанию `"SecondSonPC"`), обрезая до `bufSize-1`. Если `buf == nullptr` или `bufSize == 0` → `0x80A10003` (`ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER`).
  - Неизвестный `paramId` → `0x80A10003`, буфер не трогать (поведение реального железа для неизвестных id нужно проверить; до проверки — ошибка + лог `Warning`).
  - Логировать paramId, bufSize, длину ответа и код возврата (категория `SystemService`).
- Спецификация `ParamGetInt(Lang)`: значение из `SecondSonPC.toml [Language] system` (`"en-US"` → 1, `"ru-RU"` → 8), id валидировать по таблице 3.2, лог в категории `Language`.

### 3.4 Эксперимент (стоковый shadPS4, портативный режим, `E:\SecondSonPC\runtime\shadps4-ref`)
| Прогон | `console_language` | Открытые языковые файлы |
|---|---|---|
| `logs\reference\trace_lang1.txt` | 1 | `lang_english_{text,text_p,audio,audio_p}.xpps`, `english_{a,b,a_p,b_p}.psarc` |
| `logs\reference\trace_lang8.txt` | 8 | `lang_russian_{text,text_p,audio,audio_p}.xpps`, `russian_{a,b,a_p,b_p}.psarc` |

`ParamGetInt(param_id 1)` вызывается трижды, после него идёт `ParamGetString`. Переименование не требуется.

## 4. Структура eboot.bin

| ph | тип | vaddr | memsz | права |
|---|---|---|---|---|
| 0 | LOAD (код) | 0x0 | 0xcc55b8 | R-X |
| 1 | SCE_RELRO | 0xcc8000 | 0xd8000 | R |
| 2 | LOAD (данные+bss) | 0xda0000 | 0x720547f (~114 МБ) | RW |
| 3 | SCE_PROCPARAM | 0xda0000 | 0x48 | R |
| 6 | TLS | 0xda0060 | 0x30 | R |
| 7 | GNU_EH_FRAME | 0xc6bb2c | 0x59a8c | R |
| 8 | SCE_DYNLIBDATA | — | 0x239440 | — |

Точка входа `0x61dfd0`. Релокаций 96 139 (из них 460 PLT). Модули: `libSceSysmodule, libSceVideodec, libSceAjm, libSceHttp, libSceSsl, libSceSaveData, libSceSaveDataDialog, libSceMsgDialog, libSceCommonDialog, libSceErrorDialog, libSceNet, libSceNetCtl, libSceGnmDriver, libSceAudioOut, libSceVideoOut, libSceUserService, libScePad, libSceMouse*, libkernel, libSceGameLiveStreaming*, libSceSystemService, libSceNpCommon*, libSceNpManager, libSceNpTrophy, libSceAppContentUtil, libSceFios2, libScePlayGo, libc` (`*` — объявлен, символов 0).

## 5. Импорты и их статус в shadPS4 0fe263a

Полная таблица: `docs\generated\IMPORTS.md`. Классификация автоматическая, по телу функции (`tools\audit_imports.py`): `PARTIAL` — реальный код, но с меткой STUBBED/DUMMY; `TRIVIAL_RETURN` — только `return const`.

| Модуль | Всего | Реализ. | LLE | Partial | Trivial | Dummy | Stubbed | Видно в trace |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| libkernel (+libScePosix) | 73 | 71 | | | | | | 3 |
| libc | 101 | | 101 | | | | | — |
| libSceFios2 | 41 | | 41 | | | | | — |
| libSceGnmDriver | 85 | 55 | | | 14 | | 16 | 5 |
| libSceVideoOut | 13 | 13 | | | | | | 2 |
| libSceAudioOut | 6 | 4 | | 2 | | | | 2 |
| libSceAjm | 11 | 8 | | | | | 3 | 2 |
| libSceVideodec | 6 | 4 | | 2 | | | | 5 |
| libScePad | 7 | 6 | | | | | 1 | 5 |
| libSceSystemService | 6 | 5 | | | | | 1 | 4 |
| libSceUserService | 5 | 3 | | | | 1 | 1 | 4 |
| libSceSaveData | 8 | 8 | | | | | | 6 |
| libSceSaveDataDialog / MsgDialog / ErrorDialog / CommonDialog | 13 | 13 | | | | | | 1 |
| libScePlayGo | 10 | 10 | | | | | | 7 |
| libSceAppContentUtil | 2 | 1 | | 1 | | | | 2 |
| libSceSysmodule | 2 | 1 | | 1 | | | | 1 |
| libSceNpManager / NpTrophy | 21 | 19 | | 1 | | | 1 | 9 |
| libSceNet / NetCtl / Http / Ssl | 56 | 49 | | | 1 | 1 | 5 | 12 |
| **Итого** | **466** | **270** | **142** | **7** | **15** | **2** | **28** | **70** |

«Видно в trace» — функция встретилась в Debug/Trace-логе 100-секундного прогона (загрузка → заставки → меню/старт). Большинство HLE-функций ядра ничего не логируют, поэтому полный список реально вызываемых функций даст только счётчик вызовов (Phase 0, раздел 12).

### 5.1 Заглушенные (STUBBED/DUMMY/TRIVIAL) импорты eboot и оценка
| Функция | Вызов при старте | Есть выход/побочный эффект? | Оценка |
|---|---|---|---|
| `sceSystemServiceParamGetString` | да | **да** (пишет buf) | **нельзя заглушать** → реализовать (3.3) |
| `scePadInit` | да | нет | безопасно; в своём рантайме — реальная инициализация input |
| `sceUserServiceInitialize` (DUMMY) | да | нет | безопасно |
| `sceNpSetNpTitleId`, `sceNpSetContentRestriction` | да | нет | безопасно (офлайн) |
| `sceNetCtlInit`, `sceSslInit` (DUMMY) | да | нет | безопасно (офлайн) |
| `sceGnmDrawIndexMultiInstanced`, `sceGnmDrawIndirectCountMulti` | не замечено | **да** (PM4 в cmdbuf) | в shadPS4 это `UNREACHABLE()` → краш при вызове. Подтвердить счётчиком; если вызываются, нужна реализация |
| `sceGnmDrawOpaqueAuto` | не замечено | **да** (PM4) | молчаливый no-op = пропавшая геометрия. Подтвердить счётчиком |
| `sceGnmRequestFlipAndSubmitDone(ForWorkload)` | не замечено | да (flip) | проверить; при использовании — реализовать |
| `sceGnmGetNumTcaUnits`, `GetGpuInfoStatus`, `GetShaderProgramBaseAddress` | не замечено | возвращают значение | вернуть корректные константы PS4, не 0 |
| `sceGnmSetGsRingSizes`, `SetWaveLimitMultipliers`, `FlushGarlic`, `GetEqTimeStamp`, MipStats/Razor/Capture/Marker | не замечено | нет (debug/HW hint) | безопасно |
| `sceAjmBatchErrorDump`, `sceAjmFinalize`, `sceAjmModuleUnregister` | при выходе | нет | безопасно |
| `sceNetCtlTerm/UnregisterCallback`, `sceSslTerm`, `sceUserServiceTerminate`, `sceNetTerm`, `sceHttpSetChunkedTransferEnabled` | при выходе/редко | нет | безопасно |

### 5.2 Импорты LLE-модулей, которые в shadPS4 уходят в CommonStub (возвращает 0)
- `libSceFios2.prx` ← `libSceLibcInternal`: `malloc, free, time, localtime, gmtime(_s), mktime, printf, vprintf, vsnprintf, __cxa_finalize, _Stdout, Need_sceLibcInternal`. В прогоне вызывался только `vprintf`.
- `libSceFios2.prx` ← `libkernel`: **`sceKernelTruncate`**. Если Fios обрежет файл, CommonStub вернёт «успех» без действия, и файл останется прежнего размера. **Нельзя заглушать**: реализовать (Phase 1, FileSystem).
- `libc.prx` ← `libkernel`: `getrusage, sigreturn, _is_signal_return, __elf_phdr_match_addr, __pthread_cxa_finalize, sceKernelInternalMemoryGetModuleSegmentInfo, sceKernelPrintBacktraceWithModuleInfo` (+ `msync` STUBBED, `_exit`/`sceKernelDebugRaiseException*` UNIMPLEMENTED). Диагностика и выход, на загрузку не влияют; в своём рантайме — явные реализации или явный лог-заглушка.

## 6. Dependency map: подсистема → PS4 API → shadPS4 (донор) → модуль SecondSonPC

| Подсистема Second Son | PS4 API (импортов) | Реализация shadPS4 (временный донор) | Целевой модуль SecondSonPC |
|---|---|---|---|
| Загрузка модулей, релокации, TLS | (ELF/SELF, `__tls_get_addr`) | `src\core\loader\*`, `src\core\module.cpp`, `src\core\linker.cpp`, `src\core\tls.cpp` | **Loader** |
| Таблица импортов | NID → функция | `src\core\libraries\libs.cpp` (`LIB_FUNCTION`), `src\core\aerolib\*` (171k строк) | **Loader/Bindings** (статическая таблица на 417 NID) |
| Память | `sceKernelAllocateDirectMemory, MapDirectMemory, Mprotect, QueryMemoryProtection, GetPrtAperture, ReleaseDirectMemory, MapNamedFlexibleMemory, Munmap, Mlock, mmap` | `src\core\memory.cpp`, `src\core\address_space.cpp`, `kernel\memory.cpp` | **Memory** |
| Потоки и синхронизация | `scePthread*`(~45), `pthread_*`, EventFlag, Sema, Equeue, UserEvent | `kernel\threads\*`, `kernel\equeue.cpp` | **Threading** |
| Время | `sceKernelGetTscFrequency, Gettimeofday, ClockGettime, Usleep, clock_gettime, nanosleep, ConvertUtcToLocaltime` | `kernel\time.cpp` | **KernelCompat** |
| Доступ к TCB через FS | 782 инструкции `mov reg, fs:[0]` | `src\core\cpu_patches.cpp`, `src\core\signals.cpp` | **Threading/Memory** (патчер в памяти) |
| Файлы | Fios2 (LLE) → `sceKernelOpen/Read/Pread/Pwrite/Lseek/Stat/Fstat/Getdirentries/Mkdir/Rename/Unlink/Truncate/Ftruncate/Fsync` | `kernel\file_system.cpp`, `src\core\file_sys\*` | **FileSystem** (`/app0`, `/savedata0`, `/temp0`, `/download0`) |
| Системные параметры, события | `sceSystemService*` (6) | `system\systemservice.cpp` | **SystemService** |
| Пользователи | `sceUserService*` (5) | `system\userservice.cpp` | **SystemService** |
| Загрузка системных модулей | `sceSysmoduleLoadModule/Unload` | `sysmodule\*` | **SystemService** |
| PlayGo | `scePlayGo*` (10) | `playgo\*` | **PlayGo** |
| DLC | `sceAppContent*` (2) | `app_content\*` | **SystemService** |
| Сохранения | `sceSaveData*` (8), `sceSaveDataDialog*` (5) | `save_data\*` | **SaveData** |
| Диалоги | `sceMsgDialog*`, `sceErrorDialog*`, `sceCommonDialog*` | `system\msgdialog*`, `commondialog` (ImGui UI) | **SaveData/UI** (минимальный нативный UI) |
| Ввод | `scePad*` (7: Init, Open, Close, ReadState, GetControllerInformation, SetLightBar, SetVibration) | `pad\*`, `src\input\*` (SDL3) | **Input** |
| Аудио-вывод | `sceAudioOut*` (6; main 8ch float 48 кГц, 1024 фрейма; PadSpk mono) | `audio\*` (SDL3/OpenAL) | **Audio** (WASAPI) |
| Аудио-декод (Wwise) | `sceAjm*` (11; ATRAC9, `codec_type=1`) | `ajm\*` + LibAtrac9 | **Audio** (+LibAtrac9 как сторонняя библиотека) |
| Видеоролики | `sceVideodec*` (6; H.264, `.bsf`) | `videodec\*` + FFmpeg | **Audio/Media** (FFmpeg или Media Foundation) |
| Вывод кадра | `sceVideoOut*` (13; 1920×1080 A8R8G8B8Srgb, 2 буфера) | `videoout\*` | **Graphics** |
| GPU | `sceGnm*` (85) | `gnmdriver\*`, `src\video_core\*` (30.8k), `src\shader_recompiler\*` (37.8k) | **Graphics** (Vulkan) |
| PSN/трофеи | `sceNp*` (21) | `np\*` | **SystemService/Offline** |
| Сеть | `sceNet*`, `sceNetCtl*`, `sceHttp*`, `sceSsl*` (56) | `net\*`, `network\*` | **SystemService/Offline** |
| libc / Fios | `libc` (101), `libSceFios2` (41) | LLE: модули самой игры | **Loader** грузит prx игры (это не зависимость от shadPS4) |
| libc internal для Fios2 | 17 функций `libSceLibcInternal` | `libc_internal\*` | **KernelCompat** |

## 7. Классификация API

**Критичны для загрузки — только реальная реализация:** libkernel (память, потоки, синхронизация, время, TLS, файлы для Fios2), загрузка LLE libc/Fios2, `sceSysmoduleLoadModule`, `sceSystemServiceParamGetInt`, `GetStatus`, `ReceiveEvent`, `HideSplashScreen`, `GetDisplaySafeAreaInfo`, UserService (`GetInitialUser`, `GetLoginUserIdList`, `GetEvent`), PlayGo (`GetLocus` должен сообщать «всё локально», `GetToDoList`, `SetLanguageMask`), VideoOut, GnmDriver (submit/flip/draw/dispatch), SaveData (`Mount2`, `Umount`, `DirNameSearch`, `SetParam`, `SaveIcon`), Pad (`Open`, `ReadState`, `GetControllerInformation`), AudioOut, Ajm, Videodec.

**Нужны только для UI/PSN/трофеев/сети (детерминированное офлайн-поведение):** NpManager (`GetState` → signed out, `CheckCallback`, `GetOnlineId`/`GetAccountId` → ошибка «не вошёл»), NpTrophy (локальный журнал разблокировок или no-op с корректными handle), Net/NetCtl (`GetState` → disconnected), Http/Ssl (ошибка соединения), ErrorDialog, AppContent (`GetAddcontInfo` → 0 DLC, если DLC не установлены).

**Можно безопасно заглушить (нет выходных данных, только лог):** Gnm debug/razor/markers/mipstats/wave limits/GS ring/flush garlic, `sceAjmBatchErrorDump/Finalize/ModuleUnregister`, `*Term`/`Unregister*`, `sceSysmoduleUnloadModule`, `sceNpSetNpTitleId`, `sceNpSetContentRestriction`, `sceHttpSetChunkedTransferEnabled`.

**Нельзя заглушать (есть выход или побочный эффект):** `sceSystemServiceParamGetString`, `sceKernelTruncate`, все `sceGnmDraw*`/`Dispatch*`/`Set*`, которые пишут PM4, `sceGnmGet*`, которые возвращают значения, `scePadReadState`, `sceSaveDataDirNameSearch`, `sceVideodecDecode`, `sceAjmBatch*`, `malloc`/`time`/`localtime` для Fios2.

## 8. Рантайм-наблюдения (trace-прогоны)
- **Патчи в памяти:** shadPS4 патчит 782 инструкции `mov reg, fs:[0]` (доступ к TCB; на Windows FS указывает на чужой TEB). Только в памяти, eboot.bin не меняется. Понадобится и в SecondSonPC.
- **Память:** direct memory: `0x48000000` (RW) + `0x20000000` (GPU RW, prot 0x33) + `0xb7ff0000` (GPU RW) — игра забирает практически все 4.5 ГиБ direct memory. Flexible — 448 МиБ.
- **Видео:** `sceVideoOutOpen` → 1920×1080, `A8R8G8B8Srgb`, 2 буфера (зарегистрированы дважды, индексы 0–1 и 2–3 с теми же адресами).
- **Аудио:** main port 48 кГц, Float 8ch, 1024 фрейма; PadSpk mono для user 1000/1001; Wwise (`AK::EventManager`), Ajm ATRAC9.
- **Потоки:** `Game:Main`, `SceFiosIO`, `AK::EventManager`, `MovieDecoder`, `MoviePlayer`, `GameState`, `Trophy`.
- **ENOENT при `sceKernelStat`:** dev/test-уровни (`combat_test_world_01`, `x_parkour_test`, `testscene2`, `village.sprig` и т.п.). Игра перебирает каталог уровней, это штатно.
- **Запись в `/app0`:** игра пробует создать `/app0/delete_me.snt` и `/app0/art/delete_me.snt` (проверка «только чтение»). Ожидаемый ответ — ошибка, иначе игра может решить, что работает на dev-машине.
- **CommonStub:** вызывался только `vprintf` (из Fios2).

## 9. Ввод
- Игра читает геймпад через `scePadReadState` (не `scePadRead`) для двух хэндлов (user 1000/1001), обновляет `SetLightBar`/`SetVibration` каждый кадр.
- `libSceMouse` объявлен, но **ни одной функции из него не импортировано**: игра не использует PS4 USB-мышь.
- В движке есть строки `CCmMouse`, `SCmMouseData`, `MOUSE_CAMERA`, `MOUSE_CAMERA_DATA` (вероятно, отладочная камера с мышью) и UI-события `MouseMove/MouseLeftDown/.../MouseWheel/MouseCaptured`. Это кандидаты на нативную мышь (этап 4): найти, где формируются yaw/pitch камеры, и подавать туда дельту мыши напрямую. Пока не проверено.

## 10. Целевая архитектура и политика зависимости от shadPS4

```
SecondSonPC.exe
  └─ SecondSon Loader        (SELF/ELF, релокации, TLS, статическая таблица 417 привязок)
      └─ SecondSon Runtime
          ├─ KernelCompat  ├─ SystemService ├─ FileSystem ├─ SaveData ├─ Input
          ├─ Audio         ├─ PlayGo        ├─ Threading  ├─ Memory
          └─ Graphics → Vulkan → Windows
```

**Правила:**
1. **shadPS4 — не архитектура, а временный инструмент:** (а) reference/oracle для сравнения поведения и логов; (б) донор отдельных реализаций; (в) bootstrap только в Phase 0.
2. **Таблица привязок вместо generic HLE registry.** Каждая из 417 функций имеет владельца: `NATIVE` (код SecondSonPC) или `BRIDGE` (временный вызов функции shadPS4). LLE-импорты (libc/Fios2) закрывают модули игры. Счётчик `BRIDGE` должен дойти до 0 к Phase 5; выводится при каждом запуске и в `docs\MIGRATION.md`.
3. **Изоляция.** Заголовки shadPS4 подключаются только в `src\bridge\shadps4\` (CMake-опция `SSPC_SHADPS4_BRIDGE`). Модули `src\loader`, `src\runtime\*`, `src\graphics` не включают ничего из `third_party\shadPS4`.
4. **Перенесённый код становится собственным:** копируется в `src\<подсистема>`, namespace `SecondSon::`, урезается до того, что использует Second Son. В заголовке файла указываются источник (путь + коммит `0fe263a`) и лицензия. Обратных `#include` в `third_party` нет.
5. **Сторонние библиотеки (не shadPS4) допустимы как постоянные зависимости:** Vulkan headers/loader, VMA, xbyak, Zydis, LibAtrac9, FFmpeg (или Media Foundation), SDL3 (или чистый Win32), toml11, fmt, spdlog, glslang (если нужен), sirit (SPIR-V emitter). sirit — форк под организацией shadps4-emu, поэтому его нужно вендорить как отдельную библиотеку со своей лицензией.
6. **Тулчейн: clang-cl.** Гостевой код вызывает хост-функции по SysV ABI (`__attribute__((sysv_abi))`), а MSVC это не поддерживает.
7. **Лицензия.** shadPS4 распространяется под GPL-2.0-or-later. Любой перенесённый код наследует эту лицензию, поэтому SecondSonPC с перенесённым кодом — GPL-2.0-or-later. Код, написанный с нуля по поведению (oracle), так не связан.
8. **Неизменность игры:** eboot/psarc/xpps только читаются. Runtime-патчи — только в памяти и только из `patches\`, каждый описан в `docs\PATCHES.md`.

## 11. Инвентарь зависимостей от shadPS4: что временно и как удаляется

Сейчас у SecondSonPC нет рантайм-кода. Единственный работающий путь запуска игры — стоковый shadPS4. Ниже перечислено всё, что этот путь использует, с фазой удаления.

| # | Компонент shadPS4 (строк) | Зачем Second Son | Временно? | Удаляется в | Замена в SecondSonPC | Проверка удаления |
|---|---|---|---|---|---|---|
| 1 | `main.cpp`, `emulator.cpp`, `sdl_window.cpp` (frontend/bootstrap) | запуск, окно | да | Phase 0→1 | `src\app\main.cpp`, `src\app\window_win32.cpp` | наш `main`, в Phase 1 без `Core::Emulator` |
| 2 | `core\emulator_settings*`, `user_settings*`, `config.json` | настройки | да | Phase 1 | `SecondSonPC.toml` + `src\app\config.*` | в `src\` нет `EmulatorSettings` |
| 3 | `common\logging` | лог | да | Phase 1 | `src\common\log.*` (spdlog), категории Loader/Kernel/SystemService/Filesystem/Input/Audio/GPU/Shader/SaveData/Language | `SecondSonPC.log` пишет наш логгер |
| 4 | `core\loader`, `module.cpp`, `linker.cpp`, `tls.cpp` (~5k) | загрузка eboot+prx, релокации, TLS, init | да | Phase 1 | `src\loader\*` (C++ порт `tools\ps4elf.py`) | eboot и 2 prx грузит наш Loader |
| 5 | `libraries\libs.cpp`, `SymbolsResolver`, `aerolib` (171k) | разрешение NID | да | Phase 1 | `src\loader\bindings.*`: статическая таблица 417 NID (генерируется из `docs\generated\*.csv`) | aerolib не линкуется |
| 6 | `core\file_sys`, `kernel\file_system.cpp` | `/app0`, `/savedata0`, файловые syscalls Fios2 | да | Phase 1 | `src\runtime\filesystem\*` | 0 BRIDGE в файловых функциях; тот же порядок `open()` в логе, что у oracle |
| 7 | `system\systemservice`, `userservice`, `sysmodule`, `app_content`, `disc_map` | параметры, события, пользователи | да | Phase 1 | `src\runtime\system\*` | язык из `SecondSonPC.toml`; `ParamGetString` пишет имя |
| 8 | `playgo` | чанки/язык | да | Phase 1 | `src\runtime\playgo\*` | та же маска языка, что у oracle |
| 9 | `core\memory.cpp`, `address_space.cpp`, `kernel\memory.cpp` | direct/flexible память, маппинг | да | Phase 2 | `src\runtime\memory\*` (VirtualAlloc2/MapViewOfFile3, плейсхолдеры) | 0 BRIDGE в memory; GPU-bridge работает через адаптер (риск, см. ниже) |
| 10 | `cpu_patches.cpp`, `signals.cpp` | FS-патчи TCB, обработка исключений | да | Phase 2 | `src\runtime\threading\tcb_patcher.*` (Zydis+xbyak), VEH | 782 патча ставит наш код |
| 11 | `kernel\threads\*`, `equeue`, `time`, `libc_internal` | потоки, синхронизация, время, libc для Fios2 | да | Phase 2 | `src\runtime\kernel\*`, `src\runtime\threading\*` | 147 функций libkernel — NATIVE |
| 12 | `pad`, `src\input` (SDL3) | геймпад | да | Phase 3 | `src\runtime\input\*` (XInput, RawInput, DualSense/DS4) | `scePadReadState` — NATIVE |
| 13 | `audio` (SDL3/OpenAL) | вывод звука | да | Phase 3 | `src\runtime\audio\wasapi_*` | NATIVE |
| 14 | `ajm` (+LibAtrac9) | ATRAC9 для Wwise | да (код), LibAtrac9 остаётся | Phase 3 | `src\runtime\audio\ajm_*` | NATIVE |
| 15 | `videodec` (+FFmpeg) | ролики | да (код) | Phase 3 | `src\runtime\media\*` | ролики идут |
| 16 | `save_data`, `system\msgdialog*`, `commondialog`, ImGui-диалоги | сохранения, диалоги | да | Phase 3 | `src\runtime\savedata\*`, `src\runtime\ui\*` | сохранение/загрузка; бэкап сейвов перед тестами |
| 17 | `np`, `net`, `network` (54k) | PSN, трофеи, сеть | да | Phase 3 | `src\runtime\offline\*` (~100 строк поведения «офлайн») | NATIVE |
| 18 | `gnmdriver`, `videoout` | PM4, flip | да | Phase 4 | `src\graphics\gnm\*`, `src\graphics\present\*` | NATIVE |
| 19 | `src\video_core` (30.8k) | PM4-процессор, Vulkan-рендерер, кэши | да | Phase 4 | `src\graphics\*` за `ICommandProcessor`, `IGpuBackend`, `ITextureManager` | компилируется без `third_party\shadPS4` |
| 20 | `src\shader_recompiler` (37.8k) | GCN → SPIR-V | да | Phase 4 | `src\graphics\shader\*` за `IShaderTranslator` | то же |
| 21 | `src\common\*` (утилиты: io_file, path_util, assert, thread, string_util…) | везде | да | Phase 5 | `src\common\*` (свои, минимальные) | grep по `src\` пуст |
| 22 | ImGui-оверлеи, devtools, big picture, shadNet, discord, updater, cheats, трофей-попапы | не нужны | — | не переносятся (в Phase 0 по возможности исключаются из сборки) | — | — |
| 23 | Сборка shadPS4 (`CMakeLists.txt`, сабмодули) | bootstrap-сборка | да | Phase 6 | свой `CMakeLists.txt` и свои закреплённые third-party | чистый checkout без `third_party\shadPS4` собирается |

**Главный технический риск миграции.** `video_core` завязан на `Core::Memory` (5 include), `emulator_settings` (19), `signals`, `debug_state` и videoout. В фазах 2–3 память уже своя, а GPU ещё из shadPS4. Решение: узкий адаптер `MemoryForGpu` (интерфейс ровно тех вызовов, которые делает video_core), реализованный на нашей памяти. Альтернатива — перенести GPU раньше памяти. Выбор сделаем в конце Phase 1 по фактическому списку вызовов.

## 12. План миграции по фазам

**Phase 0 — Bootstrap (временно, через shadPS4 core).**
- Сборка: CMake SecondSonPC делает `add_subdirectory(third_party/shadPS4 EXCLUDE_FROM_ALL)` и собирает из исходников цели `shadps4` временную статическую библиотеку `sspc_shadps4_bootstrap` (без `main.cpp`). Исходники shadPS4 не меняются; если изменения всё же понадобятся, они хранятся как `patches\shadps4\*.patch` и описываются в `docs\PATCHES.md`.
- Наш `main`: путь к игре и язык из `SecondSonPC.toml`, `/app0` → `E:\shdps4\CUSA00004`, `/savedata0` → `E:\SecondSonPC\userdata\savedata` (отдельно от сейвов shadPS4), лог `E:\SecondSonPC\logs\SecondSonPC.log`.
- Первые NATIVE-функции: `ParamGetInt(Lang)` и `ParamGetString`. Регистрируются до `InitHLELibs`, поэтому получают приоритет: `FindSymbol` возвращает первое совпадение.
- Счётчик вызовов импортов: xbyak-thunk на каждую привязку (`lock inc [counter]; jmp target`, ABI не меняется) → `docs\generated\CALLS.md` с точным списком реально вызываемых функций.
- **Выход:** SecondSonPC.exe доходит до меню на русском, открывает `russian_*` без переименования; есть полный список вызовов.

**Phase 1 — свой Loader + FileSystem + SystemService.**
- Свой загрузчик SELF/ELF (релокации, TLS, порядок init, LLE libc/Fios2), статическая таблица 417 привязок (owner = NATIVE/BRIDGE/LLE), VFS, SystemService/UserService/Sysmodule/AppContent/PlayGo/DiscMap.
- Память, потоки, GPU, ввод и звук остаются через BRIDGE.
- `emulator.cpp`/`Linker`/`aerolib`/`EmulatorSettings` больше не используются.
- **Выход:** игра доходит до меню. Порядок `open()` и маска PlayGo совпадают с oracle-логом; BRIDGE ≤ 417 − (FS + System + PlayGo).

**Phase 2 — Kernel / Thread / Memory.** Своё адресное пространство, direct/flexible-память, TCB/FS-патчер, VEH, pthread/sync/equeue/eventflag/sema/time, libc_internal для Fios2. GPU-bridge работает через адаптер памяти. **Выход:** все функции libkernel — NATIVE, 30 минут игры без падений.

**Phase 3 — Input / Audio / Save (+ offline-сервисы).** XInput/RawInput/DualSense, нативная мышь (исследование `CCmMouse`/камеры), WASAPI, Ajm ATRAC9, Videodec, SaveData и диалоги, NP/Net/Trophy офлайн. **Выход:** BRIDGE остаётся только в Gnm/VideoOut/video_core/shader.

**Phase 4 — GPU backend в собственный модуль.** `gnmdriver`, `videoout`, `video_core` и `shader_recompiler` переносятся в `src\graphics` за интерфейсами `IGpuBackend`, `ICommandProcessor`, `IShaderTranslator`, `ITextureManager`. Код урезается по реальному покрытию (гистограмма PM4-опкодов, покрытие инструкций GCN, форматы текстур). Сравнение кадров с oracle в фиксированных точках. **Выход:** `src\graphics` собирается без `third_party\shadPS4`.

**Phase 5 — удалить последнюю runtime-зависимость.** Удаляется `src\bridge\shadps4\` и опция `SSPC_SHADPS4_BRIDGE`, `common\*` заменяется своим. **Выход:** BRIDGE = 0, все 417 привязок NATIVE.

**Phase 6 — сборка без shadPS4.** `third_party\shadPS4` убирается из дерева сборки (эталон может лежать отдельно, например в `E:\SecondSonPC-reference\shadPS4`, только для oracle-сравнений). Свой CMake со своими закреплёнными сторонними библиотеками. Скрипт `tools\check_native.ps1`.

## 13. Критерий «native milestone»

Milestone считается достигнутым, только если выполнены **все** пункты:
1. Чистый клон SecondSonPC **без каталога `third_party\shadPS4`** проходит `cmake configure` и `build` (`tools\check_native.ps1`).
2. В `src\` и `CMakeLists.txt` нет упоминаний shadPS4: `grep -ri "shadps4\|third_party/shadPS4\|Core::Loader\|Libraries::Kernel\|EmulatorSettings"` даёт пустой результат. Опции `SSPC_SHADPS4_BRIDGE` нет.
3. Таблица привязок: 417/417 NATIVE (+LLE от модулей игры), BRIDGE = 0, CommonStub-заглушек без явного решения нет.
4. `SecondSonPC.exe` с оригинальными файлами игры (eboot/psarc/xpps не изменены, хеши совпадают с этим отчётом): (а) доходит до главного меню с языком из `SecondSonPC.toml` (ru-RU → `russian_*`, en-US → `english_*`); (б) начинает новую игру или загружает сохранение; (в) не меньше 30 минут игры в первой миссии без падений и с синхронным звуком; (г) сохранение пишется и читается.
5. `SecondSonPC.log` не содержит строк shadPS4 и записей о вызове BRIDGE/CommonStub.

## 14. Объём кода shadPS4 для временного bootstrap (Phase 0)

| Часть | Строк | Нужна Second Son |
|---|---:|---|
| `core\libraries` (все 50+ библиотек) | 157 511 | из них по импортам: kernel 13.2k, gnmdriver 4.2k, videoout 1.3k, audio 4.4k, ajm 3.0k, videodec 2.9k, pad 1.2k, system 8.6k, save_data 4.4k, playgo 0.6k, app_content 0.7k, sysmodule 1.4k, libc_internal 1.8k, np 39k (нужно ~21 функция), net+network 15.2k (нужно ~56 функций) |
| `video_core` | 30 796 | да (Phase 4) |
| `shader_recompiler` | 37 779 | да (Phase 4) |
| `core\loader` + module/linker/tls | ~5 000 | Phase 1 |
| `core\file_sys` | 3 865 | Phase 1 |
| `core\memory.cpp` (+address_space) | 1 662+ | Phase 2 |
| `common` | 10 045 | частично, Phase 5 |
| `input` | 2 446 | Phase 3 |
| `aerolib` | 171 658 | нет (заменяется таблицей на 417 NID) |
| `imgui` | 7 875 | нет (кроме диалогов в Phase 0) |

Минимальный «собственный» объём после переноса: около 15–25k строк вне GPU и порядка 50–60k с GPU (после урезания по покрытию). Это оценка, её уточнит счётчик вызовов в Phase 0.

## 15. Блокеры и предпосылки для Phase 0
- **Тулчейн неполный:** в `E:\BuildTools` есть MSVC 14.44 и CMake 3.31.6, но **нет clang-cl** (компонент `Microsoft.VisualStudio.Component.VC.Llvm.Clang` + `ClangToolset`) и **нет Windows SDK** (`C:\Program Files (x86)\Windows Kits\10\Include` отсутствует).
- **Сабмодули shadPS4** (`git submodule update --init --recursive`): несколько ГБ, только на E:.
- Всё скачивается и ставится **только на диск E:** (по указанию пользователя). Windows SDK по умолчанию ставится на C:, поэтому нужен standalone-установщик с `/installpath` на E:. Кэш установщика VS тоже нужно направить на E:.

## 16. Как воспроизвести аудит
```
python -I E:\SecondSonPC\tools\audit_imports.py --log E:\SecondSonPC\logs\reference\trace_lang1.txt --out E:\SecondSonPC\docs\generated
python -I E:\SecondSonPC\tools\find_calls.py sceSystemServiceParamGetString sceSystemServiceParamGetInt
powershell -File E:\SecondSonPC\tools\trace_run.ps1 -Lang 8 -Seconds 100 -Tag lang8
```
- `tools\ps4elf.py` — парсер SELF/ELF (только чтение).
- `tools\audit_imports.py` — сверка импортов с исходниками shadPS4.
- `tools\find_calls.py` — места вызова импортов + дизассемблер (capstone в `tools\pydeps`).
- `tools\trace_run.ps1` — изолированный прогон стокового shadPS4 (портативная папка `runtime\shadps4-ref\user`, `%APPDATA%\shadPS4` не трогается).
