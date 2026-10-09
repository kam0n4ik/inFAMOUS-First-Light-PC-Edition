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
| `patches\shadps4\0004-diag-sspc001-gpu-alloc.patch` | `src\video_core\renderer_vulkan\vk_instance.cpp`, `src\video_core\buffer_cache\{buffer,buffer_cache}.cpp`, `src\video_core\texture_cache\image.cpp` | **Диагностика** SSPC-001, только наблюдение: VMA и диспетчер vulkan-hpp получают обёртки `vkAllocateMemory`/`vkFreeMemory`/`vkCreateBuffer`/`vkCreateImage`/`vkBind*Memory*` (вызов и результат не меняются), конструкторы `Buffer`/`Image` и `BufferCache::ObtainBuffer` помечают текущий объект и гостевой диапазон; при неудаче `vmaCreateBuffer`/`vmaCreateImage` перед штатным `ASSERT` пишется отчёт (`src\bridge\shadps4\gpu_alloc_diag.cpp`). Журнал аллокаций по кучам — с `--vram-log` | вместе с GPU-мостом (Phase 4) |
| `patches\shadps4\0005-sspc001-bounded-upload-staging.patch` | `src\video_core\buffer_cache\buffer_cache.cpp` (`BufferCache::SynchronizeMemory`) | **Исправление SSPC-001** (`docs\KNOWN_ISSUES.md`): загрузка в арену больше 64 МиБ идёт порциями через один staging-буфер ≤ 64 МиБ с `scheduler.Finish()` между порциями, вместо одного staging-буфера размером со всю загрузку (2,5 ГиБ для texel-буфера 2,4 ГиБ при загрузке уровня). Размеры арены и буферов не меняются, копируются те же данные; ожидание — только при загрузках > 64 МиБ. Не зависит от 0004 (применяется и без него) | вместе с GPU-мостом (Phase 4); в своём Vulkan-бэкенде — ограниченный staging с самого начала |
| `patches\shadps4\0006-window-event-interceptor.patch` | `src\sdl_window.{h,cpp}` | Phase 2 P2.1: `Frontend::SetEventInterceptor` — каждое событие окна сначала получает SecondSonPC (главный поток, в `WindowSDL::WaitEvent`, до обработки shadPS4); `true` = событие поглощено. Клавиатура и мышь идут в нативный слой ввода (`src\input`), окно и геймпады — дальше в shadPS4. Пока перехватчик не установлен (`--loader shadps4`, `--bridge pad`), поведение исходное | со своим окном (Phase 4) |
| `patches\shadps4\0007-native-direct-memory-backend.patch` | `src\core\memory.{h,cpp}` | Phase 2 P2.3.1 (`docs\PHASE2_P23_MEMORY.md`): `Core::DirectMemoryBackend` — при установленном бэкенде (`--native kernel-dmem`, экспериментально) физическую direct memory учитывает SecondSonPC (`Memory::DirectMemory`): `Allocate`, `Free`, проверка и пометка физических страниц в `MapMemory(Direct)` (`BeginMap`; если виртуальный диапазон не найден — откат `EndMap`), возврат в `UnmapBytesFromEntry` (`EndMap`), размер бюджета (`GetTotalDirectSize`). Виртуальная сторона, `AddressSpace` и уведомления rasterizer — прежние. Новые методы: `SetDirectMemoryBackend` (только на нетронутую `dmem_map` и без direct-отображений), `UnmapDirectPhysical` (снимает ровно пересечение видов с освобождаемыми физическими диапазонами, GPU — до снятия). Не импортируемые игрой `PoolExpand/PoolCommit`, `DirectMemoryQuery`, `DirectQueryAvailable`, `Get/SetDirectMemoryType` при бэкенде возвращают ошибку с `LOG_CRITICAL`, а не ответ по неиспользуемой `dmem_map`. Без бэкенда (по умолчанию, `--loader shadps4`) выполняется исходный код | со своим менеджером виртуальной памяти (P2.3.2) |

## Отклонения папки игры от оригинала (обнаружено при аудите, сделано не SecondSonPC)
`E:\shdps4\CUSA00004\art\cache\psarc\english_{a,a_p,b,b_p}.psarc` были копиями `russian_*`. Оригиналы лежат в `E:\shdps4\CUSA00004\_LANG_BACKUP_2026-10-07_22-58-16\psarc`.
**2026-10-08 восстановлено** (копирование из бэкапа, бэкап не тронут). SHA-256 после восстановления совпадает с AUDIT.md, раздел 2:
`english_a` b3c8f38f…, `english_a_p` c19a5c66…, `english_b` 373055af…, `english_b_p` 98a2f5d8…. Файлы `russian_*` не менялись.
