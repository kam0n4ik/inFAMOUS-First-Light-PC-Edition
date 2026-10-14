# Gnm / Gnmx в eboot — реконструкция (P5.2A, начало)

Дата: 2026-10-14. Адреса — смещения eboot (Ghidra, база 0). Имена — **наши** по поведению; символов нет.
[A] анализ (дизассемблер), [E] эксперимент, [H] гипотеза, [?] неизвестно.
Скрипты: `tools/native-reconstruction/ghidra/` (запросы — разовые фрагменты для `run.py`).

## 1. Где графический код

- Все 85 импортов `sceGnm*` вызываются только из блока **`0x622000–0x63e000`**, каждый из 1–2 обёрток [A]
  (draw: `0x623810…0x6245c0`, установка шейдеров: `0x625100…0x625aa0`, маркеры `0x6275b0…0x6282e0`,
  `InsertWaitFlipDone` `0x628640`; вторая копия для вычислительных буферов `0x63c480…0x63dcc0`; отправка и флип —
  `0x1f1e30`; тонкие обёртки драйвера `0x63b790…0x63bd70`).
- Пакеты PM4 type-3 как непосредственные значения: 310 функций; `SET_UCONFIG_REG` 213 мест, `SET_CONTEXT_REG` 92,
  `NOP` 323, `DMA_DATA` 47, `SET_SH_REG` 25 и др. [A]. Методы записи регистров контекста — в `0x6221a0…0x628c10`,
  по одной функции на регистр или группу (таблица §3).

## 2. CommandBuffer (структура совпадает с `Gnm::CommandBuffer` SDK)

| Смещение | Поле | Доказательство |
|---|---|---|
| +0x00 | начало буфера [H] | — |
| +0x08 | конец (`m_endptr`) | `(end - cmdptr) >> 2 < N` перед записью [A] |
| +0x10 | указатель записи (`m_cmdptr`) | запись пакетов и `+= bytes` [A] |
| +0x18 | колбэк резервирования `bool (*)(cb, dwords, user)` | вызывается при нехватке места [A] |
| +0x20 | данные колбэка | третий аргумент колбэка [A] |

## 3. Подтверждённые функции записи состояния [A]

| Адрес | Наше имя | Аргументы (SysV) | PM4 | Уверенность |
|---|---|---|---|---|
| `0x6266f0` | setViewport | cb, id(ESI), zmin(XMM0), zmax(XMM1), scale[3](RDX), offset[3](RCX) | SET_CONTEXT_REG `0xB4+2id` {zmin,zmax}; SET_CONTEXT_REG `0x10F+6id` {xs,xo,ys,yo,zs,zo}; 12 dword | высокая |
| `0x011b60` | setupScreenViewport | cb, left, top, right, bottom, zScale, zOffset | вызывает setViewport(0), `0x626a80` (PA_CL_VTE_CNTL `0x206`), `0x626890` (screen scissor), `0x622e60` (`0x8D` HW screen offset), `0x622ee0` (guard band `0x2FA`) | высокая |
| `0x626890` | setScreenScissor | cb, l, t, r, b | `0x0C` ×2 (PA_SC_SCREEN_SCISSOR_TL/BR) | высокая |
| `0x626920` | setWindowScissor | cb, … | `0x81` ×2 | высокая |
| `0x6269d0` | setGenericScissor | cb, … | `0x90` ×2 | высокая |
| `0x6267d0` | setViewportScissor [H] | cb, id, … | вычисляемый регистр ×2 (`0x94+2id` [H]) | средняя |
| `0x622d30` | setDepthBoundsRange | cb, min, max (float) | `0x08` ×2 | высокая |
| `0x622da0` / `0x622e00` | depth / stencil clear value | cb, value | `0x0B` / `0x0A` | высокая |
| `0x622910` | setBlendColor | cb, r, g, b, a | `0x105` ×4 | высокая |
| `0x6229a0` / `0x622a00` | stencil ref/mask | cb, … | `0x10C` ×2 | высокая |
| `0x622bb0`, `0x622cd0` | DB_DEPTH_CONTROL | cb, value | `0x200` | высокая |
| `0x622c10` | DB_STENCIL_CONTROL | cb, value | `0x10B` | высокая |
| `0x622b30` | CB_COLOR_CONTROL | cb, … | `0x202` | высокая |
| `0x626e10` / `0x6270e0` / `0x626a80` | PA_CL_CLIP_CNTL / PA_SU_SC_MODE_CNTL / PA_CL_VTE_CNTL | cb, value | `0x204` / `0x205` / `0x206` | высокая |
| `0x627280` / `0x627350` / `0x6273c0` / `0x6272e0` | polygon offset clamp / front / back / format | cb, … | `0x2DF` / `0x2E0` ×2 / `0x2E2` ×2 / `0x2DE` | высокая |
| `0x627140` / `0x6271a0` / `0x627210` | point size / point minmax / line | cb, … | `0x280` / `0x281` / `0x282` | высокая |
| `0x622330`, `0x624cb0` | запись диапазона регистров (заголовок собирается из числа) | cb, reg, values, n [H] | `0xC0006900 \| (n-1)<<16` | средняя |

