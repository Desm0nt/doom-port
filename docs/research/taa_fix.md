# TAA в DOOM (2016, Switch) на Android: почему не сходится и что сделано (28.10)

Без телефона: всё ниже выведено из исходников шейдеров игры (GLSL в `.resources`), переведённого SPIR-V, кода движка
и старых логов/дампов. Проверка на устройстве — раздел «Эксперименты».

## 1. Шейдеры и ресурсы TAA

Исходники GLSL вытащены в `docs/research/taa_src/*.glsl` (из `generated/renderprogs/*_nx.bin`, через
`tools/doom_shader_jobs.py`; это код игры — только для локального разбора).

| Программа | Ключ (ctrl sha16) | Что делает |
|---|---|---|
| `taa_vsps` (fragment) | **`615e5352a9db7427`** (vs `4489ea82e08e6387`, полноэкранный треугольник) | TAA resolve, единственный, что грузится в игре (`NVN shader 615e5352a9db7427 stage=4 loaded`) |
| `computetaa_cs` | `021f32253729d78f` | то же в compute (пишет `r11f_g11f_b10f` + `rg16f`); в логах не встречается |
| `taadisabledtransfers_vsps` | `f1feb256691f7b81` | путь при выключенном TAA |
| `prezdrawmotionvector*_vsgsps` | `ef97a2735bfdeae6`, `703f1fcc3cda7204`, `919c10a8430cedb8`, `ee0022fdb45318d5`, ... | depth pre-pass + GS-куллинг + вектора движения объектов |

Входы fragment-TAA (NVN texture slot = GLSL binding):

| slot | GLSL | Роль | Как читается |
|---|---|---|---|
| 0 | `samp_tex4` | глубина | `textureGatherOffsets` (ConstOffsets ±1) — минимум глубины 2x2 |
| 1 | `samp_tex2` | скорость объектов (RG16F) | `textureLod`; `objV.x < 1` — объект, иначе скорость камеры по глубине и `prevviewprojection` |
| 2 | `samp_tex0` | текущий кадр (R11G11B10F) | `texelFetch` 3x3 → resolve-фильтр (`r_TAAResolveFilter`) и окно min/max |
| 3 | `samp_tex3` | кадр до прозрачных | `texelFetch`; `transparency_k = |M - tex3| * r_TAATransparentsK` → вес текущего кадра |
| 4 | `samp_tex1` | история | бикубика из 9 билинейных `textureLod` (нужен LINEAR сэмплер) |

Выходы: RT0 — новая история (R11G11B10F 1280x720, игра чередует две картинки, в дампах serial 43/44), RT1 — RG16F
(1/длина скорости, глубина) для motion blur. Uniform: `freqHigh` = c3 = `ub[1][0]` (texsize, prevVP w/x/y,
`renderpositiontoviewtexture`, `downsampletype.z` = TransparentsK, веса фильтров, `prevtexsize`, `motionblurparms`),
`freqLow` = c4 = `ub[1][1]` (projectionmatrixz, лучи фрустума tl/tr/bl/br, resolutionscale, globalvieworigin).

Проверено и в порядке: перевод SPIR-V (`ImageGather` с `ConstOffsets`, `ImageFetch` с `Lod|ConstOffset`,
`SampleExplicitLod` Lod 0, capability ImageGatherExtended); конвертация сэмплера (`xbg_sampler.cpp`); NaN-безопасные
min/max/clamp включены (`gfx-nan-minmax=1`, а `saturate((cMax-prev)/(cMax-cMin))` даёт NaN при cMax==cMin);
quad-рерайт на Adreno 750 выключен (и TAA производных не использует); partial upload TAA-картинки не трогает;
`read break why=6` у TAA — обычный разрыв прохода после записи текущего кадра.

## 2. Главная находка: у порта зеркальная ось Y относительно того, что ждёт игра

