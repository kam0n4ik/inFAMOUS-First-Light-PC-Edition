# P2.3.3a — кто, кроме GPU, пользуется менеджером памяти моста

Статус (2026-10-09): **аудит (A1)** и **интерфейс (A2)** — этот документ; реализация над мостом
(A3, патч `0009`) и проверка изоляции (A4, `tools\check_memory_deps.py`) — §5–§7. Предыдущий
подэтап: `docs\PHASE2_P232_GPU_MEMORY.md` (интерфейс GPU ↔ память, патч `0008`).

Источник — исходники: мост `third_party\shadPS4\src` (0fe263a + патчи `0001`–`0008`), SecondSonPC
`src\`. Номера строк — на дереве до патча `0009`. Путь игры — `--loader sspc` (по умолчанию);
путь `--loader shadps4` (эталон Phase 0) отмечен отдельно.

Обозначения столбцов:
- **Тип** — что делает обращение: *R* резерв диапазона, *M* отображение, *P* защита, *N* имя
  диапазона, *Q* запрос/проверка адреса, *W* уведомление «хост записал в гостевую память»,
  *B* бюджет/физическая память, *I* создание менеджера, *D* диагностика (чтение внутренних карт).
- **Блокировки** — что берёт менеджер моста: `unmap` = `unmap_mutex` (`std::mutex`), `w` / `r` =
  writer/reader `mutex` (`SharedFirstMutex`, у читателей приоритет), «нет» — без блокировок.
- **VEH** — может ли вызов произойти внутри обработчика исключений (векторного, `signals.cpp`).
- **H1** — опирается ли код на тождество «гостевой адрес = адрес хоста» (пишет/читает гостевую
  память по гостевому адресу или кладёт объекты хоста в гостевое адресное пространство).
- **Перенос** — можно ли перевести на интерфейс без смены поведения; **Риск** — чем грозит ошибка.

## 1. Как устроено сейчас

`Core::MemoryManager` (`core\memory.{h,cpp}`) — единственный владелец гостевого адресного
пространства: конструктор (`Common::Singleton`, создаётся при первом `Memory::Instance()`)
резервирует плейсхолдерами всё свободное ниже 1 ТиБ и создаёт секцию-подложку 8448 МиБ
(`AddressSpace`, `core\address_space.cpp`). Внутри: `vma_map` (виртуальные области), `dmem_map`
(физическая direct memory; с `--native kernel-dmem` — нативная `Memory::DirectMemory`, патч `0007`),
`fmem_map` (физическая flexible), бюджеты, слушатель GPU (`GpuMemoryListener`, патч `0008`).
GPU-мост обращается к нему только через `Core::GpuMemory` (P2.3.2). Всё остальное — ниже.

## 2. Пользователи в пути игры (`--loader sspc`)

### 2.1. SecondSonPC (`src\bridge\shadps4`)

| # | Где (файл: функция) | Тип | Что требуется (семантика) | Блокировки | VEH | H1 | Перенос | Риск |
|---|---|---|---|---|---|---|---|---|
| U1 | `bootstrap.cpp:957` (запуск, до `Trace::Init`) | I | `Core::Memory::Instance()` — создать менеджер **сейчас**: адресное пространство гостя зарезервировано до того, как хост начнёт выделять память (трамплины счётчика импортов, окно, Vulkan), иначе хост может занять гостевые адреса | нет (запуск, один поток) | нет | да: резерв определяет, куда могут попасть гостевые адреса | да — явная операция «зарезервировать» | средний: порядок запуска |
| U2 | `loader_host.cpp:49` `ReserveModule` | R | `MapMemory(Reserved, NoAccess, NoFlags, hint = желаемая база, name)`: поиск свободного от подсказки вверх (`SearchFree`), без отображения у хоста; возвращает базу модуля | `unmap` → `w` | нет | да: база модуля — адрес исполнения гостевого кода | да | высокий: адреса, видимые гостю (сверка карт `--memory-map`) |
| U3 | `loader_host.cpp:56` `MapTrampoline` | M | `MapMemory(Code, RW+X, Fixed)` поверх зарезервированного: `CreateArea` снимает резерв в диапазоне, создаёт анонимную память (без физической основы), хост — RWX | `unmap` → `w`; GPU: `OnGpuUnmap` до, `OnGpuRegister`, `OnGpuMap` после | нет | да | да | средний |
| U4 | `loader_host.cpp:63` `MapSegment` | M | `MapMemory(Fixed, prot из PF_*, Code для системных модулей / Flexible для модулей игры)`: Flexible расходует бюджет flexible и физическую flexible память (130 МиБ у игры), вид секции по её смещению; затем загрузчик пишет сегменты по гостевым адресам и патчит код | как U3 | нет | да | да | высокий: раскладка, бюджет flexible |
| U5 | `loader_host.cpp:68` `SetupMemoryRegions` | B | бюджеты direct/flexible из `SceKernelMemParam` eboot (`SetupMemoryRegions`), затем сверка нативного бюджета (`ConfigureNativeDirectMemory`, P2.3.1) | **нет** (запуск, до гостя) | нет | нет | да | средний: бюджет `GetDirectMemorySize` видит игра |
| U6 | `loader_host.cpp:80` `MapPrimaryTls` | M (через API) | как libkernel: `sceKernelMapNamedFlexibleMemory(&0x881000000, size, 3, 0, "SceKernelPrimaryTcbTls")` — проверки аргументов API (длина кратна 16 КиБ, имя ≤ 31 символа, без `Fixed`), затем `MapMemory(Flexible, RW, подсказка 0x881000000)` | `unmap` → `w` | нет | да: TLS/TCB главного потока | да — на уровне менеджера с теми же аргументами (проверки API для этих постоянных аргументов проходят всегда) | средний |
| U7 | `loader_host.cpp:87,91` `InitGnmDriverMemory` | B, M (через API) | как libSceGnmDriver: `sceKernelAllocateDirectMemory(0, GetDirectMemorySize(), 0x10000, 0x10000, тип 3)` → `Allocate` (или нативный учёт), затем `sceKernelMapNamedDirectMemory(&0xfe0000000, 0x10000, 0x13, 0, phys, 0x10000, "SceGnmDriver")` → `MapMemory(Direct, exclusive = SDK ≥ 2.50 и алиасинг не включён)` | `unmap`+`w` (Allocate), затем как U3 | нет | да | да — уровень менеджера; `exclusive` считается так же, как в обёртке API (на момент загрузчика гостевой код ещё не мог включить алиасинг) | средний: физический адрес и вид видны игре/GPU |
| U8 | `fs_host.cpp:60` `InvalidateGuestMemory` ← нативная ФС (`runtime\filesystem\file_system.cpp`, чтение в гостевой буфер) | W | `InvalidateMemory(addr, size)` → `gpu->OnGuestWrite`: до записи хостом в гостевой буфер GPU должен сбросить отслеживание страниц | **нет** | нет | да: хост пишет в буфер по гостевому адресу | да | низкий; частый вызов (каждое чтение файла) |
| U9 | `memory_host.cpp` (P2.3.1/P2.3.2) | B, D | установка нативной direct memory (`SetDirectMemoryBackend`), снятие видов при нативном освобождении (`UnmapDirectPhysical`), бюджет моста, `SecondSonMemoryAccess` (friend: карты `vma_map`/`dmem_map` для `--memory-map`, пробы для самотеста) | `unmap`/`w`/`r` по операции | нет | нет | **частично**: дамп VMA — через интерфейс (§4); остальное — склейка *реализации* моста с нативной direct memory | — |

### 2.2. Мост, вызывается в пути игры

| # | Где | Тип | Что требуется | Блокировки | VEH | H1 | Перенос | Риск |
|---|---|---|---|---|---|---|---|---|
| U10 | `threads\thread_state.cpp:23–28` `ThreadState::ThreadState` (первое обращение к `ThrState`) | M | `MapMemory(File, RW, NoFlags, подсказка = SystemReservedVirtualBase(), "ThrHeap")`: куча объектов `Pthread` моста — **объекты хоста лежат в гостевом адресном пространстве** | `unmap` → `w` | нет | да | да (нужна база system reserved) | средний |
| U11 | `threads\stack.cpp:101–108` `ThreadState::AllocStack` (создание гостевого потока) | M, P | `MapMemory(Stack, RW, NoFlags, подсказка last_stack)` — стеки сверху вниз от `usrstack` `0x7EFFF8000`; затем `Protect(guard, NoAccess)`; `memset` стека и запись `Stack` (свободные стеки) в его вершину — по гостевым адресам | `unmap` → `w`; `Protect`: `w`+`unmap` | нет | да | да | средний: стеки всех гостевых потоков |
| U12 | `threads\pthread.cpp:335–336` (инициализация потока) | Q | `IsValidMapping(name)` — указатель имени внутри управляемого диапазона (разрывы не считаются); **без блокировки** (гонка чтения `vma_map` с map/unmap — D13, §3) | нет | нет | да (имя читается по гостевому адресу) | да | низкий |
| U13 | `threads\pthread.cpp:495–524` `posix_pthread_rename_np` | N | `NameVirtualRange(stack, stacksize, name)` | `w`+`unmap` | нет | нет | да | низкий |
| U14 | `kernel\file_system.cpp:356–359` `ReadFile` ← `read`/`pread` моста (в пути игры — только «чужие» дескрипторы: `FsHost::Foreign*`) | W | `InvalidateMemory` до чтения в гостевой буфер | нет | нет | да | да | низкий |
| U15 | `np\np_trophy.cpp:364–367` `ReadFile` ← `sceNpTrophyGetGameIcon`/`GetTrophyIcon` (игра их **не импортирует**) | W | как U14 | нет | нет | да | да | низкий |
| U16 | `kernel\memory.cpp` (30 вызовов) — `sceKernel*`/`posix_mmap`/… | все | **API памяти PS4 моста**: проверки аргументов libkernel + вызов менеджера. Игра импортирует 13 из них (`docs\PHASE2_P23_MEMORY.md` §2), `libc.prx` — `mmap`, `munmap`, `sceKernelMapNamedFlexibleMemory` (кучи `malloc`) | по операции | нет | да | **нет — исключение E1** (§6): это фасад самой реализации моста; заменяется целиком группой NATIVE-функций над своим менеджером (P2.3.4) | — |
| U17 | VEH: `page_manager.cpp:432` `GuestFaultSignalHandler` → `ProtectTracking` | P | GPU (P2.3.2, G16) — через `GpuMemory` | лист `AddressSpace` | **да** | да | сделано в P2.3.2 | — |

Прочие обработчики исключений не трогают менеджер: регистратор SecondSonPC (`src\base\crash.cpp`,
`VirtualQuery` адреса), патчер моста (`cpu_patches.cpp` `TryPatchJit` — список модулей), SRT-walker
шейдеров (`flatten_extended_userdata_pass.cpp:71` — свой код). Из VEH к менеджеру ведёт только
GPU-путь U17 — **не-GPU пользователей в VEH нет**.

### 2.3. Косвенные пользователи

- **Гостевой код → API моста (U16).** Игра (`eboot`) — direct memory, `mprotect`, запросы; `libc.prx`
  (LLE) — `mmap`/`munmap`/flexible для своих куч. Сам гость меняет только то, что разрешает API.
- **Загрузчик SecondSonPC (`src\loader`)** — только через `Loader::Host` (U2–U7). TLS не-главных
  потоков: `Linker::AllocateTls` → `heap_malloc` гостевой `libc` (после
  `_sceKernelRtldSetApplicationHeapAPI`) → `mmap` моста (U16), до него — `malloc` хоста.
- **Нативные функции SecondSonPC:** `runtime\kernel\memory.cpp` (P2.3.1, `--native kernel-dmem`) —
  только нативная `DirectMemory`; освобождение снимает виды через `memory_host.cpp` (U9).
- **Адресные преобразования и проверки:** `IsValidMapping` (U12, весь управляемый диапазон без
  разрывов), `IsValidGpuMapping` (GPU, 40 бит — D10), `ClampRangeSize` (GPU), база/конец system
  reserved (U10; кольцо тесселяции GnmDriver — через `GpuMemory`), `DEFAULT_MAPPING_BASE`
  `0x200000000` (поиск свободного внутри менеджера). Других переводов «гость ↔ хост» нет: адреса
  тождественны (H1), физический адрес direct memory = смещение в подложке (H2).
- **Отправка GPU-команд** (`liverpool.cpp`, `vk_rasterizer.cpp`): прямых обращений к менеджеру нет
  (P2.3.2); чтение PM4/косвенных буферов — по гостевым адресам (H1).

## 3. Вне пути игры и отладка

| # | Где | Тип | Почему не путь игры | Решение |
|---|---|---|---|---|
| X1 | `core\linker.cpp:67,147,197–201,524`, `core\module.cpp:126–166` | R, M, B | загрузчик shadPS4 — только `--loader shadps4` (эталон Phase 0; путь `sspc` его не создаёт: `bootstrap.cpp` «The sspc path never creates shadPS4's Core::Linker») | исключение E2 |
| X2 | `emulator.cpp:562` | I | `Core::Emulator` не используется SecondSonPC (свой `bootstrap.cpp`) | исключение E2 |
| X3 | `devtools\widget\memory_map.cpp:72` `MemoryMapViewer` (friend) | D | отладочное окно ImGui моста (меню devtools); читает `vma_map`/`dmem_map` под **writer**-блокировкой на потоке отрисовки | исключение E3 (диагностика реализации моста) |
| X4 | `avplayer\avplayer_source.cpp:691` (W), `camera\camera.cpp:234,256` (M через API, Q), `vr_tracker\vr_tracker.cpp:109–130` (Q: `VirtualQuery`), `np_signaling\np_signaling_helpers.cpp:104,172,192` (Q, M/U через API), `ime\ime_dialog_ui.cpp:63` (Q), `font\font.cpp:5700` (Q) | разные | библиотеки моста, из которых игра **ничего не импортирует** (`docs\generated\imports.csv`, `prx_imports.csv`) — проверяется автоматически (§7) | исключение E4 |
| X5 | `video_core\page_manager.cpp:348` `UffdImpl` | Q | только Linux (`#ifdef __linux__`), в сборке Windows не компилируется | исключение E5 (из P2.3.2) |
| X6 | `src\bridge\shadps4\selftest.cpp` | все | самотест **реализации** моста (сравнение с моделью и интерфейсами) | исключение E6 |

