# Что d2r-mobile / nx2android / nx2apk делали с производительностью на Android/Adreno — и что из этого нужно DOOM

Дата: 2026-10-28. Только чтение, ни один репозиторий не менялся.
Источники:
- `d2r-full` (HEAD 1e0e793): коммиты, текущие `xbg_game.cpp`, `jni_android.c`, `vulkan_present.cpp`, `hle_oe.c`, `hle_nvn.c`
  и dev-заметки `docs/android-notes/*.md` (Redmi K50 Ultra, Adreno 730). Заметки — самая ценная часть истории.
- `nx2android` (HEAD af7a8c5): `perf_android.c`, `gfx_vk_*.c`, skill `switch-android-port` (SKILL.md, neo-case-study.md, switches.md).
- `nx2apk` (HEAD d0b5267): `PLAN.md`, `nvn_vk.c`, `hle_oe.c`.

Наш порт сверялся по `src/dm` (xbg_game.cpp 4641 строк, движок почти идентичен d2r HEAD; из маркеров нет только `gfx-inline`).
Догадки помечены **[догадка]**.

---

## Ранжированные находки (по ожидаемому эффекту для GPU-bound DOOM)

### 1. Fence синка из nvnWindowAcquireTexture = «вся работа до сих пор» → кадры не перекрываются CPU/GPU
- **Источник:** nx2android `1c6177d` («Vulkan/NVN: frames overlap again»), `nxnative/dm/gfx/nvn/hle_nvn.c`; SKILL.md «Performance playbook» п.2.
- **Проблема:** sync, отданный игре из `nvnWindowAcquireTexture`, фенсился на `nvn_fence_now()` (последний сабмит). Игра (XC3), ждущая
  этот sync на CPU сразу после present кадра N, ждала **весь GPU-хвост кадра N**, и только потом начинала транслировать N+1.
  Кадр = трансляция + GPU-хвост, GPU ни разу не работал параллельно с трансляцией следующего кадра.
- **Решение:** fence = выполнение работы до **последнего present этой текстуры** (как Horizon отпускает образ swapchain).
  ```c
  // present:  o->done[idx] = nvn_fence_now();
  // acquire:
  if (s) { ((sync_t *)s)->fenced = 2;
           ((sync_t *)s)->fence = acq_now || *idx >= 4 ? nvn_fence_now() : o->done[*idx]; }   // DM_NVN_ACQ_NOW=1 = старое поведение
  ```
  В том же коммите: `mtl_sub_begin` всегда брал слот 0 и ждал предыдущий сабмит (docked 37 → 33.3 мс); сброс командных буферов на
  completion-потоке (было 7%+ потока реплея).
- **Эффект:** XC3 docked heavy views 37 → 33.3 мс; затем 30 fps удержаны в бою (p50 33.2 мс).
- **У нас:** `dm/gfx/nvn/hle_nvn.c` `nvnWindowAcquireTexture` (~стр. 1248+):
  `if (s) { ((sync_t *)s)->fenced = 1; ((sync_t *)s)->fence = x_fence(); ... }` — **ровно старый вариант**.
  Наши цифры подозрительно совпадают с «трансляция + GPU последовательно»: CPU ~10 мс + GPU 30-45 мс ≈ 40-55 мс ≈ 17-22 fps,
  GPU простаивает ~25-30% кадра. И `DM_NVN_SYNC=0` ничего не дал — так и бывает, когда сериализует сама игра через sync.
- **Что сделать:** (а) трассой проверить, ждёт ли DOOM на CPU (`nvnSyncWait`) sync от acquire или fence прошлого кадра и сколько
  (nx2android добавлял `DM_NVN_SEQ=N`: порядок submit/fence/acquire/syncwait/present со временем); (б) хранить fence последнего
  present каждого индекса swapchain и отдавать его в acquire-sync; переключатель на старое поведение. **Ожидание [догадка]:** при
  подтверждении — до −10 мс/кадр (потолок ~GPU-bound 30-45 мс → 22-33 fps), плюс вторичный выигрыш от DCVS (п.2).

