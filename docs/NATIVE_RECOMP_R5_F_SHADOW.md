# R5-F: теневая подсистема — цикл `0x43d590`, draw `0x43d480` и реконструкция `0x451700`

Часть Native Engine R5 (`docs/NATIVE_ENGINE_RECONSTRUCTION.md`, рекомпилятор — `docs/NATIVE_RECOMP_COMPILER.md`,
схема сверки с внешними вызовами — `docs/NATIVE_RECOMP_CUE.md`). Адреса — смещения eboot CUSA00004 (база 0).
Метки: [A] анализ кода, [E] измерение, [H] гипотеза, [?] не разобрано. Дата — 20.10.2026. Автор — агент R5-F.

## 1. Итог

| Что | Статус | Доказательство |
|---|---|---|
| `0x451700` ShadowInstanceUv — ручная реконструкция | **OFFLINE_VERIFIED** [E] | self-check 4096 + дифф 100 000 случаев против eboot, 0 расхождений; self-check установки — passed |
| `0x451700` машинно переведённая (`recomp_451700`) | **OFFLINE_VERIFIED** [E] | self-check установки passed; дифф 100 000 случаев против eboot, 0 расхождений |
| `0x43d480` теневой draw экземпляров | **ANALYZED** [A] | оркестратор 10 вызовов Gnm/CUE; **под пробой CITY-OPT** (`draw_runs`), прямой jump не ставить |
| `0x43d590` цикл теневых кастеров | **ANALYZED** [A] | `BLOCKED_BY_ABI` для полного перевода (`and rsp,-0x20`); call graph и структуры разобраны (§2–3) |
| вспом. `0x27530`, `0x451290`, `0x450460`, `0x612110` | **ANALYZED** [A] | §3, классы рекомпилятора §5 |
| интеграция `0x451700` в игре (verify/native/guarded) | **не подтверждена в игре** [E] | сборка игры с реконструкцией собрана; прогон — см. §6, если стенд был свободен |
| FPS | **не измерено** [E] | реконструкция ≤ наносекунды на вызов, 0 влияния не ожидается; A/B не делались |

Флаги — существующие: `SSPC_NATIVE_ENGINE=verify|native|guarded|bench`,
`SSPC_NATIVE_ENGINE_FUNCS=shadow_instance_uv`, `SSPC_NATIVE_ENGINE_RECOMP=<recomp.dll>`. По умолчанию ничего не ставится.

## 2. Теневой цикл `0x43d590` (size 0xdb9, hash 0xc098a21924b37b67) [A]

Метод прохода теней (`vtbl+0x30 DrawItems`, вызывается через таблицы классов материалов `0xcd87e0`, `0xd68678`,
`0xd78d70`; `RENDER_ENGINE_ARCHITECTURE.md` §3.2). Сигнатура SysV `(rdi param_1 = material/pass-объект,
rsi param_2 = {gather, R, ..., sortedKeyRange})`. MEASURE §3 `NATIVE_ENGINE_RECONSTRUCTION.md`: 1392 вызова/кадр,
4859 мкс/кадр включительно (3491 исключительно) — **самый дорогой оркестратор рендер-подготовки**.

Вход `param_2`: `param_2[0]` = объект сбора (gather/мир), `param_2[1]` = контекст рендера R
(`R[0]`→View), `param_2[3]` = `{lo, hi}` диапазон отсортированных ключей. Из View берутся матрицы вида
(`View+0x200`, вариант `View+0x430` при `param_1+0x21 & 2`), камера `View+0x6c0..+0x6f0`, вектор `+0x6f0 − baseOffset`.

Две ветви по `param_1+0x1f`:

- **без экземпляров** (`param_1+0x1f == 0`): цикл по ключам `[lo, hi)`; для каждого индекс объекта в таблице
  `R[0x9b7]` (=S+0x4db8, записи 0x80 байт: `+0x18` meshInst → `[0]` геометрия, `[1]` описание; `+0x40..0x7f`
  матрица). Прореживание по качеству: `0x612110(&DAT_07f92020) % 0x401 >= iVar4` (iVar4 из
  `DAT_00da08d0[DAT_00f39978*0x14]`). При смене материала (`meshInst[1] != prev`) — `0x448640`. Если
  `param_1+0x120 != 0xff` — V# костей (`0x61e260(stride 0x30, n=geom+0x54−1)`, `0x16b70` в слот `stage 2`). Три
  блока констант под флагами `param_1+0x118` (мир → `0x11c890` в `R+0xd190`, копия в командный буфер `0x11be90`,
  `0x11bed0×2`), `param_1+0x11c` (`0x11cde0` в `R+0xd2f0`). Затем `0x450460`, `0x120280`, `0x1a1960` (меш-draw).
