# DOOM perf, раунд 3 (2026-10-28): почему открытые сцены Марса проседают до ~20-25 fps

Источники: `work/logs/play/{fl_chunk,fl_vtbase,sw_nozc,sw_stale0,r2_*,aa_*}/logcat.txt` (Qualcomm, chunked submit по
умолчанию), `work/logs/run_ag.txt` (единственный лог с `NVN GPASS` в геймплее, 694 draw), шейдеры `work/shaders-vk`.
Скрипты разбора: `tools/gpass_report.py` (новый), агрегатор окон PERF (ниже — таблица).

## 1. Диагноз: стоимость растёт с числом объектов, и на CPU, и на GPU

Окна `NVN PERF v=1` (по 60 кадров) из 14 прогонов, сгруппированы по `draws` за кадр (медианы):

| draws/кадр | окон | fps | submit_cpu_ms | gpu_ms | descriptor ms | draw ms | image ms |
|---|---|---|---|---|---|---|---|
| 100-300 | 57 | 57.3 | 6.3 | 12.6 | 1.2 | 1.2 | 2.6 |
| 300-500 | 221 | 49.2 | 8.9 | 15.0 | 2.2 | 2.1 | 2.9 |
| 500-700 | 316 | 42.4 | 11.3 | 17.1 | 3.6 | 3.0 | 3.0 |
| 700-900 | 125 | 35.6 | 15.5 | 20.1 | 5.5 | 3.8 | 3.1 |
| 900-1100 | 55 | 31.3 | 19.2 | 23.7 | 8.0 | 5.3 | 2.8 |
| **1100-1400** | 36 | **25.2** | **28.9** | **29.4** | **14.3** | 6.7 | 1.9 |

- Большие пустынные сцены = **1250-1340 draw/кадр** (в коридорах 250-600). passes ~61, dispatches ~12 — не меняются,
  pipelines_new в стационаре 0-5 (компиляция не виновата). `[长帧]` > 45 ms — ~40 на прогон, почти все на загрузке уровня
  (0.8-1 с, заливка 309 MB текстур); в стационаре хитчей нет. То есть это не хитчи, а ровная стоимость кадра.
- `[nvn] 计时` в такой сцене: кадр p50 32.6 ms = «出帧工作» 31.0 = NVN 提交+出图 25.0 + ~6 ms логики игры; GPU p50 25.6.
  С `DM_NVN_SYNC=1` трансляция идёт inline на потоке игры, chunked submit лишь частично прячет GPU за CPU. Обе стороны
  ~29 ms, поэтому надо резать **и CPU, и GPU**.

### 1.1 CPU: descriptor sets, которые нельзя memo-изировать (главное)
При 1338 draw: `setcache memo_hits=514`, `why=simg407/dm409`, `desc_ns build=14.9 ms`. Т.е. 816 draw из 1338 каждый кадр
строят descriptor set с нуля, **~18 us на draw** (против ~2.5 us у memo hit). Две причины, ~по 410 draw (по одному на объект):
- `dm` (written storage): draw'ы depth pre-pass с culling GS (`703f1fcc…` и ещё 5) — GS объявляет запись в `gp_s0/gp_s1`
  (счётчик + компактный index buffer) и 20-байтные indirect args объекта → свежий readback/chain buffer, pending writeback.
- `simg` (storage image): материалы с VT feedback (40 FS из 560 пишут в storage image) — memo их исключал целиком.
С ростом числа объектов доля таких draw растёт (в коридоре ~30%, в пустыне 61%) — отсюда «нелинейный» рост descriptor ms.

