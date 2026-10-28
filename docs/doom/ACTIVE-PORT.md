# DOOM → Android: текущее состояние

Подход: форк d2r-mobile (этот репозиторий, сборочный флаг `DM_GAME=doom` → `DM_GAME_DOOM`). Разведка и форматы —
[RECON.md](RECON.md).

## Текущее состояние (28.10.2026, 23:06 мск)
- **Документация:** хронология — [PORT_JOURNAL.md](PORT_JOURNAL.md); все баги (симптом → причина → фикс → переключатель →
  проверка) — [BUGS_AND_FIXES.md](BUGS_AND_FIXES.md); переключатели `DM_*`, cvar-переопределения, CUTS и инструменты —
  [SWITCHES.md](SWITCHES.md); заимствования — [THIRD_PARTY.md](../THIRD_PARTY.md).
- **Работает (Mi 14 Ultra, Qualcomm):** меню, кампания, звук, онлайн выключен (кнопки MULTIPLAYER нет), EULA автопринятие,
  экранный пад с мышиным обзором и линейными стиками, USB-геймпад, гироскоп (Motion Aiming), аим-ассист на SP-методах
  (`DM_AA_STRENGTH=1`), адаптивный игровой тик 30–60 Гц, DRS — пункт «Dynamic Resolution» в Video-меню игры (по умолчанию выкл).
- **Графика:** зелень, туман/декали/TAA (`DM_GFX_TAA_FIX=2`, NVN window origin), мигание теней (`DM_GFX_DEPTH_FEEDBACK=2`),
  подгрузка VT (`DM_GFX_VT_FIX=1`) исправлены.
- **Производительность:** ~47 fps медиана в бою (`DM_GFX_SUBMIT_CHUNK_US=2000`), пустыня ~40–43 fps (GS pass-through + memo).
  Утром 27.10 было ~5 fps, днём 28.10 ~20.
- **Открыто:** дыры при включённом GS-куллинге (по умолчанию `r_geometryCulling 0` + pass-through, дыр нет); Turnip — опция,
  с дырами и без стабильного выигрыша; 60 fps; совместимость с другими GPU.
- Разделы ниже — журнал 27–28.10 в исходном виде; часть выводов устарела (см. «Противоречия» в PORT_JOURNAL.md): DRS у DOOM
  есть (`rs_enable`), zero-copy для DOOM включён по умолчанию, данные лежат в `/sdcard/Download/DOOM Mobile/`, lone-lane для
  GS-куллинга корректен.

## Как собрать и запустить
- Патч NSO: `DM_TARGET=android python dm/tools/patch_nso.py work/exefs-update/<main|sdk> work/patched <main|sdk>`;
  проверка `python dm/verify/equiv.py work/patched <mod> work/exefs-update/<mod> --jobs 12` (0 расхождений).
- Хост (CLI): `cmake --build work/build-android --target dmhost`; запуск на эмуляторе/телефоне:
  `sh tools/run_emu.sh <имя> <сек> [ENV=..]` → `work/logs/<имя>.log`; бэктрейсы: `python tools/symbt.py <log>`.
  `DM_VK_NULL=1` — режим без окна: команды NVN не рисуются, события/копии/счётчики применяются (только для отладки).
- APK: `powershell -File tools/build_apk.ps1 [-Install <serial>]` → `work/DOOM-port-debug.apk` (`org.doom.port`,
  подпись ключом из `DM_KEYSTORE`, по умолчанию debug-ключ Android). На устройстве:
  `/sdcard/Android/data/org.doom.port/files/{romfs, dm/out (patched модули), dm/shaders (*.xgs)}`.
- RomFS на устройство: `sh tools/push_romfs.sh` (докачивает недостающее по размерам; `DST=` для другого пути).

## Сделано (2026-10-27)
- ExeFS/RomFS извлечены и проверены (IVFC), шейдеры NVN найдены (`generated/nx/*.sb`).
- Патчер: таблицы переходов со словными смещениями + `mov sp, x18`; эквивалентность 29 238 мест — 0 расхождений.
- Хост без D2R-фич (`doom_stubs.c`, огорожены адреса D2R). HLE для SDK 6.4: bcat, fs (SetAllocator/ReadFile/
  GetDirectoryEntryCount/CommitSaveData), socket/nifm/ssl/account/friends/err/web/time/vi, csrng, hid (JoyDual,
  DebugPad, SixAxis=0 датчиков), AudioRenderer (`hle_doom_audio.c`, PCM16, 48 кГц).
- NVN (в `hle_nvn.c`, блок DM_GAME_DOOM): Buffer*, MemoryPoolMap/GetSize/GetFlags, TextureHandle/Address,
  RegisterImage, fast clear, WriteTexels(+Strided), SetSize3D, GetLevels; swizzle/depth-stencil mode/reduction — заглушки с логом.
