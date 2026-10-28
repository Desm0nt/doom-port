# nxnative history: what applies to the DOOM Android/Vulkan port

Studied: `seedds/nxnative`, a blobless clone of all 594 commits, 2026-10-12 to `d0b7c84` (2026-10-28), plus the branch
`codex/nier-geometry-root-fix`. I made no changes to any repository.
Compared with our port: `dm/gfx/vulkan/engine/xbg_game.cpp`, `dm/gfx/nvn/hle_nvn.c`, `dm/gfx/tools/dmsh_vk*.py`,
and `docs/ACTIVE-PORT.md` ("Производительность").

## 0. Read this first: what nxnative is and is not

- **No DOOM anywhere in the history.** `git grep -i doom HEAD` finds nothing, and `git log --all -S DOOM` finds nothing.
  No profile has DOOM's title ID. No commit subject mentions id Tech, Adreno, Android or DCVS.
- **No NVN→Vulkan backend.** nxnative is iOS/macOS only. NVN is replayed on **Metal 4** (`dm/gfx/nvn/nvn_mtl.m`, 1 MB).
  `dm/gfx/vk/hle_vk.m` translates a *guest* Vulkan game (Hades, Mario Galaxy) to Metal. That layer explicitly does not
  translate geometry shaders (docs/reference/rendering.md, "Not translated in the Vulkan layer").
- Our `src/dm` tree (android/harmony/vulkan/`xbg_*`) comes from the **D2R/Harmony line**. nxnative removed all D2R code
  in `79d82f7` (2026-10-21, "Remove the D2R code and the legacy frame step"). So the current nxnative is a cousin of our
  code, not its parent.
- It is still useful. Apple GPUs are TBDR tilers like Adreno. Metal has **no geometry stage** and **no subgroup ops in
  vertex functions**. Its shader chain is the same Ryujinx translator. Many NVN-semantics fixes apply to us one for one.
  The ranking below is by expected impact for our GPU-bound case (GPU 30-45 ms, CPU about 10 ms, 17-22 fps).

---

## 1. GPU timestamp units and timebase: may switch on DOOM's own dynamic resolution  [HIGH, cheap]

**Commits:** `b1c814f` (2026-10-15, "GPU timestamps in nanoseconds on the guest clock"); `31b5944` (2026-10-26, NieR
port, adds `dm/gfx/nvn/nvn_timestamp.h`); the profile key `gpu_timestamps segment` (BotW, NieR;
docs/reference/frame-rate.md "GPU timestamps per submission segment" and "Raw NVN timestamp units").

**Problems they fixed:**
1. *Units.* The raw `NVNcounterData.timestamp` field is in **614.4 MHz ticks (384 ticks per 625 ns)**, not 19.2 MHz CPU
   ticks. Ryujinx's GPU report path uses the same 384/625 ratio. The host had written 19.2 MHz ticks, and it converted
   them back in `nvnDeviceGetTimestampInNanoseconds` with the same wrong rate. A game that calls that API sees correct
   time. **A game that subtracts two raw reports and converts inline reads GPU time 32 times too small.** NieR's
   dynamic-resolution controller does exactly that: a 16 ms span read as 0.5 ms.
   ```c
   // dm/gfx/nvn/nvn_timestamp.h (HEAD)
   static inline uint64_t nvn_timestamp_from_ns(uint64_t ns) { return (uint64_t)((__uint128_t)ns * 384 / 625); }
   static inline uint64_t nvn_timestamp_to_ns(uint64_t ticks) { return (uint64_t)((__uint128_t)ticks * 625 / 384); }
   ```
2. *Timebase.* GPU timestamps must share their epoch with `svcGetSystemTick` and the guest clock. Mario compares the GPU
   end time with a CPU frame-start tick (`main+0x20a0394`). With the wrong clock it fell to 30 fps (`b1c814f`).
3. *Granularity.* When every report in one submission has the same time, "GPU time = last report − first report" is 0.
   BotW's interval controller **and its dynamic resolution** then never react. The fix (`gpu_timestamps segment`): each
   report takes the GPU start or end of the segment it was encoded in.

**Our port:** we have bug 1 exactly. `xbg_game.cpp` `write_report()`:
```cpp
result.timestamp=uint64_t(double(value)*properties.limits.timestampPeriod*12.0/625.0);   // 19.2 MHz ticks
```
and `hle_nvn.c:1956` `nvnDeviceGetTimestampInNanoseconds` converts back with `*625/12`. We probably also have bug 2.
`value` is the raw Vulkan GPU clock, so its epoch is unrelated to `guest_ns()`, which `GetCurrentTimestampInNanoseconds`
returns.

