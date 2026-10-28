# Third-party code and credits

DOOM Mobile is a fork of **d2r-mobile** and borrows code and ideas from several other open-source Switch-to-native
projects. Every ported file says so in a comment and keeps the original copyright line; ideas that were
re-implemented name their source in a comment at the place they are used. Everything added for DOOM is compiled only
with `-DDM_GAME=doom` (`DM_GAME_DOOM`), except where noted, so the D2R build of the base stays as it was.

## Projects

| Project | License / copyright | Source | Used for |
|---|---|---|---|
| **d2r-mobile** | MIT, Copyright (c) 2026 jhaizhou | https://github.com/jhaizhou-ops/d2r-mobile (base: commit d93985b) | The whole base: NSO loader and offline rewriter, HLE layer, NVN recorder, xbg Vulkan engine, shader translation chain, Android app. Its license notice is kept in [`LICENSE`](../LICENSE). |
| **nxnative** | MIT, Copyright (c) 2026 seedds | https://github.com/seedds/nxnative | Ported code: threads / SVCs, SDK 6.x services, audio renderer, NVN entry points, patcher recognizers, translator pieces (tables below) |
| **nx2android** / **nx2apk** | MIT, Copyright (c) 2026 seedds (its embedded `nxnative/LICENSE`) | https://github.com/shirai91/nx2android | Window acquire-sync fix (commit 1c6177d), Android haptics / ADPF / gamepad code, shader capture reader, LRZ touch |
| **BOTWRecompAndroid** | MIT, Copyright (c) 2026 Peiikai, notice by mamcjr | https://github.com/mamcjr/BOTWRecompAndroid (studied at 5675618) | Turnip / adrenotools driver loading (`runtime/nvn/vulkan/android_driver.c`), chunked submit, "sync lag" (82b71e2) |
| **LASRecompAndroid** | MIT, Copyright (c) 2026 Peiikai, notice by mamcjr | https://github.com/mamcjr/LASRecompAndroid (studied at 9443aa3) | Image format lists to keep UBWC compression (4380fe5), Turnip notes |
| **LASRecomp** | MIT, Copyright (c) 2026 Peiikai | https://github.com/peiikai/LASRecomp (studied at a4b53a3) | Background for the two Android ports above |
| **libadrenotools** | BSD-2-Clause, Copyright (c) 2021 Billy Laws (bylaws) | https://github.com/bylaws/libadrenotools — git submodule `third_party/libadrenotools` (with its own submodule liblinkernsbypass, BSD-2-Clause) | Loading a custom Vulkan driver (Turnip) in its own linker namespace |
| **Ryujinx** (Ryujinx.Graphics.Shader) | MIT, Copyright (c) Ryujinx Team and Contributors | vendored by d2r-mobile in `dm/third_party/ryujinx` | Maxwell shader → SPIR-V translation (`dmsh_xlate`, `dmsh_xlate_doom`) |
| **AMD FidelityFX Super Resolution 1.0** (RCAS pass) | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. | https://github.com/GPUOpen-Effects/FidelityFX-FSR | `dm/android/shaders/rcas.comp` (sharpening at present) |
| `tools/content_reader.py` | own code of the DOOM Mobile author (first written for an earlier Switch port), MIT | this repository | NSP / XCI / NCA / BKTR / IVFC readers used by `setup.sh` instead of hactool |
| bcdec, FLIP, libdeflate | see `dm/third_party/*/LICENSE` / `COPYING` | vendored by d2r-mobile | unchanged from the base |

Not redistributed, only used at run time or during development if the user installs them:

| Component | License | Notes |
|---|---|---|
| **Mesa Turnip** (Vulkan driver for Adreno) | MIT | The user supplies a driver package (e.g. an adrenotools Turnip build) and picks it in the settings. No driver binary is in this repository or the APK. |
| **Khronos Vulkan validation layers** | Apache-2.0 | Packaged only for local diagnostics (`tools/build_apk.ps1 -Validation`) from a download the developer makes. |
| Vulkan headers (Android NDK) | Apache-2.0 / MIT | From the NDK. |

**DOOM** is © id Software / Bethesda Softworks (ZeniMax Media); the Switch version was made with Panic Button.
This project is not affiliated with or endorsed by any of them and contains no game code or data — see
[doom/DERIVED.md](doom/DERIVED.md) and [doom/GAME_FILES.md](doom/GAME_FILES.md).

## Code ported from nxnative and nx2android

### Host (`dm/host`)

