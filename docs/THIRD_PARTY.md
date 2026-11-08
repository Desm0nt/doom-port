# Third-party code in the DOOM port

The tree in `dm/` is a fork of d2r-mobile (MIT, Copyright (c) 2026 jhaizhou, kept in the root `LICENSE`); the third-party
code it brought is listed in [DEPENDENCIES.md](DEPENDENCIES.md) and its contributors in [upstream/README-d2r-mobile.md](upstream/README-d2r-mobile.md).
Code was also ported from two other MIT-licensed projects. Each ported file says so in a comment
and keeps the original copyright line.

| Project | License / copyright | Source |
|---|---|---|
| nxnative | MIT, Copyright (c) 2026 seedds | https://github.com/seedds/nxnative |
| nx2android | MIT, Copyright (c) 2026 seedds (its embedded `nxnative/LICENSE`) | https://github.com/shirai91/nx2android |
| BOTWRecompAndroid | MIT, Copyright (c) 2026 Peiikai + notice mamcjr | https://github.com/mamcjr/BOTWRecompAndroid (studied at 5675618) |
| LASRecompAndroid | MIT, Copyright (c) 2026 Peiikai + notice mamcjr | https://github.com/mamcjr/LASRecompAndroid (studied at 9443aa3) |
| LASRecomp | MIT, Copyright (c) 2026 Peiikai | https://github.com/peiikai/LASRecomp (studied at a4b53a3) |
| libadrenotools | BSD-2-Clause, bylaws | https://github.com/bylaws/libadrenotools — `third_party/libadrenotools` |
| zlib-ng 2.3.3 | zlib license, (C) 1995-2024 Jean-loup Gailly and Mark Adler (its `LICENSE.md`) | https://github.com/zlib-ng/zlib-ng tag 2.3.3 — `dm/third_party/zlib-ng` (unmodified subset, `VERSION.md`); used by `dm/host/hle_zlib.c` (`DM_ZLIB_FAST=1`) instead of the game's own zlib 1.2.3 inflate |
| AMD FidelityFX Super Resolution 1.0 (RCAS) | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. | https://github.com/GPUOpen-Effects/FidelityFX-FSR — `dm/android/shaders/rcas.comp` |
| Snapdragon Game Super Resolution 1 (SGSR1, mobile variant) | BSD-3-Clause, Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved. (licence text in the shader header) | https://github.com/quic/snapdragon-gsr — re-written from the published algorithm (nothing fetched) in `dm/android/shaders/gsr.frag` (`DM_GFX_UPSCALE`); the same pass reuses the RCAS math above |
| nx2android gamepad themes | MIT, Copyright (c) 2026 seedds (`dm/android/front-assets/LICENSE-nx2android-themes.txt`) | https://github.com/shirai91/nx2android — the on-screen gamepad skins (`themes/`) and the default layout (`overlay_default.tsv`) in `dm/android/front-assets`, unmodified |
| Granite (texture decode shaders) | MIT, Copyright (c) 2017-2026 Hans-Kristian Arntzen (shader headers: 2020-2026) | https://github.com/Themaister/Granite at 69f450c62dc3, `assets/shaders/decode/{bc6.comp,bc7.comp,bitextract.h}` — ported (modified) into `dm/gfx/vulkan/engine/xbg_bc_gpu.glsl` (GPU BC decode, `DM_GFX_BC_DECODE=gpu`); licence and hashes in `dm/gfx/vulkan/engine/third_party/granite_decode/` |
| bcdec (BC6H decode tables) | MIT or Unlicense, Copyright (c) 2022 Sergii Kudlai | https://github.com/iOrange/bcdec at 80859ed (already in `dm/gfx/vulkan/engine/third_party/bcdec`) — `bcdec_bc6h_half` ported to numpy in `tools/astc_overlay_doom.py` (`bc6h_unsigned_to_half`, bit-exact, `tools/test_astc_overlay_doom.py --bcdec`) |

Only the parts that DOOM uses were ported: its main imports, its SDK 6.4 code paths, its NVN calls, its shaders and its audio
renderer, on an Adreno 750 target. Everything new is behind `DM_GAME_DOOM`, except where noted.

## Vulkan driver packages bundled in the APK (`assets/drivers/<name>/`, docs/doom/BUGS_AND_FIXES.md §2.39)

Unmodified adrenotools packages (prebuilt Mesa drivers), fetched by `tools/fetch_turnip.py` from the pinned release assets below
and checked against their SHA-256. A package's own text files (e.g. `NOTICE.txt` with the licence texts of everything compiled
into it) are bundled and copied with it. The app copies to the phone only the packages for its GPU.

