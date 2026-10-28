# Ресёрч: BOTWRecompAndroid / LASRecompAndroid / LASRecomp — что полезно для DOOM-порта

Дата: 2026-10-28. Клоны (depth 200): `{BOTWRecompAndroid@5675618, LASRecompAndroid@9443aa3, LASRecomp@a4b53a3}`.
Пути ниже — относительно корня соответствующего репо (B = BOTWRecompAndroid, LA = LASRecompAndroid, L = LASRecomp).

## 0. Что это за проекты (коротко)

- L (peiikai) — static recomp Link's Awakening (Switch) → Win/Linux (Vulkan) / macOS (Metal). Код игры → C, NVN → Vulkan/Metal,
  Maxwell-шейдеры → GLSL/MSL **в рантайме** (`runtime/nvn/maxwell/translate.c`, ~2.5–3.2k строк, текст → shaderc → SPIR-V).
- LA (mamcjr) — Android-порт L; B — BOTW на том же движке (Android only). Устройства: POCO F7 Ultra (Adreno 830), Turnip
  Gen8 V37 / Mesa 26.3-devel; есть репорты с Adreno 740/750.
- Архитектурно **очень близко к нам** (нативный ARM64 гостевой код, HLE nn::, NVN→Vulkan replay), но шейдеры транслируются
  онлайн через GLSL, а не offline в SPIR-V. **Geometry/tessellation шейдеров нет вообще** (только VS/FS/CS) — GS-эмуляции
  там нет, по п.2 полезного мало.
- Лицензии: все три — **MIT** (`LICENSE`, "Copyright (c) 2026 Peiikai" + notice mamcjr). libadrenotools — BSD-2 (submodule
  bylaws/libadrenotools). SGSR1 — BSD-3 Qualcomm (`LA/third_party/sgsr/LICENSE`). FSR1/SMAA/ImGui — MIT. framegen.c (LA) —
  MIT, от Nycolazs/LASRecomp-Android-Plus. Ryujinx-порт в L — только аудио (MIT). → **Код можно брать с атрибуцией.**

## 1. Vulkan на Adreno/Android

### 1.1 Гостевая память = GPU-память: `VK_EXT_map_memory_placed` (Turnip) — ключевая находка
- `B/runtime/nvn/vulkan/exec.c` `vk_heap_place()` (~стр. 3725–3810), вызывается из `B/runtime/kernel.c` svc SetHeapSize (~стр. 927)
  **до того, как игра пишет в heap**. Создают VkBuffer (STORAGE|VERTEX|INDEX|UNIFORM|TRANSFER), выделяют HOST_VISIBLE|COHERENT
  (предпочтительно +CACHED) память и `vkMapMemory2KHR` с `VK_MEMORY_MAP_PLACED_BIT_EXT`, `pPlacedAddress = HEAP_BASE` — драйвер
  мапит GPU-память **поверх анонимных страниц гостевого heap по тому же VA**. Дальше шейдеры/вертексы/индексы/UBO читают
  гостевые байты напрямую, без копий. Коммит `LA a97bf61`: uploads ~320 MB → ~10 MB / 2 s, мощность 8.9 → 5.2 W.
- Device-фичи: `B/runtime/nvn/vulkan/device.c` ~546 и ~777 (`VK_KHR_map_memory2` + `VK_EXT_map_memory_placed`,
  `minPlacedMemoryMapAlignment`).
- **Qualcomm proprietary**: нет ни placed, ни `external_memory_host`. Испробовано и отвергнуто (`B/docs/BOTW-PORT.md` стр. ~611–623,
  коммит `B 4fc0d28`): `mremap` KGSL-маппинга → EINVAL; `OPAQUE_FD` + mmap → `INVALID_EXTERNAL_HANDLE`; AHB BLOB → gralloc
  отказывает (-129). Это совпадает с нашей заметкой "Adreno lacks ext_mem_host" — **но на Turnip путь есть**.
