# Тайловые GPU (Mali / PowerVR): что стоит DOOM и что внедрено (06.11, issue #7)

Повод: POCO M6 Pro (Helio G99, Mali-G57 MC2, Android 16, v0.4.14), картинка верная, `2 fps 576 ms (max 820) GPU 388.2`.
Лога с устройства нет; всё ниже — чтение кода, `NVN GPASS` с Adreno (`work/logs/play/r4_ab`, 1600x720, кадр 11701) и
документы Arm / IMG / Mesa. Код: `xbg_vendor_policy.h` (умолчания, геометрия прямоугольников), `xbg_game.cpp` (метки `[tile]`),
тест `engine/tests/vendor_policy_test.cpp`. Бэкапы: `work/bak_tile/`. Сборка: `work/build-android-tile`.

## 0. Источники
- Arm GPU Best Practices Developer Guide 101897, issue 10 (PDF: https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695):
  §3.9 «Vulkan pipeline synchronization» (не использовать `ALL_COMMANDS→ALL_COMMANDS` / `ALL_GRAPHICS→ALL_GRAPHICS`, между
  проходами `ALL_GRAPHICS→FRAGMENT_SHADER`, обратные зависимости fragment→vertex/compute создают пузыри); §7.3 «Efficient render
  passes» (LOAD только если нужны старые данные, DONT_CARE/CLEAR, `vkCmdClearAttachments` не бесплатен, STORE_OP_DONT_CARE для
  ненужного); §6.1/6.2 (тесселяция и GS пишут геометрию в память — избегать); §5.7 (varyings идут через память: mediump);
  §4.4 (lazily allocated — только для TRANSIENT + STORE_OP_DONT_CARE); §9.2 / §10.4 (subgroup ≤ 16, Valhall — 16 потоков в варпе).
- Arm blog «Picking the most efficient load/store operations»: на Mali DONT_CARE = CLEAR (бесплатно), LOAD стоит полосы.
- Vulkan-Samples `render_passes`, `pipeline_barriers` (−13 % времени кадра с dst = FRAGMENT).
- IMG: «Using Render Passes and Sub-passes», «Optimising Pipelines», «Geometry Shaders and Tessellation Shaders»
  (docs.imgtec.com/performance-guides/graphics-recommendations): то же — CLEAR/DONT_CARE, меньше проходов, GS избегать.
- Mesa PanVK: `panvk_vX_cmd_draw.c` (renderArea → `fb.layout.tiling_area_px`), `jm/panvk_vX_cmd_buffer.c` (`bound_min/max_x/y =
  tiling_area_px >> MALI_TILE_SHIFT` — фрагментная задача обходит только тайлы области), `GetRenderAreaGranularity` = 32x32.
  Mesa pvr: `pvr_arch_cmd_buffer.c` `pvr_is_render_area_tile_aligned` — неполная область при store depth заставляет **грузить** depth.
  Что делает проприетарный драйвер Arm — не документировано (Mesa — только механизм).

## 1. Что стоит кадр (оценка)
- Adreno: 81 проход + 3 compute на кадр, 14.9 мс GPU. Главные: G-буфер 4 вложения 1600x720 (4.0 мс, 568k вершин), атлас теней
  4096² D16 (1.3 мс, 960k вершин) + 4 мелких прохода по атласу (копия статического кэша в динамические тайлы), depth pre-pass
  (0.6 мс, 763k вершин), ~50 полноэкранных проходов пост-обработки (1 треугольник, `vs 4489ea82e08e6387`).
- G57 MC2 (~1.1 ГГц) по FP32 в ~20 раз слабее Adreno 750, по полосе LPDDR4X ~4 раза: 15 мс × 20 ≈ 300 мс. 388 мс — лишь ~1.3x
  сверх пропорции. Значит, основа — ALU/текстуры при docked-пресете, а не патология; главный рычаг — handheld «лёгкий режим» и
  разрешение. TBDR-надбавка (загрузки/выгрузки, пузыри барьеров, 2.5 M вершин через память IDVS) — оценочно 10–25 %.
