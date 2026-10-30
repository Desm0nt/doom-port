# DOOM → Android: переключатели, cvar-переопределения и тестовые инструменты

Состояние на вечер 28.10.2026 (по исходникам `dm`, сверено с журналом [PORT_JOURNAL.md](PORT_JOURNAL.md) и
[BUGS_AND_FIXES.md](BUGS_AND_FIXES.md)). «По умолчанию» — значение, когда переменная не задана. Значения, проставляемые
Android-обёрткой (`jni_android.c`, `setenv(..., 0)` — не перекрывают заданное), помечены «(APK)».

## Как задать переключатель

- **`dm/env`** — файл `/sdcard/Download/DOOM Mobile/dm/env`, строки `KEY=VALUE`, `#` — комментарий. Читается при старте
  (`load_env_file` в `jni_android.c`) до умолчаний APK. Все серии (`play_series.sh`, `diag_run.sh`, `ab_run.sh`) пишут его
  сами и в конце удаляют. **Если хозяин играет сам — проверить, что старого `dm/env` не осталось.**
- **Маркер-файлы** в `dm/` (`gfx-dump-now`, `gfx-capture-now`, `gfx-pipe-skip`, `gfx-lone-lane`, `gfx-noload`, `gfx-tex-dirty`
  и т. п.) — старый механизм d2r-mobile; маркер имеет приоритет над env того же смысла.
- **Меню ⚙ в приложении** (`DmActivity.showDoomSettings`, SharedPreferences `doom_graphics` / `doom_controls`; тексты —
  `res/values*/doom_settings.xml`, 13 языков по локали системы, английский по умолчанию). У каждого пункта в подписи сказано,
  когда он действует. **Со следующего запуска** (env до загрузки `libdm.so`): разрешение 3D `DM_DOOM_RES`, резкость
  `DM_GFX_SHARPEN`, ограничение частоты кадров `DM_PRESENT_MAX_HZ` + `DM_DOOM_TICK_MAX` (30 / 60 / 90 / 120, по умолчанию 60;
  90/120 — только если у дисплея есть такой режим), широкий экран `DM_DOOM_WIDE` (на лету только с `DM_DOOM_WIDE_LIVE=1`),
  драйвер `DM_VK_DRIVER`, язык `DM_LANG`, аим-ассист `DM_AA_STRENGTH`, вибрация `DM_RUMBLE`. **Сразу**: чувствительность обзора,
  компенсация мёртвой зоны, счётчик FPS (`DoomPadView.reloadSettings`) и динамическое разрешение (см. ниже). `dm/env`
  перекрывает всё, что пишет ⚙.
- **Динамическое разрешение — одно состояние** (BUGS_AND_FIXES §2.14): cvar игры `rs_enable`, сохраняется в профиле игры.
  Менять можно в Video-меню игры («Dynamic Resolution», бывший Chromatic Aberration) **или** галочкой ⚙ — она читает текущее
  значение и пишет его на лету (`hle_doom_drs.c`: `idCVar::SetInteger` между кадрами + запрос сохранения профиля). Дефолт cvar'а
  пропатчен на 0 (CUTS `0x1c2feb4`), так что на свежей установке DRS выключен без `+set`. Последний выбор игрока хост дублирует в `files/save.drs` и восстанавливает после каждой загрузки профиля (diag45: сохранение профиля игрой не подтвердилось).
- **Драйвер по умолчанию по GPU** (BUGS_AND_FIXES §2.16): Adreno 6xx/7xx → `turnip27`, 710/720/722 → `turnip27-a710` (если
  скачан), 8xx → `turnip27-a8xx` (если скачан), ≤5xx и не-Adreno → системный. Фолбэк на системный показывается тостом;
  crash-guard `files/driver-trying` переключает на системный после двух запусков подряд, умерших до первого кадра.
- **cvar'ы игры** — через `DM_DOOM_ARGS="+set имя значение ..."` (через пробел, дописываются в конец командной строки
  после умолчаний ниже; итоговая строка видна в логе `命令行 | command line:`).

## 1. Командная строка игры (`dm/host/hle_args.c`, HLE `nn::os::GetHostArgc/GetHostArgv`)

id Tech 6 разбирает `+set <cvar> <value>`. Порт по умолчанию добавляет:

| Переключатель | По умолчанию | Что ставит | Когда менять |
|---|---|---|---|
| `DM_DOOM_RES=<0.25..1>` | не задан | `rs_forceFractionX/Y` = доля 3D от 1280×720 (UI остаётся 720p). Меню ⚙: 100/85/75/67 % | Ради fps на слабом GPU; 0.75 = 960×540 |
| `DM_DOOM_DRS=<n>` | не задан | только отладка: `+set rs_enable <n>` при каждом запуске (`0` — принудительно выкл). Без него `+set` не добавляется: дефолт `rs_enable` пропатчен на 0, решают Video-меню / ⚙ (одно состояние в профиле); для старого `main.patched.bin` без CUTS `0x1c2feb4` по-прежнему `+set rs_enable 0` (лог `[drs] rs_enable default`) | DRS включать в Video-меню или галочкой ⚙ |
| `DM_DOOM_GEOMCULL=1` | 0 | без него `+set r_geometryCulling 0 +set r_smallTriangleCulling 0` | Только для отладки дыр GS-куллинга (см. BUGS) |
| `DM_DOOM_LINEAR_STICKS=0` | 1 (линейно) | `joy_deadZone 0`, `joy_nxDefaultDeadzone 0`, `joy_nxProDeadzone 0`, `joy_powerScale 1`, `joy_gammaLook 0`, `joy_nxDefaultJoystickCurve 0`, `joy_dampenLook 0`, `joy_deltaPerMSLook 1000`, `joy_edgeAccelerationScalar 0`, `joy_mergedDeadZoneAngle 0`, `joy_smoothingEnabled 0` | `=0` вернёт кривые игры (0.35 мёртвая зона, степень, слью 0.003/мс, ускорение у края) — для физического геймпада, если линейность мешает |
| `DM_DOOM_ADAPTIVE_TICK=0` | 1 | `com_fixedTic 1`, `com_adaptiveTick 1`, `com_adaptiveTickImmediateMode 1`, `com_adaptiveTickMinHz/MaxHz` | `=0` — исходный фиксированный тик 30 Гц (скорость игры = fps/30). Только для сравнения |
| `DM_DOOM_TICK_MIN` / `DM_DOOM_TICK_MAX` | 30 / 60 | границы адаптивного тика; MAX (APK) = ограничение частоты кадров из ⚙ | MIN=20 — попробовать тики ниже 30 Гц (не проверено) |
| `DM_AA_STRENGTH=<s>` | 1 | добавляет `+set aa_DESIGN_override 1`; в `hle_hid.c` выключает MP-методы aim assist и держит SP-значения оружия × s | `=0` — слабый MP-ассист игры; >1 — сильнее |
| `DM_DOOM_WIDE` | **0 (выкл)** | `1` — широкий вывод Hor+: до разбора командной строки (`hle_args.c doom_wide`) таблица `r_mode` в .rodata (`main+0x39f1b10`, было {1280,720},{1920,1080}) → {W,720},{1.5W,1080}; W = 720 × соотношение экрана (`front_game_res(0)`, кратно 16, 1280..2160), только если экран шире 16:9; 2400×1080 → 1600×720. Пул RenderTarget растёт с шириной (см. `DM_DOOM_POOL_MARGIN`: 204 МБ при 1600; без запаса — минимум ⌈117·W/1280⌉+4 = 151). Лог `[wide] r_mode table ...` | `=<ширина>` (≥1280) — явно, напр. `1440`. Выкл по умолчанию после diag30 (белый экран = переполнен пул RenderTarget, BUGS §2.10); включать после проверки на телефоне. +25 % пикселей при 1600 — компенсировать `DM_DOOM_RES` |
| `DM_DOOM_WIDE_LIVE` | **0 (выкл)** | `1` — галочка «Широкий экран» в шестерёнке переключается на лету (BUGS §2.12): игра всегда рисует W×720 (как `DM_DOOM_WIDE=1`, пул RenderTarget по широкому размеру), «выкл» = презентер показывает центральные 16:9 кадра (`vulkan_present.cpp crop169`; Hor+ → та же картинка, что у 16:9). Java `nativeWideLive` → `dm_doom_wide_live()` + `dm_present_crop169`; лог `[wide] live switch ...`, `dm-vulkan [wide] live: ...`. Без него — «со следующего запуска». Цена: в режиме «выкл» рисуется 1600×720 (+25 % пикселей); игры путь смены размера на лету не имеет (vid_restart — пустая заглушка) | — |
| `DM_DOOM_POOL_MARGIN` | **по RAM** (с 30.10) | запас пулов VRAM игры. Уровни: полный (TextureBuffer 1536, RenderTarget 1.35×min, малые ×1.5, +512 МБ кучи), половинный (1350, 1.15×, ×1.25, +256), стоковый. Без env: MemTotal ≥ 14 ГБ → полный, ≥ 10 ГБ → половинный, иначе стоковый (лог `[pools] margin level`). `0` — стоковый, `1`/`2` — полный, `1h` — половинный | полный запас загнал 12-ГБ Xiaomi 13 Pro в 4.9 ГБ свопа |
| `DM_TEX_DISCARD=1` | **0 (выкл)** | освобождать гостевую копию статичных текстур (BUGS §3.13): текстура залита ≥ `DM_TEX_DISCARD_AGE` с назад и с тех пор не переписывалась (VT-кэши, RT, storage, CPU-декодированные BC, depth, linear исключены), её целые страницы в пуле → `PROT_NONE` + `MADV_DONTNEED`; при касании (чтение/запись гостя, запись хоста, undo движка) поток `dm-texdiscard` читает Vulkan-образ на втором queue (`vkCmdCopyImageToBuffer` → swizzle → `mremap`) и возвращает страницы под защиту записи. Требует второй queue в семействе (Qualcomm — есть, Turnip — нет: тогда выкл с логом) и `DM_GFX_TEX_DIRTY=1` (APK). Включает: пул ≥ `DM_TEX_DISCARD_ZC_MIN_MB` **без zero-copy** (снапшоты для его вершин/UBO), лог `NVN TEXDISCARD …` каждые 10 с и `NVN TEXCLASS …` каждые 60 с | A/B на 8 Gen 2 (§3.13): своп, PSS, время перезагрузки уровня; fps (минус zero-copy пула TextureBuffer) |
| `DM_TEX_DISCARD_AGE` / `_MIN_KB` / `_MAX_MB` / `_ZC_MIN_MB` / `_SCAN_MB` | 60 / 256 / 64 / 512 / 256 | возраст «статичности» (с); размер текстуры; пулы не-zero-copy при discard (512 = только TextureBuffer); сколько МБ ставить в очередь за один проход (раз в 10 с) | — |
| `DM_TEX_CLASS=<с>` | **выкл** (60 при `DM_TEX_DISCARD=1`) | раз в N с лог классов живых текстур: `NVN TEXCLASS (class images/guest MB/vulkan MB …) rt … vt … hot … young … static … unused …`, строка «static discardable … \| not: zc/decoded/untracked/format/small/big/layout/dirty/undone» (= реальная экономия) и по пулам ≥ 64 МБ: `resident` (mincore), МБ текстур, zero-copy | замер перед включением discard |
| `DM_DOOM_ARGS="..."` | пусто | любые `+set` | Изоляция проходов при диагностике (см. §7) |