**Why this matters for DOOM (inference, not verified):** DOOM 2016 on Switch is known to use dynamic resolution with a
30 fps target. Our notes say the 3D always renders at full 1280x720 even though the GPU needs 30-45 ms, which is over
the 33 ms budget. That fits a controller that thinks the GPU is idle. A 32x unit error, or a CPU-vs-GPU epoch mismatch,
would cause exactly that. Unit check: with the 32x error, our 30-45 ms reads as about 1-1.4 ms.

**What to do:**
- Write `ns*384/625` in `write_report`, and use `ticks*625/384` in `nvnDeviceGetTimestampInNanoseconds`.
- Shift GPU ns into the `guest_ns()` epoch. Sample a (GPU timestamp, CPU monotonic) pair once with
  `VK_EXT/KHR_calibrated_timestamps`, if the Adreno driver exposes it, or anchor at submit and completion.
- Check that every `TIMESTAMP_TOP` and `TIMESTAMP` report gets its own `vkCmdWriteTimestamp`. We already allocate query
  pools per report (`timestamp_query`), but `heavy_timestamps=false` may batch them.
- Then look in the log for resolution changes. Watch render-target sizes or viewport changes in the pass dump.
- If DOOM's thresholds assume 30 fps and we want 60, patch them the way NieR does (`dm/games/010056B015FE8000/patches.py`:
  four float-immediate cuts, 45/32.01/30 ms → 22.5/16.005/15 ms).

## 2. A fixed render scale via a game patch at the dynamic-resolution apply site  [HIGH]

**Commits:** `6acac1b` (2026-10-20, TOTK) and `b1a5774` (2026-10-20, BotW, `DM_RENDER_SCALE` 0.5-1.0); `7488443` (render
base in launcher.json); `c484b13` (2026-10-27, "Share physical storage for live NVN resolution variants"); `c13ddcf`
(2026-10-26, opt-in MetalFX spatial upscaling at present).

