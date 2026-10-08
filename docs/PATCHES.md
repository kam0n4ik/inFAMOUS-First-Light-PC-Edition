# PATCHES

Правило: оригинальные eboot.bin / *.prx / *.psarc / *.xpps не изменяются. Runtime-патчи — только в памяти.
Перед любым патчем записывается: offset (относительно базы модуля), исходные байты, новые байты, причина.

## Бинарные патчи SecondSonPC
Нет.

## Патчи в памяти, которые делает эталонный shadPS4 0fe263a (для переноса в Phase 2)
| Что | Количество | Причина |
|---|---:|---|
| `mov reg, fs:[0]` → трамплин на TCB | 782 (за 100 с прогона) | на Windows сегмент FS указывает на TEB хоста, а не на TCB гостя |

## Временные патчи исходников shadPS4 (только `third_party\shadPS4`, Phase 0–4)
Применяются автоматически при configure (`cmake\shadps4_bootstrap.cmake`, `git apply`, идемпотентно).
Установленная у пользователя shadPS4 не затрагивается.

| Патч | Файл | Причина | Удаляется |
|---|---|---|---|
| `patches\shadps4\0001-imgui-fontembed-subproject-path.patch` | `src\imgui\renderer\CMakeLists.txt` | `${CMAKE_SOURCE_DIR}` указывает на корень SecondSonPC, когда shadPS4 собирается как подпроект; заменено на `${CMAKE_CURRENT_SOURCE_DIR}/../../..` | вместе с bridge (Phase 5) |
| `patches\shadps4\0002-elfinfo-friend-secondson-bootstrap.patch` | `src\common\elf_info.h` | поля `Common::ElfInfo` приватные, заполнять их может только `Core::Emulator`; добавлен `friend class SecondSonBootstrap` для `src\bridge\shadps4\bootstrap.cpp` | вместе с bridge (Phase 5) |
| `patches\shadps4\0003-userservice-event-sink.patch` | `src\core\libraries\system\userservice.{h,cpp}` | Phase 1 M3: UserService — NATIVE, а логины/выходы по-прежнему порождает слой ввода shadPS4 (BRIDGE); `SetUserServiceEventSink` направляет их в SecondSonPC вместо очереди shadPS4. Пока приёмник не установлен, поведение исходное (путь `--loader shadps4`, `--bridge user`) | когда ввод станет NATIVE (Phase 5) |

## Отклонения папки игры от оригинала (обнаружено при аудите, сделано не SecondSonPC)
`E:\shdps4\CUSA00004\art\cache\psarc\english_{a,a_p,b,b_p}.psarc` были копиями `russian_*`. Оригиналы лежат в `E:\shdps4\CUSA00004\_LANG_BACKUP_2026-10-07_22-58-16\psarc`.
**2026-10-08 восстановлено** (копирование из бэкапа, бэкап не тронут). SHA-256 после восстановления совпадает с AUDIT.md, раздел 2:
`english_a` b3c8f38f…, `english_a_p` c19a5c66…, `english_b` 373055af…, `english_b_p` 98a2f5d8…. Файлы `russian_*` не менялись.
