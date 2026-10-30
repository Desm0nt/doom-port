# Не-Adreno GPU: драйверы, детект вендора, что сломается в DOOM (30.10)

Исследование без правок кода. Источники: веб (ссылки в конце), upstream d2r-mobile (`doom-port-repo/docs`),
наш код `dm` (android/vk_driver.c, DmActivity.java, gfx/vulkan/engine/xbg_game.cpp, xbg_texture.c, harmony/native/napi_ohos.c)
и статистика всех 863 шейдер-бандлов DOOM (`work/shaders-vk/*.xgs`, скрипт в scratchpad: стадии/capabilities/форматы).

## TL;DR

| Платформа | Дефолт | Опционально (скачиваемый пакет) | Что нужно починить у нас до запуска |
|---|---|---|---|
| Adreno 6xx/7xx (кроме 710/720/722) | `turnip27` (как сейчас) | системный | — |
| Adreno 710/720/722 | `turnip27-a710` (auto уже есть), иначе системный | — | — |
| Adreno 8xx | `turnip27-a8xx` (auto уже есть), иначе системный | — | — |
| Adreno ≤5xx | **системный** (Turnip не поддерживает a5xx) | — | не грузить Turnip вообще |
| ARM Mali (Exynos ≤2100/1380, Dimensity, Kirin ≤9000, Tensor G1–G4, Unisoc) | **системный** | Mesa PanVK-on-kbase (MIT, эксперимент) | GS без geometryShader, formatless storage writes, D24S8/форматы, BC-память/CPU |
| Samsung Xclipse (Exynos 2200/2400/2500/2600, 1480/1580/1680) | **системный** | RADV-Xclipse (MIT, Android 14+) | **D24S8 → D32S8 fallback** (AMD-железо без D24), BC4–7 у стока выключены (у нас уже авто-софтдекод) |
| PowerVR (Dimensity 7025/7050/6xxx, Helio G, Tensor G5+ / Pixel 10+) | **системный** | ничего пригодного | subgroupSize=1 (lone-lane уже), нет BC, баги компилятора (pipeguard) |
| Huawei Maleoon (Kirin 9000S/9010/9020, vendorID 0x19E5) | **системный** | — | уже есть фиксы d2r (8 dyn UBO, BC softdecode, readback cached) |

Главное: **ни один сторонний драйвер для Mali/Xclipse/PowerVR не стоит включать по умолчанию** — все они «testing builds»,
зависят от версии kbase UAPI конкретной прошивки и могут вешать GPU. Дефолт для не-Adreno — системный драйвер, а
PanVK/RADV-Xclipse — пакеты в `drivers/manifest.json` **без** `auto` (opt-in из диалога). Wrapper-слой Winlator нам не нужен:
всё, что он эмулирует (BCn, фичи), наш движок уже делает сам или должен делать сам на уровне трансляции NVN.

---

## 1. Что используют Winlator и форки

### 1.1 Общая схема
`Game(D3D) → DXVK/VKD3D → [Wrapper ICD] → [bcn_layer] → Android Vulkan driver (системный или адренотулс-пакет) → GPU`.
Bannerlator/WinlatorMali/Ludashi/GameNative/CMOD поставляют почти одинаковые («byte-identical») сборки wrapper'а
(таблица в Bannerlator `docs/graphics-wrappers-guide.md`). Рекомендации там же:
- Adreno: Wrapper + Turnip, BCn-эмуляция **принудительно выключена** на vendorID 0x5143 (там BC аппаратный).
- Mali / Xclipse / PowerVR: Wrapper + `bcn_layer` (GPU-транскод BC), без него — чёрные экраны, «VRAM exhaustion», пропавшие текстуры.
- Valhall Mali (G57/G68/G77/G78/G310/G610/G615/G710/G715/G720/G925, драйвер ≥ r32p1): дополнительно `compat`-слой leegao для DX12.

### 1.2 bionic-vulkan-wrapper (leegao; форки The412Banner, MXBikeraGIT, crowndoom/FristOneRR-Wrapper)
- Что это: Vk-on-Vk ICD (`libvulkan_wrapper.so`), Mesa-форк (ветка wrapper), грузит системный `libvulkan`/драйвер под собой.
  Цели: Mali, старые Adreno-драйверы (512.502 без BCn), Xclipse.
