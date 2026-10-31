# d2r-mobile: acknowledgements and license

[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) (MIT, jhaizhou) is the base of DOOM Mobile: the loader, HLE host,
NVN recorder, Vulkan engine and Android app in `dm/` come from it. This page keeps the acknowledgements and license
section of its README, because the people credited there wrote code this port is built on. The rest of that README
(Diablo II: Resurrected on iOS / macOS / HarmonyOS / Android) does not apply here; see the upstream repository.

## Acknowledgements

**The Android version in v2.0 comes from three community authors. Special thanks to:**

- **[@yyan160](https://github.com/yyan160)** (Yuechuan Yang): the Android port itself ([#113](https://github.com/jhaizhou-ops/d2r-mobile/pull/113)) — the Android app, the JNI host, the touch overlay, Vulkan presentation, a full round of performance work measured step by step on a phone (the Redmi K50 Ultra camp went from 7.7 fps to a median 60 fps), and the changes that let the shared host build and run on both Apple and Android. Every point raised in review was fixed on the branch.
- **[@biglawine](https://github.com/biglawine)**: the HarmonyOS port ([#114](https://github.com/jhaizhou-ops/d2r-mobile/pull/114)) and its NVN-to-Vulkan replay engine, which is where the Android graphics layer comes from; they also set up the Android / HarmonyOS collaboration repositories and generated the shader bundles used for on-device testing.
- **[@vickwv](https://github.com/vickwv)**: a separate, independent Android port ([#112](https://github.com/jhaizhou-ops/d2r-mobile/pull/112)). Its ideas were each measured on the Android version, and its 3D brightening approach was adopted.

This project stands on the open-source work below. Thank you to its authors and maintainers.

- **Ryujinx** (Ryubing 1.3.2, MIT): `dm/third_party/ryujinx` bundles its shader translator unchanged (the Ryujinx.Graphics.Shader, Spv.Generator and Ryujinx.Common projects), which translates the Switch game's shaders to SPIR-V. During development we also ran the original game in a patched Ryujinx as the reference for comparing pictures and game state (see [dm/verify/README.md](../../dm/verify/README.md); the scripts that build and drive it are not in this repository).
- **deko3d** (devkitPro, zlib): the texture layout size algorithm in `dm/gfx/nvn/nvn_tex.c` is ported from it.
- **nvn.h** (h1k421, MIT): the graphics API declarations in `dm/gfx/nvn/include/nvn.h`.
- **libdeflate** (Eric Biggers, MIT): decompressing game data.
- **bcdec** (iOrange, MIT or Unlicense): decoding BC textures on devices that do not support the BC formats.
- **FLIP** (NVIDIA, BSD 3-Clause): the image difference metric used in image-quality checks.
- **Offline fix patch** (LFS Mod Offline Fix): the 7 rewrites in `dm/tools/patch_nso.py` that let the game run offline come from this community patch. We have no reliable record of where it was first published or who wrote it; if you know, please open an Issue.
- Tools used to build and verify: SPIRV-Cross (SPIR-V to Metal Shading Language), XcodeGen, .NET, Capstone and Unicorn (disassembly and equivalence checks of the rewrites), Pillow, numpy, lz4, astcenc (the Legacy Device Pack), hactool (extracting files from a game package), libimobiledevice and LLVM. Versions and links are in upstream's [docs/DEPENDENCIES.md](https://github.com/jhaizhou-ops/d2r-mobile/blob/main/docs/DEPENDENCIES.md).
- Community contributions: Joe Hu reported and diagnosed several Greater Rift issues and proposed a verification matrix; seedds found and diagnosed the render-target view format issue on another port (#36). Both are credited as co-authors on the corresponding commits.

## License

MIT, see [LICENSE](../../LICENSE). Third-party code keeps its own license in its own directory; see [docs/DEPENDENCIES.md](../DEPENDENCIES.md).