`g_argv` — 128 слотов (умолчания занимают ~60).

## 2. Графика: движок xbg (`dm/gfx/vulkan/engine/xbg_game.cpp`)

### Корректность (DOOM-специфичное)
| Переключатель | По умолч. | Смысл | Когда менять |
|---|---|---|---|
| `DM_GFX_TAA_FIX` (маска) | **2** | бит 2 — эмуляция NVN `LOWER_LEFT` (вьюпорт с положительной высотой, инвертированный `frontFace`, вывод окна через перевёрнутый blit); бит 1 — проба TAA (лог `NVN TAA probe`) | `0` — старое зеркальное поведение (туман/декали/TAA снова сломаются); `3` — плюс проба. Не включается при зарегистрированном UI-сдвиге HarmonyOS |
| `DM_GFX_FOG_FIX=1` | — | alias бита 2 `DM_GFX_TAA_FIX` | историческое имя |
| `DM_GFX_DEPTH_FEEDBACK` | **2** | чтение depth-attachment открытого прохода: `2` — без разрыва только пока проход не писал глубину (фикс мигания теней); `1` — старый shortcut (чтение и после записи); `0` — shortcut выкл | `1` вернёт мигание теней; `0` — дороже |
| `DM_GFX_GSCULL_FIX` (маска) | **0x61** (1\|32\|64) | 1 — GS со subgroup-идиомой всегда lone-lane (Turnip); 32 — записываемые storage-диапазоны с запасом до слова; 64 — CPU-запись (COPY_B2B/UPDATE_UB) разрешает pending GPU-результат; диагностика: 2/4/8/16 — выключить тесты occlusion/small/backface/frustum, 0x100 — проверка count, 0x200 — identity-компакция, 0x400 — стартовый count=0, 0x800 — index carrier, 0x1000 — CAS int temps | только вместе с `DM_DOOM_GEOMCULL=1` для бисекта дыр |
| `DM_GFX_GS_PASSTHRU` | **1** (с 28.10) | draw без GS-стадии, если culling-GS доказанно pass-through (`r_geometryCulling 0`) | `0` — для сравнения/если появятся дыры |
| `DM_GFX_GS_PASSTHRU_VARY` | **1** (с 29.10) | матчер pass-through принимает и GS, пробрасывающие varyings (копия входа той же Location); на телефоне без этого отбрасывался только `703f1fcc`, остальные 5 culling-GS работали как GS и рвали проходы (BUGS_AND_FIXES §3.4) | `0` — старое правило «только gl_Position»; A/B velocity/TAA-артефактов |
| `DM_GFX_MEMO_SIMG` | **1** (с 28.10) | descriptor memo и для наборов со storage image (VT feedback) | `0` — для сравнения |
| `DM_GFX_VT_FIX` (маска) | **1** (с 28.10) | копия VT-feedback в конце guest-submit, а не на present | `0` — старый путь (страницы чёткими приходят медленнее) |
| `DM_GFX_VT_ATOMIC` | 0 | mutex между `WriteTexels` и частичной перезаливкой (гонка рваных страниц) | A/B мерцания земли; по умолчанию выкл (эффект не подтверждён) |
| `DM_GFX_VT_DIAG` | 0 | строка `NVN VT feedback ...` раз в 300 write-back'ов (lat_subs, cpu_changed, torn, reup<16, partial) | диагностика VT |
| `DM_GFX_CPU_VISIBLE` | 1 | выгрузка CPU-видимых storage-образов (VT feedback 120×90 R32) в память игры | `0` — VT перестанет подкачивать страницы |
| `DM_GFX_INVARIANT` | 0 | `Invariant` позиций | не помог от дыр |
| `DM_GFX_LRZ_TOUCH` | 0 | LRZ-«касание» в begin_pass | экспериментально |
| `DM_GFX_NO_SWIZZLE` | 0 | убить компонентный swizzle текстур | отладка (шрифты превращаются в блоки) |
| `DM_GFX_NO_FORMAT_LIST` | не задан | не давать `VkImageFormatListCreateInfo` (UBWC на MUTABLE-образах) | отладка |