- Что эмулирует (по релизам v0.0.5r3–r5, авг. 2026):
  - BC1–BC7 + BC6H: GPU compute decode (по умолчанию) и CPU-путь (`USE_CPU_BCN` для зависающих игр), `MASK_BCN`/`WATERMARK_BCN`
    для отладки, валидация GPU vs CPU, `NO_BCN_EMULATION`, `NO_BC123_EMULATION`; память в примере 2046 → 700 МБ (против
    наивного RGBA8).
  - ClipDistance/CullDistance (Mali), «фейковые» geometry shaders (PowerVR), `VK_POLYGON_MODE_LINE`, ETC2-транскод (эксп.),
    `WRAPPER_REDUCE_DEPTH_FORMAT` (safe/aggressive), `CHECK_FOR_STRIPING`; спуфинг расширений/фич для DXVK.
  - Известное: Mali (Pixel 8) — выключать `VK_EXT_extended_dynamic_state` для DXVK < 2.0; Xclipse/PowerVR — «missing feature emulation».
- Лицензия: Mesa-база (MIT); отдельного LICENSE-файла верхнего уровня в README не видно — **перед любым заимствованием кода
  проверить** конкретные файлы `src/`.
- Для нас: не нужен как слой. Наш NVN→Vulkan транслятор сам решает, в каком формате создавать образ, и уже имеет
  per-format BC fallback (см. §3). Можно позаимствовать **идею GPU compute-декода BC** (снимает CPU-стоимость декода
  VT-страниц), но это наш код в движке, а не ICD.

### 1.3 Mesa PanVK на стоковом mali_kbase (главная находка)
Апстрим Mesa PanVK требует DRM-драйверы panfrost/panthor (mainline Linux); на стоковом Android есть только проприетарный
`mali_kbase` (`/dev/mali0`). В 2026 появилось несколько независимых портов PanVK **поверх kbase UAPI, без root**:

| Проект | GPU | Статус | Примечание |
|---|---|---|---|
| zenithblue-oss/panvk-kbase-android (+ форки HAMsajalah, MineGraph) | v9 JM (G57…G78), v10 CSF (G610/G710), v11 (G615/G715), v12 (G720), v13 (G925), v14 собран; v7 Bifrost эксп. | 0.1.0 RC4 (20.10.2026), PanProbe 38/38 на большинстве Valhall/5th gen | Vulkan 1.4.363, 194 расширения; GS/TS «как compute», BC1–7 GPU-декод, transform feedback; D3D11 FL11_0/11_1; D3D12 FL12 нет (нет sparse); баги: GPU queue hang на Pixel 7, `DEVICE_LOST (CSF tiler heap OOM)` на draw без RT, v9 режет большие draw. Один `libvulkan_panfrost.so` + zip для adrenotools/Winlator/cmod/Ludashi/Bannerlator/GameHub. Mesa-лицензии + MIT для обвязки |
| MineGraph-bilibili/panvk-Android (форк wonderkast02/panvk-g720-kbase-csf) | G610/G615/G710/G715/G720 (CSF) | 0.1.0-beta.2, API ≥ 35 | «no Vulkan conformance declaration» |
| soulexit/panvk-kirin990(-winlator), panvk-exynos9611-kbase-* | G76 MC16 (Kirin 990, JM UAPI 11.16), G72 MP3 (Exynos 9611) | v1.2.4 pre-release | NieR 10–14 fps, GS/TS эмуляцией; MIT |
| FristOneRR/FristOneRR-Panvk-Driver | G57 | — | DXVK под Winlator |
| JimVulkan/mali-panvk | Bifrost | — | автор RADV-Xclipse |
| LukeValen/panvk-mali-g52 | G52 | — | — |