**D13 (новое, не исправлялось):** `MemoryManager::IsValidMapping` (U12, а также X4) читает `vma_map`
**без блокировки** — гонка с одновременным map/unmap (итератор на удаляемый узел `std::map`).
В пути игры — проверка имени при создании потока. Поведение не меняется в P2.3.3a (этап без смены
поведения); в своём менеджере проверка идёт под reader-блокировкой.

## 4. Интерфейс для не-GPU пользователей (A2)

Принцип — тот же, что у `Core::GpuMemory` (P2.3.2): интерфейс объявлен в самом мосте (новый
заголовок патча `0009`, `core\guest_memory.h`), потому что его пользователи U10–U15 — код моста;
реализация моста — адаптер без состояния в `memory.cpp`; свой менеджер (P2.3.3b/c) реализует тот же
контракт. Уровень контракта — **менеджер памяти**, т. е. после проверок аргументов API libkernel
(их делает U16 или вызывающий). Обязанности разделены на три класса — у всех один владелец (менеджер),
поэтому разделение не усложняет владение:

| Класс | Методы | Кто пользуется | Чьё состояние |
|---|---|---|---|
| `Core::GuestVirtualMemory` — виртуальные диапазоны | `Reserve()` (адресное пространство — сейчас; U1), `Map(GuestMapping)` (резерв, анонимная память Code/Stack/File/System, flexible, direct — U2–U4, U6, U7, U10, U11), `Unmap`, `Protect` (U11), `NameRange` (U13), `IsValid(addr, size)` (U12), `SystemReservedBase()` (U10), `Areas()` — снимок VMA для диагностики (U9) | загрузчик, потоки моста, диагностика | VMA, плейсхолдеры, защита хоста |
| `Core::GuestPhysicalMemory` — физическая память и бюджеты | `SetupRegions(flexible, ext1, ext2)` (U5), `DirectSize()`, `AllocateDirect(...)` (U7), `ReleaseDirect(start, len, checked)` | загрузчик, тесты | `dmem_map`/нативная `DirectMemory`, бюджеты |
| `Core::GuestWriteNotifier` — уведомления | `HostWrote(addr, size)` → слушателю GPU (`OnGuestWrite`) (U8, U14, U15) | ФС SecondSonPC и моста | — (пересылка слушателю) |

