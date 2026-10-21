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
| R6-Core B2: переиспользование снимков тени в commit CUE 0x14ad0 (`SSPC_R6_CUE_DEDUP=1`) | правило по байтам и эпохе куска аллокатора; офлайн 1000×3 commit = eboot; игра: 31,4 млн commit, память стадий 20,7 млн попаданий — 0 различий | GAME_VERIFIED; скорость: +0,00 FPS поверх T — **REJECTED** | CORE §6 |
| R6-G: draw геометрии 0x1a1770 / 0x1a1960 | ручная реконструкция; verify в городе 6,28 / 21,27 млн — 0 расхождений, guarded без откатов; recomp — офлайн | ручные GAME_VERIFIED, recomp OFFLINE_VERIFIED; 0x1a2030 ANALYZED/GENERATED | GBUFFER |
| R6-S: теневой цикл 0x43d590, draw 0x43d480, листья 0x612110/0x612150/0x451290/0x450460, UV 0x451700 | записывающий клон, 3,75 млн вызовов цикла — 0 различий; отрицательные контроли ловятся; в игре (город, recomp, 0x43d480 под пробой CITY-OPT): verify 4,87 / 16,30 млн сверок, guarded 1/16 — 5,06 / 16,91 млн нативно, 0 расхождений и откатов; 0x451700 — 4,4 млн, 0 | recomp 0x43d590 / 0x43d480 / 0x451700 GAME_VERIFIED, листья OFFLINE_VERIFIED; ручной 0x43d590 нет; ускорений нет (S2 отклонён), FPS не мерился | SHADOWS |
| R6-P: CPU / CP | `SSPC_R6_CP_POLL_TRIVIAL=1`: +3,97 FPS, 1 % low +4,9 (s2), повтор в s3; эквивалентность — verify + Validation. PREFETCH=skip и пул — в шуме, отклонены | POLL_TRIVIAL PERFORMANCE_VERIFIED (город); прочее отклонено | CP_OPTIMIZATION |

Слитое дерево: офлайн-пакет `sspc_native_engine_tests` с DLL R6-S (`E:\research\r6\shadows\recomp\recomp.dll`) —
**279 проверок, 0 ошибок** (с DLL recomp7 — 6 ошибок «not in recomp.dll»: в ней нет экспортов теневого цикла).

## 2. Итоговые A/B

Город, пресет стенда, окно 70…148 с, 3 круга чередованием.

| Что | Серия | Результат | Статус |
|---|---|---|---|
| `SSPC_R6_CP_POLL_TRIVIAL=1` против пресета | R6-P s2 | 28,80 → 32,77 FPS (+3,97), 1 % low +4,9, CP −4,2 мс; диапазоны не пересекаются; s3 повторил 32,3–33,5 | PERFORMANCE_VERIFIED |
| нативный commit 0x14ad0 (recomp) поверх T | `r6c_dd2` | +0,52 FPS, CP −0,49 мс — внутри разброса T | не подтверждён |
| + переиспользование снимков | `r6c_dd2` | +0,00 FPS, CP −0,04 мс | REJECTED |
| P5.3 hybrid против пресета | `r6c_ab2` | −4,39 FPS, CP +6,89 мс | REJECTED |

(сцены пляж / гавань / деревня — §2.1.)

## 3. Отрицательные и неподтверждённые результаты

- P5.3 hybrid в городе: −4,4 FPS (CORE §5.4). Не включать.
- Переиспользование снимков CUE: в `r6c_dd1` (без T) +3,57 FPS — целиком скрытый фактор опросов; поверх T (`r6c_dd2`)
  +0,00 FPS. Отклонено по скорости (CORE §6.2).
- R6-P: `PREFETCH=skip` +0,05, пул +0,37 FPS поверх T — внутри разброса.
- R6-S S2 (batching, кэш материала, матрицы, каскады) — потенциала нет, не мерилось.

## 4. Решения за пользователем

- Включить `SSPC_R6_CP_POLL_TRIVIAL=1` в пресет города (сейчас выкл.).
- Merge `feature/r6-core` в main и push — не делались.
