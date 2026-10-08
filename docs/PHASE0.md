# Phase 0 — результат (2026-10-08)

**Статус: первый рабочий запуск получен.** `E:\SecondSonPC\build\bin\SecondSonPC.exe` запускает
`E:\shdps4\CUSA00004\eboot.bin` без shadPS4.exe, лаунчера и `%APPDATA%\shadPS4`; язык берётся из
`SecondSonPC.toml` собственным `sceSystemServiceParamGetInt(Lang)`; игра сама открывает русские файлы.

Прогон `ru_phase0`: 150 с, без падений, закрытие окна штатное (exit 0), ~60 кадров/с
(`scePadReadState` 17 202 вызова на 2 контроллера), ~500 графических пайплайнов (как у эталона).
Скриншот: `docs\phase0\ru_phase0_pause_ru.png` — игровое меню «ПАУЗА / ✕ Продолжить» на фоне
вступительной сцены. Без сохранения игра сама уходит во вступление, поэтому это меню паузы, а не
главное меню; пауза — вероятно, из-за потери фокуса окном.

## Ключевые строки `SecondSonPC.log`
```
[Info] [Language] system language ru-RU = PS4 id 8 (Russian), game tag "russian"
[Info] [Bridge] shadPS4 core 0fe263a4... (temporary bootstrap), user dir E:\SecondSonPC\runtime\sspc\user
[Info] [SaveData] /savedata0 root: E:\SecondSonPC\userdata\home\<user>\savedata\CUSA00004\<dir>
[Info] [Bridge] bind NATIVE   sceSystemServiceParamGetInt (libSceSystemService/libSceSystemService) -> SecondSonPC
[Info] [Bridge] bind NATIVE   sceSystemServiceParamGetString (libSceSystemService/libSceSystemService) -> SecondSonPC
[Info] [Bridge] bind OBSERVED sceKernelOpen (libkernel/libkernel) -> SecondSonPC
[Info] [Bridge] bind OBSERVED sceKernelStat (libkernel/libkernel) -> SecondSonPC
[Info] [Bridge] binding table: 4 SecondSonPC exports, 4618 shadPS4 HLE functions (BRIDGE) + 12 objects, 4622 counted thunks
[Info] [Loader] loaded /app0/eboot.bin, starting guest
[Info] [Language] ParamGetInt(Lang) -> 8 (ru-RU, game files lang_russian_*) ret=0
[Info] [SystemService] ParamGetString(SystemName, buf=0x7efdf39f0, size=65) -> "" (0 bytes) ret=0
[Info] [Language] game opened language file: /app0/art/cache/lang_russian_text.xpps (tag=russian, result=4)
[Info] [Language] game opened language file: /app0/art/cache/lang_russian_text_p.xpps (tag=russian, result=4)
[Info] [Language] game opened language file: /app0/art/cache/lang_russian_audio.xpps (tag=russian, result=4)
[Info] [Language] game opened language file: /app0/art/cache/lang_russian_audio_p.xpps (tag=russian, result=4)
[Info] [Language] game opened language file: /app0/art/cache/psarc/russian_a.psarc (tag=russian, result=7)
[Info] [Language] game opened language file: /app0/art/cache/psarc/russian_b.psarc (tag=russian, result=8)
[Info] [Language] game opened language file: /app0/art/cache/psarc/russian_a_p.psarc (tag=russian, result=10)
[Info] [Language] game opened language file: /app0/art/cache/psarc/russian_b_p.psarc (tag=russian, result=10)
[Info] [App] window closed
[Info] [Trace] import counter: final dump -> E:\SecondSonPC\logs\calls.json
```

## Владельцы 417 функций (подробно — `docs\generated\CALLS.md`)

| Владелец | Всего | Вызваны за 150 с |
|---|---:|---:|
| NATIVE | 2 | 2 |
| OBSERVED (наблюдатель SecondSonPC перед BRIDGE) | 2 | 2 |
| BRIDGE (shadPS4) | 392 | 167 |
| COMMON_STUB | 21 | вызван только `vprintf` (1 раз) |
| LLE — модули игры `libc.prx`/`libSceFios2.prx` (вне 417) | 147 | — |