`GuestMapping` — запрос отображения в терминах менеджера: адрес (подсказка или точный с `Fixed`),
размер, `MemoryProt`, `MemoryMapFlags`, `VMAType`, имя, физический адрес и `exclusive` для direct,
выравнивание. Перечисления `MemoryProt`, `MemoryMapFlags`, `VMAType` переносятся из `core\memory.h`
в `core\guest_memory.h` (тот включается в `memory.h`), чтобы переведённые файлы **не включали**
`core\memory.h` вовсе — тогда любое оставшееся прямое обращение к менеджеру не скомпилируется.

Чего в интерфейсе нет и почему:
- **Доступ к гостевым данным** — по гостевому адресу (H1: гость = хост). Отдельного «чтения/записи
  гостевой памяти» не вводится; проверка указателя — `IsValid`. Запись мимо защиты — только GPU
  (`TryWriteBacking`, H2).
- **API PS4** (`sceKernel*`) — остаётся у реализации (U16, исключение E1).
- **Пулы, PRT, файловые отображения** (`PoolExpand/Commit/Decommit`, `SetPrtArea`, `MapFile`) —
  ими пользуется только API (U16) и GPU (PRT); не-GPU пользователей нет.

Подключение своего менеджера (P2.3.4) — одна точка: `Core::GetGuest*()` и `Core::GetGpuMemory()`
возвращают реализацию, выбранную до первого обращения; см. `docs\PHASE2_P233_NATIVE_VMM.md`.

