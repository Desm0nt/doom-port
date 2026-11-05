# Mali / PowerVR / Xclipse: что внедрено в движок (05.11)

Продолжение `non_adreno_drivers.md` (30.10, исследование) и `switch_emu_mali_powervr.md` (05.11, эмуляторы). Повод — force close
на Poco X7 Pro (Dimensity 8400, Mali-G720 MC7, драйвер r49p1). Тестового устройства нет: всё ниже собрано из профилей
vulkan.gpuinfo.org, исходников d2r-mobile / эмуляторов и чтения нашего кода, и сделано так, чтобы каждый путь можно было включить на
Adreno переключателем. Код: `dm/gfx/vulkan/engine/xbg_vendor_policy.h` (новый), `xbg_game.cpp` (метки `[nonadreno]`),
`dm/android/vulkan_present.cpp` (ожидание fence). Тест: `engine/tests/vendor_policy_test.cpp`. Сборка: `work/build-android-mali`.
Бэкапы исходных файлов: `work/bak_mali/`.

## 0. Данные устройств (gpuinfo, проверено субагентом через JSON API)

| | G720 r49 (44439 = Poco X7 Pro) | G57 r54 (52927) | G57 r32 (52984) | BXM-8-256 25.1 (52993) | BXM 1.15 (52802) | DXT-48 (52657, Pixel 10) |
|---|---|---|---|---|---|---|
| geometryShader / gatherExtended | 1 / 1 | 1 / 1 | 1 / 1 | 1 / 1 | 1 / **0** | 1 / 1 |
| textureCompressionBC | **0** | **0** | **0** | **0** | **0** | **0** (биты BC1/3/4/5 есть) |
| depthBounds / fillModeNonSolid | **0 / 0** | **0 / 0** | **0 / 0** | **0 / 0** | **0 / 0** | **0** / 1 |
| clip/cull distance | 0/0 | 0/0 | 0/0 | 1/1 | 1/1 | 1/1 |
| subgroup | 16, только FS+CS, quad не во всех стадиях | 16 | 16 | 128 (4–128) | 128 | 128 (32–128) |
| LINEAR у R16G16_UNORM / RGBA16_UNORM | **нет** | **нет** | **нет** | есть | есть | есть |
| LINEAR у depth | все | все | все | только D16 | ни у одного | только D16 |
| maxTexelBufferElements | 2^28 | 2^28 | 2^28 | **65536** | **65536** | **65536** |
| robustness2.nullDescriptor | **нет расширения** | 1 | нет | 1 | нет | 1 |
| shared mem / invocations | 32 KB / 1024 | 32 KB / 512 | 32 KB / 512 | 16 KB / 1024 | 16 KB / 512 | 32 KB / 1024 |
| D24S8 | есть | есть | есть | есть | есть | есть |
| VERTEX_BUFFER у наших вершинных форматов (vk 37/78/85/91/103/106) | все | все | все | все | все | все |

Что используют шейдеры DOOM (863 бандла, `work/shaders-vk`, скрипт в scratchpad): GS 6, CS 35; shared ≤ 2 КБ, workgroup ≤ 256,
ClipDistance нет, VS-выходов ≤ 10 локаций, все входы VS — vec4. Значит лимиты shared memory / invocations / varyings / clip distance
нам не мешают даже на старом BXM. Вершинные форматы из capture-логов: R8G8B8A8_UNORM, R32G32B32_SFLOAT, **R16G16B16_SNORM**,
R16G16_SNORM, R32G32_SFLOAT, R16G16B16A16_UNORM — все с VERTEX_BUFFER на всех шести профилях.

## 1. Внедрено (по проблемам)

Формат: источник → что было → что сейчас → переключатель → как проверить на Adreno → риск.

### 1.1 Сэмплер на формате без линейной фильтрации (самый вероятный fatal на G720)
- Источник: gpuinfo 44439 (R16G16_UNORM `0xcd83` без LINEAR), d2r-mobile 0733eb4 + c386573 (MediaTek-Mali падал на 13-й секунде),
  https://github.com/jhaizhou-ops/d2r-mobile/commit/0733eb4 .
- Было: `xbg_fatalf("NVN sampler linear filtering unsupported …")`. DOOM сэмплит RGBA16_UNORM (vk 91) и RG16.
- Сейчас: min/mag/mip → NEAREST, анизотропия выкл., лог `NVN sampler: format N … NEAREST` раз на формат. Срабатывает только там, где
  раньше был fatal (условие то же: min/mag LINEAR).