### 2. GPU DCVS «lock-in»: недогруженный GPU снижает частоту, и GPU-время растёт
- **Источники:**
  - d2r `docs/android-notes/2026-10-17-frame-overlap.md`: после включения перекрытия кадров (defer-events=2 + present-overlap)
    **GPU-время упало 61 → 42 мс при тех же draw** — «likely GPU DVFS holding a higher clock». 7.7 → 19.5 fps.
  - d2r `hle_nvn.c` (pace_adapt_tick): «after stepping down the system lowers GPU/CPU clocks and load times stretch (Mate90: GPU 15 → 28 ms),
    so a load-based step-up never fires again (DVFS lock-in)» — поэтому решение о пейсинге принимается по пропущенным слотам, а не по нагрузке.
  - nx2android neo-case-study: «Pinned at 40 fps, GPU busy fell from 20.6 to 15.2 ms as the clocks rose».
  - nx2apk `d0b5267`: Samsung держит GPU на 377 МГц (thermal MODERATE, dvfs_max_lock) — GPU-время сцены ×2.
- **Вывод для нас:** 30% простоя GPU — это не только потерянные мс, но и сигнал governor'у снизить частоту; поэтому наши 30-45 мс GPU
  могут быть «раздуты». Сначала убрать сериализацию (п.1), потом мерить GPU-время. Частоту GPU на retail не прочитать
  (`/sys/class/kgsl/kgsl-3d0/gpuclk` требует root) — косвенно по GPU busy при фиксированном темпе.
- **Не делать:** пейсинг ниже реального темпа «с запасом» (растит простой → ниже частота). На Android d2r держит `DM_PACE_ADAPT` выключенным.

### 3. OEM «игровой режим» снимает клок-капы (самый большой одиночный шаг в d2r)
- **Источник:** d2r `docs/android-notes/2026-10-18-gpu-profile.md` «MIUI Game Turbo: the biggest single step».
- **Проблема:** MIUI держит CPU и GPU клоки ограниченными, пока приложение не добавлено в Game Turbo; `appCategory="game"` не хватает.
- **Эффект:** X2 cap 2.25 → 3.19 ГГц, **GPU ~28% быстрее** (17.7 → 13.8 мс), 53 → 60 fps; цена — нагрев (32.4 → 38 °C за 4 мин).
  Индикатор: `settings get secure gb_boosting` = 1 во время игры.
- **У нас:** манифест уже `appCategory="game"`, `isGame="true"`. Winlator (45-50 fps на том же телефоне), вероятно, уже в списке
  игрового режима вендора **[догадка]**. Проверить: добавлен ли наш APK в Game Turbo / Game Booster / Game Space производителя;
  сравнить `cpuinfo_max_freq`/`scaling_max_freq` и GPU busy с Winlator в той же сцене. Бесплатный тест, потенциально +20-30% GPU.

### 4. Сабмит кадра кусками, чтобы GPU стартовал раньше (альтернатива/дополнение к п.1)
- **Источник:** nx2android `gfx_vk_exec.c` (`DM_VK_CB_DRAWS`, default 128; `vk_cmd_split`, `DM_VK_SUBMIT_SPLIT`), neo-case-study:
  «submit in pieces so replay overlaps the GPU: **36.7 → 15.7 ms**».
  ```c
  // DM_VK_CB_DRAWS: the replay is cut every that many draws and what is recorded goes to the GPU at once (vk_cmd_split)
  g_cb_draws++;   // the cut itself happens between passes (begin_pass), never inside one
  ```
  LRZ-правило: резать только перед проходом, чья depth-attachment не использовалась раньше в этом сабмите
  (иначе мигание; «LRZ-safe cut rule: about 16 → 19.5 ms»).
- **Контр-данные:** d2r `2026-10-17-frame-overlap.md` «gfx-submit-passes»: N=4/8/16 проходов — GPU 59 → 64-82 мс, fps не вырос;
  ручка удалена, т.к. перекрытие кадров решает то же самое.