- На LDPlayer (CLI, DM_VK_NULL) игра проходит init SDK, nnMain, потоки, монтирование RomFS, создание окна NVN.

## Решения (2026-10-27)
- Целевое устройство №1 — Snapdragon 8 Gen 3 / Adreno 750 (Mi 14 Ultra). Новые флагманы вторичны.
- База остаётся нашей (d2r-mobile fork): патчер корректнее на Android (NOREDZONE + equiv.py), xbg быстрее.
  Из nxnative / nx2android (MIT) заимствуем всё лучшее (HLE SDK 6.x, потоки/SVC, NVN-рекордер, шейдеры, Vulkan-идеи,
  аудио) с указанием авторства в `docs/THIRD_PARTY.md`. Исходники для сверки — публичные репозитории (см. THIRD_PARTY.md).
- Шейдеры: `tools/doom_shader_jobs.py` → `tools/build_dmsh_xlate_doom.py` → `dm/gfx/tools/dmsh_vk_doom.py`
  → `work/shaders-vk` (863 `.xgs`, 0 ошибок). Контейнер `.sb` = id-обёртка + GLSLCoutput.
- APK `DOOM Mobile` (`org.doom.port`): данные в `/sdcard/Download/DOOM Mobile/{romfs,dm/out,dm/shaders}`, нужен
  «доступ ко всем файлам» (`adb shell appops set org.doom.port MANAGE_EXTERNAL_STORAGE allow`). `dm/env` там же —
  отладочные переключатели. `hle_fs`: xattr на FUSE не поддерживается (EOPNOTSUPP) — трактуется как «нет алиаса».
- Отладка: `DM_DBG=1` печатает лог игры (`[guest]`); дамп всех потоков — `pkill -SEGV dmhost`.
- LDPlayer (x86 + libhoudini) годится для CPU/HLE-отладки, но очень медленный; правда о графике — только на Adreno.

## Текущий блокер: нет первого present
- И в CLI (`DM_VK_NULL`), и в APK (настоящий Vulkan) игра доходит до `------- Initializing renderSystem media --------`,
  создаёт 4163 буфера / 1089 текстур / 752 программы и дальше стоит. Главный поток ждёт игровой (0x1002, condvar).
- Рендер-поток (0x100a) крутится в автомате `main+0x1c5bf80` (состояние `[obj+0x1b8]`): при состоянии 4 зовёт
  проверку `main+0x1c08110` и путь кадра `main+0x1c5bbf0 → 0x1c08c00 → 0x1c72ce0 → 0x1c06970`; `nvnQueuePresentTexture`
  не вызывается ни разу. Гипотеза: проверка ждёт условия от NVN (sync / window acquire / событие / результат GPU).
- Следующий шаг: счётчик NVN-вызовов вне кадров (какие функции крутятся в цикле) + разбор `0x1c08110`.

## Ввод в меню: найдено и исправлено (2026-10-27)
- Цепочка: `idNXJoystick::PollInputEvents` → `PostInputEvent` → `idSysLocal::QueueEvent` (update `0x1df78b0`) →
  `idCommonLocal::Frame` (update `0x1ed2930`, цикл событий `0x1ed4c44`) → `idMenuManager_Shell::HandleEvent` (`0x107beb0`) → `idSWF::HandleEvent`.
- Причина: каждый кадр `idSessionLocal::UpdateTitleStorage` (онлайн-конфиг Bethesda, cvar `tss_enable`) добавлял диалог
  `GDM_LOADING_TITLE_STORAGE`, а `idCommonDialog::AddDialog` (`0xf96110`) при каждом добавлении зовёт `idKeyInput::ClearStates`
  и `sys->ClearEvents` — очередь событий обнулялась до цикла в `Frame`.
- Исправление (CUTS, build-id 2847991952…): `0x1dcf4cc` cbz → b, всегда ветка «UpdateTitleStorage: disabled» (онлайн не трогаем).
  Плюс страховки: `0x1ed4e6c` (Frame: отбрасывание событий чужого устройства) и `0x107bf48` (Shell: проверка master device) → nop.
- Диагностика: `DM_INPUT_OBS=1` включает OBS-гейты 23–33 (hle_hid.c): QueueEvent, Frame, ClearEvents (с вызывающим),
  AddDialog (тип + имя из таблицы `0x3f75958`), HandleEvent меню/SWF. Настоящий lr гостя в гейте — `regs + 808`.
