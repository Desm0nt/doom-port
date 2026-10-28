# DOOM (2016) Switch → Android ARM64: разведка

Срез: 2026-10-27. Подход: как `d2r-mobile` (прямое исполнение ARM64 + офлайн-патч NSO + HLE + NVN→Vulkan),
максимально переиспользуя его код.

## Вход
- `<dump>/DOOM [0100416004C00000][v0].nsz` — base (program NCA 14.6 ГБ: ExeFS + RomFS).
- `<dump>/DOOM [0100416004C00800][v262144].nsz` — update v262144
  (program NCA 8.9 ГБ: ExeFS + RomFS-патч, BKTR).
- Распаковка ExeFS: `python tools/extract_exefs_nsz.py <nsz> work/exefs-update` (и `exefs-base`).
  NCZ без NCZBLOCK, один zstd-поток; поток читается вперёд с отбрасыванием, хэши PFS0 проверяются.
- Символы: `python tools/nso_symbols.py work/exefs-update main sdk` → `<mod>.raw`, `<mod>.symbols.json`.

## ExeFS (update)
| Модуль | Размер NSO | Заметки |
|---|---|---|
| main | 32.2 МБ | build_id `2847991952b3d7abd228624fcd3b0779…`, .text 0x22a2390, 567 импортов, 823k RELA |
| sdk | 5.2 МБ | nnSdk **6.4.0** (у D2R 11.4.3), 24 737 экспортов |
| subsdk0 | 7 КБ | заглушка (размер = rtld) |
| rtld, main.npdm | — | — |

## Импорты main (567)
- Графика: только `nvnBootstrapLoader` → схема перехвата NVN из d2r-mobile подходит напрямую.
- **217** совпадают по имени с HLE d2r-mobile, **346** разрешаются экспортами Doom `sdk`
  (исполняется как есть), **0** реально нерешённых (4 — маркеры `__rel_*`, их закрывает загрузчик).
- По пространствам имён: c 173, nn::os 74, nn::audio 55, crypto 30, socket 29, fs 26 (22 есть в HLE),
  friends/bcat/account/nifm/web/err — онлайн и системный UI, оставить заглушками.

## Сайты для офлайн-патча
- main: 0 `svc`, 0 обращений к `tpidr*`/`cntpct` (x18 ещё не считали — это делает patch_nso.py).
- sdk: 60 разных SVC (0x01–0x72); 54 `mrs tpidrro_el0`, 32 `cntpct`, 6 `tpidr_el0`.
  В `hle_svc.c`/`hle_thread.c` d2r-mobile реализовано то, что нужно SDK 11.4.3; недостающие
  SVC проявятся как `SVC #x` в логе при первом запуске.

## Главные отличия от D2R (риски)
1. **SDK 6.4.0 вместо 11.4.3**: другие имена и пути init в `hle_sdkinit.c`, возможно другой layout
   ThreadType/TLR, другие SVC (IPC, GetInfo ids).
2. **Шейдеры**: id Tech 6 хранит шейдеры в своих `.resources` в RomFS; нужно найти формат и прогнать через
   тот же Ryujinx-конвейер `dmsh_xlate`. Строка `glslc` в sdk — проверить, нет ли онлайн-компиляции GLSL.
3. **Объём NVN**: набор реально вызываемых функций шире, чем 216 у D2R (полноценный 3D, compute, MSAA/TAA).
4. Все адреса в CUTS и хуках UI d2r-mobile завязаны на build-id D2R → для Doom свой профиль.
5. RomFS = base 14.6 ГБ + BKTR-патч; на телефоне нужно ~20+ ГБ.

## RomFS и ресурсы (id Tech 6)
- `tools/ncz2nsp.py` восстанавливает точные NSP из NSZ (обратное AES-CTR шифрование; проверено хэшами секций
  и совпадением `main`) → `work/nsp/{base,update}.nsp`. `tools/romfs_doom.py list|extract` — base+BKTR через
  читалки d2r-port (`content_reader.py`), извлечение в `work/romfs` с проверкой IVFC. 343 файла, 21.8 ГБ.
- Ресурсы: `base/gameresources.resources` + `gameresources{,_002,_003}.patch`; актуальный каталог —
  `gameresources_003.pindex` (`tools/doom_index.py`: BE `\x05SER`, запись = id, 3 строки (u32 LE длина),
  u64 off, u32 size, u32 zsize, u32 flags, u8 patch; patch 0 = .resources, 1 = .patch, N = _00N.patch).
  Сжатие — raw deflate без финального блока (`zlib.decompressobj(-15)`).
- **Шейдеры**: `renderProg` с `name2 = generated/nx/<имя>_{vsps,vsgsps,cs}.sb` — готовый вывод glslc (как у D2R:
  код GPU `0x12345678`, контрольная секция `0x98761234`). 2565 записей → 619 уникальных файлов.
  Рядом лежат `generated/renderprogs/*_nx.bin` (GLSL-исходник) — для порта не нужны.
  `tools/doom_shaders.py <romfs> <out>` экспортирует их с ключами. **11 коллизий ключа d2r-mobile**
  (sha256 первых 0x880 байт контрольной секции) → для Doom ключ нужно расширить (вся контрольная секция + код),
  синхронно в `xbg_shader_key.c`.

## Патч NSO (d2r-mobile `patch_nso.py`)
- Doom собран старым компилятором: x18 как обычный регистр (main ~18k мест, sdk ~10.6k), таблицы переходов
  со словными смещениями (`ldrsw x18,[xB,xI,lsl #2]; add; br x18`, 4 места) и `mov sp, x18` (alloca, 1 место).
  Добавлены `site_br_targets` (цели читаются из таблицы офлайн, сравнение без NZCV) и `site_mov_sp_x18`.
- `equiv.py` (Unicorn): 0 расхождений на всех 29 238 местах; поддержка `JW`, пул `spawn` на Windows.
  Остаток: `mov sp, x18` при размере alloca = 0 оставляет слот сохранения ниже sp на 2 инструкции (сигнал в это
  окно — теоретический риск).

## Инструменты
- Rizin: установлен отдельно (в PATH для REA его нет —
  REA rizin-провайдер не стартует; задать `REA_RIZIN_COMMAND`). NSO-импорты rizin не разбирает.
- База: d2r-mobile (https://github.com/jhaizhou-ops/d2r-mobile), этот репозиторий — его форк для DOOM.