- Ловушка Adreno 7xx: на Turnip `maxStorageBufferRange` = **128 MB на Adreno 750** (2 GB на 830). Данные выше читались нулями
  (нет текста, неверный свет). Фикс — доступ к heap через `GL_EXT_buffer_reference` (BDA), адрес в отдельном маленьком
  буфере (binding 17): коммит `LA 1883b8d` (по указателю даже чуть быстрее: 10.86 vs 11.04 ms). Подтверждено на Adreno 750
  (`LA/README.md` стр. ~95, issue #3).
- Ловушка: S23 (Adreno 740) с Turnip + placed — Android LMK убивал процесс (`LA/README.md` ~181).
- Fallback без placed (Qualcomm): page write tracking через mprotect+SIGSEGV (`B/runtime/platform/android.c`
  `plat_written_pages`, коммит `B 734bb3e`): вертекс-кэш копирует только изменённые страницы, render-thread 37 → 18.5 ms.
  `pread` в fs заранее снимает защиту (иначе EFAULT).

### 1.2 Render pass / barriers / тайлер
- Dynamic rendering, **все attachments в `VK_IMAGE_LAYOUT_GENERAL`**, loadOp LOAD (CLEAR при первом/"свежем" использовании),
  storeOp STORE; **полный барьер в начале каждого пасса** (`B/exec.c` begin_pass ~1855). Стоимость барьеров измерена: ~1.7 ms/кадр
  на ~220 пассах (`B/docs/BOTW-PORT.md` Фаза 7), позже "без барьера — то же самое" (`B/docs/DESEMPENHO-60FPS-2026-10-23.md`).
- **Turnip autotune GMEM/sysmem**: G-buffer (6 RT + depth, ~3000 draws) Turnip сам гонит в **sysmem**; `TU_DEBUG=gmem` хуже
  в 2.5×, `sysmem` ≈ autotune. Вывод: на draw-heavy пассах тайлинг не выигрывает; split пасса стоит дёшево (≈0.5 ms) на Turnip.
  (`B/docs/DESEMPENHO-60FPS...` стр. 44, `B/docs/ANALISE-DRIVER-PADRAO...` таблица "TU_DEBUG", коммит `B 8feebcb`.)
- **Стоимость на Turnip — per-draw, не per-pixel**: ~1.6 µs фикс. на draw + вершинная работа; 50% разрешения GPU не меняет
  (`B 8feebcb`, `B f35b06a`). Это стоит проверить у нас (наши ~35 ms могут быть того же рода).
- **Дубликаты attachment'ов** (одна и та же image в двух color-слотах, один замаскирован): на тайлере Qualcomm стухшая копия
  слота перезаписывала результат → чёрные модели/нет альбедо. Фикс: держать привязанным только слот, который пишется по
  channel mask, другие `VK_NULL_HANDLE` (`B/exec.c` begin_pass `pass_dup_null` ~1925; `B/docs/ANALISE-DRIVER-PADRAO...` §"causa").
  **Проверить у DOOM** — UB по спеку, Turnip "угадывает" случайно.
- Feedback loops (draw читает RT, в который пишет): вместо копий — конец пасса + чтение in-place (только Turnip, т.к. без
  `VK_EXT_attachment_feedback_loop_layout` это UB): 32 пары полноэкранных копий (~14 ms GPU) → 31.2 → 40.5 fps
  (`B 9d0ea24`, `B 1725400`, `B/exec.c resolve_feedback` ~3233). Только-читаемый attachment сэмплируется без копии (`B d60a1a8`).
- Ленивые/алиасные полноэкранные color-копии, пропуск копий которые никто не читает (`LA d0b1c66`, ~1 ms).

### 1.3 UBWC / LRZ
- **UBWC**: mutable-format image **без `VkImageFormatListCreateInfo` никогда не сжимается в Turnip**. Фикс — перечислять формат
  и его sRGB/UNORM-двойник (`LA 4380fe5`, `textures.c vk_view_format`; на S20 FE 25.7 → 23.6 ms GPU). В B `TU_DEBUG=noubwc`
  дал лишь +1 ms.
- **UBWC-баг**: `texelFetch` из сжатой A2B10G10R10 на Turnip/830 возвращал блоки 16x4 нулей → чёрные штрихи; фикс — делать
  эту промежуточную image без сжатия (mutable) (`B b9321e7`, `B/exec.c` ~6417).
- **LRZ**: включается/выключается через `setenv("TU_DEBUG","nolrz")` **до загрузки драйвера** (`B/device.c` ui_window_init
  ~1698–1717; GPU-модель читают из `/sys/class/kgsl/kgsl-3d0/gpu_model`). Изначально LRZ отключали вне 8xx как подозреваемого
  в "пропадающих draw'ах"/артефактах на Adreno 750, **но причина оказалась в 128 MB storage range**; LRZ возвращён на 7xx
  (`LA 4f0bfb1`). `TU_DEBUG=nolrz` в BOTW по перфу ≈ 0 (`B/docs/BOTW-PORT.md` Фаза 7).
  → Для наших дыр: дешёвый тест `TU_DEBUG=nolrz` (если на Turnip), но сначала — п.2.1 (invariance).

### 1.4 Дескрипторы / пайплайны
- `UNIFORM_BUFFER_DYNAMIC` для UBO: offset в `vkCmdBindDescriptorSets`, set переиспользуется → время на sets −25% Turnip,
  −47% Qualcomm (`B 37aaefe`). Кэш сетов в пределах сабмита по 2×64-bit хешам записей (`LA 7cdb67e`). Не перебиндивать
  идентичный set (`B a824638`). Разнос UBO/текстур по разным сетам — GPU не изменился (`B` docs, ревертнуто).
- Per-slot command pool + `vkResetCommandPool` целиком вместо implicit reset в `vkBeginCommandBuffer` (`B/docs/ANALISE...` стр. ~377).
- Pipeline cache на диск с проверкой header/size (`B/device.c` ~830–880); SPIR-V кэш по хешу текста; strip OpName/OpLine
  (−16% SPIR-V), в GLSL только используемые helper'ы → ожидание компиляции 17.3 → 11.2 s (`B 9f94aba`). Компиляция шейдеров
  в фоне на N/2 воркерах (max 4).
- `VK_KHR_pipeline_executable_properties` (`LARECOMP_SHADER_STATS`) — регистры/waves/spills Adreno-компилятора на пайплайн
  (`B/device.c` ~540). **Очень полезный инструмент для нас.**

### 1.5 Swapchain / pacing
- Present mode: **MAILBOX по умолчанию** (fallback FIFO) — с FIFO всё стояло в очереди за acquire, и game fence wait следовал
  за композитором (4–21 fps при простаивающей GPU) (`B/device.c present_mode` ~886). Без pre-rotation (компоновщик крутит).
- **Fence игры не должен включать present**: `nvnQueueFenceSync` прикрепляли к следующему сабмиту (present, ждущий acquire) →
  SyncWait ~10 ms/кадр; привязали к последнему сабмиту с командами игры → ~1.4 ms (`LA 54686f6`). **Проверить у нас.**
- ADPF (`APerformanceHint`, API 33+, через dlsym) с wall-time сигналом, сессии на game и render thread
  (`B/runtime/platform/android.c` ~630–760, `LA c0b4443`, `LA/docs/ANDROID-OPTIMIZATIONS.md`); CPU-only сигнал хуже.
  uclamp.min через `sched_setattr` — экспериментально (`B edcbe12`). Пиннинг главного потока на prime-ядро ломал гонки в job
  system (`B` docs 26/10).
- GPU timestamp-счётчики игры (ReportCounter): игра считала простой GPU между нашими чанк-сабмитами как GPU-время → сидела в
  30 fps / резала dynres. Фикс: end-counter = start + busy-время (idle между сабмитами вычитается) (`LA 2d1ec05`,
  `B/exec.c cq_resolve` ~520). **DOOM имеет dynamic resolution по GPU-времени — проверить, что мы не скармливаем ему простои.**
- Своя dynamic resolution по реальному GPU busy (`B 625f8c3`, `B/runtime/nvn/output_scale.c`); FPS-пейсер, который заставляет
  игру выбрать свой 30-fps режим вместо slow-motion (`L/LA de59a87`).

## 2. Шейдеры (Maxwell → GLSL → SPIR-V)

### 2.1 Дыры в depth prepass → EQUAL (прямо наш симптом)
- `B/runtime/nvn/vulkan/shaders.c` ~64–115 `no_contraction()`: пост-проход по SPIR-V, вешает **`NoContraction` на каждую
  float add/sub/mul/div/fma/dot/vector-matrix операцию во всех vertex-шейдерах**. Комментарий: depth prepass и color pass —
  разные шейдеры, color тестирует EQUAL, любое различие во fusing даёт дыры ("foliage flickered black"); `precise gl_Position`
  не помогает, т.к. glslang не прослеживает регистровый код транслятора.
- Плюс `invariant gl_Position; precise gl_Position;` (`B/maxwell/translate.c` ~2816) и **spirv-opt выключен** — он переписывал
  prepass/color VS по-разному и глубины расходились (`shaders.c` комментарий над `vk_glsl_to_spirv`).
- Для нас: в DOOM prepass идёт через **GS**, main — без GS → позиция считается в разных стадиях/модулях. Нужно гарантировать
  побитово одинаковую позицию: NoContraction на всю арифметику, ведущую к gl_Position, во всех стадиях (VS и GS), без
  spirv-opt-переписываний, `Invariant` на Position. Кандидат №1 на причину "силуэт в prepass есть, в G-buffer нет".
- Ещё про точность: Switch-текстурный блок взвешивает bilinear с 8 битами sub-texel; Adreno точнее → щели на стыках heightmap-
  тайлов. Фикс — в VS snap UV к 1/256 текселя (`B 9c3a0f3`, `glsl_sample`). Может быть релевантно для любых VS-текстурных выборок.

### 2.2 Качество сгенерированного кода для Adreno-компилятора (огромные выигрыши)
- SSY/PBK/PCNT стеки как массивы → резолв статически при трансляции: GPU 18.5 → 13.5 ms (`LA 8017b9c`).
- Структурирование: if/else/for вместо `switch(pc)` state machine: регистры 24.7 → 16.4, waves 7.6 → 11.6 (`LA 24a0def`).
- BRX/jump table помечала каждую инструкцию label'ом → блок-границы, без планирования латентности; SSAO-пасс ~20 ms → <1 ms
  (`LA caa833c`).
- `uint lmem[256]`, обнуляемый в каждой инвокации → 1 KB scratch/thread; один draw 95–102 ms на 830. Фикс: размер local memory
  по фактическим доступам, power-of-two + маска (`LA 156b8a1` / `B fc0bb10`).
- Qualcomm proprietary компилятор падает/возвращает `VK_ERROR_UNKNOWN` на некоторых паттернах (бинарный поиск + кэш в большом
  CS → заменили линейным; structurizer выключен на Qualcomm) (`B/docs/ANALISE-DRIVER-PADRAO...`, `LA 327ce07`).
- Для нас: проверить наш offline SPIR-V на те же анти-паттерны (динамически индексируемые массивы-стеки, огромный local array,
  переходы с метками на каждой инструкции) через pipeline executable statistics.

### 2.3 Subgroup / форматы
- SHFL → `subgroupShuffle*` как есть; в VS — lane-local значение (нет subgroup ops в VS на Qualcomm). **Разницу warp 32 vs
  wave 64/128 Adreno не обрабатывают** (нет `requiredSubgroupSize`) — у них просто "работает" на их шейдерах; нам не образец.
  Факт из LA/docs/ANDROID-PORT.md ~134: Qualcomm — subgroup 64, только fragment/compute; Turnip — 64–128, все стадии.
- Textures: block-linear deswizzle на CPU (`B/runtime/nvn/tegra.c`), BC/ASTC нативно на Adreno, CPU-декод BC для Mali.
  Sampler с compare, используемый в обычном `texture()` → UB, делают plain-вариант (`vk_sampler_plain`).

## 3. Кастомный драйвер (Turnip через libadrenotools)

- `B/runtime/nvn/vulkan/android_driver.c` (≈75 строк, легко адаптировать):
  `adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hookLibDir=nativeLibraryDir, customDriverDir,
  driverName, NULL, NULL)` → `dlsym(vkGetInstanceProcAddr)` → `volkInitializeCustom(gipa)`. Перед использованием — пробный
  vkCreateInstance/EnumeratePhysicalDevices (Turnip-сборка для A7xx не видит A830 → fallback на `dlopen("libvulkan.so")`).
  Всё приложение через volk (`VK_NO_PROTOTYPES`), VMA с `VMA_DYNAMIC_VULKAN_FUNCTIONS`.
- Linker namespace — внутри adrenotools (свой namespace + hook-библиотеки из `nativeLibraryDir`), поэтому в gradle
  `useLegacyPackaging = true` (`B/android/botwrecompandroid/app/build.gradle.kts` ~59), CMake линкует `adrenotools`.
- Доставка драйвера: пользователь кладёт `.so` или adrenotools `.zip`/`.adpkg` (meta.json `libraryName`) в
  `<external files>/drivers`; Activity копирует во internal storage (dlopen отказывает с external) и передаёт путь через env
  `LARECOMP_VK_DRIVER` (`B/.../BOTWRecompAndroidActivity.java` ~4–115, ~1069–1140). Отдельный probe-APK для сравнения
  extension/feature драйверов (`B/android/probe`).
- Turnip vs Qualcomm (Adreno 830): Turnip быстрее везде у них — BOTW: Qualcomm render 33.7 ms / 13–15 fps vs Turnip ~30 fps;
  LA: Turnip 60 / Qualcomm 30 в оверворлде (`LA/README.md` ~316). Но главная причина — placed memory (на Qualcomm copy mode).
  Отдельно: в copy mode на 8 Elite "каждый draw ~1.6× медленнее", GPU clock низкий (`LA 2d1ec05`). Turnip Gen8 V37 ок на 830;
  Turnip v26.3.0-R6 рисует мусор на 830 (`LA fb37f0f`); v26.0.0 R8 — для A7xx (тестировалась на S23/740).
  Для Adreno 750 (наш) у них данных мало: OnePlus Pad 2 (750) — 55–60 fps LAS с LRZ on и placed+BDA (`LA 4f0bfb1`, `1883b8d`).

## 4. Потоки и CPU/GPU overlap

- **Render thread** (`B/runtime/nvn/vulkan/async.c`, ~240 строк): game thread только кладёт запросы EXEC/PRESENT/FENCE в очередь;
  трансляция NVN→Vulkan на отдельном потоке; **command list снапшотится при submit** (`snap_list`, игра может сразу
  перезаписать command memory); fence получает логический id, `nvnSyncWait` сначала ждёт, пока render thread дойдёт до него,
  потом GPU. Game thread 11.4 → 7.4 ms (`LA 89c9f44`). Ahead = 1 кадр.
- **Chunked submit**: на границе пасса, если с прошлого сабмита прошло >2 ms трансляции → `vkQueueSubmit` (GPU стартует раньше)
  (`B/exec.c` ~1830–1850, `LARECOMP_VK_CHUNK_US`). **Split длинного пасса** посреди (после draw, раз в 32 draw проверка часов,
  порог 3 ms, переоткрытие с LOAD): GPU idle в начале кадра 9 → 2.9 ms, 20 → 30 fps у BOTW (`B bf2369b`, `fcc810d`,
  `split_long_pass` ~6777). На Qualcomm оба выключены (GPU clock проседает в паузах между чанками, игра видит "длинный" GPU).
- **Sync lag** (= multi-frame overlap): ожидание самого нового fence заменяется ожиданием fence предыдущего кадра → игра пишет
  кадр N+1 пока GPU рендерит N. `B 82b71e2` (`runtime/nvn/objects.c nvnSyncWait`, +25 строк): 41.7 → 45.6 fps, GPU 15.6 →
  13.95 ms. Цена: CPU читает GPU-результаты (readback/queries/write-backs) на кадр старее → в BOTW "ноги Линка тонут"; честного
  быстрого варианта не нашли, вынесли в опцию (`B e2ae712`). Безопасно, потому что константы/UBO копируются при трансляции и
  command lists снапшотятся; опасно для данных, читаемых GPU in-place (вершины в heap).
- Upstream L делает это аккуратнее: **"Vulkan overlap"** (`L 1339812`, `523ad97`, `2b90025`, `3adfa30`): fence игры ждётся на кадр
  позже, а storage-диапазоны, которые CPU переписывает на кадр вперёд, получают снапшоты-копии (решение по диапазону: COPY /
  IN PLACE, probe по hash или OS page write tracking — GetWriteWatch / userfaultfd+PAGEMAP_SCAN), ring копий 16 MB в два полу-
  кольца, VK_SUBS 8 → 24. Тест-харнесс `L/tools/test_vulkan_overlap.py`.
- **Recorder thread** (`L/runtime/nvn/vulkan/record.c`, `L bb52932`, `c2bdc17`): транслятор пишет аргументы vkCmd*/descriptor
  writes/submit/present в пакетный ring, отдельный поток делает реальные Vulkan-вызовы (driver-часть = 1/4–1/3 времени
  render-потока на Steam Deck). Выключен на Mali (`L 76f4f21`). Это прямой рецепт против нашего "CPU submit 10–19 ms".
- Write-backs (GPU→CPU readback мелких RT): 128 малых + 32 больших слота, чтобы CPU не ждал GPU каждый второй кадр
  (`B e0df814`). UBO-копии в отдельный поток — не окупилось (`B` docs 26/10).
- Профилирование: `LARECOMP_FRAME_TIMELINE`, `LARECOMP_LOG_GPU_IDLE` (idle GPU после present / внутри кадра), A/B-переключатель в
  одном прогоне `LARECOMP_AB=<frames>:<key>` (нагрев телефона даёт разброс больше эффекта!), `simpleperf` на debuggable APK.

## 5. Upscaling / прочее
- FSR1 (EASU+RCAS) и **Snapdragon GSR1** (BSD-3, один пасс) — SGSR дешевле: 35.9 → 39.0 fps (`B 6a67bd8`, шейдеры в
  `B/runtime/nvn/vulkan/shaders.c`). Финальный present — bilinear (SGSR на 1.33× стоил 1.9 ms без видимой разницы, `B 60ac287`).
  Output "Auto": итоговое изображение ≤1080 строк — GPU 22.8 → 17.6 ms.
- Frame generation через depth-reprojection (`LA/runtime/nvn/vulkan/framegen.c`, ~950 строк, MIT) 30→60, ~0.4 ms GPU;
  нужны depth сцены, view-proj камеры, HUD-маска. Оценка применимости — `B/docs/FRAMEGEN-AVALIACAO.md`.
- `-O3`+ThinLTO хуже `-O2` (I-cache) (`B` docs 26/10).

## 6. Что взять нам (по приоритету)

| # | Что | Откуда | Ожидаемый эффект | Трудозатраты |
|---|---|---|---|---|
| 1 | NoContraction на всю float-арифметику VS **и GS** (SPIR-V пост-проход), `Invariant`+NoContraction для Position, никаких оптимизаций, переписывающих prepass/main по-разному | `B/runtime/nvn/vulkan/shaders.c` `no_contraction`, `translate.c` ~2816 | Вероятный фикс дыр prepass→G-buffer (EQUAL) | 0.5–1 день (функция ~40 строк, копируется почти как есть) |
| 2 | Sync lag / overlap: `nvnSyncWait` на newest fence → ждать fence предыдущего кадра; снапшот cmd-lists + копия UBO на трансляции (у нас, вероятно, уже есть); опция вкл/выкл | `B 82b71e2` (objects.c), полноценно — `L 1339812` | Снять сериализацию CPU/GPU, +10–25% fps | 1–2 дня (простой) / 1–2 нед (как в L со снапшотами storage) |
| 3 | Fence игры привязывать к последнему game-сабмиту, а не к present/acquire; MAILBOX если есть | `LA 54686f6`, `B/device.c present_mode` | Минус ms ожидания на кадр | 0.5 дня |
| 4 | Chunked submit на границе пасса (≈2 ms) + split длинного пасса (≈3 ms, re-open LOAD) | `B/exec.c` ~1830, `split_long_pass` ~6777 | GPU стартует раньше; −2…−6 ms latency кадра | 1–2 дня |
| 5 | GPU timing counters игры (dynres DOOM): busy-time без idle между нашими сабмитами | `LA 2d1ec05`, `B/exec.c cq_resolve` | Dynres не будет зря понижать разрешение/дёргаться | 1 день |
| 6 | Turnip через libadrenotools как опция (A/B против Qualcomm на 750), env `TU_DEBUG` до загрузки; если Turnip — `VK_EXT_map_memory_placed` для гостевого heap (zero-copy) + BDA из-за 128 MB storage range на 750 | `B/runtime/nvn/vulkan/android_driver.c`, `exec.c vk_heap_place`, `LA 1883b8d` | Потенциально самый большой выигрыш по памяти/копиям; zero-copy, которого на Qualcomm нет | 2–4 дня (загрузчик ~1 день; placed heap — зависит от нашей модели памяти) |
| 7 | Аудит наших шейдеров через `VK_KHR_pipeline_executable_properties` (регистры, waves, spills); искать стеки-массивы, большие local arrays, label-на-каждую-инструкцию | `B/device.c` ~540, `LA 8017b9c/24a0def/caa833c/156b8a1` | У них отдельные шейдеры ускорялись в 20–100×, общий GPU −25% | 1 день инструмент + по находкам |
| 8 | UBWC: `VkImageFormatListCreateInfo` для MUTABLE RT (и без mutable где можно); A2B10G10R10 c texelFetch — без сжатия | `LA 4380fe5`, `B b9321e7` | ~−2 ms GPU на Turnip; фикс артефактов | 0.5 дня |
| 9 | Проверка дубликатов одного image в нескольких color-слотах пасса; feedback-in-place вместо копий (Turnip) | `B/exec.c` begin_pass `pass_dup_null`, `resolve_feedback` | Корректность на тайлере; −мс за копии | 0.5–2 дня |
| 10 | Dynamic UBO + кэш descriptor sets, per-slot command pool reset | `B 37aaefe`, `LA 7cdb67e` | −CPU submit (sets −25…−47%) | 1–2 дня |
| 11 | Recorder thread (vkCmd* на отдельном потоке) | `L/runtime/nvn/vulkan/record.c` | −25–33% времени render thread | 3–5 дней |
| 12 | ADPF hint sessions (wall-time) для game и render thread | `B/runtime/platform/android.c` ~630–760 | Стабильнее клоки/пейсинг | 0.5 дня |
| 13 | SGSR1 вместо FSR1; финальный present bilinear; выход ≤1080p | `LA/third_party/sgsr`, `B/shaders.c`, `B 60ac287` | −1…−5 ms GPU на апскейле | 0.5–1 день |
| 14 | Инструменты: A/B в одном прогоне (`LARECOMP_AB`), GPU-idle лог, frame timeline | `B/exec.c vk_ab` ~6764, `cq_resolve` | Честные замеры при нагреве | 0.5 дня |

Не применимо / мало пользы: GS-эмуляции у них нет (GS→compute/мерж стадий придётся делать самим); subgroup-размер 32 vs 64/128
не решён; framegen — только как запасной вариант 30→60.

Важные числа для калибровки ожиданий: Turnip на Adreno 830 — ~1.6 µs фикс. стоимости на draw; ~190–230 пассов/кадр × 20–40 µs;
GMEM-режим хуже sysmem для G-buffer с тысячами draw; разрешение почти не влияет (per-draw bound).
