# DOOM perf, раунд 2 (2026-10-28): что реально держит 20-24 fps

Источники: `work/logs/play/f_fix2/logcat.txt` (текущий дефолт, Qualcomm), `work/logs/play/k_nozc`, `work/logs/run_ag.txt`
(NVN GPASS — GPU-время по проходам), `docs/research/recomp_android_repos.md`, `d2r_nx2android_history.md`, `nxnative_history.md`.

## 1. Диагноз

**Кадр = трансляция + GPU, строго последовательно.** `[nvn] 计时` (f_fix2, бой): frame p50 47.1 ms, «出帧工作» 44.8 =
«NVN 提交+出图» 13.5 + GPU 31.3. На 30-fps участке: 31.3 = 7.6 + 23.6. Сумма сходится в каждом окне.
- `NVN PERF submits=1.00`, `submit_by present=1.00`: один `vkQueueSubmit` в кадр, на present → GPU стоит, пока идёт трансляция.
- `block_ms` пустой: движок сам ни разу не ждёт fence. Ждёт **игра**: `SyncWait` 3.8-4.1 M вызовов / 10 s (≈400k/s,
  ~15k на кадр) — DOOM крутит poll (timeout 0) на fence последнего кадра. Дефолт `DM_NVN_SYNC=1` (трансляция inline на потоке
  игры), поэтому `DM_NVN_SYNC=0` и не помогал: сериализует сама игра.
- Acquire-sync уже лагает на кадр (nx2android 1c6177d) — значит ждут другой sync (QueueFenceSync ≈8/кадр).
- Каждый неготовый poll = `try_lock` движкового мьютекса + `vkGetFenceStatus`; 400k/s — один ядро 100% busy, конкурирует с
  трансляцией за лок и за общий энергобюджет SoC (GPU DCVS).

**Read breaks почти бесплатные.** ~45-62/кадр `why=6`, но по GPASS видно: mip/blur-цепочки (0x3f 1280x720→20x11, 0x5 128→2)
— каждый уровень и так отдельный проход (другой RT, 1 draw, 11-25 us). Разрыв лишь «приписан» строке prepare_read вместо
смены RT; барьер нужен (RAW). Subresource-aware check даст ≈0 проходов. Реально лишние — только depth-feedback:
`e7304a6b`/`7e1eea1f`/`c5677925` (1280x720 color+depth, dwritten=1, GS ef97a27) — 6-8 однодровных проходов подряд
(GPASS i=2..10), ~0.4-0.8 ms GPU.

GPU-бюджет (run_ag, кадр 27 ms): main shading 12.9 ms (att=4, 121 draw), GS-prepass 3.9, shadow 4096² 2.6, depth-feedback
проходы ~2.4 (из них overhead ~0.5), ~50 мелких post-проходов ≈0.8, compute ~0.8.

## 2. Рейтинг возможностей