- `DM_GFX_SAMPLER_STRICT=1` — вернуть fatal. Проверка: `DM_GFX_SIM_GPU=nofilter16` (или `mali`), `depthnolinear` (PowerVR).
- Риск: ступенчатость там, где фильтровался 16-битный UNORM; тени без PCF-сглаживания на PowerVR.

### 1.2 depthBounds / fillModeNonSolid / depthBiasClamp
- Источник: gpuinfo (depthBounds=0 везде на Mali/PowerVR). Было: `need(...)` → fatal при первом таком draw.
- Сейчас: depth bounds test снимается (лог раз), polygon mode → FILL, bias clamp → 0.
- Проверка: `DM_GFX_SIM_GPU=nodepthbounds,nofillmode`. Если в логе есть `NVN depth bounds test requested` — DOOM его использует;
  смотреть свет/туман на предмет лишнего затенения.
- Риск: depth bounds у idTech — оптимизация (объёмы света), но если какой-то проход использует его для корректности, будут
  лишние пиксели.

### 1.3 Пайплайны: ошибки, VK_INCOMPLETE, краши компилятора, потокобезопасность
- Источники: Eden PR #3383 (`VK_INCOMPLETE` на Mali, `vulkan_wrapper.cpp` `Device::CreateGraphicsPipeline`); d2r-mobile 2428e5b / fec76d1
  (graphics-pipeguard, careful mode); forums.imgtec.com t/4334 (компилятор BXM не потокобезопасен), t/4280 (Pixel 10: >400 мс/PSO, OOM).
- Было: `VK(vkCreateGraphicsPipelines)` → fatal; graphics-краши компилятора не ловились; warm-up строит в нескольких потоках.
- Сейчас (`namespace gpguard`):
  - ошибка графического пайплайна → комбинация `<vs>+<fs>…` в skip, draw пропускается, лог; `VK_INCOMPLETE` с хэндлом = успех
    (и для compute);
  - `DM_GFX_PIPE_GUARD` (умолч. не-Adreno): маркер `gfx-gpipe-inflight.<n>` на поток вокруг сборки непроверенной комбинации
    (в `DM_FILES_DIR`, внутреннее хранилище — без FUSE-задержек). Запуск, нашедший маркеры, собирает по одному (careful) и
    запоминает подозреваемых; комбинация в полёте на двух краш-запусках подряд → `gfx-gpipe-skip`, её draw и warm-up пропускаются;
  - `DM_GFX_PIPE_SERIAL` (умолч. PowerVR): мьютекс вокруг всех `vkCreate*Pipelines` (draw, warm-up, async, внутренние clear/bright).
- Проверка: `DM_GFX_PIPE_GUARD=1` (маркеры появляются/исчезают в `files/`), `DM_GFX_PIPE_SERIAL=1` (warm-up медленнее, лог
  `pipe_serial=1`). Краш компилятора на Adreno не симулируется; можно положить строку `<vs>+<fs>` в `files/gfx-gpipe-skip` и увидеть
  `NVN draws with graphics pipeline … skipped`.
- Риск: пропущенный draw = пропавший эффект; первый краш всё равно роняет запуск (как compute-pipeguard).

### 1.4 vkWaitForFences → VK_ERROR_INITIALIZATION_FAILED (Mali)
- Источник: Skyline `fence_cycle.h`, коммит cbc896c8f8 (ppoll не перезапускается после сигнала); у нас сигналов много (SIGSEGV
  write-tracking). Было: `VK(...)` → fatal (слоты, legacy wait) / ошибка презента.
- Сейчас: обёртка `dm_wait_for_fences` (xbg_game.cpp, включая `xbg_tex_discard.inc`) и `dm_present_wait_fences` (vulkan_present.cpp):
  повтор до 1000 раз, лог первых 4. Другие коды без изменений.
- Проверка на Adreno невозможна (драйвер так не отвечает); на Mali — строка `interrupted wait: retried` в логе.