Как грузится: обычный adrenotools-zip (`meta.json` + `.so`) — Eden/Winlator открывают его через
`adrenotools_open_libvulkan(ADRENOTOOLS_DRIVER_CUSTOM, …)` в отдельном linker namespace. Т. е. **adrenotools годится для
любого Vulkan-драйвера**, не только KGSL/Turnip; хуки gsl/file-redirect просто не используются. Наш `vk_driver.c` уже так и
делает, и `driver_has_gpu()` отсеет пакет, если не увидит GPU.

Вывод по PanVK: технически совместим с нашим загрузчиком, лицензия (MIT/Mesa) позволяет и бандлить, и скачивать (с NOTICE).
Но: зависимость от версии kbase UAPI прошивки (MEM_ALLOC_EX fallback и т. п.), тестовые сборки, DEVICE_LOST/hang на части
устройств → **только opt-in пакет**, с crash-guard (§4.3). Плюс: на PanVK есть BC (GPU-декод) и GS — снимает две наши
проблемы на Mali; минус — производительность Mali на compute-эмуляции GS ниже стока.

### 1.4 Samsung Xclipse: RADV-Xclipse
- JimVulkan/radv-xclipse (+ форки Dnyw, jhinzuo2, shin5968): порт Mesa RADV на Xclipse 920/530/940/540/550/950/960.
  v1.9.0-RC1 от 28.10.2026 (Mesa 27.0.0-devel), до этого v1.0…v1.8 — самый «живой» из сторонних не-Adreno драйверов.
- Требует Android 14+. Пакет: `meta.json` + `vulkan.radeon.so` + `NOTICE.txt`, грузится Eden/Winlator (= adrenotools).
  Лицензия: MIT (правки) + Mesa (в основном MIT).
- Известное: RT на 950/960 вешает телефон; фолиаж DL2 на 940; ошибки transform feedback size на 530/940; «experimental, без
  валидации в играх» для свежих RC. BC: «выключите BCn-эмуляцию приложения» — драйвер сам конвертирует BC на GPU.
- Стоковый `vulkan.samsung.so` (AMDVLK/PAL-стек внутри): BC4–BC7 выключены программной таблицей при аппаратной поддержке
  (XclipseDecomp, xclipse-vulkan-decompiled). У нас это покрывается per-format проверкой (BC1–3 нативно, 4–7 софтдекод).
- vendorID: стоковый — 0x144D (Samsung), RADV, вероятно, сообщит 0x1002 (AMD) — проверять по `driverID`/имени.

### 1.5 PowerVR
- Апстрим Mesa `imagination` (Vulkan 1.2 conformant на BXS-4-64) требует нового `pvr` DRM (mainline 6.8+), на Android его нет.
- Энтузиасты: ameya-deshpande-5056/mesa-pvr-bxm-8-256 (BXM-8-256 / Dimensity 7025, стоковый `pvrsrvkm` DDK 25.1 через
  реверснутый bridge ABI; MIT; 12 коммитов, «physical testing gated», ранние пробы паниковали ядро; GS/TS/BC нет),
  Ahmed234567890/mesa-pvr-ge8322. **Непригодно**.
- Стоковые драйверы IMG: subgroupSize=1 (Rogue и BXM), BC нет, GS у BXM есть; известны падения компилятора
  (`vkCreateComputePipelines` → VK_ERROR_UNKNOWN на fp16/RTE, CXTP-48 в Pixel 11 Pro).
- Важно для детекта «Tensor»: Tensor G1–G4 — Mali (G78/G710/G715), **Tensor G5 (Pixel 10) и новее — PowerVR** (DXT/CXT).

### 1.6 Huawei Maleoon
Vendor 0x19E5 (6629), Vulkan 1.3; Kirin 9000S (Maleoon 910), 9010, 9020. Под Android это HarmonyOS 4 (AOSP-база);
d2r-порт на HarmonyOS NEXT (Mate 90 Pro Max, Maleoon 955) идёт 59.7 fps. Сторонних драйверов нет.

---

## 2. Что реально можно сделать

1. **Дефолт для не-Adreno = системный драйвер**, без попытки Turnip (сейчас `defaultDriver()` отдаёт `turnip27` всем, у
   кого есть папка, — на Mali Turnip грузится, не видит KGSL и откатывается: лишние ~100+ мс и риск, сообщения нет).