### Производительность
| Переключатель | По умолч. | Смысл | Примечание |
|---|---|---|---|
| `DM_GFX_SUBMIT_CHUNK_US` | **2000** | отправлять записываемый слот на GPU после N мкс трансляции (на границе guest-submit) | главный выигрыш 28.10 вечер: ~20 → ~47 fps медиана. `0` — один submit на present |
| `DM_GFX_ZEROCOPY` | **1** для DOOM | пулы NVN на AHardwareBuffer по гостевым адресам (MAP_FIXED), без снапшотов | `0` — снапшоты (медленнее) |
| `DM_GFX_ZC_MAX_MB=<n>` | 0 (без предела) | пулы больше n МБ остаются на обычной памяти кучи гостя (ленивые, свопятся) вместо dma-buf (весь пул резидентен с запуска и не свопится; `MADV_DONTNEED` его не освобождает). `600` = без zero-copy только TextureBuffer. Лог `NVN zero-copy: pool … left on guest heap memory` | A/B памяти отдельно от `DM_TEX_DISCARD` (тот делает то же для пулов ≥ 512 МБ) |
| `DM_GFX_ZC_INDEX` | 0 | индексы indirect-draw прямо из AHB | `1` ронял драйвер Adreno |
| `DM_GFX_ZC_UBO_MIN`, `DM_GFX_ZC_RACE` | 0 | эксперименты zero-copy UBO (гипотеза теней, отвергнута) | — |
| `DM_GFX_PARTIAL_TEX` | 1 | частичная перезаливка изменённых GOB-блоков больших текстур (VT-кэши) | `0` — полная перезаливка (очень медленно) |
| `DM_GFX_PARTIAL_PROLOGUE` | **1** (с 29.10) | partial upload в пролог-буфер слота, если образ ещё не использовался в этом submit (как `gfx-upload-prologue`); иначе — старый путь (конец прохода + 2 полных барьера) | `0` — старый путь; A/B мерцания VT-страниц. Нужен `DM_GFX_UPLOAD_PROLOGUE=1` (APK) |
| `DM_GFX_GPU_INDIRECT` | 1 | indirect-draw из GPU-копий аргументов | **`0` больше не использовать**: CPU-путь вешал игру (SyncWait) |
| `DM_GFX_STORAGE_CHAIN` | 1 | передавать тот же SSBO дальше вместо копии | — |
| `DM_GFX_GUEST_BARRIER` | 0 | соблюдать каждый guest-барьер NVN (~118/кадр) | `1` — контроль хазардов (дорого) |
| `DM_GFX_LAZY_STORAGE` | 1 | ленивый диапазонный барьер после storage-записей | `0` — барьер после каждого draw |
| `DM_GFX_PRO_COPY` | 1 | копии результатов прошлого кадра в пролог слота | — |
| `DM_GFX_EARLY_Z` | 1 | `EarlyFragmentTests` для FS с записью в образы/SSBO без discard/FragDepth | основной проход 36 → 17 мс |
| `DM_GFX_QUAD_DERIVATIVES` | 1 (APK) | quad-рерайт производных | A/B мерцания земли |
| `DM_GFX_SHARPEN=<stops>` | не задан | FSR RCAS при выводе (`rcas.comp`); ⚙: 1.0 / 0.5 / 0.2 | вместе с `DM_DOOM_RES` |
| `DM_GFX_PERF_SAMPLE` | 0 (APK) | строки `NVN PERF` (4 — каждый кадр окнами по 60) | всегда в замерах |
| `DM_GFX_PASS_PROF="from period count"` | выкл | per-pass GPU timestamp профиль (`NVN GPASS`, с 29.10 поле `end=L<строка>` — кто закрыл проход), разбор `tools/gpass_report.py` (печатает и разбивку по `end=`) | `1` = `300 600 0` |
| `DM_GFX_RELAXED=<маска>` | 0 | **СЛОМАНО, не включать** (A/B r2_b2: сцена почти чёрная, красные пятна). RelaxedPrecision (fp16) на float-математике FS вне слайса текстурных координат / целых / записей (`xbg_spirv_relaxed.cpp`) | отклонено 29.10: экономии у главного FS всё равно мало (фактически 18 % операций), статистика драйвера не показывает регистрового голода (21–25 регистров, без scratch) |
| `DM_GFX_PIPE_STATS="from period"` | выкл | статистика драйвера (`VK_KHR_pipeline_executable_properties`) для топ-60 пайплайнов по draw в каждом 60-кадровом окне с present `from` через `period` (`1` = `3000 1800`), работает с warm-up. Строки `NVN PSTAT use` / `NVN PSTAT`; разбор `tools/pstat_report.py` | диагностика (perf4) |
| `DM_GFX_DRAW_PROFILE="from frames [min_draws] [desc]"` | выкл | один раз за сессию включает живой CPU-профиль с детализацией draw (`NVN DRAW_TIMING`/`CPU_TIMING`; с perf6 и `section=` / `sub=` — разбивка `draw()` и descriptor-пути) или descriptor (`desc` → `NVN DESC_TIMING`) на первом present ≥ from, где в кадре ≥ min_draws draw; напр. `3000 30 1100` | диагностика per-draw CPU (perf4) |
| `DM_GFX_ARENA_UNCACHED=1` | 0 | арены per-draw (снимки UBO, потоки вершин/индексов, staging) на типе памяти HOST_VISIBLE\|COHERENT **без** HOST_CACHED, если буфер его допускает; сейчас берётся тип 6 = 0xf (кэшируемый → GPU читает с IO-coherent snoop). Лог `NVN DM_GFX_ARENA_UNCACHED=… buffer memory bits=…` и `NVN arena chunk … flags=` | A/B (perf5): GPU main/pre-pass ms; риск — медленные CPU-чтения арен |
| `DM_GFX_ZC_CPU=1` | 0 | zero-copy пулы игры (вершины, UBO, SSBO — 1.3 ГБ) с `CPU_READ_RARELY` вместо `READ_OFTEN` → gralloc может дать некэшируемые (write-combined) страницы, которые GPU читает без snoop. Лог `NVN zero-copy memory: allowed=… picked=… flags=…` | A/B (perf5); риск — медленные CPU-чтения пулов игрой и нами (хэши, снимки) |
| `DM_GFX_ANISO_MAX=<n>` | 0 (как у игры) | потолок анизотропии всех сэмплеров (`1` — выкл); первые 64 сэмплера логируются (`NVN sampler created … aniso=`) | A/B (perf5) |
| `DM_DIRTY_BLOCKS` | **1** (с 30.10; A/B: descriptor-путь 11.3 → 10.1 мкс/draw, артефактов нет) | трекер грязных страниц (`nvn_dirty.c`) ведёт сводку «новейшее поколение» на блок 64 страницы (256 КБ); `nvn_dirty_since` пропускает чистые блоки, а не читает поколение каждой страницы. Проверка идёт раз в guest-submit на каждую сэмплируемую текстуру (~7 submit/кадр), VT-кэши DOOM 33–89 МБ = 8–22 тыс. страниц | A/B (perf6): `DRAW_TIMING part=image` и `sub=desc_hit_get_image+prepare_read`; риск низкий (точная сводка, CAS-max) |
| `DM_GFX_NAN_SAT` | **1** (с 30.10; A/B в динамике: main pass ~−24 %, артефактов нет) | при `DM_GFX_NAN_MINMAX=1` (APK) не менять `FClamp(x,0,1)` на `NClamp`: NIR делает из него `fsat` (NaN → 0, как .SAT Maxwell; на ir3 — бесплатный модификатор (sat)), а `NClamp` разворачивается в проверки NaN (fneu+bcsel) + max + min. 59 насыщений в `3ed1dd77`, 17 в `1101f5db`. Лог `NVN DM_GFX_NAN_SAT=1` | A/B (perf6): main pass ms; артефакты — чёрные/белые пиксели в местах NaN |
| `DM_GFX_WARP_MODE=native\|lone` | **авто** (с 30.10): `lone`, если subgroup ≠ 32 | модули с subgroup-операциями в lone-lane форме (каждая инвокация — свой варп из одной дорожки), чтобы Maxwell-идиома warp-aggregated append (ballot → popcount → atomicAdd лидера → shuffle базы) не теряла дорожки 32+ на 64/128-дорожечных subgroup Adreno; compute оставляет subgroup при включённом `gfx-warp32`. Лог `NVN warp mode: …` и `NVN lone-lane shader=… (subgroup size is not 32)`. В DOOM это 7 модулей: 6 culling-GS (и так lone-lane через `DM_GFX_GSCULL_FIX`) и частицы `cec755c4e27678b5` (раньше lone-lane только из-за списка `gfx-lone-lane` после падения компилятора) | `native` — для A/B; на устройстве владельца изменений нет, защищает чистую установку |
| `DM_GFX_ALIAS_LOG` | 0 | логи алиасинга storage/копий | отладка |
| Наследие D2R (APK): `DM_GFX_WB_SKIP 1`, `DM_GFX_PIPE_MEMO 1`, `DM_GFX_NATIVE_BC 1`, `DM_GFX_WARP32 1`, `DM_GFX_LDC_FOLD 1`, `DM_GFX_FORWARD_TEMPS 1`, `DM_GFX_NAN_MINMAX 1`, `DM_GFX_DEFER_EVENTS 2`, `DM_GFX_READ_PREFLIGHT 0`, `DM_GFX_XVIEW_GUARD 1`, `DM_GFX_UPLOAD_PROLOGUE 1`, `DM_GFX_SHADER_REUSE 1`, `DM_GFX_VCACHE 1`, `DM_GFX_TEX_DIRTY 1`, `DM_GFX_ASYNC_POOL_RESET 1`, `DM_3D_SCALE 0.85` | | для DOOM отдельно не исследовались; `DM_GFX_NATIVE_BC=0` падал на лимите 256 МБ; `TEX_DIRTY=0`/`VCACHE=0` нужны только на LDPlayer (houdini). Влияние `DM_3D_SCALE` на DOOM не подтверждено (таргеты 1280×720 честные) | |