### 1.5 Null storage image / storage texel buffer без nullDescriptor
- Источник: yuzu PR #12110 (идея), gpuinfo: у G720 r49 нет robustness2. Было: `need(...,"Null writable image descriptor needs robustness2")`.
- Сейчас: отдельная заглушка (1×1 storage-образ RGBA8/RGBA32UI/SI или scratch-буфер 64 КБ, не общий с нулевыми sampled-ресурсами),
  лог `NVN defined null writable … resource`. Раньше это был fatal на любом GPU, так что на Adreno, где он не случался, ничего не меняется.
- Риск: запись в заглушку теряется (как и задумано).

### 1.6 Usage образа, которое драйвер не принимает целиком
- Было: `VK(vkGetPhysicalDeviceImageFormatProperties)` → fatal. Сейчас: повтор без STORAGE, затем без COLOR/DEPTH attachment, лог.
  Срабатывает только на прежнем fatal.

### 1.7 Texel-буферы > maxTexelBufferElements (PowerVR 65536)
- Сейчас: `range` вью режется до целых текселей лимита, лог 4 раза. Adreno/Mali (2^27–2^28) не затрагиваются.
- Проверка: `DM_GFX_SIM_GPU=texel64k` (и игре через `NVN_DEVICE_INFO_MAX_BUFFER_TEXTURE_SIZE` уходит 65536). Если в логе есть
  `exceeds maxTexelBufferElements` — на PowerVR что-то из этого будет рисоваться неверно.

### 1.8 Вершинный формат без VERTEX_BUFFER
- Источник: yuzu `GetFormatAlternatives`. По gpuinfo на Mali/PowerVR не нужно; нужно AMD-подобным (Xclipse/RADV).
- Сейчас: 3-компонентный формат → 4-компонентный того же типа (4-й канал = следующие байты, а не 1), лог раз на формат; раньше fatal.
- Проверка: `DM_GFX_SIM_GPU=novertex3` (входит в `xclipse`). Ожидаемы артефакты там, где шейдер читает w у R16G16B16_SNORM.
- Риск: точная эмуляция требует SPIR-V-варианта VS с w=1 по маске локаций (не сделано).

