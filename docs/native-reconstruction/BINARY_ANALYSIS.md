# Анализ бинарников

Дата: 2026-10-14. Обозначения: [A] анализ, [E] эксперимент, [H] гипотеза, [?] неизвестно.

## 1. Инструменты

| Инструмент | Версия | Где | Назначение | Проверка |
|---|---|---|---|---|
| Ghidra | 12.1.4 PUBLIC | `E:\tools\ghidra` | статический анализ, декомпиляция | проект `E:\tools\ghidra-projects\SecondSon.gpr` открыт, 41 167 функций [E] |
| pyghidra | 3.1.0 (JPype 1.5.2) | venv `E:\tools\ghidra-mcp\.venv` | скриптовый доступ к проекту (headless) | все скрипты `tools/native-reconstruction/ghidra/*.py` отработали [E] |
| pyghidra-mcp (MCP-сервер Ghidra для Claude Code) | 0.2.7 | тот же venv, сервер `ghidra` в `~/.claude.json` | интерактивные запросы из Claude Code | в начале сессии не подключился (CONNECTION_CLOSED); ручной запуск отвечает на `initialize` и открывает проект [E] → переподключение `/mcp`. Работа шла через pyghidra напрямую |
| JDK | OpenJDK 21.0.12.1 | `E:\tools\jdk21` | Ghidra | [E] |
| Universal Modder | плагин Claude Code | — | вспомогательный | MCP `fal` не подключился (timeout); не использовался |
| LLVM clang-cl / lld | 20.1.8 | `E:\SecondSonPC\toolchain\LLVM` | сборка | [E] |
| Windows SDK / MSVC libs | 10.0.26100.0 / 14.44 | `toolchain\winsdk`, `E:\BuildTools` | сборка | [E] |
| CMake / Ninja | 3.31.6 / 1.12.1 | `E:\BuildTools` | сборка | [E] |
| Python | 3.14.4 | системный `py` | `tools/ps4elf.py`, скрипты | [E] |
| Nsight Graphics | 2026.2 | `E:\tools\NsightGraphics-2026.2` | GPU-трассы (P4.6) | ранее [E] |
| Vulkan SDK (валидация, spirv-dis) | — | `E:\VulkanSDK` | GPU | ранее [E] |
| WPR / WPA | встроены в Windows | `C:\Windows\system32` | CPU/ETW-профили | наличие [E]; в этой фазе не запускались |
| RenderDoc | не установлен | — | — | — |
| Tracy | не установлен | — | — | — |
| IDA Free/Pro | не установлена | — | — | — |
| radare2 | не найден в PATH | — | — | — |

Ничего нового не скачивалось и не устанавливалось.

## 2. Доступные бинарники

`E:\shdps4\CUSA00004` — установленная копия игры (v1.07): `eboot.bin` (17 МБ), `sce_module\libc.prx`,
`sce_module\libSceFios2.prx`. Формат — fake-signed SELF (магия `4F 15 3D 1D`, сегменты лежат открыто), внутри ELF x86-64
[A] (`tools/ps4elf.py`, `src/loader/image_file.cpp`). Расшифровка или обход защиты не требовались и не выполнялись.

eboot в Ghidra (база 0): `text` 0x0–0xcc55b7 (12,8 МБ кода), `relro` 0xcc8000–0xd9ef07, `data` 0xda0000–0xe0b557,
`data_bss` до 0x7fa547e (~113 МБ) [A]. Компилятор — clang (x86:LE:64 gcc-совместимый), стековые канарейки
(`__stack_chk_fail`), TLS через `fs:` [A]. Отладочных символов нет; 1 293 функции имели имена от прошлых сессий
(`sspc_*` — наши, не оригинальные) [A].

Строки сборки: Havok (`Y:\NightlyJobs\09-12-Thu-02\Source\...`, в т.ч. `hkTaskScheduler.cpp`, `hkPs4Socket.cpp`),
Wwise (`AK::BankManager`), Sony WWS (`WwsPlatformThread`) [A].

## 3. Что выполнено в Ghidra (реальные операции)

1. **Обзор** (`overview.py`): блоки, 41 167 функций, 1 275 внутренних thunk, **0 внешних символов** — проект был
   импортирован без загрузчика PS4, вызовы API шли в безымянные PLT-заглушки [A].
2. **Разметка импортов** (`export_import_slots.py` → `label_imports.py`): из DT_SCE_JMPREL/RELA eboot 908 слотов GOT
   (460 PLT), имена по NID из `aerolib.inl` — 908/908. В проект записаны метки `got_<имя>`, 460 PLT-заглушек
   переименованы в `ps4import::<имя>` [E]. Остальные 432 слота — указатели в vtable (`__cxa_pure_virtual` и т.п.), без
   кода-заглушки. **Проект изменён**; копия до изменений: `E:\tools\ghidra-projects\_backup_20261014_SecondSon.{rep,gpr}`.
   Распределение импортов: libc 542, GnmDriver 85, libkernel 58, Fios2 42, Net 30, Http 18, Posix 15, NpManager 14,
   VideoOut 13, Ajm 11, PlayGo 10, SaveData 8, Pad 7, NpTrophy 7, прочие ≤6.
3. **Поиск по строкам** (`xref_strings.py`, `overview.py`): 473 строки про потоки/задачи; таблицы `JOBAK_*`,
   `JobWorker1..5`.
4. **Граф вызовов создания потоков** (`callers.py`, 2 уровня от `scePthreadCreate`, `pthread_create_name_np`,
   `scePthreadAttrSetaffinity`, `scePthreadSetaffinity`).
5. **Декомпиляция и дизассемблирование** ~40 функций системы задач и потоков (`run.py` + разовые запросы):
   JOB_SYSTEM_RECONSTRUCTION.md §5.
6. **Эталонные хеши** тел функций (`function_hashes.py`) для безопасной замены.

## 4. Инструменты, добавленные в репозиторий

`tools/native-reconstruction/`:

| Файл | Что делает |
|---|---|
| `export_import_slots.py` | eboot.bin → JSON слотов импорта (слот, NID, библиотека, имя) |
| `function_hashes.py` | FNV-1a 64 тел функций из eboot.bin → строки для `GuestFunction` |
| `ghidra/gh.py` | общий доступ к проекту Ghidra (по умолчанию только чтение), декомпилятор |
| `ghidra/overview.py` | обзор программы, внешние символы, строки про потоки |
| `ghidra/label_imports.py` | **пишет в проект**: метки GOT и имена PLT-заглушек |
| `ghidra/xref_strings.py` | ссылки на строки (и через таблицы указателей) + декомпиляция |
| `ghidra/callers.py` | вызывающие функции N уровней + декомпиляция |
| `ghidra/run.py` | выполнение произвольного запроса к программе (prog, fm, refm, dec, A, F) |

Запуск: `set GHIDRA_INSTALL_DIR=E:\tools\ghidra & set JAVA_HOME=E:\tools\jdk21`, затем
`E:\tools\ghidra-mcp\.venv\Scripts\python.exe -X utf8 <скрипт>`. Проект может держать только один процесс — на время
запуска закрыть GUI Ghidra и MCP-сервер.

## 5. Ограничения

- Псевдокод Ghidra в документах — исходный материал, не оригинальный C++. В проект он не копировался; нативный код
  написан заново по дизассемблеру и проверен тестами и хешами.
- `libc.prx`, `libSceFios2.prx` в Ghidra не импортированы (исполняются как LLE-гостевой код; на этой фазе не нужны).
- Динамических трасс гостевого кода (WPR/ETW с символизацией по Ghidra) в этой фазе не снималось.