### Отладка Vulkan / драйвер
| Переключатель | По умолч. | Смысл |
|---|---|---|
| `DM_VK_DRIVER=<пакет>` | по GPU (APK, `defaultDriver()`) | драйвер через adrenotools (`vk_driver.c`, трамплины `vk_tramp.S` из `tools/gen_vk_tramp.py`); ⚙ «Драйвер Vulkan». Итог загрузки — `dm_vk_driver_status()` / JNI `nativeDriverStatus` (`ok` / `no-library` / `load-failed` / `no-gpu`) |
| `DM_VK_VALIDATION=1` | 0 | слой Khronos validation; нужен APK, собранный `build_apk.ps1 -Validation` |
| `DM_VK_NULL=1` | — | CLI без окна: команды не рисуются, побочные эффекты применяются (только эмулятор/отладка) |

## 3. NVN-слой (`dm/gfx/nvn/hle_nvn.c`)

| Переключатель | По умолч. | Смысл | Итог A/B |
|---|---|---|---|
| `DM_NVN_SYNC` | **0** (APK, с 29.10) | `0` — трансляция на своём потоке (A/B 29.10: тяжёлые сцены 25 → 33 fps); `1` — inline на потоке игры | 28.10 `0` fps не менял (тогда всё упиралось в GPU) |
| `DM_NVN_SYNC_LAG=1` | 0 | ждать fence кадра N-1 вместо N | средний fps как у chunk, но в 2-3 раза больше длинных кадров + мерцание — не включать |
| `DM_NVN_POLL_US=N` | 0 | сон N мкс в неготовом poll | эксперимент (CPU/нагрев), не дефолт |
| `DM_NVN_GW_STATS=1` | 0 | раз в 300 кадров `[nvn] GW per frame` — какой fence ждёт игра | диагностика |
| `DM_NVN_ACQ_NOW=1` | 0 | старый acquire-fence (до фикса nx2android 1c6177d) | откат |
| `DM_NVN_COUNT=1` | 0 | счётчики вызовов NVN | диагностика |
| `DM_PRESENT_INTERVAL` | display | `game` — следовать интервалу игры (2 → 30 fps; точная скорость, но кап 30) | — |
| `DM_PRESENT_HZ` | 60 (авто) | кап present (жёсткая частота; главнее `DM_PRESENT_MAX_HZ`) | — |
| `DM_PRESENT_MAX_HZ=<L>` | 60 (APK, ⚙ «Ограничение частоты кадров») | авто-режим: наибольшая целая доля частоты экрана не выше L (`nvn_pace_cap_hz`; 120 Гц: 30/60/120, 90 Гц: 45/90). L > 60 — поверхность голосует L (`vulkan_present.cpp`), окно просит режим дисплея с частотой, кратной L. Без него — старое правило «не ниже 60» (на 90/144 Гц игра шла быстрее) | выше 60 не проверено: UI-логика, считающая кадры |
| `DM_PRESENT_STRETCH` | 0 | `1` — растянуть картинку на весь экран (старое поведение); по умолчанию буфер в пропорциях окна, картинка с полосами там, где её соотношение уже экрана (с `DM_DOOM_WIDE=1` полос нет) | — |

