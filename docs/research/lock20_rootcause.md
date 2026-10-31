# 20-fps lock (Mi 13 Pro / 8 Gen 2, TV room): root cause analysis

Status: DONE (analysis only; no source/doc/build/adb/git changes). Scripts used (scratch audit folder): win.py, lk_timeline.py,
lk_up.py, lk_corr.py, lk_phase.py, lk_pass.py, lk_tex.py, plt.py, relrefs.py. Log paths: G:\Games\PORT\doom-port\work\logs\play\<run>\logcat.txt.

## 0. Summary

1. **This is not an 8 Gen 2 problem, and the GPU is not what starts it.** The same step change happens on the 8 Gen 3 (Adreno 750) at the
   same game moment. There the frame goes 17.3 -> 34.8 ms (d_base), 18.8 -> 35.3 (p_p0), 17.7 -> 35.0 (mesa_dbg), 18.9 -> 36.8 (r4_r0x)
   and 17.8 -> 34.8 (d_aniso4b), with GPU only 19-22 ms (lk_corr-style split, section 2). On the 8 Gen 2 TV runs it is 25-26 -> 48-50 ms.
   So the frame time roughly **doubles on both SoCs**, while GPU, translation thread and presenter sit mostly idle.
2. **What starts it** is a game-content event in the TV room, the same moment in every run: a **per-frame texture-update stream** begins
   ('NVN PERF uploads window full=...' goes from about 1.3 to 3.2-4.7 full uploads per frame and 0.75-1.2 MB per frame, the signature of
   a playing Bink video: the dump in run_g2_g1b shows BinkDecode1/2, BinkIO and BinkSound threads). **At the same moment the HLE audio
   renderer locks into "catch-up mode"**: '[dm] audren: ... catch-up renders so far N' goes from +0 per 10 s to exactly +2000 per 10 s,
   i.e. every 5 ms audio frame is a catch-up render.
3. **When it ends:** it lasts exactly as long as the video stream. In every TV run it never ends before the run ends. On the 8 Gen 3
   it ended 3 times in turnip_test (12:37:20, 12:40:45, 12:43:37) and once in mesa_dbg (11:25:01), each time when the upload stream
   dropped back to about 1.5 per frame. In those exits the frame recovered while catch-up mode was **still on for another 0-3 s**
   (counter arithmetic, section 3). So catch-up mode **alone** does not make the slowdown. It is only present with it.
4. **Consequences, not causes:** the 8 Gen 2 GPU downclock (fixed full-screen passes slow down only *after* onset, progressively; at
   tv_g1 frame 6901, 12 s into the lock, they are still at normal clock 952/419/288 us while the frame is 49 ms), the lower slot waits,
   and the 20 (not 30) fps on the 8 Gen 2.
5. **The pacer/AcquireTexture hypothesis is rejected** by the code itself (section 4, H1). The "16.67-ms quantisation" is weak: the
   game-side intervals at onset are 43.5-56.4 ms with no grid.
6. **Root cause (what is proven):** a stall on the guest-thread side, in the HLE, that starts when the TV-room Bink video starts. It is
   not GPU, translation, presenter, pacer, thermal or display. **The exact blocked thread is NOT proven.** Ranked mechanisms, each with an
   A/B switch: section 5.

## 1. Onset sequence (timestamps are logcat times)

### tv_m1 (8 Gen 2)
- 23:02:20.9 to 23:02:29.06: heavy normal phase. 1254-1507 draws, frame 25.6-29.8 ms, GPU 22.9-24.8 ms, uploads 0.3-0.5 per frame
  (NVN PERF v=1 and uploads windows).
- 23:02:27.27 / 23:02:29.06 windows: full uploads per 60 frames rise to 125 / 153 (37.8 / 40.8 MB). **The video stream ramps up.**
- About **23:02:28.66**: audren catch-up mode starts. The counter is 10573 at 23:02:21.862 and 11210 at 23:02:31.843, so +637
  catch-ups at <=200/s puts the start at 23:02:28.66 or earlier.
- 23:02:29.24-29.74: '[长帧]' frames 7076-7086 are 40.5/48.8/45.9/55.4/47.1/52.0/53.5/56.4/43.5 ms with **GPU still 24.7-25.8 ms** and
  NVN 10-23 ms. The frame doubles before any downclock.
