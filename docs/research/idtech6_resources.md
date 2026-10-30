# id Tech 6 (DOOM 2016, Switch) — контейнеры ресурсов, сжатие, идеи «распаковать заранее» и «грузить текстуры сами» (2026-10-30)

Статус: исследование без правок кода. Источники: веб/GitHub (ссылки внизу), заголовки и индекс `work/romfs`,
`work/logs/fslog.tsv` (DM_FSLOG прогона 27.10), дизассемблер базового `main` v0 (с символами,
`work/exefs-base/main.raw` + `main.symbols.json`) и поиск строк/xref в `work/exefs-update/main.raw` (сборка порта).
Скрипты разбора лежали в scratchpad сессии (в git не кладутся); данные игры наружу не копировались.

## TL;DR

- Формат Switch-дампа **совпадает с PC DOOM 2016** (тот же `\x05SER`-индекс v5, `.resources` + `.patch`/`.pindex`
  цепочка), сжатие — **zlib raw deflate** (wbits −15, zlib **1.2.3** вшит в `main`), **не Oodle**. Oodle Kraken/Leviathan —
  это уже Wolfenstein II (id Tech 6.5, контейнер IDCL) и DOOM Eternal (id Tech 7, .resources + .streamdb).
- **Признак «не сжато» = `size == zsize`, движок его честно соблюдает**: в `idResourceContainer::ReadData` и в
  фоновом загрузчике (`idDecompressSpawnThread::Run`) при равенстве — прямое чтение/`memcpy` без `inflate`.
  В шипнутом индексе уже 8 677 таких записей; PC-моддинг (DOOMExtract/DOOMModLoader) годами пишет несжатые записи.
- **Проверки целостности контейнеров в нашей сборке нет**: файлы `*.verify` (HMAC-блоки `idFile_Protector`)
  есть в romfs, но в update-`main` нет ни строки `.verify`, ни `idResourceContainer::Verify`; в base v0 эта функция
  есть, но **ни одного вызова**; в fslog игра ни разу не читает `.verify`. Индекс без хэшей на запись.
- Распаковка заранее (a) **технически реализуема и безопасна для движка**, но по цифрам **вряд ли объяснит 72 с**:
  уровень = ~0.8–1.5 ГБ на выходе inflate → ~2–5 с одного большого ядра (zlib на x86 у нас 390 МБ/с). Цена —
  +4.8 ГиБ (консолидация) … +13 ГиБ (доп. патч) на диске и ×2 байт через FUSE/page cache при уже наблюдаемом
  memory pressure (lmkd убивает фон на 8 Gen 2). Сначала — профиль перезагрузки.
- Своя загрузка текстур (b) — **не делать**: обычные `image` всего ~397 МБ (BIM v7, уже в block-linear под NVN),
  а основное — VT `.pages`/`.mega2` (10.5 ГиБ, собственный кодек страниц, энтропия 7.85 бит/байт), транскод на CPU
  игрой (`vt_useTranscodeJobs`) в DXT5-кэш. Публичного описания кодека страниц DOOM 2016 нет; это переписывание
  idVirtualTextureManager — недели работы и прямое противоречие «reuse, don't overbuild».

## 1. Кто что реверсил (id Tech 6 / 6.5 / 7)