| # | Что | Ожидание | Доказательство |
|---|-----|----------|----------------|
| 1 | Снять сериализацию игры: sync lag (ждать fence кадра N-1 вместо N) | 47 → ~32-36 ms (+30-45% fps) | кадр = 13.5 + 31.3; BOTW 82b71e2 |
| 2 | Chunked submit на границах guest-submit (GPU стартует во время трансляции) | −5…−10 ms кадра без семантических рисков | submits=1.00; 7 QueueSubmitCommands/кадр; nxnative sub_cut, BOTW bf2369b |
| 3 | Убрать poll-storm (sleep в poll) | CPU/термо-запас, −лок-конкуренция; 0-2 ms | SyncWait 400k/s |
| 4 | Depth-feedback для GS-проходов (e7304a6b…) без разрыва | −0.4-0.8 ms GPU | 6-8 однодровных проходов, why=6 dwritten=1 |
| 5 | CPU трансляции: descriptor 4.7-6.8 ms из 11-13 (setcache misses 59/кадр) — dynamic UBO / кэш sets | −2-4 ms CPU (важно после #1/#2) | parts_ms_per_frame descriptor |

Основной потолок после #1-#2 — сам GPU (31-36 ms в бою); дальше — main shading 12.9 ms и разрешение 3D.

## 3. Что сделано (всё выключено по умолчанию; компилируется `cmake --build work/build-android --target dm`)

1. **`DM_GFX_SUBMIT_CHUNK_US=N`** (`xbg_game.cpp`, `submit_chunk()` перед `xbg_submit_now`): после N us трансляции в
   записывающий слот он отправляется на GPU на ближайшей границе guest-submit (snapshot_reset и Slot::covers уже по guest
   submit — разрез точный; тот же путь `submit_slots(false)` использует legacy_wait). Только если второй слот свободен или
   уже signalled — трансляция не блокируется на GPU. `submits=` в PERF покажет >1. `gpu_ms` остаётся суммой за кадр.
2. **`DM_NVN_SYNC_LAG=1`** (`hle_nvn.c nvnSyncWait`, Android-ветка): ожидание fence из (p1, p0] (только что презентованный
   кадр) превращается в ожидание p1 (кадр раньше); команды цели всё равно должны быть оттранслированы (`x_wait_sub`).
   p0/p1 пишутся в `nvnQueuePresentTexture`. Лаг максимум один кадр.
3. **`DM_NVN_POLL_US=N`**: неготовый poll (timeout 0) спит N us перед возвратом.
4. **`DM_NVN_GW_STATS=1`** (включается и любым из двух выше): раз в 300 кадров
   `[nvn] GW per frame: old/last/cur calls notdone span_ms | lagged` — какой класс fence игра ждёт и сколько мс крутится.

Без env поведение байт-в-байт прежнее (в nvnSyncWait один `getenv` при первом вызове и ветка `if(!g_gw_on)`).

## 4. A/B-план на устройстве (одна и та же точка боя, ≥3 мин каждый, чередовать из-за нагрева)

| Прогон | env | Смотреть |
|--------|-----|----------|
| A0 base | `DM_NVN_GW_STATS=1` | GW: ожидаем `last span_ms` ≈ 25-30 → подтверждение диагноза |
| A1 chunk | `DM_NVN_GW_STATS=1 DM_GFX_SUBMIT_CHUNK_US=2000` | fps, `submits=` ≈2-4, `[nvn] 计时` frame vs GPU |
| A2 chunk1ms | `… DM_GFX_SUBMIT_CHUNK_US=1000` | то же (Qualcomm может проседать по DCVS при паузах) |
| B1 lag | `DM_NVN_SYNC_LAG=1` | fps, `lagged`≈1/кадр, артефакты: мерцание, «прыгающие» декали, дыры GS-culling |
| B2 lag+noZC | `DM_NVN_SYNC_LAG=1 DM_GFX_ZEROCOPY=0` | если B1 даёт артефакты (zero-copy читает память на месте; noZC стоит ~0.6 fps: k_nozc 20.8 vs 21.4) |
| C lag+chunk | `DM_NVN_SYNC_LAG=1 DM_GFX_SUBMIT_CHUNK_US=2000` | итоговая комбинация |
| D poll | `DM_NVN_POLL_US=50` (затем 200) поверх лучшего из выше | fps, температура, `NVN PERF parts … submit_cpu_ms` |

Успех: frame p50 → ≈ GPU p50 (+1-2 ms). Если A0 покажет, что крутится класс `cur` (fence текущего кадра), а не `last` —
lag бесполезен, тогда основной рычаг — chunk (+ надо смотреть, какой sync, в `SyncWait` call site).

## 5. Не делать / оговорки
- Subresource-aware read check ради mip-цепочек — выигрыша нет (см. §1).
- Lag: readback'и CPU (VT feedback, GPU-culling, occlusion) становятся на кадр старее; BOTW видел мелкие артефакты.
- Chunk на Qualcomm у LA/BOTW был выключен (DCVS); решает только A/B.
