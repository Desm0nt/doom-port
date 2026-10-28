# Прогоны на телефоне после сборки diag12 (28.10, вечер)

Сборка: `work/DOOM-port-diag12.apk` (ориентация окна по умолчанию, GS-фиксы 0x61 по умолчанию, отсечение треугольников
по умолчанию выключено; новые переключатели ниже по умолчанию выключены). По кабелю adb serial другой (`adb devices`).
Сначала забрать с телефона логи прошлых прогонов: `DOOM Mobile/dm/shots*/logcat.txt` (d_A, d_B, d_C, d_gyro) и удалить
`DOOM Mobile/dm/env`, если хозяин играл без меня.

Порядок (tools/play_series.sh, хозяин бегает; досрочный переход tools/series_next.sh):

| # | Имя | env (через `;`) | Что смотрит хозяин / что смотрю я |
|---|---|---|---|
| 1 | g_carrier | `DM_DOOM_GEOMCULL=1;DM_GFX_GSCULL_FIX=0x861` | дыры и большие чёрные треугольники; лог `NVN index carrier` |
| 2 | g_carrier_id | `DM_DOOM_GEOMCULL=1;DM_GFX_GSCULL_FIX=0xa7f` | то же (контроль identity) |
| 3 | p_stats | `DM_NVN_GW_STATS=1;DM_GFX_PERF_SAMPLE=4` | fps на счётчике; `[nvn] GW per frame` (ждём span ~25–30 мс) |
| 4 | p_lag | `DM_NVN_SYNC_LAG=1;DM_NVN_GW_STATS=1;DM_GFX_PERF_SAMPLE=4` | fps, мерцание/артефакты |
| 5 | p_chunk | `DM_GFX_SUBMIT_CHUNK_US=2000;DM_GFX_PERF_SAMPLE=4` | fps |
| 6 | p_both | `DM_NVN_SYNC_LAG=1;DM_GFX_SUBMIT_CHUNK_US=2000;DM_GFX_PERF_SAMPLE=4` | fps, артефакты |
| 7 | p_poll | лучшее из 4–6 + `DM_NVN_POLL_US=200` | fps, нагрев |
| 8 | gyro | `DM_GYRO_FORCE=1;DM_GYRO_LOG=1` (без тяжёлых DM_*) | крутит телефон 10 с с оружием; потом то же с `DM_GYRO_GAIN=8`; логи `gyro state`, `first gyro sample`, `motion sensors sampling` |
| 9 | vt_base | `DM_DBG=1;DM_NVN_COUNT=1;DM_GFX_VT_DIAG=1;DM_DOOM_ARGS=+set vt_showPageLoads 1` | стоять 60–90 с перед мутной скалой (Марс) |
| 10 | vt_fix | то же + `DM_GFX_VT_FIX=1` | быстрее ли становится чётко |
| 11 | vt_ppf | vt_base + `+set vt_maxPPF 128` | то же |

Отчёты субагентов: gs_cull_fix.md (раунд 6), perf_round2.md, gyro_fix.md (раздел 7), vt_streaming_fix.md.