- TBDR-специфика в нашем движке:
  - каждый проход начинался с `LOAD` всех вложений и `STORE` всех, на весь framebuffer (renderArea = весь кадр). Атлас 4096² D16 —
    32 МБ чтения + 32 МБ записи **на проход**, 5 проходов ≈ 320 МБ/кадр, даже если draw трогает один тайл;
  - ~50 полноэкранных пост-проходов грузили старое содержимое, которое тут же перекрывается (noload был мёртв: `first_draw_covers`
    отказывал при `DM_GFX_TAA_FIX=2`, т.е. всегда в DOOM);
  - все барьеры `ALL_COMMANDS→ALL_COMMANDS`, зависимости прохода `ALL_COMMANDS→ALL_GRAPHICS` и `ALL_GRAPHICS→ALL_COMMANDS`:
    вершины следующего прохода ждут фрагменты предыдущего — ровно то, что Arm §3.9 называет полным осушением конвейера;
  - разрез чанка каждые 32 draw мог закрыть открытый проход (на тайлере = store + reload всех вложений).

## 2. Что не было проблемой (проверено)
- **GS:** `NVN PERF perf3 gs_passthru skipped≈160–440 kept=0` — culling-GS DOOM при `r_geometryCulling 0` снимается на всех GPU.
  Остаётся только при `r_geometryCulling 1`. Отдельно для Mali ничего не нужно (`nogs` — для драйверов без GS).
- **lone-lane:** `xbg_spirv_lone.cpp` не создаёт циклов — каждая инвокация становится варпом из одной дорожки (ballot = свой бит,
  shuffle = своё значение). Цена — лишь потеря кооперации (атомик на инвокацию вместо варпа). Затронуты 6 GS (сняты pass-through)
  и `cec755c4` (частицы, compute; на Adreno тоже lone из-за краша компилятора). Compute в кадре — 0.2–0.8 мс на Adreno: эмуляция
  32 на 16 (shared memory) не окупится, не делалась.
- **Occlusion-запросы:** копируются `vkCmdCopyQueryPoolResults` в конце слота и читаются при ретайре — без синхронного ожидания.
- **Zero-copy:** на Mali уже выключен (`xbg_zero_copy_default`). Гостевые `OP_BARRIER` по умолчанию пропускаются.

## 3. Внедрено (умолчания по семейству; на Adreno всё выключено)
| Переключатель | Mali | PowerVR | Остальные | Что |
|---|---|---|---|---|
| `DM_GFX_NOLOAD` | 1 | 1 | 0 | DONT_CARE вместо LOAD, если первый draw доказанно перекрывает все цветовые пиксели. lower-left: покрытие в NDC от начала координат не зависит; при culling — требуется покрытие при обеих ориентациях |
| `DM_GFX_TILE_AREA` | 1 | 0 | 0 | renderArea = габарит draw прохода (`tile_scan` вперёд по командам гостя: через JUMP/CALL/конец CALL/следующие хэндлы сабмита, до draw в другие цели / dispatch / копии; ≤ 4096 команд; ранний выход при ≥ 90 % кадра). Каждый draw и clear проверяется (`tile_draw_rect`: scissor, для треугольников без GS/тесселяции ∩ viewport ±1 px); вне области → проход закрывается, новый. Выравнивание по `vkGetRenderAreaGranularity`; ≥ 90 % → весь кадр. lazy-clear / noload / poison — всегда весь кадр |
| `DM_GFX_TILE_BARRIER` | 1 | 1 | 0 | `dep[0]` dst и `dep[1]` dst — только фрагментные стадии (+ transfer у `dep[1]`); барьер чтения draw без картинок в VS/TCS/TES/GS — `ALL_COMMANDS→fragment` (`frag_seq`, `dirty_begin_f`); чтения из до-фрагментных стадий, `pending_view`, `Commands` — по-прежнему полный барьер; draw с записью storage в до-фрагментных стадиях после закрытого прохода — полный барьер (WAR) |
| `DM_GFX_CHUNK_MID_SPLIT` | 0 | 0 | 1 | разрез чанка «каждые N draw» ждёт момента без открытого прохода |
| `DM_GFX_TILE_POISON` | 0 | 0 | 0 | отладка: noload-двойник чистит в пурпурный |

Диагностика: `NVN tile policy: area= barrier= chunk_mid_split= poison= noload default=` при старте; `NVN GPASS … area=x,y:WxH ld=`
(L load, N DONT_CARE, P poison, C lazy-clear двойник, A clear-attachments); `NVN PERF tile area_passes= area_px=% splits=
scan_ops= scan_cut= frag_barriers= war_barriers= poisoned= chunk_kept_open= gran=`; `NVN PERF noload_passes_per_frame= … why=`.