2. **Пакеты opt-in** в `drivers/manifest.json` (не бандлить в APK):
   - `panvk-kbase` (zenithblue RC4 или новее) с полем `"vendor": "0x13b5"` и без `auto`;
   - `radv-xclipse` (JimVulkan v1.8/1.9) с `"vendor": "0x144d"`, `"minSdk": 34`, без `auto`.
   Лицензии MIT/Mesa: класть `NOTICE.txt` из zip рядом, упомянуть в `docs/THIRD_PARTY.md`. Бандлить в APK не стоит:
   +10–30 МБ на редкие устройства и быстро устаревают.
3. **Wrapper-слой не нужен.** Всё полезное — в движке:
   - BC: уже per-format fallback (§3.2). Улучшение на потом — GPU compute-декод (как bcn_layer/PanVK) или BC→ASTC транскод
     (как d2r Legacy Device Pack для iPhone A16: BC3→ASTC 4×4 тот же размер блока, offline через astcenc; ASTC есть у всех
     Mali/PowerVR/Xclipse/Maleoon) — экономит память и CPU.
   - GS: у нас culling-GS уже сбрасывается в draw (`DM_GFX_GS_PASSTHRU`), осталось не требовать `geometryShader` при загрузке (§3.1).
4. **Сообщение пользователю о фолбэке** + crash-guard (§4.3).

---

## 3. Код: что уже есть для Mali/PowerVR/Maleoon и что сломается в DOOM

### 3.1 Статистика шейдеров DOOM (863 бандла)
Стадии: FS 560, VS 262, CS 35, **GS 6** (TCS/TES нет). Capabilities: Shader, ImageGatherExtended (все модули →
нужен `shaderImageGatherExtended`), SampledBuffer/ImageBuffer/ImageQuery, **StorageImageWriteWithoutFormat (70 модулей FS+CS)**,
Geometry (6 GS), GroupNonUniformBallot+Shuffle (7: 6 GS + частицы `cec755c4e27678b5`). Типизированные storage images —
только `R32ui` (2), остальные formatless. До 12 UBO-биндингов в одном FS. Один descriptor set на pipeline layout.

### 3.2 Уже есть в движке (унаследовано от d2r / HarmonyOS)
| Фикс | Где | Активен в DOOM? |
|---|---|---|
| BC per-format: если формат не даёт SAMPLED+FILTER_LINEAR+TRANSFER — софтдекод (bcdec, параллельный) | xbg_game.cpp ~2081, xbg_texture.c:212 | **да, автоматически** (даже при `DM_GFX_NATIVE_BC=1`) |
| BC4/BC5 → 8-бит каналы, если R16G16_UNORM без linear filter (Mali-G925) | xbg_game.cpp:2079, xbg_texture.c:30 | да, авто |
| D16 → X8_D24, если D16 не фильтруется линейно | xbg_game.cpp:4874 | да, авто |
| Динамические UBO по лимиту `maxDescriptorSetUniformBuffersDynamic` (Maleoon = 8): режимы 1/2/0 | xbg_game.cpp:3149 | да, авто |
| lone-lane для всех модулей с subgroup-операциями, если subgroupSize ≠ 32 (Mali 16/8/4, PowerVR 1, Adreno 64/128) или стадия/операции не поддержаны | xbg_game.cpp:2456, xbg_spirv_lone.cpp | да, авто |
| warp32 (32-лейн варпы в 64/128) | xbg_spirv_warp.cpp | только 64/128 — на Mali/PowerVR неприменим, работает lone-lane |
| pipeguard: модуль, уронивший компилятор, → lone-lane / skip при следующем запуске (`gfx-lone-lane`, `gfx-pipe-skip`) | xbg_game.cpp ~2330, 2863 | да (важно для PowerVR) |
| Ключ кэша пайплайнов по vendorID/deviceID/UUID | xbg_game.cpp:941 | да |
| Набор «Maleoon best practice» (readback cached, NATIVE_BC=0, swapchain 4, PACE_ADAPT, NVN_SYNC=0, 3D_SCALE_GPU) | harmony/native/napi_ohos.c:265 | **нет** — только HarmonyOS-сборка, в Android-JNI не по вендору |
| `xbg_adreno_rewrites_fault` (quad/warp32 off на Adreno 740–799) | xbg_adreno_policy.h | только 0x5143 |
| Лог caps (features/subgroup/limits/все форматы) | `capability_report` xbg_game.cpp:1453 | да — первым делом смотреть его на новом устройстве |