## 4. HLE, потоки, ввод

| Переключатель | По умолч. | Смысл |
|---|---|---|
| `DM_TIMERSLACK_NS` | 1000 | `prctl(PR_SET_TIMERSLACK)` гостевых потоков (Android по умолчанию 50 мкс) |
| `DM_YIELD_BACKOFF=<cap us>[:<start us>]` | 250:2000 | сон на повторных yield; `0` — выкл |
| `DM_HANDOFF=0` | 1 | без передачи на то же ядро при пробуждении |
| `DM_HANDOFF_PINNED` | **1** (с 29.10) | handoff только если будящий сам привязан к одному ядру (как миграция Horizon: WorkerThread_00/01 с маской 0x7 больше не ждут главный поток); ~420 handoff/с по ~2 мс в старых дампах | A/B 29.10: тяжёлые сцены 33 → 35 fps, окна < 30 fps 39 % → 26 %; `0` — старое правило |
| `DM_GFX_PIN_CPU=<hex>` | X4 (`80`) | маска ядер потока present (в sync-режиме = поток игры с трансляцией), как маркер `gfx-pin-cpu`; `0` — без привязки, `f0` — X4 + A720 | A/B (perf4) |
| `DM_OPMODE` | 1 | режим работы Switch для игры (по коду Horizon: 1 = док/console, 0 = портатив/handheld); `0` fps не менял (замер ночи 27→28.10) |
| `DM_ADPF=0` | вкл | выключить ADPF/perf hints (нужно под libhoudini) |
| `DM_GYRO=0` | вкл | не отдавать игре гироскоп |
| `DM_GYRO_FORCE=1\|<sens>` | 0 | держать гейты гиро-прицела игры открытыми (чувствительность 0.15 или заданная) |
| `DM_GYRO_GAIN=<k>` | 1 | множитель угловой скорости |
| `DM_GYRO_LOG=1` | 0 | лог сенсора/состояния раз в секунду |
| `DM_AIM_ASSIST_FORCE`, `DM_AA_LOG`, `DM_AA_SET="name=v,..."`, `DM_AA_OWN`, `DM_AA_OWN_ADH` | 0 | диагностика/ручная настройка aim assist (aim_assist.md) |
| `DM_RUMBLE=0` | вкл | без вибрации (игра не видит вибро-устройств); `DM_RUMBLE_PHONE=0` — не вибрировать телефоном; `DM_RUMBLE_GAIN` 0..2 |
| `DM_INPUT_OBS=1` | 0 | OBS-гейты 23–34 в меню/событиях (диагностика ввода) |
| `DM_KEYS="PLUS@95+0.4,A@100+0.4,..."` | — | скриптованный ввод в CLI (время от `keys_parse`) |
| `DM_DOOM_TICKLOG=1` | 0 | раз в секунду `[tick] ... speed 0.96x` (проверка скорости игры) |
| `DM_DBG=1` | 0 | лог игры `[guest]` |
| `DM_FS_STATS=1` | 0 | статистика чтений файлов |

## 5. Патчи бинарника игры (CUTS, `dm/tools/patch_nso.py`, build-id `2847991952b3d7abd228624fcd3b0779`)

Генерация: `DM_TARGET=android PYTHONUTF8=1 python dm/tools/patch_nso.py work/exefs-update/main work/patched main` →
`work/patched/main.patched.bin` + `main.cuts.tsv`; на телефон в `DOOM Mobile/dm/out/`. Сейчас 19 записей CUTS (+ 5 zlib-гейтов, ниже):