| Проект | Игра / формат | Лицензия | Что умеет / полнота |
|---|---|---|---|
| [emoose/DOOMExtract](https://github.com/emoose/DOOMExtract) (C#, 2016–2022) | DOOM 2016: `.index/.resources`, `.pindex/.patch` | **нет файла LICENSE** (all rights reserved по умолчанию) — читать можно, копировать код нельзя | Полный разбор индекса (v≤4: поле 8 байт, v5+: 4 байта — у нас это `flags`), извлечение, **Rebuild** и `--createPatch`. `IsCompressed => Size != CompressedSize`; inflate через SharpZipLib `Inflater(true)` (raw). Комментарий в коде: сжатые ресурсы DOOM заканчиваются `00 00 FF FF` (sync flush, без BFINAL). При замене файла пишет `Size = CompressedSize` (несжатым) — игра это принимает |
| DOOMModLoader (в том же репо; форк [ZwipZwapZapony/DOOMModLoader](https://github.com/ZwipZwapZapony/DOOMModLoader), без лицензии, жив в 2026) | DOOM 2016 | нет | Копирует последний `gameresources_NNN.pindex`, делает свой патч с номером N+1 и несжатыми записями → игра берёт самый свежий pindex. Ровно схема «доп. патча» из §3a |
| idCrypt (тот же репо, `idCrypt/readme.txt`) | `.bfile` (binaryFile: `strings/*.lang`, `*.cfg`) | нет | PC: соль 0xC + IV 0x10 + AES-128-CBC + HMAC-SHA256, ключ = SHA256(salt, "swapTeam\n", internal path)[:16]. На Switch эта схема **не сошлась** (см. `doom_video_menu.md`) — для распаковки не важно, такие записи и так хранятся без deflate |
| DOOMExtract v1.0–1.2 (Ekey, Haoose, Chain) — [ZenHAX t=2153](https://www.zenhax.com/viewtopic.php@t=2153.html) | DOOM 2016 | исходники в теме | Ранние экстракторы; `.pages` в теме не разобраны |
| [DooM Resources Explorer](https://www.moddb.com/games/doom-4/downloads/doom-resources-explorer-v04) | DOOM 2016 | закрытый | GUI-просмотр контейнеров |
| `resource_loadLooseAssets 1` ([DOOMModLoader wiki](https://github.com/ZwipZwapZapony/DOOMModLoader/wiki/Creating-Mods-~-Tips)) | DOOM 2016 PC | — | На PC работает только с DoomLegacyMod (снимает скрытие cvar); `strings/*.bfile` при этом роняют игру. Cvar есть и в нашем `main` (строка `resource_loadLooseAssets`) — не проверял, режется ли он в shipping |
| Wolfenstein II: [ZenHAX t=5148](https://zenhax.com/viewtopic.php@t=5148.html), [wolfenstein-modloader wiki](https://github.com/LopeKinz/wolfenstein-modloader/wiki/TNC-Modding-Surface), [WolfstoneExtract](https://github.com/ECWolfEngine/WolfstoneExtract/) (GPL-3.0) | id Tech 6.5: IDCL-чанки в `.resources`, VT в `.texdb` | разные | **Oodle Kraken** (mode 2), **mode 0 = несжатое** (31 125 записей), mode 4 = Kraken за 12-байтовым префиксом. Распаковка через ooz / DLL игры |
| DOOM Eternal: [SAMUEL](https://github.com/brongo/SAMUEL) (GPL-3.0), [EternalModLoader](https://github.com/dcealopez/EternalModLoader) (без лицензии), [EternalModLoaderCpp](https://github.com/brunoanc/EternalModLoaderCpp) (GPL-3.0), [DivinityMachine](https://github.com/PowerBall253/DivinityMachine) (MIT, BIM↔DDS), idFileDeCompressor (`.entities`), VEGA (упоминается в доках SAMUEL как другой экстрактор), [eternalmods wiki](https://wiki.eternalmods.com/books/8-reverse-engineering-file-formats/chapter/file-formats/export/html) (за bot-check) | id Tech 7: `.resources` + `.streamdb`, «.tga» = BIM | разные | Kraken/Leviathan, BIM-заголовки текстур, mip-цепочки в streamdb. К DOOM 2016 применимо только концептуально (BIM-магия та же, версия/поля другие) |
| [Adrian Courrèges, DOOM 2016 graphics study](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/) | рендер | — | VT: огромные физические текстуры из тайлов 128×128 |

Итог по полноте: контейнеры DOOM 2016 разобраны полностью и давно (наш `tools/doom_index.py` = тот же формат).
Форматы `image`/BIM v7 частично (заголовок), **кодек VT-страниц (`.pages`, `PageCompression.cpp`) публично не описан**.

## 2. Что в нашем дампе (`work/romfs`, 343 файла, 21.8 ГБ)

| Файл(ы) | Размер | Магия / формат | Вывод |
|---|---|---|---|
| `base/gameresources.resources` | 4.58 ГБ | `05 'SER'` + 12 нулей, дальше raw deflate | как PC |
| `base/gameresources{,_002,_003}.patch` | 3.73 / 0.34 / 0.28 ГБ | `05 'SER'` (4 байта), дальше данные | как PC |
| `base/gameresources.index`, `*.pindex` (×3) | 11.8 / 14.7 МБ | `05 'SER'`, BE размер (=файл−0x20), @0x20 BE число записей; запись: BE id, 3×(LE u32 len + str), BE u64 off, u32 size, u32 zsize, u32 flags, u8 patch | индекс **v5** (как PC после патча) |
| `base/master.index` | 58 Б | `05 'SER'` + имена `gameresources.index`/`.resources` | игра его не читает (fslog) |
| `base/*.verify` (8 шт.) | 104 Б для индексов; 14 056 / 11 432 / 1 096 / 904 Б для данных | высокая энтропия с первого байта | HMAC-блоки (`idFile_Protector::VerifyHmacBlocks`, в base v0 `"%s.verify"`); **в нашей сборке не используются** (§4) |
| `base/snap_models.preload`, `rcbootstrap.bin` | 2.8 МБ / 233 Б | список моделей / зашифровано | не контейнеры |
| `virtualtextures/*.pages` (132), `_vmtr_q1..q4.mega2` | 5.00 ГиБ / 5.50 ГиБ | свои заголовки (`.pages` `06 99 33 77 03…`, `.mega2` `21 BB 3F A6 02…`), `.vmtr` — текст «file format version 26» | VT; данные практически несжимаемы (энтропия 7.85/7.76 бит/байт, deflate даёт 0.95) |
| `base/sound/**` (`.bnk/.pck`), `base/video/**` (`.bik/.bk2`) | 2.93 ГиБ | Wwise / Bink | не трогаем |

Индекс `_003.pindex` (единственный, что читает игра): 87 305 записей, после дедупликации 74 913 блобов,
**13.09 ГиБ raw / 6.71 ГиБ в файлах** (коэффициент ≈1.95). Сжато 69 155 блобов (13.06 → 6.68 ГиБ); 8 677 записей
хранятся как есть (`size == zsize`: `sound`, `renderParm`, `layer`, `table`, 61 `image`, зашифрованные `binaryFile`).
`flags` бывают только 0x0 и 0x10 (1 237 записей: `file`, `image`, `binaryFile`); 0x10 стоит и на сжатых, и на
несжатых записях — **это не флаг сжатия** (решает только `size != zsize`); при переписывании сохранять как есть.
Выравнивание смещений не требуется (в оригинале большинство смещений не кратны 16).

Крупнейшие типы (raw / в файле): `model` 6.77 / 3.12 ГиБ (`_world.bmodel` уровней до 324 МБ raw), `cm` 3.42 / 2.05,
`file` 1.86 / 1.07, `image` 0.40 / 0.16, `baseModel` 0.20 / 0.13, `anim` 0.21 / 0.13. На карту (по префиксу
`maps/game/...`) ≈470–560 МБ raw, `challenges` 762 МБ.

Сжатые данные — именно raw deflate с sync-flush хвостом: декомпрессия `zlib.decompressobj(-15)` даёт ровно `size`
байт на всех проверенных типах; в update-`main` строка `"1.2.3"` передаётся в `inflateInit2_` (update: `inflateInit2_`
= `main+0x1ffeb30`, `inflate` = `main+0x1ffede0`, `inflateEnd` ≈ `main+0x20009f0` — по xref строк `zlib inflate error`).

## 3. Как движок читает (base v0 с символами; update — та же логика по строкам/xref)

- `idResourceContainer::ReadData(off, dst, size, zsize, keepCompressed)` (base `0x13a9478`):
  `if (size == zsize) → UncachedScheduledRead(dst, size)` — прямо в буфер назначения; иначе временный буфер zsize,
  `inflateInit2_(-15, "1.2.3")`, `inflate(Z_FINISH)`, проверка `total_out == size`, иначе `"zlib inflate error"`.
- `idStreamFileCache::UncachedScheduledRead` ждёт завершения чтения циклом `Sys_Sleep(1)` — каждое синхронное
  чтение стоит ≥1 мс (в fslog за 108 с всего ~2 160 чтений контейнеров, т.е. это ≤2 с, не главное).
- Фоновая загрузка уровня: `idBackgroundLoader` + `StreamingReadThread` (блоки по **4 МБ**, `0x400000`; в fslog
  356 чтений размером 4–8 МБ) + `idDecompressSpawnThread::Run` / `StreamingDecompressThread::DecompressNextChunk`
  (потоковый `inflate` по кускам, `Z_NO_FLUSH`, на последнем `Z_FINISH`). В `Run` (base `0x13b3788`):
  `cmp size, zsize; b.ne → inflate`, при равенстве — `memcpy` из блока чтения. Записи >4 МБ (178 шт.) идут по
  oversize-пути (`bgl_preallocateOversized`, `TAG_RESOURCE_BGL_OVERSIZE`). Чтение и распаковка — разные потоки
  (конвейер), статистика `"Read sleep msec"` / `"Decompress sleep msec"`.
- Полезные cvar'ы (есть в update-`main`): `resource_trackLoadTimes`, `resource_showLoads`, `resource_spamLoads`,
  `bgl_debug`, `resource_disableBGL`, `resource_disableBGLForLargeFiles`, `fc_minCacheMemoryMB`/`fc_maxCacheMemoryMB`,
  `fs_reportReads`, `resource_loadLooseAssets`. Через `DM_DOOM_ARGS` можно получить время загрузки от самой игры.

## 4. Проверки целостности

- `*.verify` = HMAC-SHA256 блоки (`idFile_Protector::WriteHmacBlocks/VerifyHmacBlocks`, `idResourceContainer::InitHMAC`).
  Размеры сходятся с ~20 МиБ блоками на 64 байта + заголовок (`.resources`: 219 блоков × 64 + 40 = 14 056).
- base v0: `idResourceContainer::Verify` (`0x13a8108`) открывает `"%s.verify"` и проверяет блоки, **но не вызывается
  ни из кода (bl/b), ни через указатель** (его адрес встречается только в `.dynsym`).
- update (наша сборка): строк `.verify` / `%s.verify` **нет вовсе**; остались только сообщения
  `idFile_Verified::VerifyChunk…` и `idFileSystemLocal::ReadVerifiedFile` (не для контейнеров).
- fslog: ни одного чтения `.verify`, `gameresources.index`, `gameresources{,_002}.pindex`, `master.index` — игра
  открывает **только `gameresources_003.pindex`** и контейнеры по номеру патча.
- Индекс не содержит хэшей записей. Зашифрованные `binaryFile` (HMAC внутри файла) переносить байт-в-байт.

Вывод: переписанный индекс и контейнеры игра примет; ломается только то, что мы сами испортим (смещения/размеры).

## 5. Идея (a): распаковать контейнеры при установке

**Совместимость: да.** Варианты:

| Вариант | Как | Диск | Откат |
|---|---|---|---|
| A1. Доп. патч (как DOOMModLoader) | Оригиналы не трогаем. `gameresources_004.patch` = несжатые копии выбранных записей; `gameresources_004.pindex` = копия `_003` с `patch=4`, новым `off`, `zsize=size` для них | +raw выбранных: всё — **+13.06 ГиБ** (итого ~35 ГБ); только ≥1 МиБ (478 блобов) — +11.47 ГиБ; ≥4 МиБ (178) — +10.92 ГиБ | удалить 2 файла |
| A2. Консолидация | Новый `gameresources.resources` только с живыми записями (все несжатые, patch 0) + новый `_003.pindex`; старые `.patch` удалить | контейнеры 8.32 → 13.09 ГиБ, **+4.77 ГиБ** нетто (romfs ≈25 ГиБ); на время сборки нужны обе копии (~21 ГиБ) | только повторной распаковкой дампа |

Нужно проверить до работы: что игра сама находит `_004.pindex` (на PC — да, DOOMModLoader на этом построен; на
Switch fslog показывает выбор «самого свежего», но перебор открытий не логируется — проверить DM_FSLOG/strace на
открытие `_004`). Делать на ПК (Python, по образцу `tools/doom_index.py`), на телефон только пушить.

**Даст ли ускорение — скорее нет / немного:**
- Объём: окно загрузки в fslog (40–120 с, интро/первый уровень) — прочитано 848 МБ, целиком попавших записей
  368 МБ сжатых → 785 МБ raw; загрузка (0–40 с) 188 → 442 МБ. Полный уровень ≈ 0.8–1.5 ГБ на выходе inflate.
- Скорость inflate: Python/zlib 1.2.13 на этом ПК — 390 МБ/с на выходе (`_world.bmodel` 128 → 309 МБ за 0.79 с).
  На Cortex-X3/A715 ожидаемо ~250–400 МБ/с, на A510 в 3–4 раза меньше. Итого **2–5 с CPU на уровень** (до ~15 с,
  если поток распаковки сидит на малом ядре). 72 с так не набрать, если поток не голодает по другой причине.
- Обратная сторона: в ×1.95 больше байт через FUSE `/sdcard/Download` и page cache. В прогоне
  `work/logs/play/g2_8gen2/run_g2` (8 Gen 2, 30.10) **lmkd убивал фоновые приложения 54 раза** (`reason: cw`, psi),
  footprint процесса 1.7–1.8 ГиБ — удвоение файлового трафика под таким давлением может и ухудшить загрузку.
- Прямой выигрыш stored-пути ещё и в памяти: `ReadData` не выделяет временный буфер zsize.

**Подозреваемые для «один поток 100%» помимо inflate** (гипотезы, проверять профилем):
1. Наш write-protect трекинг пулов: в том же логе 8 Gen 2 к кадру 5 100 — `写保护缺页 281 124`, `宿主写前解保护
   303 410 页`, к 13:36:34 — уже 1.01 млн страниц и 9 934 переливки «池内容变化重传». Загрузка пишет сотни МБ в
   охраняемые пулы → SIGSEGV+mprotect на каждые 4 КБ.
2. CPU-транскод VT-страниц игрой (`vt_useTranscodeJobs`, `nvnTextureWriteTexels`) и наш partial upload.
3. Разбор decl/renderProg (48 тыс. записей `renderProg` за прогон) и наша трансляция шейдеров/пайплайнов.

**Как решить измерением (главному агенту, телефон):** `tools/perf_run.sh grab` во время перезагрузки чекпойнта +
`DM_DOOM_ARGS="+set resource_trackLoadTimes 1 +set bgl_debug 1"`. Если доминирует `main+0x1ffede0…` (inflate /
inflate_fast рядом) — распаковка виновата; если host-код (`xbg_*`, сигнал-хендлер, `mprotect`) — (a) не поможет.

**Если inflate всё же доминирует — дешевле, чем (a):** HLE-подмена `inflateInit2_/inflate/inflateEnd/inflateReset`
в `main` на хостовый zlib с NEON (zlib-ng в compat-режиме или chromium zlib; `z_stream` 1.2.3 на LP64 совпадает по
раскладке). libdeflate (уже в `dm/third_party`) годится только для `ReadData` (целый буфер, нужно дописать
финальный пустой блок `03 00` к копии ввода), а BGL распаковывает потоково. Выигрыш ~1.5–2.5× на inflate, 0 байт
диска, данные не трогаются. Дальше при необходимости — A1 только для крупных записей (≥1–4 МиБ, 178–478 блобов,
>90% объёма) — минимум файлов, полный откат.

## 6. Идея (b): грузить и конвертировать текстуры самим

- `image` в контейнерах: 2 084 блоба, 397 МБ raw (161 МБ сжато); после 4-байтового timestamp идёт `07 'MIB'` (BIM v7),
  дальше BE/LE поля (тип, w, h, мипы, формат). Данные уже в формате NVN (block-linear BC/ASTC); наш слой их
  деswizzle'ит при загрузке в Vulkan. Перехват дал бы мало: объём маленький, а пришлось бы повторять создание
  idImage, NVN-дескрипторы, VRAM-аллокатор `idNvnVRamAllocator` и стриминг мипов.
- Основная масса — VT: 5.0 ГиБ `.pages` + 5.5 ГиБ `.mega2`, собственный кодек страниц (`PageCompression.cpp`,
  cvar «Use uncompressed physical page images», `vt_usePreCompressedVmtrs`), транскод на CPU игрой в физкэш
  DXT5 4096²×3 + BC6H (см. `vt_streaming_fix.md`). Обойти = реверс кодека (публичных описаний нет; инструменты
  Eternal работают с другим форматом) + своя цепочка feedback → запрос страниц → транскод → слот кэша → таблица
  страниц. Это недели и высокий риск рассинхрона с игровым VT-менеджером.
- Предтранскод страниц в BC/ASTC при установке: 128² DXT5 = 16 КБ на слой, ≥3 слоя на страницу против исходных
  ~несжимаемых данных — по грубой оценке ×3–5 → **30–50 ГБ**. Для телефона неприемлемо.
- Вывод: (b) не делать. Если VT-транскод окажется горячим в профиле — оптимизировать его исполнение (потоки/ядра,
  partial upload, write-protect), а не переписывать загрузчик.

## 7. Рекомендация и риски

1. Сначала профиль перезагрузки (п. 5) — без него (a) рискует дать +5–13 ГиБ ради 2–5 с.
2. Если inflate горячий: HLE-подмена zlib на NEON-версию (0 диска, обратимо переключателем `DM_*`).
3. Только потом (a) в варианте A1 (доп. патч `_004`, крупные записи), инструментом на ПК; A2 — если важен диск и
   есть уверенность (нужно ~21 ГиБ временного места).
4. (b) — нет.

Риски (a): смещения/размеры в индексе — единственный каталог (`_003.pindex` или новый последний), ошибка = краш
при загрузке конкретного ассета, поэтому проверять сборку побайтно (распаковать каждую запись из нового контейнера
и сравнить с inflate оригинала). `size`/`zsize` u32 — максимум записи 324 МБ, влезает. `flags` и порядок записей
сохранять. `.verify` и `master.index` не используются — трогать не надо. Обновление игры с новым `_004.pindex`
не придёт (порт на фиксированной сборке), но номер патча лучше брать «последний+1» и проверять. Диск на телефоне:
сейчас 21.8 ГБ romfs; A1 полный — ~35 ГБ, A2 — ~25 ГБ. FUSE `/sdcard/Download`: больше трафика; перенос romfs в
`Android/data/<pkg>` (для своего каталога приложения FUSE, по моим сведениям, обходится в Android 11/12+ — не
проверено на наших телефонах) — отдельная, возможно более полезная мера.

## 8. Реализация §7 п.2 (30.10): zlib 1.2.3 → zlib-ng за переключателем

- Входы zlib в update `main` (все вызовы — прямые `bl`/`b`, ни релокаций, ни adrp+add, ни blr на них): `inflateReset`
  `0x1ffeac0`, `inflateInit2_` `0x1ffeb30` (проверяет `"1"` и `stream_size == 0x70`), `inflateInit_` `0x1ffecb0` (libpng),
  `inflate` `0x1ffede0`, `inflateEnd` `0x20009f0`; внутренние: updatewindow `0x20008b0`, inflate_table `0x2000a80`,
  inflate_fast `0x2001530`, adler32 `0x1ffe6f0`. `inflateSync/SetDictionary/Copy/Prime/GetHeader` в этой сборке нет.
- Пользователи: `ReadData` `0x1ef2710` (z_stream на стеке, zalloc/zfree игры из GOT `0x48d9af8/b00`, `inflate(Z_FINISH)`,
  `ret == 0` или `total_out == size`), BGL `0x1efc810` (z_stream в объекте +0x70, кольцо вывода, `Z_NO_FLUSH` → требует 0,
  на последнем блоке `Z_FINISH`), libpng (`0x1fe97a0` init_, `0x1fe9fd0`/`0x1ff2e00` inflate по строкам, `0x1fef260` zTXt,
  inflateReset), ещё `0x1786e0`, `0x189f60`, `0x1dbd7e0`, `0x1ef1b90`, `0x1ef7840`, `0x1efb840`, `0x1f04c60`.
- Код: гейты `ZLIB_GATES` в `dm/tools/patch_nso.py` (svc 0x90..0x94), хост `dm/host/hle_zlib.c`, библиотека
  `dm/third_party/zlib-ng` 2.3.3; переключатели `DM_ZLIB_FAST` / `DM_ZLIB_STATS` / `DM_ZLIB_VERIFY` (docs/SWITCHES.md §5).
- Офлайн-сверка (WSL x86-64, 1 514 записей `_003.pindex`, 959 МБ вывода; 5 шаблонов: ReadData, BGL-кольцо 4 МБ блоками,
  случайные границы/flush, испорченный ввод, zlib-обёртка + inflateReset): трасса «код возврата / in / out / totals / хэш»
  на каждый вызов **идентична** zlib 1.2.3 и для голого zlib-ng, и для `hle_zlib.c` через эмуляцию гейта во всех режимах
  (FAST, STATS при FAST=0, VERIFY). Скорость на этом ПК: 1.2.3 — 394 МБ/с, zlib-ng — 605 (×1.54), libdeflate — 653 (ещё +8 %,
  поэтому отдельный путь libdeflate для ReadData не делался).
- A/B на телефоне (главный агент): `dm/env` + новый `main.patched.bin` и `libdm.so`.
  1. `DM_ZLIB_STATS=1` (база): холодный старт до меню (секундомер от тапа до меню + `[zlib]` строки), затем перезагрузка
     чекпойнта 2–3 раза. Смотреть, сколько % одного ядра сидит в inflate и на каком потоке (`threads:`), и долю ReadData/BGL.
  2. `DM_ZLIB_STATS=1 DM_ZLIB_FAST=1`: то же. Сравнить время спиннера и `inflate ms`.
  3. Один раз `DM_ZLIB_VERIFY=1 DM_ZLIB_STATS=1`: загрузка уровня, в логе `VERIFY … 0 mismatches`.
  Если в (1) inflate занимает < ~10 % окна спиннера — 80/72 с объясняет не он (см. гипотезы §5), FAST даст секунды.