### 3.3 Что сломается / нужно сделать
1. **GS без `geometryShader` → fatal при загрузке.** `validate_capabilities` (xbg_game.cpp:2302) падает на capability 2,
   хотя в draw эти 6 culling-GS всё равно выбрасываются (`gs_gate_slot`, `DM_GFX_GS_PASSTHRU=1`, `r_geometryCulling 0` по
   умолчанию). Стоковые Mali Valhall и PowerVR BXM GS заявляют (у Mali «advanced geometry» path — медленно), но Maleoon,
   PowerVR Rogue, community-драйверы — не гарантированно. Фикс: при `!geometryShader` для GS с найденным gate не
   валидировать/не создавать модуль, а в draw всегда выбрасывать стадию; fatal только если gate когда-нибудь «on».
   Сделать так же и при наличии GS на Mali — выигрыш (ARM прямо советует избегать GS).
2. **Formatless storage writes (70 модулей).** Нужен `shaderStorageImageWriteWithoutFormat` или per-format
   `STORAGE_WRITE_WITHOUT_FORMAT` через `VK_KHR_format_feature_flags2` (код есть, xbg_game.cpp:3500). Если у старого Mali/
   PowerVR/Rogue нет ни того, ни другого — `xbg_fatalf("Image format … lacks formatless-write")`. Фикс на будущее:
   SPIR-V-рерайт, проставляющий `Format` в `OpTypeImage` по формату вьюхи на момент создания пайплайна (вариант пайплайна
   на формат). Сначала собрать caps-лог с реального устройства.
3. **D24S8 на Xclipse (AMD-база): `D24_UNORM_S8_UINT` обычно не поддерживается** (у AMD нет D24). Fallback'а в
   таблице форматов нет (xbg_game.cpp:1425) → создание образа/вьюхи упадёт. Фикс: D24S8 → D32_SFLOAT_S8_UINT,
   X8_D24 → D32_SFLOAT с конверсией при upload/readback (по образцу `depth16_as_d24`, xbg_game.cpp:1960/4302); учесть
   copy/blit-пути и `capture`. Проверить и на PowerVR.
4. **Storage/attachment форматы.** HDR-таргет B10G11R11 читается вьюхой R32_UINT в post-compute (xbg_game.cpp:3095) —
   требует MUTABLE_FORMAT (на Mali это выключает AFBC, как UBWC на Adreno; добавить `VkImageFormatListCreateInfo`).
   Если формат не даёт STORAGE_IMAGE, usage тихо не ставится (2089) → упадёт позже. Нужно логировать и иметь путь
   «render target копией».
5. **BC: память и CPU.** Софтдекод BC1 ×8, BC3/BC7 ×4 по памяти (d2r: 320 → 827 МБ на Adreno без native BC); у DOOM VT-страницы
   обновляются постоянно → CPU-декод на каждой подгрузке (риск статтера VT на Mali/PowerVR/Maleoon; Xclipse — только BC4–7).
   Варианты: GPU compute-декод в движке; offline BC→ASTC.
6. **Subgroup/lone-lane.** Частицы `cec755c4…` и GS на lone-lane — тот же путь, что на Qualcomm 750 по умолчанию
   (проверен), но с риском «каждая инвокация — лейн 0 удваивает счётчики» (gs_cull_fix.md, раунд 3). На PowerVR (size 1)
   `xbg_spirv_lone_lane` обязателен; если драйвер вообще не объявляет subgroup ops в CS, тоже уходим в lone-lane.
7. **Quad-derivatives рерайт** включён для всех Android (jni_android.c:217) — на Mali безвреден и полезен
   (helper-lanes), оставить. Но `gfx-quad-derivatives`/`gfx-warp32` политики сейчас только для Adreno — сделать
   вендорную таблицу.