Почему Adreno бит-в-бит: все пять флагов на `0x5143` дают прежние значения (`vendor_policy_test`); при выключенных флагах новые
ветки не исполняются, `begin_pass` без аргументов собирает тот же `VkRenderPassBeginInfo`, зависимости и барьеры те же; добавлены
только переменные учёта (`frag_seq`, `exec_pc`, …) и поля в строке GPASS.

## 4. Не сделано
- **storeOp DONT_CARE** (depth после последнего использования, части G-буфера). В Vulkan storeOp фиксируется при begin, а конец
  прохода движок узнаёт позже (разрывы чтением, чанки); Metal-сторона (`nvn_mtl.m` sdx) решает в конце энкодера — в Vulkan так нельзя.
  Можно было бы патчить ещё не опубликованный пакет `K_BEGIN_RP` в кольце `vkrec`, но поток записи исполняет пакеты сразу. Плюс DOOM
  читает почти все свои цели позже. `nvnCommandBufferDiscardColor/DiscardDepthStencil` в `hle_nvn.c` — пустышки; имена в `main` есть
  (игра их резолвит), вызывает ли — неизвестно: `DM_NVN_FRAMES=100000` печатает счётчики функций за каждый кадр.
- **TRANSIENT / LAZILY_ALLOCATED:** все цели — гостевые текстуры в памяти NVN, любая может быть прочитана.
- **mediump varyings** (Arm §5.7): меняет точность (fp16 у UV атласа 4096 — 1/2048), без устройства не проверить.
- **PowerVR `TILE_AREA`:** по Mesa — неполная область даёт лишнюю загрузку depth; включать после лога с PowerVR.

## 5. Проверка на Adreno (Mi 13 / Mi 14), `dm/env`
1. Без переменных: `NVN tile policy: area=0 barrier=0 chunk_mid_split=1 poison=0 noload default=0`, строки `NVN PERF tile` нет,
   fps как в v0.4.14, в GPASS `area=0,0:<весь кадр> ld=L`.
2. `DM_GFX_SIM_GPU=mali`: `area=1 barrier=1 chunk_mid_split=0 noload default=1`. Смотреть: нет мерцания/гонок (барьеры),
   `splits` ≈ 0, `area_px` < 100 % (если атлас теней обновляется тайлами), `frag_barriers` ≈ число разрывов чтением,
   `war_barriers` ≈ 0, `noload_passes_per_frame` > 0 и `why=` (что отказало).
3. Корректность там, где sysmem её прячет: `DM_GFX_NOLOAD=1 DM_GFX_TILE_POISON=1` — ни одного пурпурного пятна;
   `DM_GFX_SIM_GPU=mali TU_DEBUG=gmem` — в GMEM Turnip правда пропускает загрузку DONT_CARE и пишет только область: тени,
   декали, UI на месте. По отдельности: `DM_GFX_TILE_AREA=1`, `DM_GFX_TILE_BARRIER=1`, `DM_GFX_CHUNK_MID_SPLIT=0`.
4. Если есть APK с валидацией: `DM_VK_VALIDATION=1` + syncval при `DM_GFX_TILE_BARRIER=1` — без новых hazard'ов.

## 6. Что смотреть в логе Mali-пользователя (debug APK с `DM_GFX_PASS_PROF`)
- `NVN gpu family=mali`, `NVN tile policy: area=1 barrier=1 …`, `NVN fallbacks on this device: …`.
- `tools/gpass_report.py`: какие проходы дороже всего; сумма `us` по `kind=pass` против `GPU …` оверлея; ширина/высота
  проходов (рендерит ли игра больше 1600x720 на 2400x1080); проходы `4096x4096` и их `area=` (сработала ли область, и сколько
  стоят до/после — A/B `DM_GFX_TILE_AREA=0`); доля `ld=N`.
- `NVN PERF tile …`, `noload_passes_per_frame … why=`, `NVN PERF chunk … cuts mid=`, `pass_end_lines` (сколько разрывов на кадр).
- Если одна правка не дала ничего — `=0` по одной (A/B) и сравнить `GPU` мс; для итогового вывода нужен прогон в handheld-пресете.