| Package (folder) | License / copyright | Source |
|---|---|---|
| Mesa Turnip v27.0.0-20261029 (`turnip27`, `turnip27-a710`, `turnip27-a8xx`, `turnip27-8g2`) | Mesa 3D (Turnip / freedreno): MIT, Copyright the Mesa authors; build repository licence GPL-3.0 (its scripts and patches) | https://github.com/The412Banner/Banners-Turnip release `v27.0.0-20261029` |
| RADV Xclipse 1.8.0 (`radv-xclipse-1.8.0`, Mesa 27.0.0-devel-269238e) | MIT, Copyright the Mesa authors, Copyright 2026 JimVulkan; bundled libraries (libdrm etc.) as listed in its `NOTICE.txt` | https://github.com/JimVulkan/radv-xclipse release `v1.8.0`, asset `radv-xclipse-27.0.0-devel-269238e.zip` |
| PanVK Mali kbase 1.6.0 (`panvk-jim-1.6.0`, Mesa 27.0.0-devel r10) | MIT, Copyright the Mesa authors, Copyright 2026 JimVulkan; bundled libraries as listed in its `NOTICE.txt` | https://github.com/JimVulkan/mali-panvk release `v1.6.0`, asset `mali-panvk-27.0.0-devel-r10.zip` |
| Mesa PanVK kbase Android 26.3.1 (`panvk-zenith-26.3.1`, Mesa 26.3.0-devel git-795ca7ff3e) | Mesa 3D (PanVK): MIT, Copyright the Mesa authors; the build repository declares no licence and the zip carries no notice file | https://github.com/zenithblue-oss/panvk-kbase-android release `panvk-kbase-v26.3.1`, asset `Mesa-PanVK-Android-v26.3.1-795ca7ff-adrenotools.zip` |

Packages the player downloads in the app's driver manager (`drivers/sources.json`: the repositories above, K11MCH1/AdrenoToolsDrivers,
MineGraph-bilibili/panvk-Android) are fetched from those projects' GitHub releases on the phone and are not distributed by us; the
Qualcomm proprietary drivers among them (`Qualcomm_*_adpkg.zip`) are closed source.

## Setup-time Python packages (PC only, not in the app; `tools/requirements-astc.txt`, pip-installed by setup into `tools/.runtime`)

Used by `tools/astc_overlay_doom.py` (ASTC / EAC texture overlay for GPUs without BC, docs/doom/BUGS_AND_FIXES.md §2.26). Nothing
of them is copied into the repository or the APK; the overlay they produce is made on the player's PC from the player's own RomFS.

| Package (pinned) | License / copyright | Source / use |
|---|---|---|
| astc-encoder-py 0.1.12 | MIT, Copyright (c) 2024 Rudolf Kolbe; bundles ARM astc-encoder: Apache-2.0, Copyright 2011-2024 Arm Limited | https://github.com/K0lb3/astc-encoder-py (PyPI `astc-encoder-py`), https://github.com/ARM-software/astc-encoder — BC3 / BC7 → ASTC 4x4 LDR, BC6H → ASTC 4x4 HDR |
| etcpak 0.9.15 | MIT, Copyright (c) 2021 K0lb3; bundles wolfpld/etcpak: BSD-3-Clause, Copyright (c) 2013 Bartosz Taudul | https://github.com/K0lb3/etcpak (PyPI `etcpak`), https://github.com/wolfpld/etcpak — BC4 → EAC R11, BC5 → EAC RG11 (BC1 → ETC2 RGB8 with `--bc1-etc2`) |
| texture2ddecoder 1.0.6 | MIT, Copyright (c) 2020 K0lb3 | https://github.com/K0lb3/texture2ddecoder (PyPI) — BC1 / BC3 / BC4 / BC5 / BC7 decode; ETC2 / EAC decode for the quality report |
| numpy 2.2.6 (2.3.5 on Python 3.14+) | BSD-3-Clause, Copyright (c) 2005-2024 NumPy Developers | https://numpy.org (PyPI) — image arrays, the BC6H decoder |
| archspec 0.2.7 | Apache-2.0 OR MIT, Copyright 2019-2024 Lawrence Livermore National Security, LLC and other Archspec Project Developers | https://github.com/archspec/archspec (PyPI) — CPU detection, a dependency of astc-encoder-py and etcpak |

## Host (dm/host)

| Our file | Ported from | What |
|---|---|---|
| `hle_thread.c` | nxnative `dm/host/hle_thread.c` | Thread scheduling and sync:<br>• same-core wake handoffs (`note_runnable`, `dm_yield_to_core_peers`)<br>• native HLE waits, `dm_host_lock`<br>• ideal core / affinity rules<br>• `WaitForAddress` type 3; `SignalToAddress` type 2 with count ≤ 0<br>• per-object WaitSynchronization wakes<br>• yield back-off with progress detection, precise sleeps, `dm_spin_wait`<br>• SystemEvent binding lifetimes, free thread count |
| `hle_svc.c` | nxnative `dm/host/hle_svc.c` | Memory SVCs:<br>• SetHeapSize returns ResultOutOfMemory<br>• MapMemory argument and borrowed-source checks<br>• QueryMemory over the alias page bitmap and module segments<br>• MapPhysicalMemory page bitmap, with zeroing<br>• GetInfo 16 / 17 / 20 / 24 |
| `hle_guestthread.c` | nxnative `dm/host/hle_guestthread.c` | Host-created guest threads are marked as host-owned |
| `hle_audren.c` (new) | nxnative `dm/host/hle_audren.c` | The SDK's own audio renderer runs on the CPU (DFC manager, CommandListProcessor) and plays through AAudio |
| `hle_audio.c` | nxnative `dm/host/hle_audio.c` | `dm_pcmout_*` host PCM output (new functions; D2R paths unchanged) |
| `hle_sf.c` (new) | nxnative `dm/host/hle_sf.c` | `nn::sf::hipc` client takeover: names any service DOOM reaches that we don't handle |
| `hle_oe.c` | nxnative `dm/host/hle_oe.c` | Focus handling mode, language mapping |
| `hle_fs.c` | nxnative `dm/host/hle_fs.c` | Path normalisation, EFAULT retry on ReadFile, directory entry count filter, Delete/RenameDirectory |
| `hle_net.c` | nxnative `dm/host/hle_net.c` | nifm request on hold, Poll/Select idle wait |
| `hle_haptics.c`, `hle_haptics.h` (new) | nxnative `dm/host/hle_haptics.{c,h}` | Vibration state |
| `hle_hid.c` | nxnative `dm/host/hle_hid.c` | Vibration handles and send, SixAxis layout and handles |