**D17 (новое, не исправлялось — `docs\KNOWN_ISSUES.md` SSPC-006):** `UnmapMemory` снимает по одной VMA, а
анонимная память у хоста — одно частное выделение, которое нельзя резать: если VMA такой области
разрезана (guard-страница стека, имя, частичный `Fixed`), снятие падает на `ASSERT "Cannot split unbacked
mapping"`. Найдено самотестом этого подэтапа (стек с guard-страницей); в пути игры не проявляется (стеки
не снимаются).

## 5. Реализация над мостом (A3)

Патч `patches\shadps4\0009-guest-memory-interface.patch` (8 файлов, +274/−64; `docs\PATCHES.md`):
`core\guest_memory.h` (интерфейсы, `GuestMapping`, `GuestArea`, перечисления из `memory.h`), адаптер
`BridgeGuestMemory` в `memory.cpp` (каждый метод — тот вызов менеджера, который делали пользователи до
патча, с теми же аргументами; новый `MemoryManager::GetAreas` под reader-блокировкой — для снимка
областей), переведены U10–U15 моста. В SecondSonPC переведены U1 (`bootstrap.cpp`: `Reserve()`), U2–U7
(`loader_host.cpp`), U8 (`fs_host.cpp`), дамп VMA из U9 (`memory_host.cpp` → `Areas()`, формат строк
прежний). U6/U7 раньше шли через функции API libkernel моста; теперь — тот же вызов менеджера, который
делали эти функции (их проверки аргументов для постоянных аргументов загрузчика проходят всегда;
`exclusive` у GnmDriver = SDK ≥ 2.50, т. к. `sceKernelEnableDmemAliasing` не импортирует ни один модуль
игры). Из лога моста пропали две строки `Kernel.Vmm` Info этих вызовов — вместо них строки `Loader`
SecondSonPC с адресом и кодом. Второго набора отображений, новых выделений, новых блокировок и
уведомлений нет.

