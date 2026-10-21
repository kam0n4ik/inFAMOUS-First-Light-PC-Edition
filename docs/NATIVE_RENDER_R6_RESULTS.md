# SecondSonPC R6 — Native Render Engine: итоги

Ветка интеграции `feature/r6-core` (worktree `E:\SecondSonPC-native-r5`), без merge в main и без push. В неё слиты
`feature/r6-gbuffer` (R6-G), `feature/r6-shadows` (R6-S) и `feature/r6-cp-optimization` (R6-P). База сравнения —
пресет CITY-OPT Phase 6 (стенд: `SSPC_CP_MODE=native`, `REA_MERGE_RUNS=1`, `CP_ROUND_NODRAIN=1`,
`P411_ASYNC_ACQUIRE=1`, `ASYNC_PIPELINES=1`, `CITY_PERFORMANCE=1`).

Статусы: **ANALYZED** — разобрано по коду; **GENERATED** — код получен переводчиком; **OFFLINE_VERIFIED** — сверка с
кодом eboot вне игры; **GAME_VERIFIED** — двухпутевая сверка в игре, 0 расхождений; **PERFORMANCE_VERIFIED** — прирост
вне разброса в чередующейся серии. Все флаги R6 по умолчанию **выключены**; оригинальные пути не удалены.

## 1. Сводка по направлениям

| Направление | Что сделано | Статус | Документ |
|---|---|---|---|
| R6-Core B1–B4: мост P5.3 hybrid в городе поверх Phase 6 | двухпутевая сверка 27,27 млн draw — 0 расхождений; A/B: −4,4 FPS, поток 1 +6,9 мс (поиск подготовки по описанию в `shader_frontend.cpp`, зона кэша шейдеров — не трогалась) | GAME_VERIFIED; производительность **отрицательная** | CORE §5 |
| R6-Core B2: переиспользование снимков тени в commit CUE 0x14ad0 (`SSPC_R6_CUE_DEDUP=1`) | правило по байтам и эпохе куска аллокатора; офлайн 1000×3 commit = eboot; игра: 31,4 млн commit, память стадий 20,7 млн попаданий — 0 различий | GAME_VERIFIED; скорость — §2 | CORE §6 |
| R6-G: draw геометрии 0x1a1770 / 0x1a1960 | ручная реконструкция; verify в городе 6,28 / 21,27 млн — 0 расхождений, guarded без откатов; recomp — офлайн | ручные GAME_VERIFIED, recomp OFFLINE_VERIFIED; 0x1a2030 ANALYZED/GENERATED | GBUFFER |
| R6-S: теневой цикл 0x43d590, draw 0x43d480, листья 0x612110/0x612150/0x451290/0x450460, UV 0x451700 | записывающий клон, 3,75 млн вызовов цикла — 0 различий; отрицательные контроли ловятся; 0x451700 в игре 4,4 млн — 0 | OFFLINE_VERIFIED (0x451700 GAME_VERIFIED); ускорений нет (S2 отклонён) | SHADOWS |
| R6-P: CPU / CP | `SSPC_R6_CP_POLL_TRIVIAL=1`: +3,97 FPS, 1 % low +4,9 (s2), повтор в s3; эквивалентность — verify + Validation. PREFETCH=skip и пул — в шуме, отклонены | POLL_TRIVIAL PERFORMANCE_VERIFIED (город); прочее отклонено | CP_OPTIMIZATION |

Слитое дерево: офлайн-пакет `sspc_native_engine_tests` с DLL R6-S (`E:\research\r6\shadows\recomp\recomp.dll`) —
**279 проверок, 0 ошибок** (с DLL recomp7 — 6 ошибок «not in recomp.dll»: в ней нет экспортов теневого цикла).

## 2. Итоговые A/B

(заполняется: серия `r6c_dd2` поверх `SSPC_R6_CP_POLL_TRIVIAL=1`; сцены.)

## 3. Отрицательные и неподтверждённые результаты

- P5.3 hybrid в городе: −4,4 FPS (CORE §5.4). Не включать.
- `r6c_dd1` (переиспользование без T): +3,57 FPS у B, но серия смешана со скрытым фактором опросов WAIT_REG_MEM —
  не подтверждено (CORE §6.2).
- R6-P: `PREFETCH=skip` +0,05, пул +0,37 FPS поверх T — внутри разброса.
- R6-S S2 (batching, кэш материала, матрицы, каскады) — потенциала нет, не мерилось.

## 4. Решения за пользователем

- Включить `SSPC_R6_CP_POLL_TRIVIAL=1` в пресет города (сейчас выкл.).
- Merge `feature/r6-core` в main и push — не делались.