- **с экземплярами** (`param_1+0x1f != 0`): группирует соседние ключи с `(key ^ first) < 0x10000` (тот же меш и
  материал) в серию; память `0x1f0870(&DAT_00f1bcd0, n*0x48, 16)`; на объект 0x48 байт = матрица объекта × матрица
  вида-проекции каскада (4 строки по 16 байт, `vpermilps`/`vmulps`/`vaddps` — ручной матмул, строки 358–434
  декомпиляции) + 8 байт нулей; при `param_1+0x94 > 0` — `0x451700` (атлас-UV, §4) и `0x451290` на объект/серию;
  материал `0x448640` при смене; draw — `0x43d480(param_1, R, records, n, mesh)`.

Каскады теней — отдельные контексты (свои вызовы цикла со своим диапазоном и матрицей вида-проекции;
`CITY_ENGINE_OPTIMIZATION.md` §2).

Стековый кадр 0x2a0 байт, `__stack_chk` (`0x984360`), стабилизатор стека `and rsp,-0x20`. 16 вызываемых:
`0x11be90 0x11bed0 0x11c890 0x11cde0 0x120280 0x16b70 0x1a1960 0x1f0870 0x43d480 0x448640 0x450460 0x451290
0x451700 0x612110 0x61e260 0x984360`.

## 3. Теневой draw `0x43d480` (size 0x106, hash 0xaffc4c64777b1e37) [A]

```
System V: (rdi param_1 = pass-объект, rsi param_2 = R, rdx records(0x48·n), ecx n, r8 mesh) -> void
  0x120280(R)                                     константы draw (4.3)
  uVar2 = param_1+0x121                            ресурсный слот стадии
  0x61e260(&V#, records, 0x48, n)                  V# над записями экземпляров (stride 0x48, n)
  uVar4 = 0x27530(0)                               ShaderStage(0) -> CUE-стадия (=2, VS)
  0x16b70(R+0x128, 2, uVar2, 1, &V#)               ресурсный слот stage 2
  0x16aa0(R+0x128, 2, 0, mesh+0x6c, mesh+0x60)     таблица V# вершин меша в VS
  0x6250b0(R+8, n)                                 NUM_INSTANCES = n
  0x623740(R+8, 4)                                 VGT_PRIMITIVE_TYPE = TriList
  0x624f80(R+8, 0, 2)                              INDEX_TYPE 16 бит
  0x14ad0(R+0x128)                                 commit CUE (тень ресурсов → user data)
  0x623c60(R+8, mesh+0x68, mesh+0x58, 0)           sceGnmDrawIndex (count, ib)
```
MEASURE §3: 5118 вызовов/кадр, 2163 мкс/кадр включительно (423 исключительно). Оркестратор: вся работа — вызовы
библиотеки Gnm и CUE, которые пишут в DCB. Своей арифметики нет.

**Совместимость с CITY-OPT (критично).** На входе `0x43d480` стоит входная проба `draw_runs.cpp` (`Probe<1>`,
источник Shadow=2), а при `SSPC_CITY_ENGINE_GBUFFER_RUNS=1` ещё и проба на `0x448640`. Эти пробы — основа shadow
runs командного процессора (`parallel_cp.*`). Поэтому **ставить свой jump на вход `0x43d480` нельзя**: (1) при
включённом пресете первые байты уже перезаписаны пробой — проверка хеша тела в реестре не пройдёт; (2) второй
перехват конкурировал бы с CITY-OPT. В ветке R5 главного агента есть механизм «установка под пробой»
(`guest_probe.h ProbeRecord/FindEntryProbe`, реестр `OriginalBytes/InstalledBelowProbe`): реестр не ставит второй
jump, а записывает свой dispatcher в слот оригинала пробы. Я свой реестр под это **не меняю** (владение главного
агента); интеграцию `0x43d480` под пробой выполнит главный агент при переносе. Естественная цель для R5-F —
вызываемая подфункция **без** пробы: `0x451700` (§4).

### Вспомогательные функции [A]

| Адрес | Размер | Роль | Пробы | Класс рекомп. (§5) |
|---|---|---|---|---|
| `0x451700` | 0xa0 | атлас-UV теневого экземпляра (float2 в `out+0x40`); лист, 0 вызовов | нет | SUPPORTED |
| `0x27530` | 0xe | `ShaderStage→CUE-стадия`: `return *(u32*)(0x987c80 + i·4)`; лист, pure | нет | SUPPORTED (RIP-относит. глобаль) |
| `0x612110` | 0x3d | счётчик/ГПСЧ отсева качества (`[rbx+0x40c]`, при ≥0x100 — `0x612150` перезаполняет); 325 вызывающих | нет | SUPPORTED (вызов `0x612150` BLOCKED_BY_ABI) |
| `0x451290` | 0x1a5 | доп. данные экземпляров (`0x11e5d0/0x59d700/0x60d4c0`); вызывает 4 функции | нет | SUPPORTED |
| `0x450460` | 0xb17 | крупный помощник (деформация/скиннинг?) | нет | BLOCKED_BY_ABI (неизв. глубина стека) |
| `0x11be90`/`0x11bed0`/`0x120280` | — | аллокатор блока / setConstantBuffers / константы draw | нет | SUPPORTED (OFFLINE_VERIFIED у R5-B для `11be90`-семьи) |
| `0x448640` | 0x15f | установка материала (CUE-слоты) | да (Phase 6) | — (пропуск/подмена ломает следующий draw) |