8. **Timestamps.** Пулы game-counter (xbg_game.cpp:1042) создаются без проверки `timestampValidBits` (в отличие от 869) —
   на очереди с 0 битами `vkCmdWriteTimestamp` невалиден. Плюс на тайлерах (Mali/PowerVR) таймстемпы внутри прохода
   неточны → GPU-время в оверлее/`DM_3D_SCALE_GPU` будет врать; мерить на границах submit.
9. **Mali-специфика производительности:** AFBC теряется на MUTABLE без format list; descriptor alloc медленный
   (на Maleoon ~15 мкс/сет — уже recycle); barriers/pass-split дороже на тайлере (как на Adreno); `DM_GFX_READBACK_CACHED=1`.
10. **Лимиты:** 12 UBO в одном FS против `maxPerStageDescriptorUniformBuffers` (старые Mali — 12–16, ок, но на грани);
    `shaderImageGatherExtended` нужен всем 863 модулям — у PowerVR Rogue может не быть → fatal на старте.

---

## 4. Детект и выбор драйвера

### 4.1 Порядок (Java, до старта нативного кода; `minSdk 31`)
```
if (/sys/class/kgsl/kgsl-3d0/gpu_model читается)                 // Qualcomm
    m = число после "Adreno" (Adreno750v2 → 750)
    m < 600              → ""  (системный; Turnip не поддерживает a5xx)
    m in 710|720|722     → turnip27-a710, если скачан; иначе ""
    800 ≤ m < 900        → turnip27-a8xx, если скачан; иначе ""
    иначе                → turnip27
else                                                              // не Qualcomm
    → ""  (системный); в диалоге подсветить пакет по вендору:
      Build.SOC_MANUFACTURER/SOC_MODEL (API 31) + ro.hardware.vulkan (mali / samsung / powervr / …)
      Mali → panvk-kbase, Xclipse → radv-xclipse (если SDK ≥ 34)
```
Выбор пользователя (pref `driver`) всегда главнее, как сейчас. В manifest можно добавить поля `vendor` (hex vendorID)
и `minSdk`, и `auto` писать только для Adreno.

### 4.2 Проверка в нативе (vk_driver.c)
- `driver_has_gpu()` расширить: возвращать `vendorID`, `deviceName`, `driverID` (VkPhysicalDeviceDriverProperties, 1.2:
  MESA_TURNIP=18, MESA_PANVK=20, MESA_RADV=3, ARM_PROPRIETARY=9, QUALCOMM_PROPRIETARY=8, IMAGINATION_PROPRIETARY=7,
  SAMSUNG_PROPRIETARY=21) и логировать.
- Вендоры: 0x5143 Qualcomm, 0x13B5 ARM, 0x1010 ImgTec, 0x144D Samsung, 0x1002 AMD (RADV на Xclipse), 0x19E5 Huawei.
- Поколение Adreno — `xbg_adreno_model(deviceName)` уже есть (три цифры после «Adreno»); Mali — по `Mali-G…`:
  Bifrost (G31/G51/G52/G71/G72/G76, subgroup 4/8), Valhall (G57/G68/G77/G78/G310/G510/G610/G710, 16),
  5th gen (G615/G620/G715/G720/G925/G1-*, 16).
- Если пакет не загрузился / не видит GPU / vendor не совпал с полем `vendor` пакета → системный + причина в
  `dm_vk_driver_status()` (новое), Java показывает тост/строку в оверлее: «Драйвер <name> не запустился на <GPU>,
  используется системный».

### 4.3 Crash-guard (обязательно для PanVK/RADV/Turnip-test)
Перед `adrenotools_open_libvulkan` писать маркер `files/dm/driver-trying=<name>`, удалять после первого успешно
отрисованного кадра (или ~10 с без DEVICE_LOST). Если при старте маркер остался → в этот запуск системный драйвер +
сообщение «прошлый запуск с драйвером <name> упал/завис, переключено на системный». То же для `s.reset_result failed: 2`
(GPU hang, см. upstream 2026-10-25-adreno-740-750-rewrites).

