# Туман / кластеры DOOM: причина и фикс (2026-10-28)

## Проходы тумана
- `globalfog_vsps.sb`: VS `4489ea82e08e6387` (общий fullscreen), **PS `59d472473469f858`**. Читает только `samp_viewdepthmap`
  (tcb 8, 2D, `textureLod` mip 0) и UBO c3/c4 (`renderpositiontoviewtexture`, `windowpostoglobal{x,y,z,w}`, `fogparms*`,
  `globalvieworigin`, `projectionmatrixz`). Froxel/3D-объёма у глобального тумана нет.
- `fogvolume*` (VS `f47f9f432b8e8885`, PS `e738c82f73dd4e61` / `132537888b3e2013` / `37f215d665a00f9c`): локальные объёмы,
  высотный туман считается в VS, в PS только depth-fade.
- Исходники GLSL лежат в romfs (`generated/renderprogs/<name>_nx.bin`), достаются через `tools/doom_shader_jobs.py`
  (`load_renderprogs` + `glsl_sources`).

## Сбойная операция
```glsl
tc = gl_FragCoord.xy * renderpositiontoviewtexture.zw + .xy;   // строка текстуры, для выборки глубины верна
ndcZ = textureLod(samp_viewdepthmap, tc, 0).x;
globalPosition = windowpostoglobal * vec4(tc / resolutionscale.xy, ndcZ, 1);   // матрица строится в GL-терминах: tc.y=0 = низ кадра
... exp2(-fogparms1.w * effectiveZ) ...  // высотный туман зависит от cameraToPixel.z
```
DOOM не вызывает `nvnDeviceSetWindowOriginMode` (в логах нет строки `[nvn] 窗口原点`, при этом `深度模式` есть), значит
окно NVN по умолчанию LOWER_LEFT (GL): строка 0 = NDC y −1. `xbg_nvn_viewport` строит отражённый вьюпорт D2R (NDC +1 → строка 0).
Выборка RT по `gl_FragCoord` остаётся согласованной (и пиксель, и текстура отражены), но любое преобразование строка/tc → NDC/мир
даёт луч, отражённый по вертикали. Пиксели горизонта вверху кадра восстанавливаются как точки далеко **под полом**, и высотный
туман там экспоненциально плотный. Отсюда «смотрю вниз — молочная дымка», «вверх — чисто», и слишком плотный туман на Марсе.

Та же причина у декалей и света. В forward-шейдерах (`litglass*` и др.) кластер ищется так:
`clusterCoordinate.xy = screenPosToTexcoord(gl_FragCoord.xy, positiontoviewtexture) * resolutionscale.zw * (16, 8)`.
Сетка 16×8×24 заполняется на CPU в GL-терминах, поэтому ряд кластера зеркален: пиксель получает списки света, декалей и проб из
зеркального участка экрана. Декали мигают при повороте камеры (`work/logs/burst_grid.png`), свет пятнами. Знак `dFdy` тоже
был инвертирован. TAA (`computetaa`: `mix(frustumvecbl, frustumvectl, winPos.y)`), SSAO, SSR и motion blur страдают так же.

## Фикс
Сделан TAA-агентом как `DM_GFX_TAA_FIX=2`:
- вьюпорт с положительной высотой;
- перевёрнутый `frontFace`;
- переворот окна при выводе;
- `first_draw_covers` выключен.

`DM_GFX_FOG_FIX=1` — это его alias (`taafix::mode()` в `xbg_game.cpp`). Пока зарегистрирован UI-сдвиг HarmonyOS, режим не
включается. Компиляция `xbg_game.cpp` проходит. Линковка `libdm.so` сейчас падает на `vkDeviceWaitIdle` в `taafix::flip`
(трамплины Vulkan): это код TAA-агента, а не alias.

## Эксперименты на устройстве
1. `DM_GFX_FOG_FIX=1` (или `DM_GFX_TAA_FIX=2`): дымка при взгляде вниз должна исчезнуть, декали перестанут мигать, свет
   станет стабильным, картинка не перевёрнута. Проверить: меню и HUD (у них NDC те же), катсцены, сцену с узким scissor.
2. Контроль без сборки: `+set r_skipFog 1` (дымки нет). `r_skipDecalCulling 1` / `r_skipLightRangeCulling 1` /
   `r_skipLightCPUCulling 1` декали **не** починят: списки при этом верные, неверен только ряд, по которому их читают.
   `DM_GFX_ZEROCOPY=0` тоже не поможет.