* `hle_nvn.c` логирует `nvnDeviceSetDepthMode` («深度模式 近=0») во всех прогонах, а `nvnDeviceSetWindowOriginMode`
  **не вызывается никогда** → DOOM работает с умолчанием NVN, `LOWER_LEFT` (GL). Конвенцию «upper-left» в движок
  перенесли из D2R («measured on the D2R side», `dmsh_xlate_doom/Program.cs`), для DOOM её никто не измерял.
* Движок строит viewport как в D2R/Metal (`xbg_nvn_viewport`: `{x, y+h, w, -h}`): NDC y=+1 → строка 0.
  Для LOWER_LEFT строка 0 = NDC y=-1.
* Шейдеры, написанные под Switch, прямо на это опираются:
  * GS-куллинг (`prezdrawmotionvector`): `scPos = (ndc*0.5+0.5)*targetSize`, потом `texture(depthSampler, px/1280, px/720)` —
    Hi-Z-тексель для треугольника считается в GL-строках. У нас он берётся из зеркальной области → неверные отбрасывания
    = дыры/чёрные силуэты (п.3 graphics_plan) независимо от subgroup-эмуляции.
  * вектора движения: `curr = ndc*0.5+0.5; out = (curr - prev)`; TAA: `tc_prev - velocity`, мир восстанавливается
    `mix(frustumvecbl.., frustumvectl.., tc.y)` — tc.y=0 = низ кадра.
  * кластерный свет (16x8x24): ячейка по `gl_FragCoord` → `positiontoviewtexture`; кластеры заполняет CPU в GL-терминах.
* Что видно на экране сейчас — правильно ориентировано, потому что все проходы, где позиция берётся из gl_FragCoord
  (TAA, `viewcolorupsample`: `texcoord = gl_FragCoord.xy * positiontoviewtexture.zw`, без bias), копируют строки как есть,
  а зеркало «сидит» только там, где NDC сопоставляется со строками текстур.

Следствие для TAA: при неподвижной камере `world_pos` восстанавливается по лучу зеркальной строки, `winPosPrev`
указывает на зеркальный пиксель, скорость ≈ `2*tc.y - 1` (огромная везде, кроме средней строки). История берётся не
оттуда, режется окном min/max соседей, её «край» почти всегда на границе окна → `k ≈ 0`, `w ≈ transparency_k`: на
выходе зажатая в окно чужая история. Это ровно симптомы: зерно (дизеринг `fract(gl_FragCoord*0.5)` из lit-шейдеров не
усредняется), «лесенка» и ореолы (клэмп к экстремумам окна), шлейфы (вектора объектов с обратным знаком по y),
«меньше бит» (дизеринг R11G11B10F без усреднения). Те же зеркала объясняют пелену тумана при взгляде вниз (п.2),
пятна света (п.4) и часть дыр (п.3).

Не 100 %: умолчание NVN (LOWER_LEFT) и то, что Switch выводит lower-left окно снизу вверх, — вывод из кода игры, а не
из документации. Проба (бит 1) это решает однозначно, см. ниже.

## 3. Что сделано (`xbg_game.cpp`, `namespace taafix`, переключатель `DM_GFX_TAA_FIX`, по умолчанию 0)

* **бит 1 — проба TAA.** На draw с ps `615e5352a9db7427` (первые 6 кадров, потом раз в 600 презентов):
  * `NVN TAA probe frame=.. out=RT0/RT1 prev_out=.. history_is_prev_out=0/1 inputs: depth=.. objvel=.. current=..
    opaque=.. history=..` — для каждого входа serial, NVN-формат, размер, мипы, базовый мип вида, VkFormat,
    last_write, gpu/uploaded, сэмплер (min/mag/wrap);
  * `NVN TAA probe ubo ...` — texsize, renderpos2tex, TransparentsK, prevtexsize, motionblur, центр. вес фильтра;
  * `NVN TAA probe frustum tl=.. bl=.. ... -> rows_vs_reconstruction=agree|MIRRORED` — сравнивает, какой луч фрустума
    шейдер берёт для строки 0, с тем, какая строка 0 на самом деле (Z вверх у idTech);
  * раз в 600 кадров `summary frames= history_continuous= history_breaks= repeated_in_frame=`.