---

## 5. Риски
1. **Нет тестового устройства** не-Adreno: все выводы по Mali/PowerVR/Xclipse — из кода и чужих отчётов. Первый прогон
   на каждом вендоре — ради `NVN caps`-лога, а не fps.
2. PanVK-kbase / RADV-Xclipse: тестовые сборки, зависимость от kbase UAPI/версии Android, GPU hang вместо DEVICE_LOST
   (зависание приложения), RADV RT на 950/960 вешает телефон (нам RT не нужен, но всё равно).
3. Fatal-пути при загрузке: GS capability (§3.3.1), formatless write (§3.3.2), D24S8 на Xclipse (§3.3.3),
   gatherExtended на Rogue.
4. Память/CPU софтдекода BC + VT-стриминг DOOM → статтер и OOM на 6–8 ГБ устройствах.
5. Производительность: Mali/PowerVR мидрейндж — сильно ниже 8 Gen 3; DOOM уже на Adreno 750 не держит 60 в тяжёлых сценах.
   Нужен agressive `DM_DOOM_RES`/DRS по умолчанию для слабых GPU.
6. Ошибки компиляторов (PowerVR, Maleoon) на транслированном SPIR-V → pipeguard спасает только со второго запуска.
7. Turnip на не-Adreno: сейчас грузится зря (adrenotools не падает, но тратит время и прячет причину) — убрать дефолтом.
8. Лицензии: Turnip/PanVK/RADV — MIT/Mesa, ок при сохранении NOTICE; wrapper leegao — проверить файлы перед
   заимствованием кода; бинарники стоковых драйверов (vulkan.samsung.so, libGLES_mali.so) распространять нельзя.

## Источники
- PanVK-kbase: https://github.com/zenithblue-oss/panvk-kbase-android ; https://github.com/MineGraph-bilibili/panvk-Android ;
  https://github.com/soulexit/panvk-kirin990-winlator ; https://github.com/soulexit/panvk-exynos9611-kbase-winlator ;
  https://github.com/FristOneRR/FristOneRR-Panvk-Driver ; https://github.com/JimVulkan/mali-panvk ; https://github.com/LukeValen/panvk-mali-g52/releases
- RADV-Xclipse: https://github.com/JimVulkan/radv-xclipse ; https://github.com/JimVulkan/radv-xclipse/releases/tag/v1.9.0-RC1 ;
  https://github.com/avavo/XclipseDecomp ; https://github.com/WearyConcern1165/xclipse-vulkan-decompiled
- Wrapper: https://github.com/leegao/bionic-vulkan-wrapper/releases ; https://github.com/The412Banner/bionic-vulkan-wrapper ;
  https://github.com/The412Banner/Bannerlator/blob/main/docs/graphics-wrappers-guide.md ; https://github.com/crowndoom/FristOneRR-Wrapperv1 ;
  https://github.com/GunaCharanTeja/WinlatorMali/releases/tag/bionic-mali-1.0
- PowerVR: https://github.com/ameya-deshpande-5056/mesa-pvr-bxm-8-256 ; https://www.phoronix.com/news/Open-Source-PowerVR-Vulkan ;
  https://forums.imgtec.com/t/bxm-4-64-vkcreatecomputepipelines-fails-vk-error-unknown-on-valid-spir-v-using-roundingmoderte-16-and-fp16-compute-returns-wrong-results/4305
- Mali: https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695 (Arm GPU Best Practices: избегать GS) ;
  https://docs.mesa3d.org/drivers/panfrost.html
- Maleoon: https://www.notebookcheck.net/HiSilicon-Maleoon-910-Benchmarks-and-Specs.803657.0.html ; upstream README-d2r-mobile (HarmonyOS, Maleoon 955)
- Наш код/доки: `doom-port-repo/docs/research/d2r_nx2android_history.md` (Mali-G925 17b4baa), `docs/BUGS_AND_FIXES.md` п.8,
  `docs/research/gs_cull_fix.md`, `doom-port-repo/docs/android-notes/2026-10-25-adreno-740-750-rewrites.md`.