| Our file | Ported from | What |
|---|---|---|
| `hle_thread.c` | nxnative `dm/host/hle_thread.c` | Thread scheduling and sync: same-core wake handoffs (`note_runnable`, `dm_yield_to_core_peers`); native HLE waits, `dm_host_lock`; ideal core / affinity rules; `WaitForAddress` type 3, `SignalToAddress` type 2 with count ≤ 0; per-object WaitSynchronization wakes; yield back-off with progress detection, precise sleeps, `dm_spin_wait`; SystemEvent binding lifetimes, free thread count |
| `hle_svc.c` | nxnative `dm/host/hle_svc.c` | Memory SVCs: SetHeapSize returns ResultOutOfMemory; MapMemory argument and borrowed-source checks; QueryMemory over the alias page bitmap and module segments; MapPhysicalMemory page bitmap with zeroing; GetInfo 16 / 17 / 20 / 24 |
| `hle_guestthread.c` | nxnative `dm/host/hle_guestthread.c` | Host-created guest threads are marked as host-owned |
| `hle_audren.c` (new) | nxnative `dm/host/hle_audren.c` | The SDK's own audio renderer runs on the CPU (DFC manager, CommandListProcessor) and plays through AAudio |
| `hle_audio.c` | nxnative `dm/host/hle_audio.c` | `dm_pcmout_*` host PCM output (new functions; D2R paths unchanged) |
| `hle_sf.c` (new) | nxnative `dm/host/hle_sf.c` | `nn::sf::hipc` client takeover: names any service DOOM reaches that the host does not handle |
| `hle_oe.c` | nxnative `dm/host/hle_oe.c` | Focus handling mode, language mapping |
| `hle_fs.c` | nxnative `dm/host/hle_fs.c` | Path normalisation, EFAULT retry on ReadFile, directory entry count filter, Delete / RenameDirectory |
| `hle_net.c` | nxnative `dm/host/hle_net.c` | nifm request on hold, Poll / Select idle wait |
| `hle_haptics.c`, `hle_haptics.h` (new) | nxnative `dm/host/hle_haptics.{c,h}` | Vibration state |
| `hle_hid.c` | nxnative `dm/host/hle_hid.c` | Vibration handles and send, six-axis layout and handles |

### Graphics (`dm/gfx`)

| Our file | Ported from | What |
|---|---|---|
| `nvn/hle_nvn.c` | nxnative `dm/gfx/nvn/hle_nvn.c` | NVN entry points DOOM uses: binds, draws, indirect draws, Downsample, clears; raster state, fence sync; texture swizzle, depth-stencil mode, sampler reduction, getters |
| `nvn/hle_nvn.c` (window acquire sync) | nx2android commit 1c6177d | The acquire sync is fenced at the texture's last present instead of at all work so far (`DM_NVN_ACQ_NOW=1` restores the old fence) |
| `nvn/hle_nvn.c` (timestamps) | nxnative `nvn_timestamp.h` | Counter timestamps in NVN's 614.4 MHz ticks |
| `nvn/nvn_obj.h` | layouts follow nxnative `dm/gfx/nvn/nvn_obj.h` (ideas, no code copied) | New record ops and fields |
| `tools/dmsh_xlate_doom/Program.cs` (new) | nxnative `dm/gfx/tools/dmsh_xlate/Program.cs` | DOOM shader translator: stage linking, translator messages, c2 image handle decode, FP64 lowering, image query retry |
| `tools/dmsh_vk_doom.py` (c1 guard) | idea of nxnative `dm/gfx/tools/c1_guard.py`, redone on SPIR-V | Out-of-range c1 reads return 0 |
| `vulkan/engine/xbg_game.cpp` (LRZ touch, `DM_GFX_LRZ_TOUCH`, off) | nx2android `lrz_touch` | Experimental |

### Tools

| Our file | Ported from | What |
|---|---|---|
| `dm/tools/patch_nso.py` | nxnative `dm/tools/patch_nso.py` | Jump-table recognizers (offset / byte / halfword tables, indirect tail calls) and new x18 site kinds: `blr` / `br` calls, writes to sp, `sub sp` alignment, exclusive-window rules |
| `dm/tools/nso_code.py` (new) | nxnative `dm/tools/nso_code.py` | Code / data block analysis (copied unchanged) |
| `tools/doom_shader_jobs.py` | nx2android `nxnative/dm/android/tools/shader_vk.py` | `captured()`: reads shader captures made on the device |

### Android (`dm/android`)

| Our file | Ported from | What |
|---|---|---|
| `haptics_android.c` (new) | nx2android `nxnative/dm/android/haptics_android.c` | Controller and phone rumble |
| `perf_android.c` (new) | nx2android `nxnative/dm/android/perf_android.c` | ADPF hint session, thermal status, display rate vote |
| `front_android.c` | nx2android `nxnative/dm/android/front_android.c` | Gamepad button mapping, pad publish |

## Ideas from the BOTW / LAS recompilation ports (MIT)

Studied on 28.10.2026 (report: [research/recomp_android_repos.md](research/recomp_android_repos.md)). No files were
copied; the ideas below were re-implemented in our code, which names the source in a comment.

| Our code | Idea from | What |
|---|---|---|
| `dm/android/vk_driver.c`, `vk_tramp.S` / `vk_tramp_names.h` (generated by `tools/gen_vk_tramp.py`) | BOTWRecompAndroid `runtime/nvn/vulkan/android_driver.c` (adrenotools + volk) | Vulkan driver chosen at run time (system driver or Turnip through libadrenotools, `DM_VK_DRIVER`); we use trampolines instead of volk |
| `dm/gfx/nvn/hle_nvn.c` `nvnSyncWait` (`DM_NVN_SYNC_LAG=1`, off by default) | BOTWRecompAndroid 82b71e2 "sync lag" | Wait for the fence of frame N-1 instead of N |
| `dm/gfx/vulkan/engine/xbg_game.cpp` `submit_chunk()` (`DM_GFX_SUBMIT_CHUNK_US`, default 2000) | BOTWRecompAndroid `exec.c` chunked submit (`LARECOMP_VK_CHUNK_US`), bf2369b | Submit the recording slot after N µs of translation at a guest-submit boundary |
| `xbg_game.cpp` `upgrade_mutable` with `VkImageFormatListCreateInfo` | made independently the night before; LASRecompAndroid 4380fe5 has the same finding | Mutable-format images keep UBWC compression |
| `xbg_game.cpp` `invariant_rewrite` (`DM_GFX_INVARIANT`, off) | BOTWRecompAndroid `shaders.c` `no_contraction()`, invariant Position | Tried against the model holes; not their cause here |
