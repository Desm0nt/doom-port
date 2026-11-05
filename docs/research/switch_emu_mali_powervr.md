# Switch-эмуляторы на Mali / PowerVR / Xclipse: что они чинят и что взять нам (05.11)

Только исследование, код не правился. Дополняет `non_adreno_drivers.md` (30.10), повторы оттуда не расписаны, только
отмечены. Сопоставлено с нашим кодом: `dm/gfx/vulkan/engine/xbg_game.cpp`, `xbg_vulkan.cpp`, а также с
`xbg_vendor_policy.h`, который сейчас пишет другой агент (симуляция Mali/PowerVR, политики по семейству GPU).

## 0. Источники и доступность

| Проект | Где читали | Состояние |
|---|---|---|
| yuzu (архив) | история целиком есть в **eden-emulator/mirror** (коммиты yuzu 2023–2024 с исходными SHA) | `yuzu-emu/yuzu`, `yuzu-mirror/yuzu` и `pineappleEA/pineapple-src` на GitHub закрыты по DMCA (HTTP 451) |
| Eden | `github.com/eden-emulator/mirror` @ `5df599128d68` (master, 04.11.2026), PR-ы на git.eden-emu.dev | активен |
| Citron | `github.com/citron-neo/emulator` @ `a37026c483ff` (main, 04.11.2026) | активен |
| suyu / Torzu / Sudachi | `suyu-emu/suyu` и `litucks/torzu` закрыты по DMCA; Sudachi на GitHub есть только как форки под iOS | не читали: это форки yuzu, своих правок под Mali в них не нашли |
| Skyline | `skyline-emu/skyline` теперь содержит только README с уведомлением об архивации (18.08.2026) | вся история есть в Strato |
| Strato (преемник Skyline) | `github.com/strato-emu/strato` @ `ae1566a48285`, `strato-emu/shader-compiler` @ `8717c8f6d7de` | заморожен (последний коммит 09.2024) |
| Granite (не эмулятор, взят ради GPU-декодеров BCn) | `github.com/Themaister/Granite` @ `69f450c62dc3` | MIT, активен |

Главные выводы:
1. **Под PowerVR в Switch-эмуляторах нет ни одного обходного пути.** Есть только строка с именем драйвера, VK_IMG_filter_cubic в
   Eden и одна строка, перенесённая в Eden `docs/DriverBugs.md` из Skia. PowerVR там не поддерживают, и подглядеть нечего.
2. **Список для Mali короткий и устоялся к 2023 году.** По прогресс-репорту yuzu за июнь 2023, драйвер Mali
   «much better at following the Vulkan specification than others». Почти всё, что они чинили, — это фичи, которых нет
   (multiViewport, nullDescriptor, BC, scaled-форматы вершин, debug_utils, stencil export, geometryStreams), а не баги драйвера.
3. **Для нашего force close важны четыре находки, которых нет в `non_adreno_drivers.md`**:
   (a) `vkWaitForFences` на Mali возвращает `VK_ERROR_INITIALIZATION_FAILED`, если ppoll прерван сигналом (Skyline). Наш `VK()`
   на это делает fatal, а сигналов у нас много (mprotect/SIGSEGV dirty-tracking);
   (b) `vkCreateGraphicsPipelines` возвращает `VK_INCOMPLETE` (Mali, Adreno 5xx; Eden 06.2026). У нас это fatal или провал warm-up;
   (c) в нашей таблице вершинных форматов есть 3-компонентные `R8G8B8_*`/`R16G16B16_*`, а `xbg_game.cpp:5148/5153` делает
   `need(VERTEX_BUFFER_BIT)`, то есть fatal. yuzu держит для таких форматов таблицу альтернатив и эмулирует SCALED;
   (d) без `nullDescriptor` yuzu подставляет пустые ресурсы. Мы делаем так же, но **null storage image у нас fatal**
   (`xbg_game.cpp:2987`).
4. **Готовых GPU compute-декодеров BCn в Switch-эмуляторах нет.** yuzu, Eden, Citron и Skyline декодируют BCn на CPU
   (`bc_decoder` из Skyline, MPL-2.0, производная от SwiftShader) в те же целевые форматы, что и наш bcdec. Готовые
   compute-шейдеры BC1–7 под MIT есть в Granite (§4).

## 1. Лицензии (проект может перейти на GPL-3.0-or-later)

| Источник | Лицензия файлов | Что делать при переносе |
|---|---|---|
| yuzu (код 2018–2024 в Eden/Citron) | `SPDX-License-Identifier: GPL-2.0-or-later`, `Copyright 20xx yuzu Emulator Project` | совместима с GPL-3.0-or-later; сохранить SPDX-строку и копирайт |
| Изменения Eden | двойной заголовок: `Copyright 2026 Eden Emulator Project` / `GPL-3.0-or-later` + исходная строка yuzu | у затронутого файла итог GPL-3.0-or-later |
| Изменения Citron | `Copyright 2025 citron Emulator Project`; `vulkan_device.cpp` — GPL-2.0-or-later, `vk_query_cache.cpp` — GPL-3.0-or-later | то же |
| Skyline/Strato C++ (`trait_manager.*`, `texture.cpp`, `fence_cycle.h`, `command_nodes.cpp`) | `SPDX-License-Identifier: MPL-2.0`, `Copyright © 2021/2022 Skyline Team and Contributors` | MPL-2.0 — файловый копилефт. Пометки «Incompatible With Secondary Licenses» нет, значит, по MPL §3.3 файл можно объединять с GPL-3. Перенесённый файл остаётся под MPL (заголовок оставить), в NOTICE указать источник. Весь репозиторий Strato объявлен GPL-3.0 (`LICENSE.md`), но заголовки файлов — MPL-2.0 |
| `strato-emu/shader-compiler` | у репозитория MPL-2.0, **но файлы — yuzu `GPL-2.0-or-later`** (`Copyright 2021 yuzu Emulator Project`) | считать GPL |
| `externals/bc_decoder` (yuzu/Eden ← Skyline) | MPL-2.0 + `Copyright 2019 The SwiftShader Authors` (Apache-2.0 derivative) | Apache-2.0 совместима только с GPL-3; нам не нужен, bcdec закрывает ту же задачу |
| Eden `host_shaders/astc_decoder.comp` | GPL-3.0-or-later (Eden) + GPL-2.0-or-later (yuzu, 2021) | ASTC на Mali/PowerVR аппаратный, переносить незачем |
| Granite `assets/shaders/decode/*.comp`, `vulkan/texture/texture_decoder.cpp` | MIT, `Copyright (c) 2017/2020-2026 Hans-Kristian Arntzen` | годится при любой лицензии проекта, нужен только текст MIT в THIRD_PARTY |