Самотест `GuestMemory` (5 проверок): (1) простые запросы интерфейса = менеджера; (2) сценарий всех
перенесённых вызовов (резерв модуля, трамплин, сегменты Flexible/Code, стек + guard + имя, куча потоков,
`IsValid` имени, direct-память GnmDriver с повторным эксклюзивным отображением → EBUSY, чтение файла,
снятие всего) через интерфейсы и напрямую в менеджер — одинаковые коды, адреса, области, состояние
страниц хоста и уведомления GPU (с блокировками); (3) `InitGnmDriverMemory` загрузчика =
`sceKernelAllocateDirectMemory` + `sceKernelMapNamedDirectMemory` (та же область, физический адрес,
страницы, EBUSY); (4) `MapPrimaryTls` = `sceKernelMapNamedFlexibleMemory`; (5) всё снято, число VMA
прежнее, `dmem_map` нетронута. Мутация (адаптер теряет `exclusive`) ловится проверкой (2).

## 6. Исключения (прямые обращения, оставленные намеренно)

| # | Что | Почему | Когда уходит |
|---|---|---|---|
| E1 | `core\libraries\kernel\memory.cpp` (API памяти моста, U16) | фасад реализации моста: проверки libkernel + её менеджер | NATIVE-группа над своим менеджером (P2.3.4+) |
| E2 | `core\linker.cpp`, `core\module.cpp`, `emulator.cpp` | путь `--loader shadps4` (эталон) и неиспользуемый `Core::Emulator` | с мостом (Phase 5) |
| E3 | `core\devtools\widget\memory_map.{h,cpp}`; `src\bridge\shadps4\memory_host.cpp` (кроме дампа VMA) | диагностика и склейка **реализации** моста (friend-доступ к картам; `DirectMemoryBackend` P2.3.1) | с мостом / при переключении — свои аналоги |
| E4 | `avplayer`, `camera`, `vr_tracker`, `np_signaling`, `ime`, `font` | игра не импортирует эти библиотеки (проверяется автоматически) | с мостом |
| E5 | `video_core\page_manager.cpp` `UffdImpl` | только Linux | — |
| E6 | `src\bridge\shadps4\selftest.cpp` | тест реализации моста | — |
| E7 | `core\memory.{h,cpp}`, `core\address_space.{h,cpp}` | сама реализация | — |