- 23:02:32.009: window fps 20.35, frame 49.1 ms, GPU 30.6 ms, 1526 draws, uploads 195 per 60 frames (48.4 MB).
- 23:02:34.9 / 37.8: draws fall to 912 / 606, but frame is still 48.0 / 48.2 ms (GPU 24.7 / 24.3).
- 23:02:49.5: GPASS frame 7501, fixed passes 615e5352 / daa747b7 / 270b1b7a = 1486 / 652 / 450 us, against 942-952 / 413-426 / 285-303 us
  at frames 4501-6901. **The GPU clock is now lowered.**
- 23:02:41-23:03:01: catch-ups +2000 every 10 s until the end (17210).

### tv_r2 (8 Gen 2)
- 23:13:43.6 / 45.3: 1403 / 1484 draws, frame 28.4 / 28.8, uploads 1.30 -> 2.08 per frame.
- About **23:13:45.6**: catch-up mode starts (27377 at 23:13:44.565, 29161 at 23:13:54.546, +1784).
- 23:13:48.066: frame 45.4, GPU 30.0, uploads 3.25 per frame (0.83 MB).
- 23:13:51-23:14:41: frame 48.2-49.8 ms at 463-1750 draws, GPU 26.2-31.9, catch-ups +2000 every 10 s.
- GPASS fixed passes: 941/425/287 (23:13:36) -> 1311/573/397 (23:13:59) -> 1482/656/456 (23:14:29). The downclock is progressive.

### d_base (8 Gen 3, same moment)
- 12:40:13.9-17.4: 1397-1510 draws, frame 18.2-19.5 ms. Uploads per 60 frames ramp 14 -> 59 -> 115 -> 133.
- About **12:40:17.5**: catch-up mode starts (10855 at 12:40:12.221, 11800 at 12:40:22.202).
- 12:40:19.4 window: frame 33.1, 254 uploads (65 MB). From then on 33-37 ms at 499-1628 draws with GPU 17-23 ms.

**Onset of all three markers by run.** The catch-up start is derived from the counter. The lock window is the first NVN PERF window
with frame > 44 ms.

| run | stream ramp (uploads/frame) | catch-up start | first lock window |
|---|---|---|---|
| tv_g1 | 1.82 -> 2.88 -> 3.25 | ~23:05:29.9 | 23:05:34.96 (23:05:32.0 already 43.1 ms) |
| tv_r1 | 1.45 -> 2.20 -> 3.33 | ~23:08:24.6 | 23:08:28.65 (23:08:25.7 34.5 ms) |
| tv_m1b | 1.25 -> 2.75 -> 3.17 | ~23:18:19.9 | 23:18:22.44 |
| tv_sysmem | 1.85 -> 2.25 -> 3.65 | ~23:27:31.3 | 23:27:33.99 |
| run_g2_g1b (earlier build) | 4.45 per frame at 14:42:52 | ~14:42:49.7 | none: frame median rises only to 33.4 ms |

## 2. Per-phase table (medians; lk_phase.py; heavy = draws >= 300)

| run | phase | fps | frame ms | GPU ms | submit wall ms | present wall ms | slot wait ms | translator CPU ms | ADPF CPU work ms | uploads/frame (MB) | catch-ups /10 s |
|---|---|---|---|---|---|---|---|---|---|---|---|
| tv_m1 | normal | 38.6 | 25.9 | 22.7 | 11.9 | 5.0 | 2.9 | 9.2 | 18.8 | 1.40 (0.29) | 0 |
| tv_m1 | lock | 20.5 | 48.7 | 28.1 | 8.4 | 4.5 | 0.8 | 10.4 | 18.7 | 3.22 (0.75) | 2000 |
| tv_g1 | normal | 39.5 | 25.3 | 21.8 | 11.2 | 4.8 | 2.4 | 8.7 | 18.9 | 1.45 | 0 |
| tv_g1 | lock | 20.2 | 49.5 | 28.8 | 12.3 | 5.7 | 0.9 | 10.2 | 16.5 | 3.17 | 2000 |
| tv_r1 | normal | 40.0 | 25.0 | 18.9 | 10.6 | 7.2 | 2.1 | 8.7 | 16.3 | 1.23 | 0 |
| tv_r1 | lock | 20.2 | 49.6 | 28.4 | 9.2 | 1.8 | 0.8 | 9.3 | 18.1 | 3.18 | 2000 |
| tv_r2 | normal | 37.9 | 26.4 | 21.8 | 11.7 | 6.9 | 2.6 | 8.9 | 19.0 | 1.30 | 0 |
| tv_r2 | lock | 20.3 | 49.4 | 28.4 | 8.9 | 2.1 | 0.8 | 10.8 | 17.4 | 3.15 | 2000 |
| tv_m1b | normal | 38.2 | 26.2 | 22.2 | 11.3 | 5.0 | 2.5 | 9.0 | 19.9 | 1.32 | 0 |
| tv_m1b | lock | 20.4 | 49.1 | 29.3 | 8.5 | 1.2 | 0.8 | 9.5 | 17.9 | 3.19 | 2000 |
| tv_sysmem | normal | 40.8 | 24.5 | 21.2 | 10.9 | 3.8 | 2.7 | 8.8 | 12.2* | 1.62 | 0 |
| tv_sysmem | lock | 20.7 | 48.3 | 26.5 | 10.3 | 4.3 | 0.9 | 11.1 | 19.5 | 3.23 | 2000 |

