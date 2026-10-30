# Third-party code in the DOOM port

The tree in `src/` is a fork of d2r-mobile (MIT, Copyright (c) 2026 jhaizhou, `src/LICENSE`).
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

Only the parts that DOOM uses were ported: its main imports, its SDK 6.4 code paths, its NVN calls, its shaders and its audio
renderer, on an Adreno 750 target. Everything new is behind `DM_GAME_DOOM`, except where noted.

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