**How it works:** the host does not resize render targets. The rewriter (`dm/tools/patch_nso.py`) puts a stub where
the game's dynamic-resolution code *applies* its scale. That way the scene buffers, the shader's resolution ratio and
the LOD checks all agree. A second stub forces the apply block every frame. The game normally runs it only after it
recomputes the scale. The loader writes the factor into a state page read with ADRP:
```asm
ldr  s0,[x8,#0x10]   ; host render scale
fcmp s0,#0.0 ; b.le orig
fmov s1,#1.0 ; fcmp s0,s1 ; b.eq orig   ; 1.0 = keep the game's dynamic resolution
fmov s8,s0           ; replace the game's scale register
orig: ldr x8,[x20,#0x158]
```
Their comment explains why the scale must be patched at the apply site: patching a size function (TOTK's first way)
"would leave the ratio and those readers at the old scale". Measured on BotW (Mac): at 0.5 every scene pass halves,
while shadow maps, the 1080p output and the HUD keep their size. GPU time on the Mac did not change because the Mac is
not fill-bound. Factors above 1.0 are refused because the per-frame texture pool size is unknown.

`c484b13` matters once resolutions change at run time. Different-size variants of the same guest render target share
one physical allocation, so memory does not grow with each size.

**Our port:** this is the "патч размера таргетов в игре" item in our notes. Recommended order:
1. Fix item 1 and see whether DOOM scales on its own.
2. If not, find DOOM's dynamic-resolution apply site. It is the code that writes the per-view scale or viewport that
   the renderer reads every frame. Stub it with a host-controlled float, as above. Keep 1.0 meaning "game's own".
3. Upscale at present with FSR1 EASU+RCAS or Snapdragon GSR in our present/blit path. nxnative's equivalent is MetalFX
   spatial, opt-in, default off.

## 3. Geometry shaders, subgroups, GS culling and compaction  [HIGH for correctness (holes), MEDIUM for perf]

**Commits:**
- `bb585cd` (2026-10-23, "Add compute-backed geometry shader replay"); docs/geometry-metal-research.md.
- `cbbb600` (2026-10-25, OCTOPATH II `geo_split`, three-dispatch index compaction).
- `31b5944` (2026-10-26, NieR "geometry fixes").
- `f0ff511` (2026-10-21, indirect-count multi-draws masked on the GPU).
- `8b21049` (2026-10-18, indirect NVN draws read by the GPU).
- The one-lane subgroup rewrite (`dm/gfx/tools/dmsh_build.py` `spv_single_lane`; Monster Hunter Rise, then Xenoblade 3
  `9bdef35` 2026-10-27).

### 3a. One-lane subgroup emulation: nxnative uses it only where it is exact
`spv_single_lane` runs for **vertex shaders only**. nxnative states it is exact *only* for RE Engine's uniform-load
scalarisation: ballot the active lanes, shuffle the first lane's value to all, and take a fast path when all agree.
It maps ballot → own bit, shuffle/broadcast → own value, all/any → the predicate, elect → true, laneId 0, size 1.
**It refuses** any other subgroup op: `if any(340 <= op <= 366 and op not in SG_OPS): return False`. Refused ops
include InverseBallot, BitExtract, BitCount, FindLSB/MSB and the arithmetic ops. The shader then fails loudly instead
of being silently wrong.

This supports our suspicion in ACTIVE-PORT.md. DOOM's culling GS `703f1fcc` calls a helper (%63) six times with
shuffles. That helper is very likely Ryujinx's SHFL helper, which exchanges real data between primitives. With one lane,
"own value" is not exact. nxnative never ran any GS through the one-lane rewrite.

### 3b. What nxnative does for real GS: Ryujinx's GeometryToCompute
- Offline: each GS is translated with `Translate(asCompute: true)`, which uses Ryujinx's
  `Transforms/GeometryToCompute.cs` and `VertexToCompute.cs`. Five input-topology variants are built, plus a vertex
  "output reader" made by `GenerateVertexPassthroughForCompute`. Every vertex library also gets a rasterizer-discard
  capture entry. It writes a 144-word record per vertex: position, point size, clip distances, layer, viewport and 32
  generic attributes.
- Replay (`dm/gfx/nvn/nvn_geometry.inc`):
  1. Capture the VS once per unique vertex and instance.
  2. Dispatch the GS kernel with the logical NVN stage-2 resources.
  3. Compact the emitted strips into ordered lists.
  4. Draw them through the original fragment shader.
- Output goes to fixed per-primitive, per-invocation regions with restart markers, then an **ordered** compaction.
  The research doc warns that "a single atomic append cursor is insufficient as an ordering design".
- Perf (NieR, `31b5944`): a single-thread offset scan took **7.25 ms of a 16.19 ms GPU frame**. It became a parallel
  chunked scan (`geo_pack_scan`, up to 256 threads). The geometry kernel and the three compaction dispatches share one
  encoder, with dispatch-to-dispatch memory barriers instead of full queue barriers. That frame went to 4.19 ms of
  compute/copy in an 11.17 ms span; the route and mode differed, so it is not a clean A/B. Removing CPU waits
  (`mdi_wait`) before indexed GS draws was the other big NieR win.
- NieR's GS are **passthrough "index-building" geometry shaders that run before depth-writing passes**. That is the
  closest analogue to DOOM's culling GS anywhere in this repo. The NieR fixes that matter for us:
  - **Passthrough output bounds follow the input topology.** The SPH says max output 1, but a triangle variant writes
    3 vertices and 6 raw index words per group. Mirror Ryujinx `ShaderDefinitions`: vertex bounds `1,2,2,3,3` and index
    strides `2,4,4,6,6` for point/line/lineadj/tri/triadj.
  - **Native passthrough must be off when the GS runs as compute.** Otherwise no raster vertices are emitted.
  - **Geometry-stage SSBO table at `c0 + 0x410`.** Our `dmsh_vk_doom.py` already patches this in.
  - **Storage-buffer write flag.** Ryujinx `BufferUsageFlags.Write` is **bit 0**. nxnative's builder tested bit 1, so the
    GS counter atomic and the output stream looked read-only, and write tracking and barriers skipped them.
    **Our `dm/gfx/tools/dmsh_build.py:353` has the same bug** (`e['rw'] = bool(b and b['flags'] & 2)`). That file is
    the Metal-path builder. Check whether the Vulkan exporter (`dmsh_vk.py export`) or the engine decides SSBO
    writability another way, for example from SPIR-V `NonWritable`. If it ever uses this flag, the GS culling outputs
    would get no write hazard, and the next pass could read the index list or indirect args early. That would give
    missing triangles.

**Application to us:**
- **Correct semantics:** run DOOM's culling GS as compute through `GeometryToCompute`. Adreno supports subgroup
  ballot/shuffle in **compute**, so the shuffles become real. Ryujinx already handles a host subgroup wider than 32:
  `InstEmitWarp.EmitVote` emulates vote with ballot when `QueryHostSubgroupSize() > 32`, and `InstEmitMove`
  masks laneId with `& 0x1f` and picks the ballot half by `laneId & 32`. Our accessor must report Adreno's real
  subgroup size, 64 or 128. If it reports 128, check that the 128-wide branch exists. Optionally force 64 with
  `VK_EXT_subgroup_size_control`.
- **Cost on a tiler:** the compute expansion must run *before* the depth pre-pass render pass, not inside it. Batch all
  GS-culled draws of the pass into one compute stretch: VS capture, GS, scan and write in one command stretch with
  buffer-memory barriers. Then run the pre-pass, then the main pass, both reading the compacted lists. This also
  replaces the 6.6 ms of GS work in the pre-pass with compute. Whether it is cheaper on Adreno is unknown; the
  GS-in-raster path is not cheap there either.
- **Cheap test before building that:** see whether the holes come from synchronisation or from the shuffle emulation.
  Run with `DM_GFX_GUEST_BARRIER=1` and `DM_GFX_STORAGE_CHAIN=0`. If the holes go away, our barrier or chain elision is
  the cause, not the lanes. Two nxnative fixes are relevant here:
  - "Primitive barriers order vertex storage writes" (Xenoblade 3): after shader storage writes, a barrier with
    `ORDER_PRIMITIVES` must end the pass.
  - "Rasterizer discard preserves vertex storage writes": a discarded draw must still run its VS/GS for their SSBO
    side effects.
  The Vulkan equivalent: srcStage `GEOMETRY_SHADER` (SHADER_WRITE), dstStage `VERTEX_INPUT | DRAW_INDIRECT`
  (INDEX_READ | INDIRECT_COMMAND_READ).
- **Position invariance** (docs/reference/shaders.md "Position invariance across shaders", Odyssey): pre-pass and G-buffer
  pass with different shaders and LEQUAL leave exactly our pattern ("present in pre-pass, missing in shading"). On
  Metal it needed `[[invariant]]` *and* the `-fpreserve-invariance` compiler flag. Our `DM_GFX_INVARIANT=1` did not help.
  Together with 3a, that points to the GS compaction. Make sure the flag decorates the **GS** output `Position` as well
  as the VS one. Adding `NoContraction` to the position math in both VS variants would be the Vulkan way to remove
  fma-fusion differences (my suggestion, not from nxnative).

### 3c. Indirect and indirect-count draws
- `8b21049`: indirect draws are read by the GPU from the pool buffer. A compute pass (`ixc_run`) converts the record
  forms Metal lacks (quads, fans, 8-bit indices). We already have this (`gpu_indirect_try`).
- `f0ff511`: MultiDrawElementsIndirectCount is recorded as `max` ordinary indirect draws. A compute pass (`mdx_run`)
  copies each record or zeroes it past the count. Waiting for the count on the CPU had held the submit thread
  **14 of 20 ms** (Monster Hunter Rise, 36-54 fps). We use `VK_KHR_draw_indirect_count`, which is better. One idea to
  reuse: run the conversion compute **once per stretch up to the next buffer write**, outside the render pass, so it
  never splits a pass.

## 4. Submission segments and fence wake (CPU/GPU overlap)  [MEDIUM]

**Commits:** `sub_cut` (docs/reference/frame-rate.md "Submissions handed to the GPU in segments"); `6062518` (2026-10-26,
"Harden GPU fence wake and pass scheduling", #158/#213); `06b7b3e` (finite SyncWait budget); `b0a9079` (QueueFinish waits
for the GPU only).
- `sub_cut`: the replay commits what it has encoded **every 64 draws, between passes**. The GPU starts while the host
  is still translating. The fence signal and completion work stay on the last segment. BotW: GPU idle per frame
  7.7 → 3.8 ms, docked frame p50 20.8 → 16.7 ms; Dread p95 19.6 → 16.9 ms. With an encode thread, the cut size barely
  matters at 60 fps on the Mac. The doc says a GPU-bound device is where a smaller cut could show.
- Fast wake: the game's `SyncWait` on its newest fence wakes on the GPU's own signal. The CPU-side writes (events,
  counter reports) are made by whichever of the waiter or the completion handler comes first, under one lock and in
  order. Wake after GPU end: 0.29 → 0.08 ms (M2 Max).
- `nvnQueueFinish` must not wait for queued presents. Three Houses' first load went from 78 s to 6 s.

**Our port:** `xbg_game.cpp` does one `vkQueueSubmit` per slot (line 895). If one guest submission is a whole frame,
the GPU sits idle during our about 10 ms of translation. Our frame is about 50 ms and the GPU is busy 30-45 ms, so part
of the gap may be this. Check the `NVN PERF submits=` and gpu-idle numbers. If they show it, add a mid-submission
`vkQueueSubmit` every N draws at pass boundaries. Keep the fence and writebacks on the last one.

## 5. Tiler render-pass economy  [MEDIUM]

The general fixes, with their measured effects:

| Mechanism | Where | Effect |
|---|---|---|
| Inline uniform updates inside a pass are deferred, and later draws read a shadow built from them (`DM_NVN_UB_DEFER`, `ubp_get`) | `df7badd` 2026-10-19, `f5646ef` 2026-10-21 | Dread 272 → 26 passes, GPU 8.3 → 2.8 ms. BotW outdoors 386 → 158 passes, GPU p50 9.3 → 6.5 ms. Engage attachment traffic 1,850 → 360 MB/frame |
| Clear-only passes fold into the next pass's load action (lazy clears) | `f5646ef` | BotW: 4 passes and about 30 MB/frame less |
| Compute dispatches between draws are hoisted ahead of the pass when that is provably independent (`cs_hoist_try`, `DM_NVN_CS_HOIST`) | `6062518` | BotW 28 → 8 compute encoders, 149 → 132 passes, p50 9.9 → 9.0 ms (M2 Max) |
| Passes into other layers of one level run as one layered pass (`DM_NVN_SLICE_MERGE`) | `6062518` | fewer passes |
| Same-pass feedback (sampling an attachment of the open pass) gets a snapshot copy **only for listed shaders** (`feedback_snap`) | `b68ac08` 2026-10-17, `1963fab` | Snapshotting every such draw is too expensive: BotW 16.6 → 36.6 ms |
| A barrier with `ORDER_FRAGMENTS` ends the pass only if a draw in it wrote an image or SSBO (`swrite_note`) | FFT fix | right ordering for image read-modify-write draws |
| Store/load elision and pass merge (`DM_NVN_TBDR=1`) | `tbdr_*.py` | **off by default**, because bindless texture reads defeat the "what does this draw read" inference |

**Our port:** we already do the depth-feedback and VT-storage-image pass-break avoidance, the prologue copies, and
early-Z for image-writing fragment shaders (17 ms saved). We have the same `tbdr_*.py` tools in `dm/gfx/tools`.
Next steps:
- Run `tbdr_budget.py` on our pass dump for the about 80 passes. Adreno's GMEM load and store cost matters more than
  on Apple.
- Port **cs_hoist** if our `pass_end_lines` shows dispatch-caused breaks.
- Use **lazy clear folding** and `LOAD_OP_DONT_CARE`/`STORE_OP_DONT_CARE` where provable. Note that nxnative keeps
  general store elision off.
- Our uniform updates are CPU memcpy plus snapshots (`OP_UPDATE_UB`), so the UB-defer issue may not split our passes.
  Verify with the `pass_end_lines` counter.

## 6. Thermal and clock behaviour, frame step  [MEDIUM on device]

**Commits:** `c4b74a6` (2026-10-26, thermal frame-rate cap with synchronized recovery); `4acde72` (recover after the cap);
`9409472` (2026-10-21, probe back to 60).
- At thermal state serious, iOS caps the GPU at about 1/3 clock: a pass median of 6 → 22 µs, BotW GPU 17-26 ms for the
  same scenes. The host holds 30 fps until the device has been calm for 30 s.
- `9409472`: "A game that waits out a hold of 2 vsyncs gets a lower clock on the iPhone (… BotW: GPU 14.4 → 25 ms with
  the same draws)". The DVFS governor follows the idle time, so the frame step periodically probes the shorter hold.