\* tv_sysmem's "normal" ADPF median includes the menu phase. The ADPF line comes only every 600 presents, so the lock has 1-2 samples.

8 Gen 3, heavy windows with catch-up mode active vs inactive (frame / GPU medians):

| run | catch-up active | catch-up inactive |
|---|---|---|
| d_base | 34.8 / 20.0 | 17.3 / 15.0 |
| p_p0 | 35.3 / 19.6 | 18.8 / 14.9 |
| mesa_dbg | 35.0 / 19.0 | 17.7 / 15.1 |
| r4_r0x | 36.8 / 21.9 | 18.9 / 16.2 |
| d_aniso4b | 34.8 / 18.9 | 17.8 / 14.7 |

**Other rows the owner asked for:**
- **Fixed full-screen passes 615e5352 / daa747b7 / 270b1b7a (us):**
  - Normal: 941-969 / 413-481 / 285-303 (tv_r1 is faster: 754-760 / 333-338 / 230-234).
  - Lock: 1043-1751 / 471-765 / 323-533, rising over time.
  - tv_g1 frame 6901, 12 s into the lock, is still 952 / 419 / 288.
- **Present intervals ('上屏'):**
  - Normal p50 is 18-33, on a display quantum of about 11.1 / 14.3 / 16.7 ms that changes between runs.
  - Lock p5/p50/p95 = 26-35 / 45.1-50.4 / 59.6-67.7.
  - Game-side '帧时间' p50 48.2-49.6 in the lock. No strict 16.67 grid (section 1, frames 7076-7086).
- **Game-thread CPU and clock:** not measurable on Android.
  - '[nvn] 主线程 ... 平均主频 0.00 GHz' in every line: thr_counts has no cycle counter.
  - The present-calling guest thread (RenderSubmit) uses 0.07-0.17 ms CPU per frame in both phases.
  - The CPU policy snapshots (thermal.txt, policy0/3/7 cur kHz every 10 s) look the same in both phases. Gold is mostly 1920000; prime
    alternates 864000-1843200. ThermalStatus is 0 throughout, and '[perf] ... thermal none'.
- **Acquire wait:** not in logcat. @wait_ms and @pq_wait_ms go only to the TSV (hle_nvn.c:1728). The pacing part is provably 0 for
  T > 16.67 ms (section 4).