- Инструменты: `tools/vtables.py` (vtable через RELATIVE/ABS64-релоки), `tools/disa.py`, `tools/callers.py`, `tools/fmatch.py`, `tools/wmatch.py`.
- Второй блок: `idMenuManager_Shell::State_Transition_Initial_Screen` видел профиль в состоянии 2 (грузится), показывал
  `GDM_LOADING_PROFILE` и больше не возвращался к экрану, хотя профиль догружался (SetState 2→0 через ~1.5 с). Патч:
  `0x107fd78` b.eq → nop, `0x107fd84` b.ne → b. Результат на LDPlayer: соглашение → яркость → главное меню, всё экранными кнопками.

## Графика на Adreno 750 (Mi 14 Ultra): найдено и исправлено (2026-10-27)
- Subgroup-операции: драйвер Qualcomm разрешает их только в fragment/compute (stages 0x30). `xbg_spirv_lone.cpp`
  переписывает такие модули в «варп из одной дорожки» (ballot/shuffle/маски/built-in'ы), только если стадия или класс операций
  не поддерживается. При падении компилятора драйвера шейдер сначала пробуется без subgroup (`gfx-lone-lane`), потом уже `gfx-pipe-skip`.
- Копия NVN между разными форматами = конвертирующий blit (2D-движок), не побитовая (RGBA8 → R11G11B10F давала зелень в Bink/меню).
- **Зелёный мир**: post-compute `927db1fc` читает HDR-сцену (R11G11B10F) как storage `R32ui`; view давался в формате образа →
  UB → мусор в G. Теперь целочисленным storage-привязкам с объявленным форматом даётся view R32_UINT/R32_SINT (`storage_int_view`).
- **Виртуальные текстуры**: feedback (3 образа 120×90 R32, флаги 0x44 IMAGE|LINEAR_RENDER_TARGET) CPU читает прямо из памяти —
  после кадра такие образы выгружаются в память игры (`cpu_visible_flush`, `DM_GFX_CPU_VISIBLE=0` выключает). Без этого страницы
  не запрашивались, кэши BC7/BC6H пустые.
- 3D block-linear: высота блока для 3D считается как у 2D (`nvn_tex.c`). Реальная раскладка 3D-сеток ещё не подтверждена.
- Ввод: защёлка нажатий на 4 опроса npad (DOOM опрашивает несколько раз за кадр при ~10 fps).
- Патчи CUTS: EULA автопринятие `0x1080110`, без уведомления автосохранения `0x1080118`; пропуск заставки `0x1f08e3c`
  выключен (без ролика экран загрузки рисовал без таргетов — теперь такие draw пропускаются, перепроверить).
- Отладка: `dm/gfx-dump-now` (опц. номера текстур) → `dm/dump/` + `tools/dump2png.py`; `dm/gfx-capture-now "<skip> <n>"` →
  покадровый захват событий; `DM_GFX_ALIAS_LOG=1`; `DM_INPUT_OBS=1`.

## Производительность: разбор (2026-10-27/28)
Замер в геймплее (Adreno 750, 720p — рендер-таргеты 1280×720, т.е. «240p-вид» моделей не от разрешения): ~5 fps,
submit_cpu ≈ 148 мс, gpu ≈ 66 мс, ~460 render pass и ~720 draw за кадр. simpleperf: рендер-потоки заняты ~22% времени,
ядра на 0,9 ГГц — **мы ждём, а не считаем**. Причины и правки (все с переключателями в `dm/env`):
- **Indirect-аргументы читались на CPU** (`execute_indirect` → `cpu_access(…,5)`): GS-куллинг DOOM пишет аргументы
  DrawElementsIndirect на GPU, каждое чтение = полный слив GPU (sync_wb ~1,75/кадр, CPU и GPU по очереди + инпут-лаг).
  Теперь `vkCmdDraw*Indirect[Count]` / `vkCmdDispatchIndirect` прямо из GPU-копий (`gpu_indirect_try`, `VK_KHR_draw_indirect_count`).
  `DM_GFX_GPU_INDIRECT=0` — старый путь.
- **Цепочка storage**: ~200 draw depth-prepass с GS пишут атомиками в один SSBO; движок каждый раз копировал прошлый результат
  (end pass + copy + full barrier). Теперь тот же буфер передаётся дальше (`DM_GFX_STORAGE_CHAIN=0` — копия).
- **Гостевые барьеры NVN** (~118/кадр) рвали проход + 2 полных барьера. Хазарды движок отслеживает сам (образы: last_write /
  barrier_seq, теперь и storage-image из draw; буферы: pending + ленивый барьер), барьер соблюдается только после draw со
  storage-image. `DM_GFX_GUEST_BARRIER=1` — всегда.
- **VT-кэши** (BC7 8192² 89 МБ, BC6H 33 МБ) перезаливались целиком при записи CPU в пару страниц → частичная заливка
  изменённых GOB-блоков по трекеру страниц (`DM_GFX_PARTIAL_TEX=0` — выкл).
- **Общая память как на Switch** (`DM_GFX_ZEROCOPY=1`, по умолчанию выкл): у Adreno нет `VK_EXT_external_memory_host`,
  поэтому пулы NVN при `nvnMemoryPoolInitialize` перекладываются на страницы AHardwareBuffer (dma-buf, `mmap MAP_FIXED` по тем же
  гостевым адресам) + VkBuffer поверх; чтения буферов (vertex/index/uniform/storage-ro/indirect) биндят саму память без снапшотов.
  Страницы, которые реплей пишет CPU (UpdateUniformBuffer, CPU-копии/clear), остаются на снапшотах. Проверка когерентности:
  `work/zc_test.cpp` (`tools/perf_run.sh zctest`). GPU-записи пока идут через pending-writeback (этап B — писать прямо в память).
- Замеры: `tools/perf_run.sh` (install / env / start / grab — PERF-хвост + simpleperf dwarf). Логи 27.10: `work/logs/perf_1027`.


### Ночь 27→28.10: от 5 до ~20 fps (Adreno 750, 720p, бой)
Замер `tools/perf_run.sh` + `tools/doom_enter.sh` (автовход в геймплей с сохранения). Итог: CPU на трансляцию 148 → 11 мс,
синхронизаций 0, проходов 500+ → ~86, GPU ~60 → ~35-45 мс. Что сделано (всё с выключателями):
- Общая память (`DM_GFX_ZEROCOPY=1`): `work/zc_test` подтвердил, что AHardwareBuffer по фиксированному адресу когерентен в обе стороны.
  Индексы indirect-draw из AHB роняли драйвер — для них снапшот (`DM_GFX_ZC_INDEX=1` вернёт). Пока не по умолчанию.
- Падение `vkCmdBeginRenderPass`/"Storage alias allocation not found": `cpu_access` посреди сборки дескрипторов сливал GPU и
  ретайрил слот, чьи дескрипторы заполнялись. Теперь пересечения с GPU-результатами склеиваются на GPU (`gpu_merge_pending`).
- Разрывы проходов: VT-feedback (storage-образы) больше не помечают запись для storage-чтений; чтение глубины открытого прохода
  без разрыва, когда это точно или консервативно (`DM_GFX_DEPTH_FEEDBACK`); диапазонный ленивый барьер; копии результатов
  прошлого кадра в пролог (`DM_GFX_PRO_COPY`). Счётчики: `NVN PERF pass_end_lines` (строка-источник разрыва) и `read_break`.
- `EarlyFragmentTests` фрагментным шейдерам с записью в образы/SSBO без discard/FragDepth (`DM_GFX_EARLY_Z`): основной
  шейдинг-проход 36 → 17 мс.
- Ожидание фенсов без лока движка (`legacy_waitall=0` для DOOM): игра опрашивает старый фенс каждый кадр.
- Частичная перезаливка 3D и массивов текстур (VT page-table 128×128×5760 и т.п.); `WriteTexels` снимает защиту только с задетых GOB.
- Арены держат до 6 кусков по 32 МБ (24 давали нехватку памяти).
- Итоговый билд (04:00): 17-22 fps в бою, CPU трансляции ~10 мс, GPU 30-45 мс, ~80 проходов/кадр; zero-copy по умолчанию
  для DOOM (`DM_GFX_ZEROCOPY=0` выкл). `DM_NVN_SYNC=0` (поток трансляции) и `DM_OPMODE=0` (портатив) fps не меняют.
- Динамического разрешения нет: 3D рендерится в честные 1280x720, cvar'ов масштаба в бинарнике нет.
- Дыры/выпадающие треугольники: видны в G-буфере как силуэты, которые есть в depth-prepass, но не в основном проходе.
  Инвариантность позиций (`DM_GFX_INVARIANT=1`, по умолчанию выкл) не помогла. Главная зацепка: GS-куллинг 703f1fcc
  обменивается данными между дорожками (функция %63, вызывается 6 раз с shuffle) — lone-lane «своё значение» тут неверен.
Дальше: дыры GS (см. выше), основной шейдинг 15-17 мс (ALU), GS-prepass ~6.6 мс, для 60 fps нужен масштаб 3D-разрешения
(патч размера таргетов в игре).

## Дальше
1. Шейдеры: контейнер glslc в `.sb` → записи для dmsh_vk, факты из GLSL `_nx.bin`, пакеты `.xgs` (агент в работе).
2. Первый кадр в APK (настоящее Vulkan-окно), потом меню.
3. Ввод: тач-раскладка FPS + геймпад на Android + гироскоп (SixAxis); см. отчёт по вводу в истории сессии.
4. Не реализовано в Vulkan: компонентный swizzle текстур, stencil-sampling, min/max reduction samplers.
