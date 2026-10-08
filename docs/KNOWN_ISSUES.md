# Известные проблемы SecondSonPC

## SSPC-001 — непустое SystemName → падение после записи настроек (Phase 0, открыто)

**Симптом.** Если `sceSystemServiceParamGetString(6 /*SystemName*/)` возвращает непустую строку,
игра через ~20–30 с (после заставок, во время загрузки `sh-world_village(sunbreak)` и записи
`/savedata0/dis.dat` с флагами `0x601`) падает. Сбои всегда разные: `ErrorOutOfDeviceMemory` в
`video_core/buffer_cache/buffer.cpp:105`, access violation в потоке `GpuCommandProcessor`, C++-исключение
там же, переход `Game:Main` по мусорному адресу (обработчик shadPS4 затем падает в `ZydisInputPeek`).

**Бисекция (2026-10-08, `logs\runs\diag_*`).**

| Прогон | Экспорты SecondSonPC | Результат |
|---|---|---|
| ru_first, ru_second, diag_en | все 4 | падение (175–234 пайплайна) |
| diag_nocounter | все 4, без счётчика | падение — счётчик ни при чём |
| diag_stock | штатный `Emulator::Run` в нашем бинарнике | ок |
| diag_noexports | нет | ок, 250 пайплайнов |
| diag_observers_only | `sceKernelOpen/Stat` (наблюдатели) | ок, 508 пайплайнов |
| diag_paramstring_only | всё, кроме `ParamGetInt` | **падение** |

`ParamGetInt` в этих прогонах возвращает игре то же, что shadPS4 (вызывается только с `Lang`), поэтому
единственное поведенческое отличие — непустой `SystemName`.

**Что известно.** Игра (`0x15d6ee`) копирует строку в объект настроек по адресу `0xE17170 + 0x70`
(refcounted string через `0x60d8d0`), только если первый байт не 0. Заглушка shadPS4 буфер не заполняет,
и фактически игра получает пустое имя. На реальной PS4 имя всегда непустое (например, `PS4-123`), то
есть игра это поддерживает — проблема в связке Phase 0 (bridge shadPS4 или наш слой), а не в игре.

**Временное решение.** `[System] name = ""` по умолчанию: `ParamGetString` по-прежнему корректно
заполняет буфер (строка `""` с завершающим нулём, проверка размера), но игра идёт по тому же пути, что и
в эталоне. Непустое имя можно задать в `SecondSonPC.toml`, при этом в лог пишется предупреждение.

**Что сделать.** Найти потребителей поля `+0x70` (динамически: watchpoint на запись/чтение
`0xE171E0`), понять, где путь расходится, и устранить причину до Phase 3 (SaveData), так как она
связана с записью `dis.dat`.

**Воспроизведение.** `powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\repro_sspc001.ps1
[-Loader sspc|shadps4] [-Seconds 90] [-Name PS4-SSPC001]` — запуск с непустым SystemName через флаг
`--system-name` (TOML не меняется); итог `REPRODUCED` / `NOT REPRODUCED`, улики в
`logs\runs\sspc001_<loader>`.

**Обновление 2026-10-08 (Phase 1): тот же сбой и при пустом SystemName.** Подпись та же:
`ErrorOutOfDeviceMemory` в `video_core` (`buffer.cpp:104`, `image.cpp:121`) или C++-исключение в
`GpuCommandProcessor`, через 17–20 с после старта, при подгрузке уровня. Проявляется на
**неизменённом пути Phase 0** (`--loader shadps4`, загрузчик и FS shadPS4): `logs\runs\or_1..3`,
`fresh_oracle` — 4 падения из 4 подряд, в том числе с пустыми сохранениями (`fresh_*`; прежние
сохранения перенесены в `userdata\savedata_backups\2026-10-08_10-04-41_before_fresh_start`). Утром
того же дня тот же путь проходил 60 с в 3 прогонах из 3 (`m1_oracle`, `m1_oracle2`, `ru_phase0`),
новый Loader — 4 падения из 9 до этой серии: частота плавает и от кода SecondSonPC не зависит.
GPU: NVIDIA RTX 2060, 6 ГБ (свободно 5,4 ГБ перед прогонами). Вывод: связь с SystemName в бисекции
Phase 0, скорее всего, случайна (малая выборка); это нехватка видеопамяти в GPU-мосте shadPS4
(Phase 4: `video_core`), а не Loader/FileSystem/SystemService.