### 1.9 Политики по семейству (`xbg_vendor_policy.h`; на Adreno = старые умолчания, `vendor_policy_test` это фиксирует)
| Политика | Adreno | Mali | PowerVR | Xclipse/прочие | Почему |
|---|---|---|---|---|---|
| zero-copy (`DM_GFX_ZEROCOPY`) | 1 | 0 | 0 | 0 | когерентность CPU-кэшируемых AHB-страниц проверена только на Adreno 750 (`work/zc_test`) |
| `DM_GFX_PIPE_GUARD` | 0 | 1 | 1 | 1 | §1.3 |
| `DM_GFX_PIPE_SERIAL` | 0 | 0 | 1 | 0 | компилятор IMG не потокобезопасен |
| `DM_GFX_UBO_DYNAMIC` | 1 | 1 | 0 | 1 | PPSSPP `PVR_UNUSED_DYNAMIC_UBO_OVERRUN` (page fault на BXM) |
| `DM_GFX_NOOUT_MASK` | 0 | 1 | 1 | 1 | FS без выходов: Mali пишет 0 в цвет (d2r-mobile #179, PR #229) |
- `DM_GFX_READBACK_CACHED` по семейству **не** меняется: Skia/Eden `DriverBugs.md` — cached readback на Mali медленнее.

### 1.10 Quad-рерайт производных
- `DM_GFX_QUAD_DERIVATIVES=1` ставит APK всем. Теперь выключается, если в FS нет quad-операций или subgroup < 4 (PowerVR BXM-4-64:
  size 1). На Adreno условия выполнены. Проверка: `DM_GFX_SIM_GPU=subgroup1` → лог `quad shuffles … off`.

### 1.11 Симуляция `DM_GFX_SIM_GPU` (главный инструмент проверки)
- Профиль сужает `cfg.enabled_core`, `subgroup`, `properties.limits` и ответы `vkGetPhysicalDeviceFormatProperties` (макрос-обёртка
  `dm_sim_format_properties` на весь xbg_game.cpp). Устройство создано со всеми фичами, так что использовать меньше — всегда валидно.
- `mali`: нет BC → CPU-декод bcdec (как Maleoon), RG16/RGBA16 UNORM без LINEAR → BC4/5 в 8 бит и NEAREST-сэмплеры, subgroup 16 только
  FS+CS → lone-lane и для частиц `cec755c4` (вместо warp32), без depth bounds / fill mode, + умолчания Mali (zero-copy off, guard,
  noout mask).
- `powervr`: нет BC, depth без LINEAR кроме D16, SRGB без storage, E5B9G9R9 только sampled, subgroup 1 (quad-рерайт выкл.), texel 64K,
  + умолчания PowerVR (serial, без dynamic UBO, guard, noout mask, zero-copy off).
- `xclipse`: BC4–7 нет (BC1–3 нативно), D24 нет → D32-заместители (`[nonadreno] D24 stand-ins`), 3-компонентные вершины без
  VERTEX_BUFFER.
- `nogs` отдельно: GS-модули unusable (`module_gap`), culling-GS снимается pass-through'ом; при `r_geometryCulling 1` draw с GS
  пропускаются.

### 1.12a BC без BC: заливка пачками, декод на GPU, грязные блоки (issue #6, 05.11, BUGS §2.24)
- Падение POCO X6 Pro (G615): вся декодированная текстура шла одним staging → abort арены на 256 МБ (357 МБ = BC7 8192² 5 мипов в
  RGBA8 / BC6H 8192x4096 5 мипов в RGBA16F — физкэши VT). `BcUpload` (`xbg_game.cpp`) режет на пачки ≤ `DM_GFX_BC_BATCH_MB` (32):
  уровни, полосы строк блоков, грязные GOB-блоки. Не-декодированные текстуры — пачками только если > 256 МБ (Adreno как был).
- `DM_GFX_BC_DECODE=gpu` (умолч.): сырые блоки → `xbg_bc_decode.comp` (BC6H/BC7 — порт Granite 69f450c62dc3, MIT; BC1–5 — наша
  целочисленная арифметика) → device-local буфер 32 МБ → `vkCmdCopyBufferToImage`. Те же байты, что CPU (`engine/tests/bc_gpu_decode_test.cpp`,
  GLSL компилируется как C++ через `tests/glsl_shim.h`; все форматы, все моды BC6H/BC7, 300k блоков на формат). `=cpu` — старый bcdec.
- Кэши VT меняются **на лету** (WriteTexels, до 32 страниц/кадр): декодированные текстуры теперь тоже идут через `partial_upload`
  (`DM_GFX_BC_PARTIAL=1`) — декодируются только грязные блоки, раньше — вся текстура на каждое изменение.
- `DM_GFX_BC6_E5=1`: беззнаковый BC6H → `E5B9G9R9` (4 Б/пикс. вместо 8). VRAM VT-кэшей на Mali по логам Adreno: 27 BC7 357 МБ, 1880
  BC6H 179 МБ (было бы 357), 94 BC6H куб-массив ≈ 503 МБ (было бы ≈ 1006), 103 BC3 192 МБ; ≈ 1.23 ГБ против ≈ 0.31 ГБ нативного BC.
  Строка на устройство: `NVN BC decode vram=… native_would_be=…`.
- ASTC/ETC2 на лету — нет (это повторное сжатие: GPU-энкодер ASTC 4x4 LDR реален, ~1–5 мс/кадр на страницы VT, но двойные потери;
  BC6H требует ASTC HDR); запланирован дисковый ASTC-кэш: вход `BcUpload::blocks` (тут же хэш сырых блоков), формат — `bc_store`,
  объём — `NVN BC totals raw= decoded=` / `NVN PERF bcdecode … total … first …`.

### 1.12 Строка диагностики
`NVN fallbacks on this device: …` — все включённые фолбэки (BC CPU-декод N/7 форматов, no-geometry-shader, no-depth-bounds,
rg16/rgba16-unorm-nearest, depth-nearest, d24-as-d32, no-formatless-storage, texel-buffer-clamp, subgroup-lone-lane, cpu-timestamps).
На Adreno 740/750 ожидается только `subgroup-lone-lane` (+ `depth-nearest` на Adreno 730 512.615). Плюс `NVN gpu family=… policy
family=… sim=…` и `NVN gpu policy: …`.

## 2. Уже было до этого (30.10, BUGS §2.13), проверено, что работает вместе
GS без `geometryShader` (unusable + pass-through), formatless storage через format_feature_flags2, D24 → D32 (Xclipse),
gatherExtended → без offset, timestamps без валидных бит, dynamic UBO по лимиту, lone-lane по ширине subgroup и стадиям,
compute-pipeguard, MUTABLE только по необходимости + `VkImageFormatListCreateInfo` (AFBC на Valhall всё равно не теряется от
MUTABLE — Arm Best Practices §8.9), BC per-format CPU-декод, BC4/5 в 8 бит без LINEAR у RG16.

## 3. Не сделано и почему
- ~~**GPU-декод BC**~~ — сделан 05.11 (§1.12a): через буфер и копию, поэтому SRGB-storage не нужен; VT-атлас — грязными блоками.
- **Culling-GS при `r_geometryCulling 1` без GS** — GS пишет компактный индекс-буфер (gs_cull_fix.md), нужна compute-эмуляция; по
  умолчанию culling выключен и GS снимается.
- **Точная эмуляция 3-компонентных вершин** (w=1) — не нужна Mali/PowerVR по gpuinfo.
- **PowerVR: пайплайн против прохода с теми же load/store op** (Skia/Eden DriverBugs P2) и не уничтожать пайплайны в кадре (UE, P3) —
  после первого лога с PowerVR.
- **Occlusion-запросы на тайлерах** без синхронного чтения (yuzu dbb9e0c5fe) — нужен аудит пути запросов.
- **Quirk-таблица по `driverID`/версии** (Skyline QuirkManager) — `VkPhysicalDeviceDriverProperties` пока не читаем; семейство по vendorID.
- Maleoon-набор env (`napi_ohos.c`) для Android — в `jni_android.c` (зона другого агента).

## 4. План проверки на Adreno (Mi 13 / Mi 14), всё через `dm/env`
1. Базовый прогон без переменных: лог `NVN gpu family=adreno … sim=0`, `NVN gpu policy: pipe_serial=0 pipe_guard=0 ubo_dynamic=1
   noout_mask=0 zero-copy default=1`, `NVN zero-copy guest memory=1`, `NVN fallbacks on this device: subgroup-lone-lane`; fps как в v0.4.x.
2. `DM_GFX_SIM_GPU=mali`: меню → уровень → 2–3 мин. игры. В логе `bc-gpu-decode(7/7)`, `NVN BC decode device=… -> decoder=gpu
   bc6=e5b9g9r9`, `NVN BC decode: pipeline fmt=…`, `NVN chunked texture upload … mode=gpu`, `NVN partial texture upload … decoded=gpu`,
   `NVN BC decode vram=…` (повтор с `DM_GFX_BC_DECODE=cpu` — та же картинка), `NVN sampler: format 91/77 … NEAREST` (если
   встречаются), `zero-copy guest memory=0`, lone-lane для `cec755c4`, `depth bounds … drawn without it` (если используется). Смотреть:
   нет fatal, текстуры на месте (VT-страницы подгружаются, возможен статтер), RSS процесса (память декода BC), fps.
3. `DM_GFX_SIM_GPU=powervr`: то же + `pipe_serial=1 ubo_dynamic=0`, `quad shuffles … off`, тени (NEAREST на D24/D32), прогрев дольше.
4. `DM_GFX_SIM_GPU=xclipse`: `D24S8 -> D32_SFLOAT_S8_UINT` в логе, тени и глубина; вероятны артефакты от `novertex3` (ожидаемо).
5. `DM_GFX_SIM_GPU=mali,nogs`: GS-модули unusable, игра идёт (culling выключен).
6. Точечно: `DM_GFX_PIPE_GUARD=1` (маркеры `files/gfx-gpipe-inflight.*` пропадают после прогрева), `DM_GFX_NOOUT_MASK=1` (картинка
   без изменений), `DM_GFX_ZEROCOPY=0`, `DM_GFX_UBO_DYNAMIC=0`, `DM_GFX_PIPE_SERIAL=1`.
7. После каждого прогона удалить `dm/env`; при неожиданной `gfx-gpipe-skip` в `files/` — удалить её.

## 5. Риски
Без устройства: ни один путь не исполнялся на Mali/PowerVR. Главные: CPU-декод BC (статтер, память), depth bounds снят, NEAREST на
16-битных/depth-форматах, zero-copy off = медленнее CPU-путь, первый краш компилятора всё равно роняет запуск. Первый лог с Mali
нужен ради `NVN caps`, `NVN fallbacks`, `NVN gpu policy` и того, на чём он умер (crash-лог 2.22).
