# DOOM VT (virtual texturing): почему чёткие страницы приходят минутами — разбор и диагностика (2026-10-28)

Статус: цепочка проверена по логам/дампам/SPIR-V без запуска игры. Однозначного обрыва в коде не найдено. Добавлены
кандидат-исправление `DM_GFX_VT_FIX=1` (по умолчанию выключено) и диагностика `DM_GFX_VT_DIAG=1`. Нужны прогоны на телефоне (ниже).

## Цепочка VT в DOOM на Switch и где она у нас

| Звено | Что это (ключ / serial из `work/dump11/all.txt`, логов p1/q_base) | Состояние у нас |
|---|---|---|
| Feedback (запись) | Фрагментные шейдеры сцены (напр. `3ed1dd77c77bdca6`, ещё 9 с `early fragment tests`) делают `imageStore` в 3 картинки **120x90 R32F** (`fmt 0xa`, flags `0x44` = IMAGE+LINEAR_RENDER_TARGET, block-linear), serial 95/96/97, по кольцу. Значение = упакованный uint через bitcast: `mip<<19 \| (y&0xff)<<8 \| (x&0xff) \| id<<16 \| hi<<24 \| c4[1].x<<27`; mip = `trunc(min(0.5*log2(max(|dUV/dx|²,|dUV/dy|²)) + c3[2].y, c3[2].z))` по нативным `OpDPdx/OpDPdy`. | **Работает**: дампы `dump1..dump11/9[567]_na_*.raw` — все 10800 пикселей заполнены, денормалов 0 (бит 27 всегда стоит → flush-to-zero на Adreno не бьёт), mip-гистограмма осмысленная. |
| Feedback → CPU | На Switch CPU читает память текстуры напрямую (ни ReadTexels, ни копий в `[nvn] tex/copy/compute`). У нас: `cpu_visible_mark` → `cpu_visible_flush()` (в **present**) пишет `vkCmdCopyImageToBuffer` в текущий слот → `apply_image_writebacks` при retire слота свизлит в guest-память. | Работает (`NVN cpu-visible write-back serial=95..97` в p1), **но копия ставится только в present** → может попасть в слот *позже* того, что покрывает игровой sync кадра → игра читает feedback старого кадра (или ни разу не видит свежий для данного sync). Это кандидат №1, ниже. |
| Подгрузка страниц | `.pages` (`intro.pages`, `_vmtr.pages`, mega2 чанки), транскод на CPU (`vt_useTranscodeJobs`), лимит `vt_maxPPF`=32 страниц/кадр. | `[fs]`: диск не узкое место; стоя перед мутной скалой игра почти не читает → **игра не просит страниц** (либо считает, что всё есть / переподписка). |
| Физический кэш | `nvnTextureWriteTexels` (1k–130k вызовов/10 с в q_base/f_fix2/k_nozc/m_nofog), т.е. CPU-транскод, не GPU (`gputranscode*` не используются). Кэш = serial **103**: DXT5_SRGB 4096x4096x**3 слоя** (≈1024 слота страниц 128²), + serial 1880 BC6H 8192x4096, 94 BC6H cube-array 128²x5760. | **Доходит до GPU**: `NVN partial texture upload serial=103 / 1880 / 94` регулярно (≈27k partial uploads за сессию p1). `write_texels` вызывает `dm_guest_write_prepare` → dirty-трекер → `partial_upload`. |
| Таблица страниц | serial **98**: RG8 2048x2048, 12 мипов, flags 0x4. Обновляется CPU (WriteTexels), не compute: во всех дампах `written=0` (ни одной storage-записи). | Доходит: `partial texture upload serial=98` регулярно. |

Сопутствующее:
- `RenderParm updatepagetablemip / gputranscodecover / gputranscodecoefficients does NOT have a matching structured buffer` — параметры GPU-транскода/GPU-апдейта таблицы из decl'ов; на этой сборке путь не используется (таблица 98 никогда не пишется GPU). Игровое сообщение при загрузке decl'ов, не наше. Логов Switch для сравнения нет.
- `Could not open rom:/virtualtextures/_vmtr_dlcN.vmtr, all vmtrs will be re-created` — DLC-файлов нет в romfs; базовый `_vmtr.vmtr` читается (`Read a total of 7747 vmtrs`). Безвредно.
- Зеркалирование окна (DM_GFX_TAA_FIX bit 2): на feedback не влияет — значения кодируют страницу, позиция пикселя игре не важна.
- `texdirty tracked/checks/changed` низкие закономерно: крупные VT-текстуры идут через `partial_upload`, а не через hash-проверки (в p1 нет ни одной `NVN big texture hash`).

## Подозрения (по убыванию)