## 4. Реконструкция `0x451700` — ShadowInstanceUv [A + E]

Лист, вызывается только из `0x43d590` (инстансная ветвь, `0x43e2be`, когда у прохода есть доп. данные
`R+0x94 > 0`). Для каждой записи экземпляра с видом `records[i+2] == 2` кладёт в теневую запись `out+0x40`
смещение плитки текстурного атласа (float2) по размеру атласа и индексу плитки экземпляра. Цикл перезаписывает те
же 8 байт каждую подходящую итерацию — результат = последней подходящей записи.

```
System V: (rdi records int*, rsi out, rdx atlas, ecx -, r8 table, r9 instance) -> void (пишет out+0x40, 8 байт)
  start=records[0]; count=records[1];  if count<=0 return
  base = table + (s32)atlas[0x5c]·0x20 + (s32)atlas[0x60]·0x10 + 0x20
  byte = (u8)instance[0x11]
  для i в [start, start+count):
    if records[i+2] == 2:
      tile = records[i+6]
      cell0 = *(s32*)(base + tile·4);  cell1 = *(s32*)(base + tile·4 + 4)
      n   = byte % (cell1·cell0)                          (знаковый 32-бит IDIV, как eboot; byte 0..255)
      col = n % cell0;  row = n / cell0
      *(float2*)(out+0x40) = { col·(1/cell0), row·(1/cell1) }   (vcvtsi2ss/vdivss/vmulss/vinsertps порядок eboot)
```
Поля со смещениями: запись экземпляра `out+0x40` (float2 UV); atlas `+0x5c`/`+0x60` (s32, индексы ряда/набора
в таблице ячеек, шаг 0x20/0x10); ячейка `base[tile]` = `{cell0 (столбцы), cell1 (ряды)}`; `instance+0x11`
(u8, индекс плитки экземпляра); `records` — массив int: `[0]` start, `[1]` count, `[i+2]` вид, `[i+6]` tile.

Константа `0xaad6f4 = 1.0f` (встроена, вшита в реконструкцию как `_mm_set_ss(1.0f)`). Деление целочисленное —
знаковый IDIV: при нулевых размерах атласа оригинал даёт #DE; у настоящих и генерируемых входов размеры ненулевые.

Файлы: `src/native_engine/reconstructed/shadow_instance_uv.{h,cpp}` (новые). FP — те же инструкции и порядок, что
у оригинала (`#pragma clang fp contract(off)`, `/arch:AVX`); значения целочисленно-производные, NaN невозможен,
поэтому 8 байт бит-в-бит на том же CPU/MXCSR.

### 4.1 Сверка

| Проверка | Результат |
|---|---|
| install self-check (клон eboot против реализации, 4096 сгенерированных входов) | **passed** (ручная и `recomp_451700`) |
| дифф против кода eboot через `shadow_tests.cpp`, 100 000 случаев (ручная) | **0 расхождений** |
| дифф против кода eboot, 100 000 случаев (`recomp_451700`) | **0 расхождений** |
| весь набор `sspc_native_engine_tests --eboot ... --recomp recomp.dll` | **130 проверок, 0 ошибок — PASS** |
| `--lift-check recomp_lift.dll --lift-trials 300` | 690 инструкций, 1 035 000 прогонов; **2 инструкции с «fault disagreement»** на `idiv` (§5), результатных расхождений 0 |

Verify/Guarded/Bench реализованы: функция пишет только `out+0x40` (8 байт), поэтому сверка идёт на теневых копиях
окна 0x48 байт вокруг `out` (остальные указатели — только на чтение, двойных побочных эффектов нет). Guarded при
расхождении делает `Fallback()`. `RecordCase` пишет приведённый случай (`cell0/cell1/byte` + оба выхода),
`Replay` пересчитывает формулу (а не перезапускает eboot — функция сводится к `f(cell0,cell1,byte)`).

## 5. Классы рекомпилятора и блокеры [A]

