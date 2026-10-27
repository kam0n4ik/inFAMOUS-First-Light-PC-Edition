# inFAMOUS: First Light — анализ файлов игры

Все цифры ниже получены замерами в этой папке (`E:\inFAMOUS-First-Light-PC-Edition`). Скрипты:
- `tools\ps4elf.py` — разбор SELF/ELF, импорты по NID;
- `tools\gen_import_table.py` — таблица привязок;
- разовые скрипты на Python для `param.sfo` и `trophy00.trp`.

Данные игры только читались, ничего в них не менялось.

## 1. Идентификация (`sce_sys\param.sfo`)

| Ключ | First Light | Second Son (эталон) |
|---|---|---|
| TITLE_ID | CUSA00897 | CUSA00004 |
| CONTENT_ID | EP9000-CUSA00897_00-FIRSTLIGHTSHIP00 | EP9000-CUSA00004_00-… |
| APP_VER / VERSION | 01.00 / **01.04** (репак с обновлениями 1.01–1.04, включая HDR и 4K) | 01.07 |
| SYSTEM_VER | 0x4008000 | 0x4008000 |
| SDK в PT_SCE_PROCPARAM | 0x4008061 | 0x4008061 |
| CATEGORY / APP_TYPE | gd / 1 | gd |
| INSTALL_DIR_SAVEDATA | нет: каталог сохранений = CUSA00897 | — |
| SAVE_DATA_TRANSFER_TITLE_ID_LIST | CUSA00223, **CUSA00004**, CUSA00263, CUSA00309, CUSA00305 | — |
| SERVICE_ID_ADDCONT_ADD_3 | EP9000-CUSA00004_00 (связь с DLC Second Son) | — |
| NP comm id (`npbind.dat`) | NPWR06685_00 | — |

**Вывод.**
- Тот же SDK и та же прошивка, что у SS. Поэтому последовательность init Gnm-драйвера (`gnm_driver.cpp:337-345`) и
  поведение SaveData, зависящее от прошивки, совпадают.
- Каталоги сохранений, кэша шейдеров, temp/download и трофеев рантайм берёт из param.sfo. Они получаются свои
  (CUSA00897) и с SS не пересекаются.

## 2. Исполняемый файл (`eboot.bin`)

| | FL | SS |
|---|---|---|
| Формат | SELF, 11 сегментов, e_type 0xfe10 | SELF, 11 сегментов |
| Размер файла | 18 185 397 Б | 17 079 995 Б |
| Модуль | `ne_ship` | `ne_ship` |
| Точка входа | 0x6ffd70 | 0x61dfd0 |
| Текст (PT_LOAD 0) | 0xdba370 Б (14,39 МБ) | 0xcc55b8 Б (13,39 МБ) |
| Данные (vaddr, конец образа) | 0xe9c000, конец 0x8235d4f | 0xda0000, конец 0x7fa547f |
| Нужных модулей / библиотек импорта | 33 / 34 | 28 / 29 |
| Импортов (NID) | **510** | 466 |
| SHA-1 текста | 1672a227… | 535c1ff5… |

- Движок тот же: Sucker Punch «ne_ship», те же Havok 7.0.0, Bink и Wwise.
- Код другой: ни одна из 95 функций SS, закреплённых хешами, не совпадает по RVA.
- Функция установки разрешения (VideoOut init) найдена побайтно со сдвигом +0x46a90.

### Импорты, которых нет у SS (46)

| Модуль | Функции | Что это для игры |
|---|---|---|
| libSceJson2 (15) | `sce::Json::Initializer`, `MemAllocator`, `Parser::parse`, `Value::operator[]`, `getString` / `getInteger` и др. | Разбор JSON-ответов сетевых сервисов (Web API) |
| libSceNpScore (8) | контекст, запросы, `RecordScoreAsync`, `GetRanking*Async`, `PollAsync` | Таблицы рекордов испытаний |
| libSceNpTus (8) | контекст, запросы, `Get/SetMultiSlotVariableAAsync`, `GetMultiUserVariableAAsync` | Хранилище очков друзей |
| libSceNpWebApi (10) | `Initialize`, `CreateContextA`, `CreateRequest`, `SendRequest2`, `ReadData`, заголовки | Активности и лента PSN |
| libSceRtc (4) | `GetTime_t`, `ConvertUtcToLocalTime`, `SetTick`, `GetCurrentNetworkTick` | Время |
| libSceSaveData (1) | `sceSaveDataTransferringMount` | Чтение сохранения Second Son (CUSA00004 в списке переноса) |