1. **Задержка/рассинхрон feedback относительно игрового sync** (исправление `DM_GFX_VT_FIX=1`). Копия feedback записывается в `xbg_present_texture_now`, а игра фенсит кадр раньше (QueueFenceSync / CommandBufferFenceSync до present). `completed` публикуется в `retire_slot` *после* `resolve_slot_writebacks` только для того слота, где лежит копия. Если игра ждёт sync, покрытый более ранним слотом, она читает память без записи этого кадра. Если анализ feedback отбрасывает «чужие» данные (или CPU помечает буфер прочитанным), запросов страниц почти нет — ровно наблюдаемая картина «стоим — ничего не грузится, через минуты случайно догружается».
2. **Переподписка физкэша.** В дампах 3.6–5.3 тыс. *различных* значений на 10800 пикселей feedback, а в кэше 103 ≈1024 слота. `vt_feedbackHighWater`=90 / `vt_feedbackLowWater`=80 (%) — при переподписке игра поднимает LOD-bias (`c3[2].y`) и/или трэшит. Различные значения ≠ различные страницы (в старших битах перемешаны поля), поэтому проверять cvar'ами, см. ниже.
3. Upload-сторона (partial upload / dirty tracker гонка с потоком транскода) — по логам работает, но контроль `DM_GFX_PARTIAL_TEX=0` обязателен.

## Что добавлено в код (`dm/gfx/vulkan/engine/xbg_game.cpp`)

- `DM_GFX_VT_FIX` (битовая маска, по умолчанию 0). Бит 1: `cpu_visible_flush()` в конце каждого guest-submit (`xbg_submit_now`, до `++submitted`) — копия feedback попадает в слот, который покрывает fence этого submit, и попадает в guest-память до того, как игра увидит sync завершённым (как общая память на Switch). Копия только если в этом submit была storage-запись в CPU-visible картинку (≈1 копия 43 КБ на кадр); `Commands` закрывает открытый проход.
- `DM_GFX_VT_DIAG=1`: строка `NVN VT feedback wb=300 in Xs lat_subs avg/max cpu_changed guest_zero distinct_avg mip0-7 hi fix partial(serial:uploads/blocks)` на каждые 300 write-back'ов:
  - `lat_subs` — сколько guest-submit'ов прошло от копии до записи в guest-память (большое → игра читает старые данные);
  - `cpu_changed` — сколько раз CPU игры изменил память feedback между нашими записями (≠0 → игра чистит/метит буфер после чтения: тогда VkImage тоже надо чистить — следующий шаг);
  - `guest_zero` — доля нулевых слов в guest-памяти перед нашей записью;
  - `distinct_avg`, `mip0-7`/`hi` — что реально отдаём игре;
  - `partial(...)` — сколько раз и сколько GOB-блоков дозалито в каждую крупную текстуру (103/1880/94 = физкэши, 98 = таблица) за то же окно.
- Сборка: `cmake --build work/build-android --target dm` — OK.

## Эксперименты на телефоне (для главного агента)

Сцена: Марс (p1, 13:12–13:13), встать перед скалой справа, 60–90 с без движения, скриншоты каждые 10 с. Всегда `DM_DBG=1 DM_NVN_COUNT=1 DM_GFX_VT_DIAG=1`.

| # | env / `DM_DOOM_ARGS` | Смотреть |
|---|---|---|
| A | база + `+set vt_showPageLoads 1` | строки игры на каждую загруженную страницу (есть ли запросы вообще, когда стоим); `NVN VT feedback` — lat_subs, cpu_changed, partial 103/98 |
| B | A + `DM_GFX_VT_FIX=1` | время до резкости vs A; рост `vt_showPageLoads`; lat_subs должен стать ~0–1 |
| C | A + `+set vt_showPageMips 1` (или `vt_showPageColors 1`) | раскраска мипов на экране: какой мип реально сэмплится на скале |
| D | A + `+set vt_feedbackHighWater 100 +set vt_feedbackLowWater 99` | если резко лучше — причина в переподписке/авто-bias (п.2) |
| E | A + `+set vt_maxPPF 128` | если быстрее — упираемся в лимит страниц/кадр (транскод на CPU) |
| F | A + `+set vt_feedbackLodBias 1` (и `-1`) | реакция на bias: меньше/больше страниц |
| G | A + `DM_GFX_PARTIAL_TEX=0` | контроль upload-стороны (медленно, но если резкость приходит сразу — ломается partial/dirty) |
| H | контроль: `+set vt_skipStreaming 1`; `+set r_skipFeedbackCapture 1` | должно быть стабильно мутно — подтверждает, что мы смотрим на VT |
| I | `+set vt_recordStats vt.txt` | статистика VT игры в файл (если пишет в доступный путь) |

Интерпретация: `cpu_changed>0` при `VT_FIX=1` и всё ещё медленно → игра чистит feedback на CPU; следующий шаг — при записи CPU в
CPU-visible картинку (flag 0x40) сбрасывать/перезаливать VkImage (сейчас `gpu_written` запрещает upload). D помогает → смотреть
`c3[2].y` (bias) и уменьшать число различных страниц (возможна ошибка масштаба UV/производных). G помогает → гонка `partial_upload`
с `write_texels` (`dm_guest_write_prepare` снимает защиту до memcpy).