- **У нас:** `xbg_game.cpp` `submit_slots` — один сабмит на кадр (`DM_GFX_DEFER_EVENTS=2`, кольцо из 2 слотов). Разрезки нет.
  Пробовать только если п.1 не даёт перекрытия (игра всё равно ждёт GPU кадра N перед N+1). Тогда кусками по ~128 draw между
  проходами с LRZ-правилом; требует >2 слотов или разделения сабмита внутри слота (у nx2android NSLOT=6).

### 5. Масштаб 3D-рендера / динамическое разрешение
- **d2r** (`dm/host/hle_oe.c`, `jni_android.c`): игра имеет встроенный 3D-scale (renderer+0x66a4, пересборка таргетов через
  main+0x8582f0), UI остаётся в выходном разрешении, апскейл — шейдером самой игры `ps_upsample_p`. Android: `DM_3D_SCALE=0.85`,
  контроллер по реально пропущенным кадрам (окна 2 с; вниз при >5% за 10 с или >1.5% за 60 с; вверх после 30 с без пропусков;
  серии пропусков считаются первыми 3 кадрами — «скриншот не лечится разрешением»). Есть вариант по GPU-времени
  `DM_3D_SCALE_GPU=1` (HarmonyOS): каждые 2 с кадр > 35 мс и GPU ≥ 85% кадра → вниз (×0.92 … ×0.60), GPU < 75% 10 с → вверх.
  Измерено: 0.75 → 0.6 даёт лишь 25.3 → 21.4 мс — «about half of main pass does not scale with pixels; most likely per-draw state
  replayed in each tile bin». Уже в нашем hle_oe.c, но под `DM_GAME_DOOM` отключено (смещения D2R).
- **nx2apk** `d0b5267`, `76e51d4`: игра DMC слушает `nn::oe::GetDefaultDisplayResolutionChangeEvent`, хост меняет
  `GetDefaultDisplayResolution` на лету (шаги ×0.88 до пола 480 строк, цель 45 fps, `DM_DRS*`), игра пересобирает таргеты,
  present апскейлит. Плюс «pixel budget»: 1394x660 вместо 1520x720 (−19% пикселей) вернуло тяжёлые сцены к 60.