## Graphics (dm/gfx)

| Our file | Ported from | What |
|---|---|---|
| `gfx/nvn/hle_nvn.c` | nxnative `dm/gfx/nvn/hle_nvn.c` | NVN entry points DOOM requests:<br>• binds, draws, indirect draws, Downsample, clears<br>• raster state, fence sync<br>• texture swizzle, depth-stencil mode, sampler reduction, getters |
| `gfx/nvn/hle_nvn.c` (window acquire sync) | nx2android commit 1c6177d | The acquire sync is fenced at the texture's last present instead of at all work so far (`win_t.done[]`, `DM_NVN_ACQ_NOW=1` restores the old fence). |
| `gfx/nvn/nvn_obj.h` | layouts follow nxnative `dm/gfx/nvn/nvn_obj.h` (ideas, no code copied) | New record ops and fields |
| `gfx/tools/dmsh_xlate_doom/Program.cs` (new) | nxnative `dm/gfx/tools/dmsh_xlate/Program.cs` | DOOM shader translator:<br>• stage linking, translator messages<br>• c2 image handle decode<br>• FP64 lowering, image query retry |

## Tools

| Our file | Ported from | What |
|---|---|---|
| `dm/tools/patch_nso.py` | nxnative `dm/tools/patch_nso.py` | Jump-table recognizers (offset / byte / halfword tables, indirect tail calls) and new x18 site kinds:<br>• `blr` / `br` calls<br>• writes to sp, `sub sp` alignment<br>• exclusive-window rules |
| `dm/tools/nso_code.py` (new) | nxnative `dm/tools/nso_code.py` | Code / data block analysis (copied unchanged) |
| `tools/doom_shader_jobs.py` | nx2android `nxnative/dm/android/tools/shader_vk.py` | `captured()`: reads shader captures made on the device |

## Android (dm/android)

| Our file | Ported from | What |
|---|---|---|
| `haptics_android.c` (new) | nx2android `nxnative/dm/android/haptics_android.c` | Controller and phone rumble |
| `perf_android.c` (new) | nx2android `nxnative/dm/android/perf_android.c` | ADPF hint session, thermal status, display rate vote |
| `front_android.c` | nx2android `nxnative/dm/android/front_android.c` | Gamepad button mapping, pad publish |

## Ideas from the BOTW / LAS recompilation ports (MIT)

Studied on 28.10.2026 (report: `docs/research/recomp_android_repos.md`). No files were copied; the ideas below were
re-implemented in our code, which names the source in a comment.

| Our code | Idea from | What |
|---|---|---|
| `dm/android/vk_driver.c`, `vk_tramp.S` / `vk_tramp_names.h` (generated by `tools/gen_vk_tramp.py`) | BOTWRecompAndroid `runtime/nvn/vulkan/android_driver.c` (adrenotools + volk) | Vulkan driver chosen at run time (system driver or Turnip through libadrenotools, `DM_VK_DRIVER`); we use trampolines instead of volk |
| `gfx/nvn/hle_nvn.c` `nvnSyncWait` (`DM_NVN_SYNC_LAG=1`, off by default) | BOTWRecompAndroid 82b71e2 "sync lag" | Wait for the fence of frame N-1 instead of N |
| `gfx/vulkan/engine/xbg_game.cpp` `submit_chunk()` (`DM_GFX_SUBMIT_CHUNK_US`, default 2000) | BOTWRecompAndroid `exec.c` chunked submit (`LARECOMP_VK_CHUNK_US`), bf2369b | Submit the recording slot after N µs of translation at a guest-submit boundary |
| `xbg_game.cpp` `upgrade_mutable` with `VkImageFormatListCreateInfo` | (made independently the night before; LASRecompAndroid 4380fe5 has the same finding) | Mutable-format images keep UBWC compression |
| `xbg_game.cpp` `invariant_rewrite` (`DM_GFX_INVARIANT`, off) | BOTWRecompAndroid `shaders.c` `no_contraction()`, invariant Position | Tried against the model holes; not their cause here |