Два импорта есть только у SS: `localeconv` и `malloc` из libc. Это строки LLE, их обслуживает PRX игры.

## 3. Модули (`sce_module`)

- Только `libc.prx` и `libSceFios2.prx`.
- Оба **побайтно совпадают** с модулями SS: md5 `e5686bc1…` и `377fd17a…`. Поэтому строки LLE таблицы привязок SS
  верны и для FL; генератор их переносит как есть.

## 4. Данные (`art`, 12 ГБ)

| Что | FL |
|---|---|
| `art\cache` | 11 ГБ: 536 паков `*.xpps` (у SS 1623) плюс маркеры `pgc_*_dummy_file` (PlayGo) |
| `art\cache\psarc` | 10 архивов: `english_a/b(_p)`, `russian_a/b(_p)`, `music_a/b` |
| `art\movies` | 1,4 ГБ, 55 роликов `.bsf` (Sucker Punch, как у SS), папки `cutscene`, `tv`, `ui` |
| Языки текста | `lang_british_text`, `lang_english_text`, `lang_russian_text` (+ `_p`-патчи) |
| Языки озвучки | `lang_english_audio`, `lang_russian_audio` (+ `_p`) |

- **Русский язык есть полностью: и текст, и озвучка.** В `FirstLightPC.toml` выставлено `[Language] system = "ru-RU"`
  (PS4 id 8). При этом игра открывает `lang_russian_*` и `psarc\russian_*`; имена файлов по той же схеме, что у SS.
- Ключи HDR-строки, которые правит `content_patch.cpp` (`u_options_opt_hdr`, `u_options_val_hdr_unavailable`), в
  `lang_english_text_p.xpps` FL есть. Механизм должен сработать.
- KCAP-паки (проверка парсером агента GPU, только чтение):
  - `all_shaders.xpps` FL содержит 72 программы и 163 прохода (у SS 71 и 154);
  - есть чанки KNLX, PLPS и PYTO, которых нет у SS;
  - 67 из 82 классов проходов совпадают с шаблонами `state` SS.
  
  Парсер справляется, но шаблоны SS к FL применять нельзя, пока они не сняты с самой FL.

## 5. Трофеи (`sce_sys\trophy\trophy00.trp`)

- 50 записей: `TROPCONF.ESFM`, `TROP.ESFM`, `TROP_00..`ESFM (языки) и **25 иконок `TROP000..TROP024.PNG`**, то есть
  25 трофеев с id 0..24.
- Конфигурация зашифрована (`.ESFM`). Есть ли платина и какой у неё id, по файлу пока не подтверждено.
- В рантайме платиной считается id 0, и он выдаётся автоматически, когда получены все остальные
  (`offline_services.cpp:594-605`). Проверить в TODO.
- Ключи текстов трофеев в `lang_*_text.xpps` FL имеют вид `u_trophy_fl_*`, а таблица рантайма содержит 48 ключей SS.
  Пока уведомление будет показывать «Трофей N» с правильной иконкой.

## 6. DLC и NP

- Метки DLC в eboot: `UNLOCKAPPLICATIO`, `UNLOCKCOSTUME000`. Правило рантайма «любая метка `UNLOCK*` принадлежит
  игроку» подходит без изменений.
- Хоста Paper Trail (`api.infamouspapertrail.com`) в eboot FL нет, так что этот локальный сервер не задействуется.
- Сетевые таблицы рекордов (NpScore/Tus/WebApi) недоступны: у PC Edition нет PSN. Функции честно возвращают «не выполнен
  вход» (`ORBIS_NP_ERROR_SIGNED_OUT`), а не фиктивный успех.

## 7. Что это значит для порта

1. Тот же SDK, те же Gnm-импорты и те же PRX. Ядро рантайма (загрузчик, kernel, память, GPU, звук, ввод) подходит
   без доработки игровой логики.
2. Все улучшения SS, которые сделаны **хуками движка** (меню по Esc, прямая камера мышью, разрешение и пропорции, FOV,
   TAA, качество эффектов, нативный G-buffer), на FL отключены сами. Их перенос требует новой таблицы мест (RVA + хеш)
   для eboot FL. См. PORTING_PLAN.md.
3. Содержимое игры отличается: неон, частицы, много compute. Поэтому важны проверка сортировки частиц (фикс SSPC-020
   привязан к хешу шейдера SS) и отсутствие стадий ES/GS. Это выясняется только на живом запуске.
