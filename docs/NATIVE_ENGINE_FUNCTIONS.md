# Функции движка: статус нативной реконструкции

Реестр функций eboot, по которым есть работа Native Engine Reconstruction (`docs/NATIVE_ENGINE_RECONSTRUCTION.md`).
Адреса — смещения eboot (база 0, как в проекте Ghidra). Имена — наши (символов в eboot нет).

Статусы (накопительно, каждый следующий включает предыдущие):

| Статус | Что значит |
|---|---|
| ANALYZED | границы, сигнатура, побочные эффекты разобраны по дизассемблеру/Ghidra; **кода реконструкции нет** |
| RECONSTRUCTED | есть своя C++-реализация, компилируется |
| VERIFIED | побитово сверена с оригинальными инструкциями eboot (офлайн на сгенерированных входах и/или в игре на реальных вызовах) |
| INTEGRATED | ставится в игру вместо оригинала (`SSPC_NATIVE_ENGINE`), исполняется в реальной игре |
| BENCHMARKED | измерена стоимость: своя функция, оригинал, вызов через перехват, влияние на кадр |
| REJECTED | рассмотрена и отклонена (причина в таблице) |

Метки достоверности: [A] анализ кода, [E] измерение, [H] гипотеза.

## Реконструированные

| Адрес | Размер | Наше имя | Статус | Ключ / файл | Проверено |
|---|---|---|---|---|---|
| `0x11c890` | 0x507 | ObjectConstants::Update — константы объекта draw-контекста (`R+0xd190`) с флагом изменения `+0x158` | **BENCHMARKED** — в игре verify: 19 046 190 вызовов, 0 расхождений; native в 3 прогонах A/B (~4400 вызовов/кадр); стоимость ≤ 0,19 мс/кадр; FPS: изменение не измерено | `object_constants`, `src/native_engine/reconstructed/object_constants.*` | §3, §5.2, §5.3, §6, §8.1 |
| `0x1f0870` | 0x73 | FrameScratch::Alloc — lock-free bump-аллокатор памяти кадра (две арены) | **BENCHMARKED** — в игре verify: 21 244 573 вызова, 0 расхождений (live-diff 2 799 392); native в 3 прогонах A/B (~6900 вызовов/кадр); стоимость ≤ 0,09 мс/кадр | `frame_alloc`, `src/native_engine/reconstructed/frame_scratch.*` | §3, §5.2, §5.3, §6, §8.2 |
| `0x16aa0`, `0x16b70` | 0xc6 | CUE::SetVertexBuffers / CUE::SetBuffers — V# в тень стадии или `setUserDataRegion` в DCB, флаг «ресурсы изменены» | **VERIFIED + INTEGRATED** (R5-C) — в игре verify: 48 120 923 и 57 473 163 вызова, 0 расхождений; вызовы `0x626520` в сверке записываются, а не исполняются; FPS не измерен | `cue_set_vertex_buffers`, `cue_set_buffers`, `src/native_engine/reconstructed/cue_set_buffers.*` | `NATIVE_RECOMP_CUE.md` |
| `0x169d0`, `0x16d10`, `0x16eb0` | 0xc6 | CUE::SetConstantBuffers / SetTextures / SetSamplers — то же семейство (V# 16, T# 32, S# 16 байт) | **VERIFIED + INTEGRATED** (R5-D) — в игре verify: 55,5 / 33,0 / 10,8 млн вызовов, 0 расхождений; native/guarded в игре не прогонялись; FPS не измерен | `cue_set_constant_buffers`, `cue_set_textures`, `cue_set_samplers`, `src/native_engine/reconstructed/cue_set_buffers.*` | `NATIVE_RECOMP_R5_D.md` |
| `0x448640` | 0x15f | Material::Bind — константы материала и сэмплеры/текстуры прохода через три сеттера | **VERIFIED + INTEGRATED** (R5-D) — в игре verify: 15,4 млн вызовов, 0 расхождений; ставится под пробой CITY-OPT (`SSPC_CITY_ENGINE_GBUFFER_RUNS=1`) | `material_bind`, `src/native_engine/reconstructed/material_bind.*` | `NATIVE_RECOMP_R5_D.md` §3 |
| `0x11e5d0` | 0x36 | R::SetDrawConstants — сравнить-и-записать блок констант draw `R+0xda18`, флаг `R+0xde1c` | **VERIFIED + INTEGRATED** (R5-E) — в игре verify (`recomp_11e5d0`): 9,0 млн вызовов, 0 расхождений | `draw_constants_set`, `src/native_engine/reconstructed/gbuffer_draw_constants.*` | `NATIVE_RECOMP_R5_E_GBUFFER.md` |
| `0x451700` | 0xa0 | ShadowInstanceUv — атлас-UV теневого экземпляра (float2 в `out+0x40`), лист | **VERIFIED + INTEGRATED** (R5-F) — в игре verify 3 269 990 сверок, native 3 401 160, guarded 2 102 913 (8 212 сверено), 0 расхождений; `recomp_451700` verify 3,0 млн — 0 | `shadow_instance_uv`, `src/native_engine/reconstructed/shadow_instance_uv.*` | `NATIVE_RECOMP_R5_F_SHADOW.md` |
| `0x14ad0` | 0x1184 | CUE::Commit — тень ресурсов стадий → снимки → user data | **VERIFIED + INTEGRATED** (R5-D, только `recomp_14ad0`, ставится лишь по имени) — в игре verify 174 830 сверено из 44,8 млн, native 39,6 млн, guarded 46,4 млн (181 108 сверено), 0 расхождений | `cue_commit`, `src/native_engine/reconstructed/cue_commit.*` (сверка с записью 18 вызовов) | `NATIVE_RECOMP_R5_D.md` §5 |

Ранее (P5.1, `docs/native-reconstruction/JOB_SYSTEM_RECONSTRUCTION.md`, своя система установки, не этот реестр):
`0x243f30` NextJob, `0x243d70` TryNextJob, `0x243c50` WaitBatch — INTEGRATED (`SSPC_NATIVE_JOBS=1`), сверка — тесты
и A/B, без побитового verify в игре.

Машинно переведённые (статическая рекомпиляция, §9 и `docs/NATIVE_RECOMP_COMPILER.md`; код не в репозитории):

| Адрес | Статус | Офлайн [E] | В игре [E, 20.10.2026, город, пресет CITY-OPT Phase 6] |
|---|---|---|---|
| `0x11c890` | **GAME_VERIFIED** (R5) | 1 000 000 случаев + по 250 000 в MXCSR `0x9fc0/0x3f80/0x5f80/0x7f80` — 0 расхождений; гостеподобный вызывающий — 0 нарушений ABI; `--lift-check` 260 инструкций | `recomp_11c890` как реализация `object_constants`: verify — 20 428 181 вызов сверен, 0 расхождений, fallbacks 0; native — 21 619 460 вызовов, fallbacks 0, fatal 0; guarded — 21 819 419 вызовов, 85 230 сверено выборочно, 0 расхождений, fallbacks 0 (`NATIVE_ENGINE_RECONSTRUCTION.md` §9.1) |
| `0x1f0870` | GAME_VERIFIED (verify) | 1 000 000 случаев, 6×100 000 под конкуренцией | verify: 24 420 099 вызовов, 0 расхождений |
| `0x11cde0` | OFFLINE_VERIFIED (R5) | 100 000 случаев против eboot, 0 различий (память + вызовы) | не ставится (нет реконструкции-хозяина в реестре) |
| `0x120280` | OFFLINE_VERIFIED (R5) | 100 000 случаев, вызовы `0x11be90`/`0x9843a0`/`0x11bed0` и хвостовой вызов записаны — 0 различий | не ставится |
| `0x16aa0`, `0x16b70` | **GAME_VERIFIED** (R5-C) | по 100 000 случаев против eboot (вызовы `0x626520` записаны) — 0 различий; self-check и 5 режимов через установленный вход — 0 | `recomp_16aa0` / `recomp_16b70` как реализации `cue_set_*` (`recomp3`): verify — 51 424 654 / 62 188 157 вызовов, 0 расхождений; native — 48,4 / 57,5 млн без fallback; guarded — 182 656 / 217 658 сверено, 0 расхождений (`NATIVE_RECOMP_CUE.md`) |
| `0x14ad0` | **GAME_VERIFIED** (R5-D) | 20 000 случаев против eboot, 18 вызовов с побочными эффектами записаны — 0 различий; self-check установки 300 CUE (в т.ч. с неперемещённым GOT) | `recomp_14ad0` (`recomp7`): verify — 174 830 сверено, native — 39,6 млн, guarded — 181 108 сверено; 0 расхождений, fallbacks 0 (`NATIVE_RECOMP_R5_D.md` §4.2) |
| `0x169d0`, `0x16d10`, `0x16eb0`, `0x448640` | GAME_VERIFIED (verify, R5-D) | по 100 000 случаев против eboot — 0 различий | `recomp_*` (`recomp7`): 55,1 / 33,7 / 11,1 / 14,7 млн — 0 |
| `0x11e5d0` | GAME_VERIFIED (verify, R5-E) | тесты R5-E 21/21 | `recomp_11e5d0`: 9,0 млн — 0 |
| `0x451700` | GAME_VERIFIED (verify, R5-F) | 100 000 случаев против eboot; `--lift-check` с IDIV (исправлен #DE в генераторе) | `recomp_451700`: 3,0 млн — 0 |
| `0x61e260`, `0x61f430` | OFFLINE_VERIFIED | по 1 000 000 случаев | не ставятся |

Все 654 поднятые по отдельности инструкции восьми функций — `--lift-check`, 1000 случайных состояний × 5 режимов
MXCSR, 0 расхождений (после двух поправок SLEIGH, `NATIVE_RECOMP_COMPILER.md` §2).

## Проанализированные кандидаты (ANALYZED, не реконструированы)

Стоимость каждой в игре (вызовов и мкс на кадр, `SSPC_NATIVE_ENGINE_MEASURE`, 20.10.2026) — `NATIVE_ENGINE_RECONSTRUCTION.md` §3.

| Адрес | Размер | Что [A] | Почему не пилот |
|---|---|---|---|
| `0x1a2030` | 0x1651 | цикл прохода мешей (отсечение, `0x11c890`, материал, draw) | 5,7 КБ, 26 вызываемых функций, CUE-состояние |
| `0x1a1960` | 0x225 | draw одного меша (VB через CUE `0x16aa0`, примитив, индексы, commit, drawIndex) | на входе стоит зонд CITY-OPT (`draw_runs`); почти вся работа — вызовы CUE/Gnm |
| `0x1a1770` | 0x1e4 | G-буфер, draw экземпляров (константы `0x120280`, V# `0x61e260`) | то же: оркестратор вызовов CUE |
| `0x43d590` | 0xdb9 | цикл теней (группировка экземпляров, матрицы, `0x43d480`) | 3,5 КБ, 16 вызовов, стековые структуры; у R5-F — BLOCKED_BY_ABI (`and rsp, -32`), выравнивание кадра генератор теперь поддерживает, перевод не делался (`NATIVE_RECOMP_R5_F_SHADOW.md`) |
| `0x43d480` | 0x106 | теневой draw экземпляров | зонд CITY-OPT; рекомпилятор — SUPPORTED; путь — установка под пробой, как у `0x448640` (R5-F) |
| `0x448640`, `0x14ad0` | — | реконструированы в R5-D (таблицы выше) | — |
| `0x61e260` | 0xce | Gnm `Buffer::initAsRegularBuffer` (V#: адрес, шаг, число; формат 32 float) | библиотека Sony (Gnm), а не движок; используется в статической рекомпиляции (§9) |
| `0x61f430` | 0x33 | Gnm упаковка DataFormat (6 полей в u32) | то же |
| `0x623c60`, `0x623740`, `0x624f80`, `0x6250b0` | 0x4c–0x7e | Gnm drawIndex / тип примитива / тип индексов / NUM_INSTANCES (PM4 в DCB, колбэк при нехватке места) | библиотека Gnm; колбэк `[rbx+0x18]` — вызов кода движка из реконструкции |
| `0x11cde0` | 0x459 | вариант `0x11c890` (тот же блок констант) | следующий кандидат той же формы |
| `0x120280` | 0x9d | константы draw (3 вызова) | не измерено отдельно от вызываемых |