| Адрес | Что |
|---|---|
| `0x1ddfba8` | `IsSystemUIShowing` → 0 |
| `0x1dcf4cc` | `UpdateTitleStorage`: всегда ветка «disabled» (оффлайн; чинит ввод в меню) |
| `0x107fd78`, `0x107fd84` | не застревать на `GDM_LOADING_PROFILE` |
| `0x1080110` | EULA считается принятой |
| `0x1080118` | без экрана TOS-уведомления об автосохранении |
| `0x1ed4e6c` | Frame: не отбрасывать события «чужого» устройства |
| `0x107bf48` | Shell::HandleEvent: без проверки master device |
| `0x10e7c50/54/60/64`, `0x10e0fc4/cc`, строки `0x35d6d52`, `0x39cd272` | Video: Chromatic Aberration → «Dynamic Resolution» (`rs_enable`, GOT `0x48d53b0`) |
| `0xf187d0` | главное меню без MULTIPLAYER |
| `0xcfaac` | `idDTLSWorkerThread::Run`: один раунд recv/yield за пробуждение в 1 мс вместо 9 (поток «DTLS port 27016» больше не жжёт ~9 % ядра) |
| (`0x1f08e3c`, закомментирован) | пропуск заставки — ломал экран загрузки |

OBS-гейты (наблюдение без изменения логики) для того же build-id — там же, таблица `OBS`.

### zlib: входные гейты inflate (`ZLIB_GATES`, с 30.10; `main.cuts.tsv` теперь 24 строки, `main.gates.tsv` + svc 0x90..0x94)

Игра несёт статически слинкованный **zlib 1.2.3** (raw deflate контейнеров, `ReadData`, фоновый загрузчик BGL, libpng).
Все вызовы — прямые `bl`/`b` в пять входов, указатели на них нигде не берутся. На каждый вход — гейт: хост либо отвечает
сам (`x8 = 1`, в `x0` результат), либо исполняется оригинал (`x8 = 0`; старый `libdm.so` без обработчика → всегда оригинал).
Хост: `dm/host/hle_zlib.c`, библиотека `dm/third_party/zlib-ng` (2.3.3, NEON, лицензия zlib). Сгенерировать
без гейтов: `DM_ZLIB_GATES=0` при запуске `patch_nso.py`.

| Адрес | svc | Функция |
|---|---|---|
| `0x1ffeb30` | 0x90 | `inflateInit2_` (ReadData `-15`, BGL) |
| `0x1ffecb0` | 0x91 | `inflateInit_` (libpng) |
| `0x1ffede0` | 0x92 | `inflate` |
| `0x20009f0` | 0x93 | `inflateEnd` |
| `0x1ffeac0` | 0x94 | `inflateReset` (libpng) |

| Переключатель | По умолч. | Смысл |
|---|---|---|
| `TU_DEBUG` (Turnip) | **`sysmem`** (с 30.10, ставит `vk_driver.c`, если не задан) | на Adreno 740 в тяжёлой сцене sysmem −20 % GPU на draw против GMEM с биннингом (сцена упирается в геометрию); на 750 одинаково. `TU_DEBUG=` (пусто) — пусть Turnip выбирает сам | — |
| `DM_ZLIB_FAST` | **1** (с 30.10) | inflate через zlib-ng вместо zlib 1.2.3 игры. Поток игры получает хостовый контекст в `strm->state`; поля `z_stream` 1.2.3 (LP64, 0x70 байт) копируются туда-обратно на каждом вызове. Коды возврата и раскодирование побайтно как у 1.2.3 (офлайн-сверка 1 514 записей / 959 МБ; на Mi 13 VERIFY 586 МБ — 0 расхождений); скорость на Mi 13 ×4 (437–577 против 113–134 МБ/с) | `0` — оригинальный zlib игры |
| `DM_ZLIB_STATS=1` | 0 | раз в `DM_ZLIB_STATS_SEC` (5) с: `[zlib] 5.0s game zlib 1.2.3\|zlib-ng: N calls, in/out MB, inflate ms (% одного ядра), MB/s \| streams/live \| threads: tid name ms (%)…` и `[zlib] total … by caller: ReadData / BGL / BGL-last / png / main+0x…`. При `FAST=0` хост сам вызывает оригинальный inflate через обход гейта и меряет его |
| `DM_ZLIB_VERIFY=<n>` | 0 | включает `FAST`; первые n потоков (`1` = 512) параллельно раскодируются zlib 1.2.3 игры на теневом потоке, каждый вызов сравнивается (код возврата, байты in/out, сами байты): `[zlib] VERIFY … mismatches`. Вызовы с выходным буфером > `DM_ZLIB_VERIFY_MAX_MB` (64) прекращают сверку потока (счётчик `stopped`) |

Строка `[zlib] inflate gates MISSING` при старте = на телефоне старый `main.patched.bin` без гейтов (переключатели не действуют).

## 6. Инструменты (`tools/`)