### 1.2 GPU: culling GS, который ничего не делает
Culling GS (6 вариантов) **всегда** вызывается в depth pre-pass, а у нас `r_geometryCulling 0` (из-за дыр, hle_args.c).
Разбор SPIR-V: первое, что делает main — `if (c[0][0].y != 1) { gl_Position = gl_in[v].gl_Position (v=0..2, покомпонентно),
EmitVertex ×3, EndPrimitive, return }`. Выход GS — только gl_Position. То есть при выключенном culling GS — чистый
pass-through, но Adreno исполняет GS как лишний проход через память. В run_ag (694 draw, кадр 38.4 ms GPU) GS-проходы стоят
**9.2 ms**: pre-pass 6.3 ms (114 draw, 1.23 M verts) + 6 однодровных depth-feedback проходов с GS `ef97a273` ~2.9 ms.
Остальное: main shading 18.0 ms (att=4, 120 draw — ALU/разрешение), shadow 4096² 3.4 ms. В пустыне verts pre-pass в
2-3 раза больше → GS-часть, по оценке, 12-18 ms из 29.

### 1.3 Чего это не объясняет / что отброшено
- Хитчи пайплайнов/заливок — нет в стационаре (см. выше). Async pipeline (`DM_GFX_ASYNC_PIPELINES`) уже есть.
- Включение culling (`DM_DOOM_GEOMCULL=1`) уменьшило бы main-pass draws/треугольники, но держит GS и written storage на
  каждом объекте (CPU и GPU цена остаются) и даёт дыры. Пока трек дыр не закрыт, выгоднее без GS вовсе (ниже).

## 2. Что сделано (всё за env, по умолчанию выключено; сборка `cmake --build work/build-android --target dm` — OK)

1. **`DM_GFX_GS_PASSTHRU=1`** — draw без geometry stage, если его GS доказанно pass-through и переключатель выключен.
   - `xbg_spirv_gspass.{h,cpp}` (новый): статический матчер `xbg_spirv_gs_passthru_gate` — первая ветка entry-функции
     по `c[0][0].k != K` без побочных эффектов до неё; в ветке ровно копия `gl_in[v].gl_Position.k → gl_Position.k`
     (через function vars), 3 EmitVertex, EndPrimitive, Return; Triangles in / TriangleStrip 3 out / 1 invocation /
     единственный output = gl_Position. Проверено на всех 863 bundle'ах: C++ (WSL g++ + ASan/UBSan, плюс усечённые
     модули) и Python-зеркало дают одно и то же — ровно 6 culling GS, `set 0 binding 27, byte 4, value 1` (ub slot 13).
   - `load_shader`: для stage 3 матчер на исходном SPIR-V бандла → `Shader::gs_gate_slot/offset/value`, лог
     `NVN GS pass-through gate shader=… ub slot=13 byte=4 off-unless=1`.
   - `draw()`: RAII `GsPassthru` — если слово в guest-памяти UBO (`state.ub[GS][13]+4`) != 1 и над ним нет pending GPU
     результата, `state.shader[3]=nullptr` на время draw (pipeline key, descriptors, всё — как у обычного VS+FS draw),
     потом восстанавливается. Позиции идут в растеризатор те же бит-в-бит и в том же порядке. Захват (`capture`) не трогается.
   - Эффект ожидания: GPU −(большая часть 9-18 ms GS-проходов); CPU: `why=dm` ~410 → ~0 (memo hit), без 20-байтных
     readback/writeback на объект → −5…−7 ms. Первые кадры: новые VS+FS пайплайны (warm-list подхватит их к след. запуску).
2. **`DM_GFX_MEMO_SIMG=1`** — descriptor memo для sets со storage image. В ключе memo уже есть handle образа, entry проверяет
   identity/revision образа; `MemoImage::storage` помечает storage-привязку, и hit воспроизводит побочные эффекты bind'а:
   `prepare_read(im,cmd,1)`, `storage_images_bound` (+ `mark_storage_images` после draw), `gpu_written`, `storage_write`,
   `cpu_visible_mark` (VT feedback по-прежнему выгружается в память игры). Ожидание: `why=simg` ~410 → ~0, −5…−7 ms CPU.