- nxnative has no Android ADPF, PerformanceHintManager or sustained-performance code (it is Apple only).

**For us (inference):** Adreno DCVS behaves the same way. Read `/sys/class/kgsl/kgsl-3d0/gpuclk` (or `devfreq/cur_freq`)
during combat. If the GPU is not at its maximum (about 903 MHz-1 GHz class on 8 Gen 3) while we are GPU-bound, CPU/GPU
serialisation gaps are lowering the clock. Then ADPF `reportActualWorkDuration` plus the overlap from item 4 would help.

## 7. Memory model  [already mostly done in our port]

- Buffers are zero-copy (`newBufferWithBytesNoCopy` over guest memory, `nvn_mtl.m:487`, retried under pressure). Our
  AHB zero-copy (`DM_GFX_ZEROCOPY`) is the same idea.
- Textures are host copies kept current by dirty-page tracking (`tex_dirty_upload`), `WriteTexels` in place, and
  mapped-range flushes (`tex_flush_upload`). `d6e6c57` (2026-10-27) uses a hashed CPU-written-texture set: Arkham Knight
  lookup cost 1.7 s → 10 ms per million misses. `5b0c11a` fixed dirty-page upload races.
- Upload staging is bounded and retired on completion (`7dc250e`).
- GPU-written indirect args are read by the GPU (item 3c).
- Present snapshots and frame buffering exist but are opt-in (`a188613`).
Nothing in this list is new for us except possibly the hashed CPU-written-texture set, if our submit profile shows
texture-set lookups.