### Работа с телефоном (держат `phone_lock`)
| Скрипт | Что делает |
|---|---|
| `phone_lock.sh` | `source` + `phone_lock "<кто>"`: атомарный `mkdir work/phone.lock`, владелец в `owner`, мёртвый держатель перехватывается, дочерние прогоны серии переиспользуют лок. **Не спас от двух исполнителей на одном телефоне 28.10** — поэтому телефон ведёт только главный агент |
| `play_series.sh SECONDS "name\|ENV;LINES" ...` | серия A/B, пока хозяин играет сам: пишет `dm/env`, перезапускает игру, телефон сам пишет logcat (`logcat -f` через `setsid`, переживает обрывы Wi-Fi adb) и скриншот каждые 10 с; в конце — pull всего (pull посреди серии задерживал старт следующего конфига); `NO_SHOTS=1` — только лог (мобильный интернет). Нужен `ADB_SERIAL`. Решение хозяина: прогоны по 3 минуты, конфиги чередовать ×2 |
| `series_next.sh` | досрочно закончить текущий конфиг серии (хозяин уже ответил): убивает `sleep` серии |
| `diag_play.sh start\|stop NAME ["ENV"]` | диагностика во время игры хозяина: env по умолчанию (лог игры, fs stats, perf, alias log), logcat в файл, скрин раз в 10 с |
| `diag_run.sh NAME "ENV;LINES" [stand_s]` | графический прогон: вход с сохранения, скрин вперёд/вниз/вверх, стоять N с (180) со скрином раз в 30 с (стриминг VT/LOD) |
| `ab_run.sh NAME "ENV"` / `ab_series.sh HOST:PORT [install]` / `ab_report.py` | ранний A/B со скриптовым входом (`doom_enter.sh`), дамп G-буфера (serial 2), отчёт: медианы fps/GPU/submit/passes, % чёрных пикселей (дыры); серия переподключает Wi-Fi adb и пропускает недоступные прогоны |
| `perf_run.sh zctest\|install\|env\|start\|grab NAME` | замеры: тест zero-copy, установка, `dm/env`, старт, сбор logcat + PERF + 6 с simpleperf (dwarf) |
| `perf_trace.sh NAME` + `perfetto/doom.cfg`, `perfetto/analyze.py` | системный trace без root (sched_switch, cpu/gpu frequency) |
| `doom_enter.sh`, `phone_pad.sh` | автоввод: от запуска до геймплея с сохранения; нажатия экранного пада (2400×1080, density 420), `shot:`, `wait:` |
| `push_romfs.sh` | докачать недостающие/несовпавшие по размеру файлы RomFS (`adb push --sync` ненадёжен) |
| `run_emu.sh` | CLI-хост на эмуляторе/телефоне на N секунд, лог в `work/logs/` |

### Сборка
| Скрипт | Что делает |
|---|---|
| `build_apk.ps1 [-Install <serial>] [-Validation]` | `cmake --build work/build-android --target dm` → `libdm.so`, хуки adrenotools (`main_hook`, `file_redirect_hook`, `gsl_alloc_hook`, `hook_impl`), Gradle (`-PdmGame=doom -PdmAppId=org.doom.port`), подпись ключом `DM_KEYSTORE` (по умолчанию `~/.android/debug.keystore`) → `work/DOOM-port-debug.apk`. `-Validation` кладёт `libVkLayer_khronos_validation.so` (work/vvl 1.4.363.0). Ошибки сборки — в `work/logs/native-dm.log`, `native-hooks.log`, `gradle-apk.log`. **После `-Install` проверять, что APK действительно обновился** (были тихие неудачи установки) |
| `gen_vk_tramp.py [names.txt]` | генерирует `vk_tramp.S` + `vk_tramp_names.h`: трамплин на каждый vk*-вход libdm.so через `dm_vk_table`, чтобы выбирать драйвер (системный/Turnip) во время выполнения |
| `dm/tools/patch_nso.py` | офлайн-патчер NSO (x18/TLS/svc/cntpct, CUTS, OBS); проверка `dm/verify/equiv.py` |

### Разбор бинарника и данных
| Скрипт | Что делает |
|---|---|
| `extract_exefs_nsz.py`, `ncz2nsp.py`, `romfs_doom.py`, `nso_symbols.py` | NSZ → ExeFS / точный NSP; RomFS base+BKTR; распаковка NSO и импорты/экспорты |
| `doom_index.py`, `doom_shaders.py`, `doom_shader_jobs.py`, `build_dmsh_xlate_doom.py` | ресурсы id Tech 6 (.index/.pindex), шейдеры NVN `generated/nx/*.sb` → задания офлайн-транслятора → `.xgs` |
| `disa.py base\|update <hex> <len>`, `callers.py`, `fmatch.py`, `wmatch.py`, `vtables.py`, `symbt.py`, `ipc_reach.py` | дизассемблер (capstone), вызывающие, сопоставление функций base(v0, символы)→update (маскированное), vtable через релоки + RTTI, символизация бэктрейсов, достижимость IPC |
| `work/cvarscan2.py` | numpy-сканер adrp+add по `main.raw`: имена/умолчания/описания cvar'ов (joy_*, com_*, rs_*, aa_*) |
| `dump2png.py`, `gpass_report.py` | дампы кадра `dm/gfx-dump-now` → PNG; сводка `NVN GPASS` по классам проходов |

## 7. Полезные cvar'ы для изоляции проблем (через `DM_DOOM_ARGS`)

`r_skipFog 1`, `r_antialiasing 0`, `r_motionblur 0`, `r_geometryCulling 0`, `r_smallTriangleCulling 0`, `r_skipDecals 1`,
`r_skipFoliage 1`, `r_shadowRenderDepthCopy 0`, `vt_showPageLoads 1`, `vt_showPageMips 1`, `vt_maxPPF <n>`,
`vt_feedbackLodBias 1`, `com_showFPS 1` (HUD с `Tick-Adaptive NNHz`), `rs_showResolutionChanges 1`.
Метод, который сработал 28.10: хозяин играет, серия `play_series.sh` перебирает такие cvar'ы → по скриншотам видно,
какой проход ломается → субагент разбирает шейдер/код.