3. **`DM_GFX_PASS_PROF=from period count`** (кавычки и запятые допустимы) (`1` = `300 600 0`; count — окно в presents после from, 0 = без конца) — тот же per-pass GPU timestamp профиль, что маркер
   `gfx-pass-profile`, но через `dm/env`. Отчёт: `python tools/gpass_report.py <logcat> [--min-us 20000] [--top 25]` —
   по кадру суммы по классам (gs-prepass / shadow / main / post / compute) и самые дорогие сигнатуры проходов.
4. Счётчики: `NVN PERF perf3 gs_passthru skipped=… kept=… memo_simg hits=…` (на кадр, только если включено).

Без env поведение прежнее: один `getenv` при первом draw / первой storage-привязке, ветки ранние.

## 3. A/B-план на устройстве (одна и та же пустынная точка, ≥3 мин, чередовать из-за нагрева)

| Прогон | env (поверх дефолта) | Смотреть |
|---|---|---|
| P0 профиль | `DM_GFX_PASS_PROF=300 120 0` | `gpass_report.py --min-us 20000`: доля gs-prepass/shadow/main в пустыне (подтверждение §1.2) |
| A0 base | — | fps, submit_cpu_ms, gpu_ms, `setcache why=simg/dm`, `desc_ns build` |
| A1 gs | `DM_GFX_GS_PASSTHRU=1` | лог `GS pass-through gate` ×6; `perf3 skipped` ≈ число pre-pass draw (~400), `kept` ≈ 0; why=dm → ~0; gpu_ms; **артефакты**: дыры/мерцание на EQUAL-depth (сравнить с A0 на тех же моделях) |
| A2 simg | `DM_GFX_MEMO_SIMG=1` | why=simg → ~0, `memo_simg hits` ~400; VT: текстуры догружаются (нет вечного мыла), нет мусора |
| A3 both | `DM_GFX_GS_PASSTHRU=1 DM_GFX_MEMO_SIMG=1` | итог: ожидание submit_cpu 29 → ~15-18 ms, gpu 29 → ~18-22 ms, fps 25 → 35-45 |
| A4 profile | A3 + `DM_GFX_PASS_PROF=300 120 0` | что осталось на GPU (main shading / shadows) |

Критерий: A3 даёт ≥+30% fps в пустыне без новых артефактов → включить по умолчанию (поменять `:0` на `:1` в
`GsPassthru()` и `memo_simg()`).

## 4. Риски
- GS bypass: драйвер может по-разному оптимизировать VS как последнюю стадию (FMA-контракция) → иной z, чем у main pass
  VS → EQUAL-depth дыры. GS сейчас копирует позицию без арифметики, так что при VS+GS ситуация та же по смыслу; если дыры
  появятся — проверить вместе с `DM_GFX_INVARIANT=1` (Invariant на Position, уже есть). С `DM_DOOM_GEOMCULL=1` слово == 1
  и bypass не срабатывает (никаких изменений).
- memo_simg: если игра меняет VT-feedback образ без новой регистрации — entry всё равно сверяет serial/mem/find_image; при
  `mutable upgrade` memo_epoch сбрасывает всё.

## 5. Дальше
1. После A3 — `DM_GFX_PASS_PROF` в пустыне: если main shading (18 ms на 720p) главный — Adreno-качество шейдеров
   (`VK_KHR_pipeline_executable_properties`, recomp_android_repos.md §2.2) и масштаб 3D-разрешения; shadow — отдельный трек.
2. Оставшиеся ~5 us/draw в `draw` part (6.7 ms @1300) — разбор живым профилем `xbg_game_profile_draw_live` (DRAW_TIMING writeback/vertex_prepare).
3. Трек GS-culling дыр: когда culling заработает, нужен нативный (не lone-lane) GS только для тех кадров, где он реально
   экономит main-pass; bypass остаётся для `r_geometryCulling 0`.