Ниже в колонке «Код / перенос» указано, насколько фрагмент самодостаточен.

## 2. Таблицы обходных путей

Ссылки: `E:` = `https://github.com/eden-emulator/mirror/blob/5df599128d68/`, `C:` = `https://github.com/citron-neo/emulator/blob/a37026c483ff/`,
`S:` = `https://github.com/strato-emu/strato/blob/ae1566a48285/`, коммиты — `https://github.com/<repo>/commit/<sha>`.
Детект во всех случаях идёт через `VkPhysicalDeviceDriverProperties.driverID`: `VK_DRIVER_ID_ARM_PROPRIETARY`=9,
`IMAGINATION_PROPRIETARY`=7, `SAMSUNG_PROPRIETARY`=21. Версию драйвера проверяет только Skyline (`driverVersion` в
формате `VK_MAKE_VERSION(r, p, 0)`, т. е. r42 = `VK_MAKE_VERSION(42,0,0)`).

### 2.1 ARM Mali (стоковый драйвер)

| # | Драйвер / версия | Симптом | Детект | Обходной путь | Источник (файл, функция, коммит) | Код / перенос | Значение для нашего движка |
|---|---|---|---|---|---|---|---|
| M1 | Mali, все версии (Skyline: только < r42) | `VK_EXT_extended_dynamic_state` «not fully functional»: нет картинки (yuzu); не работают vertex binding strides (Skyline) | driverID ARM; Skyline: `driverVersion < VK_MAKE_VERSION(42,0,0)` | yuzu/Citron: убрать EDS1 и EDS2 и держать всё состояние в ключе пайплайна; Skyline: снять только EDS при < r42 | yuzu `ca2c3a6d5a` (03.06.2023), `0875e158fe` (EDS2, PR #10790); `C:src/video_core/vulkan_common/vulkan_device.cpp#L566-L571` (`Device::Device`); Skyline `750dfb8f00`, `S:app/src/main/cpp/skyline/gpu/trait_manager.cpp#L263-L271` (`QuirkManager::QuirkManager`). В Eden автоматическое отключение позже убрали, теперь это пользовательская настройка `dyna_state` (`E:src/video_core/vulkan_common/vulkan_device.cpp#L661`) | 5 строк | **Не затрагивает**: динамические у нас только VIEWPORT/SCISSOR (`xbg_game.cpp:5105/5318`). Правило: EDS на Mali не вводить, а если когда-то понадобится, то только при ≥ r42 |
| M2 | Mali (и Adreno blob) | `*_USCALED`/`*_SSCALED` вершинные форматы не поддерживаются | флаг `must_emulate_scaled_formats` при is_arm | атрибут объявляется как UINT/SINT, `Profile.support_scaled_attributes=false`, перевод во float делает шейдер | `E:src/video_core/renderer_vulkan/maxwell_to_vk.cpp#L382-L390` (`VertexFormat`), `E:src/video_core/vulkan_common/vulkan_device.cpp` (`Alternatives::R16G16B16_SSCALED`, `R8G8B8_SSCALED`, `R16G16B16_SFLOAT`, `R32G32B32_SFLOAT` → 4-компонентные, функция `GetFormatAlternatives`), `Device::GetSupportedFormat` | таблица альтернатив и `GetSupportedFormat` (~60 строк) самодостаточны; для эмуляции SCALED нужен свой SPIR-V-рерайт входа VS | **Высокий риск fatal.** `format()` (`xbg_game.cpp:2125`) отдаёт `R8G8B8_*` и `R16G16B16_*`, а на `:5148` (constant attribute) и `:5153` стоит `need(...VERTEX_BUFFER_BIT,"Vertex format unsupported")`. Используются ли такие форматы, покажет `capability_report` и лог `ATTRIBUTE ... format=`. Варианты исправления: формат на 4 компоненты, если `offset+size4 ≤ stride`; иначе vertex pulling (SSBO/texel buffer + загрузка в VS) |
| M3 | Mali (`multiViewport=0`, `maxViewports=1`) | креш или нестабильность при `viewportCount>1` и записи ViewportIndex | `features.multiViewport`, `limits.maxViewports` | `viewportCount=min(maxViewports,16)`; запись ViewportIndex выбрасывается, capability `MultiViewport` не объявляется | PR #10837 (`088017b179`, 28.06.2023): `vk_graphics_pipeline.cpp` (`GraphicsPipeline::MakePipeline`, сейчас `E:...vk_graphics_pipeline.cpp#L804-L811`), `vk_rasterizer.cpp` `UpdateViewportsState/UpdateScissorsState` (`#L1303-L1381`); PR #12083 `da57f07bce` (`emit_spirv.cpp` `SetupCapabilities`, `emit_spirv_context_get_set.cpp` `OutputAttrPointer`) | 5–10 строк на месте | **Сейчас ок**: наши пайплайны — `viewportCount=1` (`:5090`), в статистике 863 модулей нет `MultiViewport`/`ShaderViewportIndex`. Дёшево подстраховаться: `validate_capabilities` для cap 57 (MultiViewport) и 5254 (ShaderViewportIndexLayerEXT) при `!multiViewport` выдаёт gap вместо fatal |
| M4 | Mali (старые r3x; на 5th gen robustness2 обычно есть) | без `robustness2.nullDescriptor` нельзя привязать `VK_NULL_HANDLE` (SSBU не запускался) | `features.robustness2.nullDescriptor` | создать 1×1 `A8B8G8R8_UNORM` образ и вью для всех типов текстур; null buffer — реальный буфер, вью с `offset=0,size=0`; у null buffer обязателен `VERTEX_BUFFER_BIT` | PR #12110 `3efaed605c` (`ImageView::ImageView(TextureCacheRuntime&, NullImageViewParams)`, `E:src/video_core/renderer_vulkan/vk_texture_cache.cpp#L2624-L2640`); PR #12410 `d73d8e8471` (`Buffer::Buffer(…, NullBufferParams)`, `BufferCacheRuntime::CreateNullBuffer`, `E:...vk_buffer_cache.cpp#L95-L131,#L727-L760`); Skyline `S:.../interconnect/maxwell_3d/active_state.cpp#L47` (null vertex buffer → пустой кусок megabuffer) | идея на 20 строк, к их TextureCache не привязана | **Частично есть**: `null_texture()` (`xbg_game.cpp:2984`) уже раздаёт нулевые ресурсы, но для `STORAGE_IMAGE`/`STORAGE_TEXEL_BUFFER` делает fatal «needs robustness2 nullDescriptor». Нужна заглушка 1×1 `R32_UINT`/`RGBA8` с usage STORAGE плюс буфер на 16 байт |
| M5 | Mali (все), а также «не всё железо» | GS с capability `GeometryStreams` или `OpEmitStreamVertex` не компилируется. Skyline сначала вообще выключил compute на Mali (`a16383fd4b`, 03.12.2022), настоящей причиной оказались streams | `transformFeedback.geometryStreams` | в обычном GS использовать `OpEmitVertex`/`OpEndPrimitive`, `GeometryStreams` не объявлять; yuzu: без поддержки бросает исключение | Skyline shader-compiler `6c258c31ff` (09.02.2023, `EmitEmitVertex/EmitEndPrimitive`), Skyline `ffdd50bdf3` «Fix geometry and compute shaders on mali GPUs»; yuzu PR #13075 `091abdc5cb` (`Profile.support_geometry_streams`, `E:src/shader_recompiler/backend/spirv/emit_spirv_special.cpp#L156-L175`) | 10 строк | **Сейчас ок**: у наших 6 GS в статистике только `Geometry` (cap 2). Защита: при cap 63 `GeometryStreams` и `!geometryStreams` выдавать gap (или переписывать `OpEmitStreamVertex` (220) → `OpEmitVertex` (218), а `OpEndStreamPrimitive` (221) → `OpEndPrimitive` (219) для stream 0) |
| M6 | Mali, 2022 (r3x; более новые версии не перепроверялись) | `vkWaitForFences` возвращает `VK_ERROR_INITIALIZATION_FAILED`: драйвер ждёт через `ppoll()`, который не перезапускается после сигнала | код возврата | цикл: `VK_TIMEOUT` и `VK_ERROR_INITIALIZATION_FAILED` → повтор, остальное — ошибка | Skyline `cbc896c8f8` (14.07.2022), `S:app/src/main/cpp/skyline/gpu/fence_cycle.h#L160-L170` (`FenceCycle::Wait`) | 8 строк, перенос тривиален | **Кандидат на force close.** Сигналов у нас много: mprotect-трекинг (`dirty_protect_im`), гостевые потоки nxnative, ART. Ожидания: `xbg_game.cpp:1136`, `:1393` (`VK()` → fatal), `:6372`, `:6402`, `xbg_vulkan.cpp:202`, `xbg_tex_discard.inc:125`. Нужна одна обёртка `wait_fences_retry()`. Тот же приём для `vkWaitSemaphores`/`vkQueueWaitIdle` |
| M7 | «некоторые неизвестные» драйверы Mali, Adreno 5xx | `vkCreateGraphicsPipelines` возвращает `VK_INCOMPLETE` (вне спецификации), хотя пайплайн создан | код возврата | считать «почти успехом»: взять `object` (может быть `VK_NULL_HANDLE`) | Eden PR #3383 `5027aecf77` (02.06.2026), `E:src/video_core/vulkan_common/vulkan_wrapper.cpp#L758-L768` (`Device::CreateGraphicsPipeline`) | 4 строки | **Кандидат на fatal**: `xbg_game.cpp:3828` (`VK(vkCreateGraphicsPipelines…)`), `:5320`, warm-up `:3677` (там просто `failed++`). Принимать `VK_INCOMPLETE`, если `pl != VK_NULL_HANDLE`; если хэндл пустой — в pipeguard/skip |
| M8 | Mali | `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` выключает AFBC | driverID ARM | `vkImageMutableFormatCostly=true`: MUTABLE только по необходимости, иначе предупреждение в лог | Skyline `750dfb8f00`, `S:.../trait_manager.cpp#L268`, `S:.../gpu/texture/texture.cpp#L600,#L880` | — | **Уже покрыто** (`non_adreno_drivers.md` §3.3.4, §3.3.9): у нас MUTABLE + `VkImageFormatListCreateInfo` (`xbg_game.cpp:2849, 4128-4138`). На Mali стоит взять тот же умолчательный `no_mutable_images`, что и на Adreno |
| M9 | Mali | Skyline помечает `brokenSpirvAccessChainOpt` («LLVM misopt») | driverID ARM | **флаг нигде не используется** (поиск по Strato и shader-compiler): обходится только `has_broken_spirv_vector_access_chain`, и только на Qualcomm | Skyline `7bfe63f679` (02.04.2023); shader-compiler `b774a83ee6` (`EmitGetCbufU32x2`, `EmitLoadStorage64/128` разбиты на скалярные загрузки) | — | Подтверждения нет. Если на Mali появится мусор в cbuf/SSBO при динамическом индексе вектора, первым пробовать этот рерайт: u32x2/x4 → скалярные `OpAccessChain` |
| M10 | Mali, Adreno, Turnip | блокирующее чтение результатов occlusion-запросов на тайлерах сильно тормозит; host conditional rendering ломается или висит | driverID | yuzu: на ARM/QCOM/Turnip выкидывать `pending_sync`, `HostConditionalRenderingCompareValues` всегда рисует; Citron: `use_cpu_counter` — счётчик на CPU вместо host query | yuzu `dbb9e0c5fe` (15.11.2023, «Query Cache: Disable write syncing on Android»), `E:src/video_core/renderer_vulkan/vk_query_cache.cpp#L238-L244,#L1519-L1525`; `C:src/video_core/renderer_vulkan/vk_query_cache.cpp#L143-L150` (`SamplesStreamer` ctor) | логика привязана к их QueryCache; переносить идею | Если игра читает occlusion-результаты синхронно (`occlusion_device_probe` + путь запросов в `xbg_game.cpp`), на Mali и PowerVR делать то же, что на Adreno: результат с задержкой на кадр и без `vkGetQueryPoolResults(WAIT)` в кадре |
| M11 | Mali, Qualcomm | у Maxwell 48 КБ shared memory, у Mali `maxComputeSharedMemorySize` часто 32 КБ: пайплайн не создаётся | driverID + лимит | урезать `shared_memory_size` до лимита и залогировать (некорректно, но не падает) | Eden `E:src/video_core/renderer_vulkan/vk_pipeline_cache.cpp#L997-L1009` (`PipelineCache::CreateComputePipeline`) | 10 строк | У нас 35 CS: проверить `Workgroup`-переменные транслированных модулей против лимита и при превышении логировать gap. Урезать только по флагу, данные могут испортиться |
| M12 | Mali, Qualcomm | лимит динамических SSBO на сет | driverID | `limit_dynamic_storage_buffers` → `maxDescriptorSetStorageBuffersDynamic` | `E:src/video_core/renderer_vulkan/vk_buffer_cache.cpp#L361-L366` | 5 строк | **Покрыто для UBO** (`non_adreno` §3.2, Maleoon 8). Для SSBO_DYNAMIC проверить, используем ли их |
| M13 | Mali, Turnip (2022) | проходы выполняются не по порядку без зависимости `VK_SUBPASS_EXTERNAL→0` (на NVIDIA и Adreno она неявная) | — | добавлять зависимость ALL_GRAPHICS / MEMORY_READ+WRITE в каждый render pass | Skyline `22ce531e6f` (14.04.2022), `S:app/src/main/cpp/skyline/gpu/interconnect/command_nodes.cpp#L97-L110` (`RenderPassNode::RenderPassNode`) | — | **Покрыто**: `xbg_game.cpp:4720` (`dep[0]/dep[1]`). Warm-up (`make_pass`, `:3640`) берёт зависимости из записи, а должен строить точно такие же |
| M14 | Mali | битые текстуры при копировании образа в staging: барьер `TOP_OF_PIPE` ничего не ждёт | — | src stage `BOTTOM_OF_PIPE`/`HOST` вместо `TOP_OF_PIPE` | Skyline `98b366c1f5` (`Texture::CopyIntoStagingBuffer/CopyFromStagingBuffer`) | — | Проверить `full_barrier()` и барьеры readback: `srcStage=TOP_OF_PIPE` с непустым access mask на Mali реально не синхронизирует |
| M15 | Mali | BC нет | `textureCompressionBC` / format features | CPU-декод в `R8`/`R8G8`/`A8B8G8R8(_SRGB)`/`R16G16B16A16_SFLOAT` (для BC6H) | yuzu PR #10837: `E:src/video_core/renderer_vulkan/maxwell_to_vk.cpp#L266-L285` (`SurfaceFormat`), `E:src/video_core/texture_cache/decode_bc.cpp` (`DecompressBCn`), `E:externals/bc_decoder/`; Skyline `S:.../gpu/texture/texture.cpp#L518-L560` (`ConvertHostCompatibleFormat`) | — | **Покрыто** (`non_adreno` §3.2, bcdec), набор форматов тот же. Отличие: Skyline считает BC «поддержанным», если есть **хоть какой-нибудь** бит формата (Adreno 512.6xx занижает список), — нам не нужно, у нас строгая проверка |
| M16 | Mali (r3x) | нет `VK_EXT_debug_utils` | instance extensions | откат на `VK_EXT_debug_report` | PR #10837 `vulkan_instance.cpp` (`RequiredExtensions`), `renderer_vulkan.cpp` (`MakeDebugCallback`) | — | У нас debug_utils только при VVL и с проверкой `mk!=nullptr` (`android/vulkan_present.cpp:701`), ок |
| M17 | Mali | нет `VK_EXT_shader_stencil_export`, и blit depth+stencil через шейдер невозможен | extension | пропускать такой blit (yuzu `BlitImageHelper::BlitDepthStencil` — `return`) | PR #10837 `blit_image.cpp` | — | Проверить, пишут ли наши конверсии D16↔D24↔D32 (`xbg_depth.c`, `depth16_as_d24`) stencil через `FragStencilRefEXT`: грепом не нашлось, значит, скорее всего, ок |
| M18 | Mali | без расширения TFB нельзя ставить `TRANSFORM_FEEDBACK_BUFFER_BIT` в usage буфера | extension | ставить usage только при наличии расширения | PR #10837 `vk_staging_buffer_pool.cpp` | — | У нас `arena_usage` (`xbg_game.cpp:482`) без TFB, ок |
| M19 | Mali | `support_float_controls`, `support_vote`, demote брались константой `true` | `IsKhrShaderFloatControlsSupported`, `subgroup.supportedOperations & VOTE`, `shaderDemoteToHelperInvocation`; базовая SPIR-V — 1.3 вместо 1.0 | брать из устройства | PR #10837 `vk_pipeline_cache.cpp` (сейчас `E:...vk_pipeline_cache.cpp#L379-L470`) | — | `validate_capabilities` (`xbg_game.cpp:3135`) проверяет capability и subgroup-операции. Нужно добавить проверку execution modes float controls (`DenormPreserve`/`RoundingModeRTE` против `VkPhysicalDeviceFloatControlsProperties`) и cap 5379 `DemoteToHelperInvocation` |
| M20 | Mali (и все мобильные) | драйвер «unsuitable» (нет `shaderClipDistance`, `logicOp`, `wideLines` и т. п.) | `GetSuitability()` | на мобильных драйверах — `LOG_WARNING("Unsuitable driver, continuing anyway")` вместо исключения | PR #10837 (`is_arm` добавлен в список); `C:src/video_core/vulkan_common/vulkan_device.cpp#L442-L459` | — | **Частично покрыто** (`cap_soft`/gap для GS, `XBG_SIM_NOCLIP` и т. п. в `xbg_vendor_policy.h`). Принцип: fatal только когда модуль/пайплайн реально нужен в кадре |
| M21 | Mali (из Skia/Chromium, приведено в Eden `docs/DriverBugs.md`) | cached-память при readback **медленнее** coherent: `__pi___inval_cache_range` ест CPU | — | для readback брать coherent uncached | `E:docs/DriverBugs.md` (строка «Cached memory is significantly slower…») | — | ⚠ **Противоречит** совету из `non_adreno` §3.3.9 (`DM_GFX_READBACK_CACHED=1`, пришёл с Maleoon). На Mali не включать cached по умолчанию, сначала замерить |
| M22 | Mali (Skia, b/359822580) | после `DEVICE_LOST` ожидание fence возвращается сразу | — | вместо fence — `vkQueueWaitIdle` перед освобождением ресурсов | `E:docs/DriverBugs.md` | — | Для crash-guard (`non_adreno` §4.3): на DEVICE_LOST не освобождать ресурсы по fence |
| M23 | Mali (Skia, старые драйверы) | dynamic stencil write mask ломается при discard/A2C, если статическое значение 0; RGBA_F32 mip-цепочки битые | — | — | `E:docs/DriverBugs.md` | — | Dynamic stencil мы не используем; RGBA32F с мипами — проверить, если увидим артефакты |
| M24 | Mali, тайлеры | MSAA выгоднее резолвить внутри render pass | `Device::IsTiler()` (QCOM/ARM/Samsung/Turnip) | resolve attachment в проходе вместо отдельного blit | `E:src/video_core/vulkan_common/vulkan_device.h#L325-L335`, `vk_texture_cache.cpp#L3028-L3036`, `vk_graphics_pipeline.cpp#L141` | — | DOOM рендерит без MSAA (TAA), не актуально |
| M25 | Mali | приоритет очереди выше HIGH не разрешён | driverID | `maxGlobalPriority=eHigh` | `S:.../trait_manager.cpp#L269` | — | `VK_EXT_global_priority` мы не используем |
| M26 | Mali (UI) | сторонний драйвер ставить некуда | — | кнопка установки драйвера скрыта на не-Qualcomm | yuzu PR #10864 `1d3609ab7c` | — | **Покрыто** (`non_adreno` §2: на не-Adreno по умолчанию системный драйвер) |

### 2.2 Samsung Xclipse (стоковый, AMD RDNA)

| # | Симптом | Детект | Обходной путь | Источник | Значение для нас |
|---|---|---|---|---|---|
| X1 | сломан `extendedDynamicState3ColorBlendEnable/Equation` (как у AMD) | driverID SAMSUNG | снять эти две фичи | Eden `389327d3f0` (14.04.2025), `E:src/video_core/vulkan_common/vulkan_device.cpp#L1273-L1281` (`Device::GetSuitability`) | EDS3 мы не используем |
| X2 | на AMD-подобных драйверах ломается demote (discard) в ветвлениях | driverID AMD/RADV/SAMSUNG | `HostTranslateInfo.needs_demote_reorder` → `DemoteCombinationPass()`: все demote сводятся в один в конце шейдера | Eden `31200d0c7a` (01.02.2024); `E:src/shader_recompiler/frontend/maxwell/structured_control_flow.cpp#L665-L667` | Если на Xclipse пропадут alpha-test/foliage — SPIR-V-рерайт по той же идее (флаг + `OpStore` в bool, один `OpDemote`/`OpKill` в конце). Делать только при симптоме |
| X3 | нет D24 | `supports_d24_depth` (формат D24S8 без DEPTH_STENCIL_ATTACHMENT) | `GetFormatAlternatives`: D24S8 → D32S8 → D16S8, X8D24 → D32 → D16, S8 → D16S8/D24S8/D32S8 | `E:.../vulkan_device.cpp` `namespace Alternatives`, `GetFormatAlternatives`, `GetSupportedFormat` | **Покрыто** (`non_adreno` §3.3.3, `XBG_SIM_NOD24`). Таблица yuzu годится как готовый порядок перебора |
| X4 | B10G11R11 без STORAGE/attachment | format features | альтернативы `B10G11R11_UFLOAT` → `R16G16B16A16_SFLOAT` → `A8B8G8R8_SRGB` | там же | К `non_adreno` §3.3.4 (HDR-таргет B10G11R11 + вью R32_UINT): если STORAGE нет — RGBA16F |

### 2.3 PowerVR (IMG)

| # | Симптом | Источник | Значение для нас |
|---|---|---|---|
| P1 | драйвер хочет `VK_IMG_filter_cubic` вместо EXT-алиаса | Eden PR #3643 `2991bd18ef` (01.03.2026) | cubic-фильтр мы не используем |
| P2 | несовместимость render pass при разных load/store op, хотя по спецификации они совместимы (Skia) | `E:docs/DriverBugs.md`, строка «Imagination … 'clear' loadop renderpass» | ⚠ Warm-up собирает пайплайны против записанного прохода (`make_pass`), а рисует в проходе с другими load op (CLEAR/LOAD). На PowerVR создавать пайплайн против прохода с точно такими же load/store op или держать отдельный ключ. Сначала проверить, есть ли проблема на устройстве |
| P3 | Pixel 10 (DXT-48-1536), Android 16/17: SIGABRT/SIGTRAP в `IMG_vkDestroyPipeline` (UE 5.6, 22.07.2026, Epic считает это багом драйвера) | forums.unrealengine.com, тема 2835883 | Не уничтожать пайплайны до выхода (наш кэш так и делает), не вызывать `vkDestroyPipeline` во время кадра |
| P4 | в yuzu/Eden/Citron/Skyline больше ничего про PowerVR нет | поиск кода и коммитов (`IMAGINATION`, `PowerVR`) | Всё по PowerVR в `non_adreno` §1.5 и в `xbg_vendor_policy.h` (`pipe_serial`, без dynamic UBO, subgroup 1) — **это больше, чем есть у эмуляторов** |

### 2.4 Ложный след

Тема на форуме Arm «Mali-G925 49.1.0 SIGSEGV в vkWaitForFences после compute + swapchain» (Tab S11) **закрыта
автором**: у него был use-after-free VkDevice в приложении, а не баг драйвера. На неё не ссылаться.

## 3. Перенос: что брать кодом, что идеей

| Что | Файлы / функции | Зависимости от их инфраструктуры | Оценка переноса |
|---|---|---|---|
| Таблица драйверных квирков (Skyline) | `S:app/src/main/cpp/skyline/gpu/trait_manager.{h,cpp}`: `TraitManager::QuirkManager::QuirkManager`, `Summary()` | `vk::raii`/vulkan.hpp, `fmt`, `adrenotools` (для BCeNabler); сама логика — switch по driverID | 1–2 ч: переписать на C поверх `VkPhysicalDeviceDriverProperties`. Удобнее сразу дописать в `xbg_vendor_policy.h` (`XbgGpuFamily` уже есть) |
| Детект квирков yuzu/Citron | `Device::Device` (`C:…/vulkan_device.cpp#L425-L760`), `GetSuitability`, `RemoveUnsuitableExtensions`, `GetFormatAlternatives`, `GetSupportedFormat` | макросы `FOR_EACH_VK_*`, `Settings::values`, `LOG_*`, VMA | брать таблицы и условия (≈150 строк), макросную обвязку не тащить |
| Fence retry (M6) | `S:…/gpu/fence_cycle.h` `FenceCycle::Wait` | vulkan.hpp | 10 строк, переписать |
| VK_INCOMPLETE (M7) | `E:…/vulkan_wrapper.cpp` `Device::CreateGraphicsPipeline` | их `vk::Check` | 4 строки |
| Null resources (M4) | `E:…/vk_texture_cache.cpp` `ImageView(…NullImageViewParams)`, `E:…/vk_buffer_cache.cpp` `CreateNullBuffer` | `MakeImage`/`MemoryAllocator`/`Scheduler` | идея, у нас 30 строк в `null_texture()` |
| CPU BCn (M15) | `E:src/video_core/texture_cache/decode_bc.cpp`, `E:externals/bc_decoder/bc_decoder.{h,cpp}` | `Extent3D`, `PixelFormat` | не нужно: bcdec уже есть |
| Scaled / 3-comp вершины (M2) | `E:…/maxwell_to_vk.cpp` `VertexFormat`; SPIR-V-конверсия в `spirv_emit_context.cpp` (`support_scaled_attributes`) | IR их рекомпилятора | таблица — 1 ч; конверсия на входе VS — наш SPIR-V-проход (0.5–1 дн.) |
| Maxwell → SPIR-V как CLI | Eden `tools/maxwell-spirv/spirv_recompiler_impl.cpp`, `tools/maxwell-ir/` | весь `shader_recompiler` | для сравнения с нашим транслятором, не для встраивания (GPL-3) |

## 4. GPU compute-декод BCn (ради чего спрашивали)

- **Switch-эмуляторы**: GPU-декодера BCn нет нигде. yuzu и Eden на устройстве без `textureCompressionBC` ставят флаги
  `Converted|CostlyLoad` и декодируют на CPU при загрузке (`E:…/vk_texture_cache.cpp#L1898`, `texture_cache/util.cpp`
  `ConvertImage` → `DecompressBCn`). На GPU у Eden только `block_linear_unswizzle_3d_bcn.comp` (деswizzle, не декод)
  и `astc_decoder.comp` (ASTC, GPL; Mali/PowerVR умеют ASTC сами).
- **Granite (MIT)** — лучший кандидат: `assets/shaders/decode/s3tc.comp` (BC1–3), `rgtc.comp` + `rgtc.h` (BC4/5),
  `bc6.comp`, `bc7.comp` (а также `etc2.comp`, `eac.comp`, `astc.comp`), обвязка `vulkan/texture/texture_decoder.cpp`
  (`decode_compressed_image`, `dispatch_kernel_*`, `compressed_format_to_payload_format`, `to_storage_format`).
  - Схема: блоки загружаются как `utexture2D` формата `R32G32_UINT` (8-байтовые блоки) или `R32G32B32A32_UINT` (16-байтовые),
    один тексель на блок 4×4. Вывод идёт в `image2D` (storage) формата `rgba8`/`r8`/`rg8` UNORM или UINT-алиас,
    BC6 → `R16G16B16A16_UINT`-алиас для RGBA16F. Push constant `resolution`, specialization constants `USE_ALPHA`/`BC_VERSION`,
    `local_size 4×4×4` (s3tc).
  - Зависимости: GLSL 450 + `GL_EXT_samplerless_texture_functions`. Хост-код завязан на `Vulkan::CommandBuffer` из Granite —
    переписать на наш `vkrec` примерно за 200 строк. Шейдеры компилируются в SPIR-V офлайн, как наши clear/bright.
  - Подводные камни у нас: SRGB-образ нельзя писать как storage — нужен MUTABLE + format list с UNORM (на Mali это
    теряет AFBC, как UBWC на Adreno) или декод в отдельный UNORM-образ и копия; для `R8G8B8A8_SRGB` у PowerVR нет STORAGE
    (`XBG_SIM_PVRFORMATS`). Для VT-страниц DOOM декодировать прямо в страницу физического атласа.
  - Оценка: 2–4 дня с тестом «GPU vs bcdec» (у Granite есть `tests/texture_decoder_test.cpp`).

## 5. Что внедрить у нас (top 15, по приоритету)

Метки: **[NEW]** — в `non_adreno_drivers.md` нет; **[COVERED §x]** — там уже есть; **[WIP]** — уже делает другой агент
(`xbg_vendor_policy.h`).

| # | Что | Почему (источник) | Где у нас | Оценка | Метка |
|---|---|---|---|---|---|
| 1 | Обёртка ожидания fence: повтор на `VK_TIMEOUT` и `VK_ERROR_INITIALIZATION_FAILED` (и для `vkWaitSemaphores`) | M6, Skyline | `xbg_game.cpp:1136,1393,6372,6402`, `xbg_vulkan.cpp:202`, `xbg_tex_discard.inc:125` | 0.5 ч | **[NEW]** |
| 2 | `VK_INCOMPLETE` от `vkCreate*Pipelines` при непустом хэндле — успех, при пустом — pipeguard skip | M7, Eden #3383 | `:3677`, `:3828`, `:5320` | 0.5 ч | **[NEW]** |
| 3 | Вершинные форматы: вместо `need(VERTEX_BUFFER_BIT)` — таблица альтернатив (3-comp → 4-comp при `offset+4c ≤ stride`; SCALED → INT + конверсия в VS); сначала собрать с Mali лог `ATTRIBUTE format=` | M2, yuzu `GetFormatAlternatives` / `must_emulate_scaled_formats` | `xbg_game.cpp:2125` (`format()`), `:5148`, `:5153` | 2–6 ч (widening); 1–2 дн. (vertex pulling) | **[NEW]** |
| 4 | Null storage image / storage texel buffer без `nullDescriptor`: заглушка вместо fatal | M4, yuzu #12110/#12410 | `null_texture()` `:2984-2987` | 2–3 ч | **[NEW]** |
| 5 | Политика «unsuitable → continue»: все проверки фич в `validate_capabilities` превращаются в gap/skip модуля, fatal — только если модуль нужен в кадре | M20 | `xbg_game.cpp:3135` | 0.5 дн. | **[WIP]** (cap_soft для GS) / **[COVERED §3.3.1]** |
| 6 | В `validate_capabilities` проверять float controls execution modes, `DemoteToHelperInvocation`, `GeometryStreams` (63), `MultiViewport` (57), `ShaderViewportIndexLayerEXT` (5254) против фич, при необходимости переписывать (stream 0 → `OpEmitVertex`) | M3, M5, M19 | `:3135` | 2–3 ч | **[NEW]** |
| 7 | Occlusion-запросы на тайлерах: без синхронного `vkGetQueryPoolResults(WAIT)` в кадре, результат с задержкой (как на Adreno) | M10, yuzu `dbb9e0c5fe`, Citron `use_cpu_counter` | путь запросов в `xbg_game.cpp` | 0.5–1 дн. (сначала аудит) | **[NEW]** |
| 8 | Проверить `Workgroup`-память 35 CS против `maxComputeSharedMemorySize` (Mali 32 КБ), логировать gap | M11, Eden | загрузчик модулей | 1 ч | **[NEW]** |
| 9 | Readback-память на Mali: не включать `DM_GFX_READBACK_CACHED` по умолчанию, замерить cached и coherent | M21 (Skia / Eden DriverBugs) | `xbg_vendor_policy.h` (дефолты по семейству) | 1 ч + замер | **[NEW]**, ⚠ противоречит `non_adreno` §3.3.9 |
| 10 | GPU compute-декод BC1–7 на шейдерах Granite (MIT) | §4 | `xbg_texture.c`, BC-путь `xbg_game.cpp` ~2081 | 2–4 дн. | **[COVERED §3.3.5]** как идея; источник кода **[NEW]** |
| 11 | Таблица альтернатив depth/HDR-форматов из yuzu (D24S8 → D32S8 → D16S8; X8D24 → D32 → D16; B10G11R11 → RGBA16F) | X3, X4 | таблица форматов `xbg_game.cpp` ~1425, HDR-таргет `:3095` | 0.5–1 дн. | **[COVERED §3.3.3/§3.3.4]** (порядок перебора — новый) |
| 12 | Квирки по драйверу с версией (по образцу Skyline `QuirkManager` + `Summary()` в лог): EDS < r42, mutable costly, max priority и т. п. | §3 | `xbg_vendor_policy.h` | 1–2 ч | **[WIP]** |
| 13 | DEVICE_LOST на Mali: `vkQueueWaitIdle` перед освобождением ресурсов, не доверять fence | M22 | обработка ошибок submit / crash-guard | 1 ч | частично **[COVERED §4.3]** |
| 14 | PowerVR: пайплайн против прохода с теми же load/store op (или ключ по ним); не уничтожать пайплайны в кадре | P2, P3 | warm-up `make_pass` `:3640`, кэш пайплайнов | 0.5 дн., после проверки на устройстве | **[NEW]** |
| 15 | Xclipse: проход «один demote в конце» — только если пропадёт alpha-test/foliage | X2 | SPIR-V-проходы (`xbg_spirv_*.cpp`) | 1–2 дн. | **[NEW]** (условно) |

Сознательно **не** делаем: EDS1/2/3 (не используем; на Mali < r42 он сломан), multi-viewport (не используем),
MSAA tiler resolve (нет MSAA), ASTC GPU-декодер (ASTC везде аппаратный), Skyline-трюк «BC поддержан, если есть любой бит» (только для Adreno 512).

Порядок проверки на Mali-G720: сначала п. 1–4 (дёшево, закрывают fatal-пути), затем первый запуск ради
`capability_report` и лога `ATTRIBUTE`/`NVN caps`, по нему решать п. 3 (widening или pulling), 6 и 8.

## Источники
- yuzu progress reports (зеркало): https://yuzu-mirror.github.io/entry/yuzu-progress-report-jun-2023 (Mali support #10837, EDS #10790, #10864), https://yuzu-mirror.github.io/entry/yuzu-progress-report-nov-2023 (multi-viewport #12083, nullDescriptor #12110)
- Коммиты yuzu/Eden (eden-emulator/mirror): `088017b179` (#10837 mali-support), `ca2c3a6d5a`, `0875e158fe`, `da57f07bce` (#12083), `3efaed605c`/`f50189075b` (#12110), `d73d8e8471` (#12410), `091abdc5cb` (#13075), `dbb9e0c5fe`, `1d3609ab7c` (#10864), `31200d0c7a`, `389327d3f0`, `2991bd18ef` (#3643), `5027aecf77` (#3383), `3d0eb4b5d7` (#3839), `3d19743d95` (#3953) — `https://github.com/eden-emulator/mirror/commit/<sha>`
- Eden `docs/DriverBugs.md`: https://github.com/eden-emulator/mirror/blob/5df599128d68/docs/DriverBugs.md
- Citron: https://github.com/citron-neo/emulator/blob/a37026c483ff/src/video_core/vulkan_common/vulkan_device.cpp , https://github.com/citron-neo/emulator/blob/a37026c483ff/src/video_core/renderer_vulkan/vk_query_cache.cpp
- Skyline/Strato: https://github.com/strato-emu/strato/blob/ae1566a48285/app/src/main/cpp/skyline/gpu/trait_manager.cpp ; коммиты `cbc896c8f8`, `750dfb8f00`, `ffdd50bdf3`, `7bfe63f679`, `a16383fd4b`, `98b366c1f5`, `22ce531e6f`, `54172322fe`, `80c8fb8791` — `https://github.com/strato-emu/strato/commit/<sha>`
- Skyline shader-compiler: https://github.com/strato-emu/shader-compiler/commit/6c258c31ff , https://github.com/strato-emu/shader-compiler/commit/b774a83ee6 , https://github.com/strato-emu/shader-compiler/commit/7d99a5cdcc
- Granite: https://github.com/Themaister/Granite/tree/69f450c62dc3/assets/shaders/decode , https://github.com/Themaister/Granite/blob/69f450c62dc3/vulkan/texture/texture_decoder.cpp
- PowerVR Pixel 10 и vkDestroyPipeline: https://forums.unrealengine.com/t/pixel-10-powervr-dxt-48-1536-gles-skeletal-mesh-explosion-on-newer-android-16-17-vulkan-renders-correctly-but-crashes-in-powervr-pipeline-destruction-path/2835883
- Ложный след Mali-G925: https://community.arm.com/support-forums/f/mobile-graphics-and-gaming-forum/57769/