## 7. Проверка изоляции (A4)

`tools\check_memory_deps.py` (запуск: `python -I tools\check_memory_deps.py`, код 0 = чисто):
1. ищет во всех `*.cpp/*.h/*.inc` моста и `src\` прямые обращения к менеджеру (`core/memory.h`,
   `Memory::Instance`, `MemoryManager`, `GetAddressSpace`, класс моста `AddressSpace`,
   `SecondSonMemoryAccess`; комментарии не считаются) и сверяет каждое со списком исключений §6;
2. требует, чтобы переведённые файлы (U1–U15) **не** включали `core/memory.h`, а если проверяют в
   `#if` макрос, который раньше приходил через него (`ARCH_X86_64`/`ARCH_ARM64` из `common/arch.h`), —
   включали его заголовок сами (§8);
3. для E4 проверяет по `docs\generated\imports.csv`/`prx_imports.csv`, что игра действительно
   ничего не импортирует из этих библиотек;
4. для E5 — что место внутри `#ifdef __linux__`.
Компилятор дополняет проверку: переведённые файлы не видят `Core::MemoryManager` вовсе.

Результат: 1012 файлов, 24 исключения (E1–E8, у каждого — причина в выводе), **0 запрещённых**. Проверка
самой проверки: временный файл с `Memory::Instance()` → FAIL; возврат `#include "core/memory.h"` в
`fs_host.cpp` → FAIL («moved to the interfaces but still includes core/memory.h»); после отката — PASS.
E8 — включение заголовка без использования (сгенерированная таблица экспортов, `imgui_impl_sdl3.cpp`
upstream); проверка требует, чтобы других совпадений в этих файлах не было.

## 8. Проверки P2.3.3a и найденная регрессия