Не вызывались опасные заглушки из AUDIT.md 5.1: `sceGnmDrawIndexMultiInstanced`,
`sceGnmDrawIndirectCountMulti`, `sceGnmDrawOpaqueAuto`, `sceGnmRequestFlipAndSubmitDone*`.

## Какие зависимости shadPS4 реально понадобились для bootstrap

**Сборка (временно).** Вся цель `shadps4` (все исходники `src\` кроме `main.cpp`) + 54 сабмодуля
`externals\`; 2 патча: `0001` (путь в CMake при сборке подпроектом), `0002` (`friend` для заполнения
`Common::ElfInfo`). Параметры линковки ядра обязательны: база образа `0x700000000000`, без ASLR,
стек 2 МБ — без них ядро не резервирует гостевое адресное пространство.

**API ядра, которые вызывает наш bootstrap** (`src\bridge\shadps4\bootstrap.cpp`):
`Common::Log` (Setup/Switch/Flush), `Common::FS` (Set/GetUserPath), `KeyManager`,
`EmulatorSettings`/`UserSettings`, `Core::FileSys::MntPoints`, `PSF`, `Common::ElfInfo`,
`Core::ReadCompiledSdkVersion`, `Core::ExtractTrophies`, `FileSys::HandleTable`, `Core::Memory`
(MemoryManager/AddressSpace), `Input::GameControllers`, `Core::Linker` (LoadModule/Execute/
GetHLESymbols), `Loader::SymbolsResolver`, `Libraries::InitHLELibs`, `LibcInternal::RegisterLib`,
`AeroLib::FindByNid` (только имена для счётчика), `VideoCore::LoadRenderDoc/SetOutputDir`,
`Frontend::WindowSDL`, `Common::NtApi`, `sceKernelGetFsSandboxRandomWord`, `g_environment`,
`g_window`, `SaveData::Backup`, `Storage::DataBase`.

**Реально исполнявшийся код shadPS4 за прогон** (BRIDGE-функции, вызванные игрой, по модулям):

| Модуль | Функций | Вызовов |
|---|---:|---:|
| libkernel (+libScePosix) | 71 | 28 487 202 |
| libSceGnmDriver | 18 | 15 421 318 |
| libSceVideoOut | 10 | 8 610 |
| libScePad | 6 | 51 609 |
| libSceAudioOut | 4 | 6 974 |
| libSceLibcInternal (для Fios2) | 3 | 4 076 |
| libSceDiscMap (для Fios2) | 2 | 2 542 |
| libSceNpManager / NpTrophy | 6 / 4 | 8 605 / 4 |
| libSceSystemService / UserService | 5 / 4 | 8 605 / 8 605 |
| libScePlayGo | 7 | 78 |
| libSceVideodec | 6 | 315 |
| libSceSaveData | 5 | 13 |
| libSceHttp / Net / NetCtl / Ssl | 6 / 3 / 4 / 1 | 6 / 3 / 5 / 1 |
| libSceAppContentUtil / Sysmodule / Ajm / CommonDialog | 2 / 1 / 2 / 1 | 17 / 9 / 2 / 1 |

Плюс внутренние подсистемы ядра без собственных импортов: `video_core` (Vulkan-рендерер, ~500
пайплайнов), `shader_recompiler`, `cpu_patches` (FS/TCB), `signals`, SDL-бэкенды окна, ввода и
звука, FFmpeg для роликов. `libSceAjm`: за 150 с только `Initialize`/`ModuleRegister` — пакетного
декодирования ATRAC9 в этом отрезке не было.

## Найдено по ходу
- **SSPC-001** (`docs\KNOWN_ISSUES.md`): непустое `SystemName` → падение после записи `dis.dat`;
  найдено бисекцией по 8 диагностическим прогонам, временно `[System] name = ""`, причина не устранена.
- Диагностические флаги exe: `--lang`, `--no-counter`, `--stock-bootstrap`, `--exports`, `--no-bind`.

## Не сделано в Phase 0
- Проверка главного меню именно на русском со скриншотом (игра без сейва сразу уходит во вступление).
- Устранение причины SSPC-001.
