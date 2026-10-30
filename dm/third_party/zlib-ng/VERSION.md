# zlib-ng (third party, zlib license, see LICENSE.md)

- Upstream: https://github.com/zlib-ng/zlib-ng tag `2.3.3` (commit `12731092979c6d07f42da27da673a9f6c7b13586`, 2026-02-03).
- Use: DOOM only. Host replacement of the game's statically linked zlib 1.2.3 inflate (`dm/host/hle_zlib.c`, switch
  `DM_ZLIB_FAST=1`), through the five entry gates `ZLIB_GATES` in `dm/tools/patch_nso.py`.
- Built by `dm/android/CMakeLists.txt` with upstream's own CMakeLists: native API (`ZLIB_COMPAT OFF`, `zng_*` names, no
  clash with the system libz), static, no tests, no gzFile functions; AArch64 NEON / CRC32 code selected at run time.
- Subset of the upstream tree, files unmodified: the top-level sources, `cmake/`, `arch/arm/`, `arch/generic/`. Left out:
  `test/`, `doc/`, `tools/`, `win32/`, the other architectures and the dotfiles.
- Checked offline against zlib 1.2.3 (2026-10-30, WSL x86-64): 1,514 real container entries (959 MB of output) decoded with
  the game's call patterns (ReadData one-shot Z_FINISH, BGL 4 MB blocks into a ring, random call boundaries and flush
  values, corrupted inputs, zlib wrapper with inflateReset): the per-call trace (return code, bytes in / out, totals, output
  hash) is identical. Speed on that PC: 1.2.3 394 MB/s, zlib-ng 605 MB/s, libdeflate 653 MB/s.