* **бит 2 — эмуляция NVN LOWER_LEFT** (кандидат на исправление, глобальный): viewport с положительной высотой
  (NDC -1 → строка y, строки совпадают со Switch), инвертированный `frontFace` (у Vulkan знак площади противоположен GL
  при тех же координатах; зеркальный viewport это гасил), вывод через перевёрнутую копию окна (кольцо из 3 картинок,
  `vkCmdBlitImage` 1:1 NEAREST) — экран остаётся прямым. noload-доказательство (`gfx-noload`, по умолчанию выкл.)
  при бите 2 отключено. Бит 2 выключается сам, если зарегистрирован UI-сдвиг HarmonyOS.
  Scissor не трогается: при LOWER_LEFT строки scissor и так совпадают со Switch (сейчас они тоже зеркальны).

Сборка: `cmake --build work/build-android --target dm` — OK.

Побочный эффект бита 2: дампы `gfx-dump-now` / capture теперь в раскладке Switch (низ кадра в строке 0) — для
просмотра переворачивать.

## 4. Эксперименты на устройстве (дёшево)

1. `DM_GFX_TAA_FIX=1`, постоять 20 с, `grep "NVN TAA probe"`:
   * `rows_vs_reconstruction=MIRRORED` → гипотеза подтверждена, дальше п.2. `agree` → откатывать бит 2, смотреть п.3;
   * `history_is_prev_out=0` / `history_breaks` растёт → история рвётся (свой serial не тот, что писали прошлым кадром);
   * `history=..samp=..min0/mag0` → история читается NEAREST (бикубика ломается в «лесенку»);
   * `opaque` = тот же serial, что `current`, или `up1` (залит с CPU) → `transparency_k` портит вес.
2. `DM_GFX_TAA_FIX=3` против `=1` на одной сцене (стоя 30 с, поворот, движущийся монстр):
   зерно должно уйти в покое, «лесенка»/ореолы/шлейфы — заметно меньше; заодно проверить пелену тумана вниз,
   пятна света на лестнице, дыры GS-куллинга, меню/HUD/видео (ориентация должна остаться прямой; если картинка вверх
   ногами — сообщить, значит Switch не переворачивает окно и бит 2 надо без флипа).
3. Игровые cvar'ы (через `DM_DOOM_ARGS`) поверх `=1`:
   * `+set r_TAASafeMode 1` (отключает всё, что держится на накоплении) — если картинка почти не меняется, накопления
     сейчас фактически нет;
   * `+set r_TAAClearAccumulationBuffer 1` — то же сравнение с другой стороны;
   * `+set r_TAATransparentsK 0` — если зерно уменьшится, виноват вход slot 3;
   * `+set r_TAAResolveFilter 0` (box) — отделяет пространственный фильтр от временного;
   * `+set r_motionblur 0` — отделяет шлейфы motion blur от TAA.
4. Выключить RCAS («Резкость (FSR RCAS): Выкл») при сравнении — RCAS усиливает остаточное зерно дизеринга.

## 5. Риски / что ещё

* Бит 2 глобальный: меняет растеризацию всех игровых draw. Пайплайны с другим `frontFace` пересоберутся (кэш прогрева
  их не найдёт — первые минуты возможны подгрузки).
* Если бит 2 подтвердится, его надо сделать умолчанием (лучше — по факту `origin` из `nvnDeviceSetWindowOriginMode` в
  `hle_nvn.c`), а `xbg_nvn_viewport` / `nvn_cover` научить обеим конвенциям.
* Сабагенту по GS-куллингу: тест окклюзии в `prezdrawmotionvector` берёт Hi-Z в GL-строках → без бита 2 часть дыр
  останется даже с правильной subgroup-эмуляцией; A/B вместе с `DM_GFX_TAA_FIX=3`.