Вызовы: `setViewport` — только из `setupScreenViewport` (`0x11c0f`) и по ссылке-точке входа; `setGenericScissor` — из
`0x439ab0`, `0x439c90`; `setWindowScissor` — из `0x11f910`, `0x11fe80`, `0x11ff00`; общий писатель диапазона
`0x624cb0` — из `0x5b40`, `0xcd40`, `0x140d0`, `0x14ad0` [A].

### 3.1 User data, ресурсы, предвыборка, commit [A]

| Адрес | Наше имя | Аргументы | PM4 / действие | Уверенность |
|---|---|---|---|---|
| `0x626520` | setUserDataRegion | cb, stage, sgpr, values*, count (≤16) | `NOP 0x6875000D` (маркер) + `SET_SH_REG USER_DATA_<stage>+sgpr` × count; базы стадий — таблица `0xaf0070` | высокая |
| `0x626400` | setPointerInUserData | cb, stage, sgpr, u64 ptr | `SET_SH_REG` × 2 (указатель) | высокая |
| `0x623630` | prefetchIntoL2 | cb, addr, size | `DMA_DATA` в `CP_DMA_READ_TAGS` (0x3022C) — CP SecondSonPC распознаёт как предвыборку без записи | высокая |
| `0x0123d0` | allocateEmbedded (линейный аллокатор контекста) | ctx, dwords | блок из `ctx+0x20…0x30`, при нехватке — колбэк `ctx+0x00` | высокая |
| `0x014ad0` | commit перед draw (Constant-Update-Engine-подобный) | ctx | смена шейдеров по флагам; тень ресурсов стадии → блок → указатели в user data; предвыборка кода | высокая (поведение), имена [H] |
| `0x0140d0` | commit перед dispatch | ctx | то же для CS (`0x63c…` — вычислительные методы) | средняя |
| `0x1f1e30` | SubmitFrame | frame, flip | EOP-метка кадра, ожидание прошлого кадра, `SubmitAndFlip`, переключение 5 пар буферов | высокая |

Подробная раскладка контекста и кадра — GFX_CONTEXT_LAYOUT.md.

## 4. Отложенный кандидат: viewport как нативная операция

Замена `setViewport` записью в командный буфер пакета `NOP` (12 dword, как оригинал) с сигнатурой и данными операции,
которую CP применяет в том же месте потока (те же `RegisterFile::Write` + `ExtentHint`), сохраняет порядок по
построению. **Отложено** (решение 2026-10-14): CP делает ту же работу (запись регистров), функция вызывается единицы
раз за кадр, весь разбор PM4 стоит ~3,2 мс/кадр (P52_PERFORMANCE.md §1.4) — выигрыша нет; годится только как проверка
механизма прямого пути.

### 3.2 Дополнено в P5.2B [A]

| Адрес | Наше имя | PM4 |
|---|---|---|
| `0x625e80` | acquireMem | `0xC0055800` ACQUIRE_MEM, 7 dword: `CP_COHER_CNTL`, размер `0xFFFFFFFF`, база, опрос 10 |
| `0x627aa0` | writeEventEop | `0xC0044700` EVENT_WRITE_EOP: событие, индекс 5, адрес, 32/64-битные данные |
| `0x626150` | allocateFromCommandBuffer | `NOP` (`0xC0001000 \| n<<16`, `0x6875000 \| align<<12`) + данные внутри буфера |
| `0x623740` | setPrimitiveType | `SET_UCONFIG_REG 0x242` (VGT_PRIMITIVE_TYPE) |
| `0x624f80` | setIndexSize | op `0x2A` INDEX_TYPE |
| `0x6250b0` | setNumInstances | op `0x2F` NUM_INSTANCES |
| `0x6227e0` | setRenderTargetMask | ctx reg `0x8E` CB_TARGET_MASK |
| `0x626c60` | setDbRenderControl | ctx reg `0x000` DB_RENDER_CONTROL |
| `0x6286a0` | вставка NOP 63 dword | `0xC03E1000` |

## 5. Что дальше по реконструкции

Пункты 1–3 закрыты в RENDER_ENGINE_ARCHITECTURE.md (P5.2B). Ниже — исходный план:

1. Объект контекста, владеющий CommandBuffer (GfxContext-подобный): вызывающие методов §3, состояние между draw.
2. Draw-обёртки `0x623810…` и подготовка user data/таблиц ресурсов перед draw (связь с тем, что фронтенд SecondSonPC
   читает через SRT — главный потребитель CP).
3. Отправка кадра `0x1f1e30` и ожидание GPU (главный поток ждёт здесь 42–58 % времени).