**Регрессия, найденная игрой и исправленная до коммита.** Первый игровой A/B (`logs\performance\p233a_ab`)
показал падение новой сборки на старте в 3 из 3 прогонов: `UNREACHABLE "_runOnAnotherStack not implemented
on target architecture"` на первом гостевом потоке. Причина — в `pthread.cpp` макрос `ARCH_X86_64` приходил
только транзитивно через `core/memory.h` → `core/address_space.h` → `common/arch.h`; без него под
`#if defined(ARCH_X86_64)` компилируется заглушка. Компилятор и самотест этого не видят (самотест не
создаёт настоящих гостевых потоков: TLS гостя даёт загрузчик, а игра в самотесте не загружена).
Исправлено явным `#include "common/arch.h"` в `pthread.cpp`; в `check_memory_deps.py` добавлено правило
(проверено: без include → FAIL). Остальные переведённые файлы других макросов из выпавших заголовков в
условиях препроцессора не используют (проверено поиском и независимым ревью).

**Сборка и тесты (финальная сборка этапа):** без ошибок и предупреждений; `--selftest` **655 / 0** ×3
(+5: группа `GuestMemory`), `--fault-test` **22 / 0**, `game_process_tests.ps1` **84 / 0**,
`check_memory_deps.py` PASS. Цепочка `0001`–`0009` на чистом HEAD воспроизводит рабочее дерево байт в байт,
`git apply --reverse --check` признаёт `0009` применённым.

**Игра (лес, статичная сцена, 100 с, копия сейвов, `--memory-map 45`):**

| Сессия | Прогоны | FPS (геймплей) | P50 / P95 / P99, мс | Итог |
|---|---|---|---|---|
| `p233a_ab2` (после исправления) | до-M0 ×2, после-M0 ×2, после-M1 ×1 | до 25,98; после-M0 23,48; после-M1 25,09 | 33,89 / 50,98 / 59,47; **48,73** / 51,36 / 60,08; 34,23 / 50,91 / 51,81 | все `closed-by-test`, first-chance 0; разница M0 — **другая сцена**: на кадрах 95-й секунды у обоих «после-M0» камера повёрнута к огням (тяжелее), у «до» и «после-M1» — привычный вид со спины; первый «до» вообще в другом месте. Окно в режиме `--press` принимает живой ввод — вероятно, мышь/фокус во время прогонов. Сравнение недостоверно → повтор |
| `p233a_ab3` (повтор, ABBA, без внешнего ввода) | до-M0 ×2, после-M0 ×2 | до **25,26** (24,82…25,69); после **25,92** (25,33…26,52) | до 34,05 / 51,05 / 59,09; после 33,92 / 50,88 / 51,65 | все `closed-by-test`, first-chance 0; разница +0,66 FPS меньше разброса одной сборки (до 1,9 в P2.3.2) — **шум, регрессии нет**; private/WS/commit/VRAM в пределах разброса |

Карты памяти (`--memory-map 45`) всех прогонов обеих сессий совпадают построчно с картой «до», кроме
4 строк сегментов `eboot.bin` (база плавает от запуска к запуску, как в P2.3.1/P2.3.2): загрузчик через
интерфейс кладёт модули, TLS главного потока и память GnmDriver в те же адреса и физические смещения.
`--native kernel-dmem` (M1) с новой сборкой — работает (25,09 FPS, тот же вид сцены, что у «до»).

**Независимое ревью** (агент `ecc:cpp-reviewer`, только чтение): изменений поведения не найдено (значения
по умолчанию `GuestMapping`, эквивалентность путей загрузчика API libkernel, `exclusive`, условия
препроцессора, порядок блокировок). По замечаниям: ветка «через интерфейс» самотеста вызывает настоящие
`LoaderHost::ReserveModule/MapTrampoline/MapSegment`; повторное эксклюзивное отображение обязано дать
EBUSY (иначе провал, вид снимается); шаблон include в проверке расширен; порядок include в трёх файлах
моста; уточнены комментарии загрузчика. Не принято (обосновано): `constinit` для адаптера — тот же
приём, что у `bridge_gpu_memory` (P2.3.2), адаптер без состояния и до `main` не вызывается.