- **nx2android** `02e336f`: «Timestamp counters from GPU queries (the game's dynamic resolution)» — DRS XC3 читает GPU-таймстемпы.
- **Для DOOM:** в ACTIVE-PORT записано «cvar'ов масштаба нет, таргеты честные 1280x720». **[догадка]** DOOM 2016 на Switch известен
  динамическим разрешением; у id Tech DRS обычно рисует в **поддиапазон viewport'а таргета фиксированного размера**, а не меняет
  размер RT — проверить размеры viewport/scissor основного прохода и чтение таймстемпов (`write_report` у нас отдаёт настоящее GPU-время
  ×12/625 — то есть DRS игры, если он есть, должен реагировать). Проверить импорты `GetDefaultDisplayResolution*`/`GetPerformanceMode`
  (у нас нет HLE для `GetDefaultDisplayResolution` — значит не импортируется или не дошло). Шаблон контроллера — `s3g_feed` из d2r.

### 6. Fragment quad-shuffle через производные: самый большой GPU-выигрыш d2r — у нас выключен политикой Adreno 750
- **Источник:** d2r `2026-10-18-combat.md`, `xbg_spirv_quad.cpp`; коммит `2461649` (#148), `xbg_adreno_policy.h`.
- **Суть:** перевод Maxwell `SHFL.BFLY` во фрагментных шейдерах в subgroup shuffle; helper-инвокации на Adreno читают мусор → NaN
  расползается по блюр-цепочке. Замена на `dFdxFine/dFdyFine`. **GPU 23.3-24.3 → 18.0-18.5 мс в бою (−17-24%)**, и чинит чёрные квадраты.
- **Почему у нас выкл:** на Adreno 740-799 `gfx-quad-derivatives` и `gfx-warp32` по умолчанию off — драйвер 740 роняет устройство
  (kgsl 00EF1087), на 750 «submission never finishes» (Redmi K70 Pro). Неясно, какой из двух переписываний виноват на 750
  (коммит выключил оба сразу; «This build has not run on an 8 Gen 2 or 8 Gen 3 phone yet»).
- **Для нас:** посмотреть `NVN PERF quad_rewrites` — есть ли в шейдерах DOOM quad-shuffle. Если есть — A/B маркером
  `gfx-quad-derivatives`=1 **отдельно от warp32** (риск DEVICE_LOST, но тест дешёвый). Возможна связь и с NaN-артефактами.

### 7. Adreno 750: subgroup-инструкции внутри вызываемой helper-функции → баг компилятора (у нас нет фикса)
- **Источник:** d2r `2428e5b` (#123/#153/#166), `758f5d4`, `81e692d`; `docs/android-notes/2026-10-27-adreno-750-inline.md`.
- **Суть:** libllvm-qgl.so на Adreno 750 падает (segfault addr=0x48) на любом пайплайне, где shuffle/ballot находится в
  **не-entry функции**. Фикс: на 750-799 такие шейдеры инлайнятся при загрузке SPIRV-Tools-пассами
  `merge-return`, `inline-entry-points-exhaustive`, `eliminate-dead-functions` (маркер `gfx-inline`), + pipeguard
  (`gfx-pipe-inflight.<slot>` / `gfx-pipe-skip`) пропускает шейдер, дважды уронивший компилятор.
- **Для нас:** в ACTIVE-PORT: GS-куллинг `703f1fcc` «обменивается данными между дорожками (функция %63, вызывается 6 раз с shuffle)» —
  ровно эта форма. У нас он не падает, но **[догадка]** тот же дефект компилятора может давать **неверный код** вместо краша →
  дыры/выпавшие треугольники. Дешёвая проверка: заинлайнить %63 в 703f1fcc (spirv-opt с этими тремя пассами, через `spv-override`)
  и посмотреть на дыры. В нашем `third_party` нет spirv-tools; портировать `gfx-inline` из 2428e5b.

### 8. Adreno LRZ/binning: выпадающие треугольники и порядок VS-only draws (корректность, но влияет на выбор оптимизаций)
- **Источник:** nx2android `74c53d5`, `c9c9e38`, `d8c685a`, `3ae3fa5`, `af7a8c5`; SKILL.md таблица симптомов.
  - LRZ предыдущего depth-буфера применяется в binning последующих проходов на **другом** depth-образе → режет целые треугольники
    (XC3 облака). Лечится записью одного тексела transfer-write в depth-образ, с которого уходим (`lrz_touch`, `DM_VK_LRZ_TOUCH`);
    transfer read LRZ не сбрасывает, write — сбрасывает. +0.6-0.8 мс GPU.
  - LRZ устаревает на разрезах command buffer между проходами с общим depth-буфером.
  - Draw, тестирующий depth, который FS писал через `gl_FragDepth`: ранний culling Adreno теряет треугольники; вариант с
    `gl_FragDepth = gl_FragCoord.z` (`DM_VK_FDZ`).
  - **Rasterizer-discard VS-only draws внутри прохода** (XC3 трансформирует меши VS-only point-draws в G-buffer-проходе): Adreno
    выполняет VS всех draw прохода в binning-фазе, поэтому последующие draw читают ещё не записанные вершины → тонкие чёрные «щепки».
    Фикс — серии таких draw в собственный проход (+1 мс CPU, +0.1 мс GPU); `af7a8c5` — transfer read в конце серии.
- **Для нас [догадка, сильная]:** наши дыры — «силуэты есть в depth-prepass, но нет в основном проходе». Это совпадает и с LRZ-кейсом
  (prepass → main с тем же/другим depth), и с binning-кейсом (GS-куллинг в prepass пишет SSBO/indirect-аргументы, потребитель в
  том же проходе или проходе, слитом без барьера). Проверить A/B: принудительный разрыв прохода + полный барьер после draw с
  записью SSBO/атомиков; one-texel lrz_touch между prepass и main. Важно: наши `DM_GFX_STORAGE_CHAIN` и «барьер соблюдается только
  после draw со storage-image» как раз убирают разрывы, которые могли маскировать эту проблему.

### 9. UBWC: MUTABLE_FORMAT без format list выключает компрессию
- **Источник:** d2r `xbg_game.cpp:235` + `2026-10-17-perf-round2.md` п.2: **GPU 42 → 36 мс, 21.2 → 23.8 fps**; nx2apk PLAN.md (Mali, AFBC):
  «about 20% fewer GPU cycles»; nx2android `gfx_vk_tex.c` `DM_VK_UBWC=1`: format list `{fmt, sRGB-близнец}` вместо голого MUTABLE.
- **У нас:** `no_mutable_images=true` уже есть (тот же код). Но `upgrade_mutable` при первом view другого формата пересоздаёт образ с
  `MUTABLE_FORMAT_BIT` **без `VkImageFormatListCreateInfo`** (FORMAT_LIST в файле не встречается) → такие образы теряют UBWC.
  Посмотреть счётчик `mutable_upgrades` в DOOM (sRGB/UNORM-вьюхи G-буфера, рендер в UINT-вью и т.п.). Если >0 на крупных RT —
  добавить format list (VK_KHR_image_format_list; инстанс 1.1 → расширение) с {исходный, запрошенный view-формат}.

### 10. Окклюзионные запросы: батчинг по сериям draw
- **Источник:** d2r `xbg_game.cpp:684-690` («~2200 begin/end pairs per D2R camp frame cost the Adreno 730 ~10 ms of GPU»),
  perf-round2 п.5: GPU 36.8 → 25.2 мс, main pass 19.1 → 12.9 мс. Тот же код у нас (`occlusion_batch_enabled=true`).
- Странность (night-merge): выключение ~47 нечитаемых запросов сделало GPU **медленнее** на 1.75 мс; без PRECISE_BIT — без изменений.
  «A guess is that an active query changes how the Adreno driver renders a pass».
- **Для нас:** проверить в `NVN PERF` `occlusion=`/`reports=`/`samples=` для DOOM; если DOOM реально считает samples (окклюзия id Tech)
  и сбрасывает счётчик часто — каждая серия заканчивается на reset/report, проверить сколько запросов/кадр.

### 11. Load/store, clear-folding, noload — в d2r дали ~0
- d2r `2026-10-21-ideas-from-112.md`: верхняя граница «все проходы начинаются с DONT_CARE-близнеца» — color loads −0.06 ± 0.10 мс,
  все четыре вместе **+0.19 мс (медленнее)**. `review-fixes`: DONT_CARE для depth-less проходов — без изменений («driver does not reload»).
  `writebacks.md`: clear-on-load (LOAD_OP_CLEAR вместо LOAD+vkCmdClearAttachments) — 17.69 мс в обеих фазах, «driver skips the load itself».
- `gfx-noload` (доказанное полное перекрытие первым draw → color DONT_CARE) **есть и у нас, выкл по умолчанию** (`noload_enabled=false`,
  `DM_GFX_NOLOAD`), Android его не включает. Включить и посмотреть `noload_passes_per_frame` / `noload_why` можно, но по данным d2r
  ждать много не стоит. nx2android: «If the load/store A/B saves under about 1 ms, the cost is the game's own shading».
- Однако: в d2r ~64 прохода, у нас ~80 с тяжёлым MRT G-буфером; **[догадка]** у DOOM разрывы проходов посреди G-buffer
  (store+load всех MRT) дороже, чем у D2R — пересчитать `pass_end_lines` по крупным таргетам.

### 12. Пейсинг/презент
- d2r `vulkan_present.cpp`: FIFO, `minImageCount` 4 (`DM_SWAP_IMAGES`), `gfx-present-overlap` (2 блита в полёте, без
  `vkQueueWaitIdle` после каждого present — без этого 7.7 fps), `gfx-present-native` (буферы окна в размере выхода игры, масштабирует
  дисплейный контроллер; 230 → 163 мкс), голос `ANativeWindow_setFrameRate(120)` (60 fps-кадр, опоздавший на 16.7 мс, висит 25 мс
  вместо 33.3; при ~20 fps на 120 Гц доступны ступени 24/30/40). Всё это у нас есть. MAILBOX нигде не использовался.
- d2r `hle_nvn.c`: адаптивные ступени `DM_PACE_ADAPT` (на Android off; HarmonyOS on), решение по пропущенным слотам с пробой вверх
  (3 → 60 с бэкофф). nx2android `perf_android.c`: 60/40/30 governor по p90 работы через 4 с, «ADPF target = период игры, не адаптивный»
  (иначе CPU-клоки падают и вверх не выйти). У нас ADPF + thermal + голос частоты уже портированы (`dm/android/perf_android.c`),
  включая `reportActualWorkDuration2` с GPU-временем (Android 15) — это единственный ADPF-канал, влияющий на GPU.
- ffc1d8f: уважение настройки 30 fps энергосбережения на Android.
- **Для нас:** при ~20 fps пейсинг не главный фактор; после п.1-3 проверить, что тайминг кадров ровный (ступень 30 при 120 Гц).

### 13. CPU-сторона (для справки — у нас уже стоит то же самое)
Включены в d2r Android и у нас (`jni_android.c` идентичен): `DM_GFX_WB_SKIP` (−1 мс/кадр readback), `PIPE_MEMO`, `NATIVE_BC`
(GPU −0.5 мс, память 827 → 320 МБ), `LDC_FOLD`+`FORWARD_TEMPS` (GPU 15.70 → 14.34 мс, −10.7%), `NAN_MINMAX`, `DEFER_EVENTS=2`,
`READ_PREFLIGHT=0`, `XVIEW_GUARD`, `UPLOAD_PROLOGUE` (первый кадр 109 → 96 мс GPU), `SHADER_REUSE`, `VCACHE` (−0.7 мс CPU),
`TEX_DIRTY`, `ASYNC_POOL_RESET` (−0.4 мс), `PERF_SAMPLE=0`, `DM_HLE_SPIN_US=30`. Плюс по коду: framebuffer cache, slot pools,
pass_fast (create_pass для повторных draw), wtrack через host hooks (−1.15 мс), пин игрового потока на самые быстрые ядра
(`gfx-pin`, 40.2 → 44.3 fps). Асинхронная трансляция (`DM_NVN_SYNC=0`) в d2r была **медленнее** (20-22 против 37.5 fps; игра ждала
fence работы, которую поток трансляции ещё не засабмитил) — у нас «не меняет fps», что согласуется с п.1.

### 14. Выключенное/не по умолчанию в d2r `xbg_game.cpp` (маркеры `files/dm/gfx-*`, env `DM_GFX_*`)
| Маркер / env | По умолчанию на Android | Что делает | Стоит ли нам |
|---|---|---|---|
| `gfx-noload` / `DM_GFX_NOLOAD` | off | color DONT_CARE, если первый draw доказуемо покрывает всё | A/B, ждать мало (п.11) |
| `gfx-quad-derivatives` | on, но **off на Adreno 740-799** | quad shuffle → fine derivatives, −17-24% GPU в d2r | A/B маркером =1 (п.6), риск DEVICE_LOST |
| `gfx-warp32` | on, но **off на 740-799** | 32-lane варпы внутри 64-lane subgroup (корректность append) | только если в DOOM есть warp-aggregated append; стоит +0.6 мс |
| `gfx-inline` | on на 750-799 | инлайн helper'ов с subgroup-инструкциями | **нет у нас** — портировать (п.7) |
| `gfx-async-pipelines` / `DM_GFX_ASYNC_PIPELINES` | off | сборка пайплайнов в фоне (статтер −, «объекты появляются поздно» до 0.5 с) | нет (не про GPU) |
| `gfx-readback-cached` | off | readback-арены в host-cached памяти (Adreno host-visible читается ~1.1-1.3 ГБ/с) | если много readback |
| `gfx-mutable-images` | off | все color-образы mutable (старое, убивает UBWC) | не включать |
| `gfx-heavy-timestamps` | off | timestamp-репорты рвут проход полными барьерами (старое) | не включать |
| `gfx-query-per-draw` / `gfx-no-query-batch` | off | запрос на каждый draw (старое, ~10 мс GPU) | не включать |
| `gfx-no-occlusion` | off | диагностика: без окклюзии | только A/B |
| `gfx-pass-profile` | off | таймстемпы по проходам (искажают binning — nx2android) | для профиля |
| `gfx-pipe-stats` | off | `VK_KHR_pipeline_executable_properties`: инструкции/регистры/occupancy | полезно для 15-17 мс шейдинга |
| `gfx-sync-mode`, `gfx-sync-submit`, `gfx-legacy-waitall` | off | диагностика синхронизации | у нас legacy_waitall=0 для DOOM |
| `DM_3D_SCALE_GPU` | off (HarmonyOS on) | 3D-scale по GPU-времени | шаблон, если найдём масштаб в DOOM |
| `DM_PACE_ADAPT` | off (HarmonyOS on) | адаптивные ступени темпа | после п.1 |

---

## Прочие факты с замерами (могут пригодиться)
- d2r gpu-profile: `spirv-opt -O` на тяжёлых шейдерах — драйвер выдаёт тот же код (Adreno сам удаляет переменные-регистры);
  required subgroup size 64/128 разрешён только для compute; `robustBufferAccess` бесплатен; transient command pools — без выигрыша.
  Тяжёлые lighting-шейдеры: 3.4-4.2k инструкций, 22-25 регистров, «max active invocations 1» (низкая occupancy, плохо прячет латентность
  текстур). Для наших 15-17 мс основного шейдинга: `gfx-pipe-stats` покажет то же самое для DOOM.
- d2r main-preflight-gpu: «vertex work is a large share» — depth-prepass 1.36 мс на 602k вершин с VS 545 инструкций, main pass гоняет
  тот же VS повторно. Shadow skinning ×2.3 быстрее только при LDC fold + оптимизаторе вместе.
- nx2android NEO: «defer uniform updates instead of ending the pass, keep the pass when same targets set again»: 145 → 28 проходов,
  53-81 → 25-30 мс. Но не через NVN-барьеры (XC3: рваные персонажи из-за storage от rasterizer-discard draws).
  Dynamic UBO offsets: reuse дескрипторов 6% → 86%.
- nx2android: `/proc/<pid>/task/*/stat` + cpufreq вместо simpleperf; `dumpsys thermalservice` показывал устаревшие 93 °C — читать
  `/sys/class/thermal/thermal_zone*/temp`; Y700 `scaling_max_freq` ~2.0 ГГц при 55 °C — кап политикой, не нагревом.
- nx2android: потоки, занятые ≥15% ядра, каждые 300 кадров добавляются в ADPF-сессию (docked worst frames 70-90 → 48-58 мс) —
  у нас уже есть.
- nx2android `DM_VK_X_NOGS=1`: GS-draw'ы XC3 стоят 0.7 из 22 мс; GS транслируется как настоящий Vulkan GS, на Adreno работает.
- d2r 17b4baa (Mali-G925): R16G16_UNORM без linear filtering → BC4/BC5 в 8-бит. Для Adreno неактуально.

## Рекомендуемый порядок для DOOM
1. Трасса синков (аналог `DM_NVN_SEQ`): ждёт ли DOOM на CPU sync от acquire / прошлого кадра и сколько мс → п.1 (fence последнего
   present) с выключателем. Мерить fps + GPU мс + простой GPU.
2. Параллельно бесплатно: игровой режим вендора (п.3), сравнить капы частот с Winlator.
3. `NVN PERF`: `quad_rewrites`, `mutable_upgrades`, `occlusion/reports`, `noload_why` → п.6, п.9, п.10, п.11.
4. Дыры: инлайн %63 в 703f1fcc (п.7) и A/B разрыва/барьера после GS-prepass + lrz_touch (п.8).
5. Если сериализация останется — сабмит кусками с LRZ-правилом (п.4); затем масштаб 3D (п.5).
