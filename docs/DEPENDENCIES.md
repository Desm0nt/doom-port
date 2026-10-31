# Dependencies: third-party code inherited from d2r-mobile

This page is kept from d2r-mobile, the base of this port, for its license summary of the third-party code in `dm/`.
Its original build instructions (Mac / iOS / Xcode) did not apply to DOOM Mobile and were removed; the DOOM build is described in the root
[README.md](../README.md) ("Building"), and the code ported for DOOM is listed in [THIRD_PARTY.md](THIRD_PARTY.md).

## Third-party code in this repository

These are already in the repository; nothing to download. Each keeps its license in its own directory.

| Path | What | License | Upstream |
|---|---|---|---|
| `dm/third_party/bcdec` | BC texture decompression | MIT or Unlicense (your choice) | <https://github.com/iOrange/bcdec> |
| `dm/third_party/flip` | Image difference metric | BSD 3-Clause | <https://github.com/NVlabs/flip> |
| `dm/third_party/libdeflate` | Decompression | MIT | <https://github.com/ebiggers/libdeflate> |
| `dm/gfx/vulkan/engine/third_party/bcdec` | BC texture decompression in the Android Vulkan graphics layer (same project as the bcdec above, pinned separately) | MIT or Unlicense (your choice) | <https://github.com/iOrange/bcdec> |
| `dm/gfx/vulkan/engine/third_party/pillow_reference` | Pillow's BC6 decoding reference tables, used only by the Vulkan graphics layer's tests | HPND (Pillow's license) | <https://github.com/python-pillow/Pillow> |
| `dm/third_party/ryujinx` | The shader translator from Ryujinx 1.3.2 (three projects, unmodified), which translates the original Switch shaders to SPIR-V | MIT | <https://github.com/Ryubing> |
| `dm/gfx/nvn/include/nvn.h` | Graphics interface declarations | MIT | Author named in the file header |
| Layout size algorithm in `dm/gfx/nvn/nvn_tex.c` | Ported from deko3d | zlib | <https://github.com/devkitPro/deko3d> |
| The 7 rewrites in `dm/tools/patch_nso.py` that let the game run offline | Taken from a community offline fix (LFS Mod Offline Fix) | Unknown | We have no reliable record of where it was first published or by whom; if you know, please open an issue, see [DERIVED.md](DERIVED.md) |
