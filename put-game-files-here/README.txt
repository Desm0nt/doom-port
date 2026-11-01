DOOM Mobile - put your game files here
======================================

Put these three things into this folder (any file names are fine):

  1. DOOM base game        title 0100416004C00000         NSP, NSZ, XCI or XCZ
  2. DOOM update v262144   title 0100416004C00800         NSP, NSZ, XCI or XCZ
                           (an XCI that already contains this update counts as both 1 and 2)
  3. prod.keys             the keys of YOUR OWN console (dumped with Lockpick_RCM)

Split dumps from a FAT32 SD card work too: the folder with 00, 01, ... parts, or name.xc0 + name.xc1 ...

Then run setup:
  Windows:        double-click setup.bat (one folder up)
  Linux / macOS:  ./setup.sh  (one folder up)

Your files are only read; nothing is copied, changed or uploaded. Everything in this folder except this
README is ignored by Git. Dump the game and the keys from a Switch you own: they are never shared by this
project. Step-by-step guide: docs/INSTALL.md (Russian: docs/INSTALL.ru.md).

----------------------------------------------------------------------------------------------------------

DOOM Mobile - положите сюда файлы игры
======================================

Положите в эту папку три вещи (имена файлов любые):

  1. Базовая игра DOOM      title 0100416004C00000        NSP, NSZ, XCI или XCZ
  2. Обновление v262144     title 0100416004C00800        NSP, NSZ, XCI или XCZ
                            (XCI, в котором уже есть это обновление, считается сразу за 1 и 2)
  3. prod.keys              ключи ВАШЕЙ консоли (снятые через Lockpick_RCM)

Подходят и разбитые дампы с карты FAT32: папка с частями 00, 01, ... или name.xc0 + name.xc1 ...

Затем запустите установку:
  Windows:        двойной щелчок по setup.bat (на уровень выше)
  Linux / macOS:  ./setup.sh  (на уровень выше)

Файлы только читаются: ничего не копируется, не меняется и не отправляется. Подробная инструкция:
docs/INSTALL.ru.md (по-английски: docs/INSTALL.md).