| Функция | Класс | Причина / замечание |
|---|---|---|
| `0x451700` | **SUPPORTED** | 42 инстр., 1 SIMD, 8 FP, 0 вызовов; переведена и сверена |
| `0x43d480` | **SUPPORTED** | 74 инстр., 11 прямых вызовов, 0 SIMD; переводима, но сверка требует записи 10 целей (как `0x14ad0`), и вход под пробой CITY-OPT — интеграция у главного агента |
| `0x43d590` | **BLOCKED_BY_ABI** | `and rsp,-0x20` на `0x43d59d` — динамический указатель стека; полный перевод генератором невозможен |
| `0x450460`, `0x612150` | BLOCKED_BY_ABI | неизвестная глубина стека |
| `0x27530`, `0x612110`, `0x451290`, `0x11be90`, `0x11bed0`, `0x120280` | SUPPORTED | вспомогательные; не реконструированы в этой итерации |

**Проблема генератора (репро готов).** `--lift-check` под случайным фаззингом сообщает «fault disagreement» на
`0x451773 idiv ebx` и `0x451778 idiv r11d`: на части случайных состояний настоящий CPU даёт #DE (деление на 0 или
переполнение частного), а поднятая инструкция — нет. Результатных расхождений (когда ни одна сторона не падает) —
**0**. Это свойство фаззера на входах вне области определения функции, не дефект реконструкции `0x451700`
(дифф полной функции 0/100 000). Нужна либо поднятая `div`/`idiv` с тем же #DE, что у железа (нуль-делитель и
переполнение частного, включая INT_MIN/−1), либо пропуск таких операндов в `--lift-check`. Репро:
`E:\research\native-shadows\generator_issues\idiv_fault_disagreement.md` (+ `idiv_fault.pcode.json`). Diff-предложение
к `pcode2c.py` не коммичу (владение главного агента) — описание в репро.

### План полной замены `0x43d590`

Полный статический перевод невозможен (`BLOCKED_BY_ABI`, `and rsp,-0x20`). Варианты:
1. **Ручная реконструкция** цикла (как `0x11c890`): реально, но цикл вызывает 16 функций с побочными эффектами в
   DCB/CUE/аллокаторы; сверка потребовала бы записывающих заглушек для всех (схема `0x14ad0` §5 `NATIVE_RECOMP_CUE.md`),
   а матмул строк 358–434 (`vpermilps`/`vmulps`/`vaddps`) — отдельного бит-в-бит теста. Объём — крупнее `0x11c890`.
2. **Снять блокер ABI в генераторе**: поддержать `and rsp, imm` как выравнивание кадра (известная константа) — тогда
   `0x43d590` станет LIFTABLE, но сверка упрётся в те же 16 внешних вызовов. Это — работа над `pcode2c.py` (главный агент).
3. Приоритетный безопасный путь (сделан): реконструировать листовые подфункции без проб (`0x451700`), оставить
   оркестраторы (`0x43d590`, `0x43d480`) под пробами CITY-OPT; выигрыш рендер-подготовки уже даёт shadow runs.

Блокеры: (а) `and rsp,-0x20` в `0x43d590`; (б) `0x43d480`/`0x448640` под пробами CITY-OPT — прямой jump ломает хеш;
(в) сверка оркестраторов требует записывающих заглушек 10–16 целей.

## 6. Воспроизведение

```
# P-code и классификация
cd tools\native-reconstruction\ghidra   (проект scripts-shadows)
run.py ..\recomp\pcode_dump.py E:\research\native-shadows\recomp\pcode.json 1f0870 61e260 61f430 11c890 11cde0 120280 16aa0 16b70 451700
py -I -X utf8 ..\recomp\pcode2c.py pcode.json recomp_gen.cpp --report recomp_report.txt --lift recomp_lift.cpp --coverage coverage.tsv
powershell -File E:\research\native-shadows\build_recomp.ps1 -Dir E:\research\native-shadows\recomp
# тесты
powershell -File E:\research\sspc_build.ps1 -Worktree E:\SecondSonPC-native-shadows -Owner native-shadows -Log E:\research\native-shadows\build.log -Target sspc_native_engine_tests
build\bin\sspc_native_engine_tests.exe --eboot E:\shdps4\CUSA00004\eboot.bin --cases 1000000 --recomp E:\research\native-shadows\recomp\recomp.dll
build\bin\sspc_native_engine_tests.exe --eboot E:\shdps4\CUSA00004\eboot.bin --lift-check E:\research\native-shadows\recomp\recomp_lift.dll --lift-trials 300
# игра (verify; 0x451700 не под пробой — ставится обычным jump)
powershell -File tools\native-reconstruction\run_native_engine.ps1 -Tag r5f_shadow_uv_verify -Worktree E:\SecondSonPC-native-shadows -Out E:\research\native-shadows\runs -Owner native-shadows `
    -Env "SSPC_NATIVE_ENGINE=verify;SSPC_NATIVE_ENGINE_FUNCS=shadow_instance_uv"
```
Сгенерированный код (`recomp_gen.cpp`, DLL) выведен из кода игры и в репозиторий не входит
(`E:\research\native-shadows\recomp`).