- **Present wait:** present_cpu_ms is *wall* time of the whole present (xbg_game.cpp:5197). It is lower in the lock: 1.2-5.7 vs 3.8-7.2 ms.
- **ADPF path:** 'ADPF session open (target 16.67 ms, ..., GPU time reported)' in every run, so report2 with GPU duration is used
  (perf_android.c:220-223). The target stays 16.7 ms throughout. No new thread joins the session after the event (no '[perf] ADPF
  session threads' change). So no guest thread (e.g. Bink*) went above 15% of a core.
- **Not present in any of the 6 TV runs:** '出图档' / pace-adapt lines, 'GW per frame' (stats off), and handoff dumps. Frame-rate vote
  120 Hz only at surface creation. No RefreshRateSelector policy change at onset; the tv_m1 changes are at 22:59-23:00:07 and 23:03:08 only.

## 3. Evidence on the trigger and what it is coupled to

- **Upload stream vs lock** (lk_corr.py, 6 TV runs): every heavy window with >= 2.9 uploads/frame and >= 0.6 MB/frame is locked
  (87 windows). No locked window lacks it. The only "stream but not locked" windows are tv_sysmem 23:24:54-57 with 2.7-3.4 MB/frame,
  which is the level-load upload burst, a different signature.
- **Catch-up counter** ('audren: render time mean ... catch-up renders so far'): in the lock it is exactly +2000 per 10 s in all 6 TV
  runs and in all 5 8-Gen-3 runs. In normal heavy gameplay it is +0. In menus and loading it is +1400-1850 per 10 s at 60 fps, with no
  slowdown there.
- **Exits** (turnip_test, mesa_dbg; 8 Gen 3): the frame recovers in the window where the upload stream falls.
  - turnip_test 12:37:20.4: uploads 1.77 per frame, frame 22.0 ms. Catch-up then +769 in 12:37:19.8-29.8, so it ran until about 12:37:23.6.
  - turnip_test 12:40:45.3: frame 21.9 ms. Catch-up +991, until about 12:40:47.2.
  - mesa_dbg 11:25:01.9 / 03.1 / 04.3: frame 22.2 / 20.6 / 20.3 ms, uploads 1.58 per frame. Catch-up +919 after 11:24:59.384, so active
    until 11:25:04.0 or later.
  - turnip_test 12:43:49-12:46:49: partial catch-up at about 74 per second with normal frames (17-22 ms).
  - **So catch-up mode is not sufficient. The video stream (or something that comes and goes with it) is required.**
- **Bink threads** (run_g2_g1b dump 14:39:50):
  - BinkDecode1: priority 32, core 1, mask 0x2. That is the same core as Renderer (priority 43, mask 0x2, CPU 194 s) and RenderSubmit
    (priority 41, mask 0x2, CPU 1.98 s; RenderSubmit is the present caller).
  - BinkIO: priority 31, core 1. BinkDecode2 and BinkSound: priority 32, core 2.
  - Main thread: priority 43, core 0, shared with higher-priority audio threads: Thread_0x786BA73534 (priority 37), AK::BankManager (38),
    AK::IOThread (39), AK::OpusDecodingThread (40).
- **Game sink code** (update main; tools/disa.py and plt.py): the MainAudioOut sink calls nn::audio::RequestUpdateAudioRenderer
  **while holding a game recursive mutex**.
  - 0x200b6a0 `bl 0x2029440` (lock main+0x7387f40), 0x200b6ac AppendWaveBuffer, 0x200b6bc RequestUpdateAudioRenderer (PLT 0x22a1bd0),
    0x200b6e0 `b 0x20291ec` (unlock).
  - The volume path does the same: 0x200b734 lock, SetVoiceVolume / SetVoiceMixVolume, 0x200b7bc RequestUpdate, unlock.
  - There are 12 RequestUpdate call sites, at 0x200ae24 to 0x200b974.
  - 0x2029440 is a CAS lock on the owner thread with a recursion count at +0x30. The slow path is 0x20292dc.

## 4. Hypotheses tested

**H1. Pacer / AcquireTexture slot quantisation (owner's hypothesis): REJECTED.**
- nvn_pace (nvn_pace.h:14-20) returns 0 when the present is late, so slot = now (hle_nvn.c:1808). Then avail[idx] = present_time + P
  (hle_nvn.c:1810).
- With 2 window textures ('WindowInitialize 交换链 2 张') the texture presented at frame k is acquired again at frame k+2, at least
  T = 49 ms after present(k). AcquireTexture sleeps only if `at > now` (hle_nvn.c:1323-1329), which needs T < P = 16.67 ms.
- The x_present_ticket wait (hle_nvn.c:1340-1341, 1192-1196) needs the translator to lag by more than one frame. It is idle 30+ ms per
  frame, and submit+present wall is 9-16 ms.
- The ADPF target stays 16.7 ms throughout, so the period never changed.
- In display mode the game's interval 2 is ignored (hle_nvn.c:1674).

**H2. GPU DVFS lock (msm-adreno-tz) as the cause: REJECTED as the cause, CONFIRMED as an amplifier.**
- At onset the GPU per frame is 24.7-25.8 ms at normal clock (frames 7076-7086).
- tv_g1 frame 6901 shows normal pass timings 12 s into the 49-ms lock.
- The 8 Gen 3 shows the same frame doubling without a comparable GPU change (GPU 15 -> 20).
- On the 8 Gen 2 the GPU is 57% busy afterwards, so the governor lowers the clock and GPU time grows from 22 to 28-30 ms.

**H3. Thermal or display mode: REJECTED.**
- Skin temperature at onset is 37.8 (tv_sysmem) to 44.4 °C (tv_r2), and tv_r1 ran normally at 41-43 °C. So there is no threshold.
- ThermalStatus is 0 throughout, and the CPU caps are the same in both phases.
- No display policy change.

**H4. Adaptive-tick spiral: already rejected** by the previous auditor (main 0x1ecfce4 zeroes the residual).

**H5. Translation thread or texture re-upload cost: REJECTED.**
- Translator CPU rises only 0.6-2.4 ms.
- Upload CPU is 0.2-1 ms per frame ('[长帧] ... 重传 3 次 0.7 MB / 0.2 ms', 'cpu_ms_per_frame full=0.31 partial=0.30 checks=0.33').
- Write-protect faults are 1.3-1.5 per frame in the lock.

**H6. Bink decode CPU on the critical path: UNLIKELY as pure CPU.** No Bink thread reached 15% of a core (the ADPF thread scan,
perf_android.c:93-126, added none). That is under 7.3 ms per 49-ms frame on the 8 Gen 2 and under 5.2 ms per 35-ms frame on the 8 Gen 3.

## 5. Root cause and fixes, ranked

**What is proven.** At a fixed game moment in the TV room (screen video starts), every guest-side pipeline stage gets about 2x slower.
GPU, translator and presenter are idle more than half the time, so the critical path is a **wait chain among guest threads inside the
HLE**. The two HLE state changes that happen exactly then are:
- (a) the TV video's texture stream;
- (b) audren catch-up mode, which latches (hle_audren.c:421-441, together with render_main hle_audren.c:122-125).

Code facts about catch-up mode:
- While catch-up frames are queued, the render thread plays them instead of calling the SDK render (hle_audren.c:124), so g_renders
  never advances on its own.
- Every game RequestUpdate then renders a catch-up frame on the *game's* thread (hle_audren.c:426-436).
- When the queue is full (CQ = 4), RequestUpdate **blocks on a host condvar for up to 50 ms**, about 5 ms per pop (hle_audren.c:430-435).
- The renderer event is never signalled, because only the SDK exec in render_main signals it.
- On Horizon, RequestUpdate is a short IPC and never blocks for output pacing. DOOM holds its sink mutex across that call
  (main 0x200b6a0-0x200b6e0).

**Not proven:** which game thread ends up waiting, and on what. Ranked mechanisms:

1. **M1: catch-up mode makes RequestUpdate a ~5 ms blocking call under the game's sink mutex.** The streams feeding the sink (Wwise
   output, the video's audio via BinkSound, volume updates) serialise in ~5 ms steps, and the game threads that touch audio stall.
   - Confidence: **medium**. The coupling in time is exact, and the code path blocks a guest-held lock. But the exits show catch-up
     mode is not sufficient alone; that is consistent only with "catch-up AND the video's audio stream active".
   - Fix: Switch-faithful audren. RequestUpdate never waits. The render thread keeps rendering autonomously every 5 ms and signals the
     event.
   - Switch `DM_AUDREN_CATCHUP=0|1|auto`. Default auto = catch-up renders only when the renderer has effects or submixes. DOOM logs
     'effects 0 submixes 0'. The catch-up exists for effect initialisation (hle_audren.c:411-414), so it is not needed for DOOM.
   - Optional switch `DM_AUDREN_CATCHUP_AB=N`: toggle every N seconds for an in-run A/B.
   - Also wrap the queue-full wait in `dm_thread_native_wait(1/0)` (hle_thread.c:131).
2. **M2: same-core handoff stalls around the Bink threads.** Renderer and RenderSubmit (core 1, mask 0x2) wake BinkDecode1 or BinkIO
   (priority 31-32, core 1), or BinkDecode1 wakes BinkIO. The waker then polls in dm_yield_to_core_peers (hle_thread.c:141-153,
   50-us nanosleep) until the woken thread blocks. Host-side blocking (file reads, NVN host waits, the audren condvar) does **not**
   count as blocked, because dm_thread_native_wait is never called outside hle_thread.c. So the waker also waits out BinkIO's FUSE
   reads, which on Horizon are kernel waits.
   - Confidence: **medium-low**. It fits the "video only" coupling, but there is no handoff counter in any TV run.
   - A/B with the existing `DM_HANDOFF=0` (hle_thread.c:717-718); `DM_HANDOFF_PINNED` is already 1.
   - Fix: mark host blocking regions as native waits. Switch `DM_HANDOFF_NATIVE=1`.
3. **M3: Bink decode wall-time latency.** The decode threads run unboosted on little cores (not in ADPF), and the game waits on decode
   completion each frame.
   - Confidence: low.
   - Fix: add guest threads named Bink* to the ADPF session, or give them big-core affinity. Switch `DM_BINK_BOOST=1`.
4. **Not recommended:** pacer, DM_PACE_ADAPT or present-interval changes (H1 is rejected), and GPU DVFS workarounds (the downclock is a
   consequence; it disappears once the stall is fixed).

## 6. One owner-paced 2-minute TV-room verification run

**Build:** M1 with `DM_AUDREN_CATCHUP_AB=10`, plus instrumentation:
- every 10 s, `audren: updates U, catch-ups C, queue-full waits W totalling S ms (max M ms), updater thread 0x10xx «name»`;
- every 10 s, `[dm] handoffs +N +T ms (top waker->woken: «Renderer»->«BinkDecode1» n/ms ...)`, read from dm_handoff_stats
  (hle_thread.c:710-711) plus a per-pair counter.

**Run:** walk into the TV room and stand where the screens play for about 60 s. The main agent creates `dm/dump` twice: before the
screens start and about 20 s in. The existing state dump prints each guest thread's wait (lock or condvar address), last svc and lr,
and the cumulative handoff total.

**What proves which mechanism:**
- **M1 confirmed:** in AB phases with catch-ups at 0, the NVN PERF windows show uploads at about 3.2 per frame (the video is playing)
  **and** frame about 25-28 ms (8 Gen 2), with GPASS passes about 943-969 us. Phases with catch-ups at 2000 show 48-50 ms.
  The dump during the lock shows a game thread waiting on the lock at main+0x7387f40 or inside sdk RequestUpdate.
  Classify phases by the logged catch-up count, not the switch state: catch-up mode only re-enters on a double update.
- **M1 rejected:** both kinds of phase are about 49 ms. Then, if the handoff line shows a jump of hundreds of ms per second at onset
  with Renderer or RenderSubmit -> Bink* pairs, it is M2: confirm with a `DM_HANDOFF=0` run, which should give frame about 26 ms with
  the video playing.
- **Neither:** the second dump's per-thread CPU (BinkDecode1/2) and wait states decide M3.

## Progress log (kept)
- [P1] Pacer math: when late, nvn_pace returns 0 -> slot = now -> avail = present + P. With 2 textures a wait needs T < P. ADPF target
  stays 16.7.
- [P2] Game-side intervals at onset are not quantised (frames 7076-7086). 上屏 p50 in the lock is 45.1-50.4.
- [P3] The GPU downclock comes after the onset (tv_g1 frame 6901 at normal clock 12 s into the lock).
- [P4] Upload stream 3.1-3.65 per frame, 0.75-0.91 MB, in 100% of lock windows and in no non-lock window (lk_corr).
- [P5] Same stream in run_g2_g1b with a milder slowdown (33.4 ms).
- [P6] audren catch-up counter: +2000 per 10 s in the lock in all 6 TV runs and 5 8-Gen-3 runs, +0 in normal heavy gameplay.
- [P7] 8 Gen 3 shows the same frame doubling (17-19 -> 35-37 ms) at the same moment.
- [P8] Exits (turnip_test x3, mesa_dbg x1) follow the video stream, not catch-up mode.
- [P9] DOOM's sink holds a game mutex around RequestUpdate (main 0x200b6a0-0x200b6e0, 0x200b734-0x200b7c0).