## 8. Shader-chain details

- `-fpreserve-invariance` on every compile (see 3b). In Vulkan terms: `Invariant` on every pre-raster `Position`.
- `fast::min/max/clamp` → `precise::`, because Maxwell `FMNMX` returns the non-NaN operand. Mario Galaxy 2's half-white
  Mario on A20 (`56704a0`). Check that our SPIR-V uses `NMin`/`NMax`, or a NaN-safe select, where Ryujinx emits
  `FMin`/`FMax`. Adreno's handling of NaN with `FMin` is undefined.
- iOS uses `-fmetal-math-mode=relaxed`, not fast. 8,337 of 12,809 shaders rely on `isnan` selects. On Vulkan the
  equivalent risk is a driver assuming no NaN; there is nothing to do unless we see NaN artifacts.
- The pipeline "prepare step" (`7a47d1c`): the first submission waits until the prewarm list is compiled on every core
  behind a progress screen. BotW went from 4-5 hitch frames to none. Our pipeline memo and prewarm play the same role.
- Nothing on fp16, robustness or `EarlyFragmentTests`. Our early-Z work is ahead of nxnative there.

## 9. Other DOOM-relevant items

- **Present interval / fps cap:** profile keys `present_interval`, `fps_target` and the frame-step stubs. For a 30 fps
  Switch game going to 60, nxnative patches the game's interval and its dynamic-resolution thresholds (NieR). That is
  the template if DOOM's 30 fps target is to be lifted later.
- **Operation mode and perf mode:** `op_mode console|handheld` and `perf_mode follow|boost` (`hle_oe.c`). DOOM may pick a
  lower base resolution in handheld. Our notes say `DM_OPMODE=0` did not change fps, which fits GPU-bound at a fixed
  720p with the dynamic resolution inactive (item 1).
- **Conditional rendering** (`render_cond`, TOTK): a timestamp report writes 0 in the counter field and the time in the
  second field. If DOOM uses `SetRenderEnableConditional` on report pairs, drawing everything wastes GPU. Our
  `write_report` already writes counter = 0 for timestamps. Check whether we evaluate `RenderEnableConditional` at all.

---

## Summary of concrete actions for our port, by expected impact

1. **Timestamps:** use 614.4 MHz units (`*384/625`), the guest-clock epoch, and one real GPU time per report.
   Files: `xbg_game.cpp` `write_report`, `hle_nvn.c:1956`. Then check whether DOOM's dynamic resolution starts working.
   This is the cheapest test with the largest possible gain.
2. If not, a **render-scale stub at DOOM's dynamic-resolution apply site** (BotW/TOTK pattern), plus FSR1 or GSR at present.
3. **Holes:** first tell sync from lanes (`DM_GFX_GUEST_BARRIER=1`, `DM_GFX_STORAGE_CHAIN=0`). Check SSBO write flags:
   Ryujinx Write = bit 0, and our `dmsh_build.py:353` tests bit 1. Long term, **GS → compute through Ryujinx
   GeometryToCompute** with real compute subgroups, ordered parallel compaction, run outside the render pass.
4. **Segment submits** every N draws at pass boundaries, if the GPU idles during translation.
5. **Pass economy:** compute hoisting and lazy-clear folding. Measure with our `tbdr_budget.py` and `pass_end_lines`.
6. **Device clocks:** check the GPU clock under load, and add ADPF hints if DCVS is not at maximum.
