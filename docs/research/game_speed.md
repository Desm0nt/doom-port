# DOOM: game speed vs render frame rate

Addresses are offsets in the update main (`work/exefs-update/main.raw`, build id 2847991952b3d7ab); disassembled with `tools/disa.py update`.

## What we saw

At 12-20 fps the game felt slow, and at 45-60 fps it felt fast. `+set com_fixedTic 0` made no clear difference.

## Root cause

The game counts time in game frames of fixed length. With the defaults, it runs exactly one game frame per render frame, and gameHz is pinned to 30.

* Cvar registration (0x1ee5xxx-0x1ee7xxx, `x2` = default string):
  * `com_fixedTic` "1" (GOT 0x4885de8, help text "run a single game frame per render frame").
  * `com_maxResidualGameFrames` "0" (0x48d95b8), `com_ignoreFixedTicClamp` "0" (0x48d95e0), `com_debugFixedTic` "0" (0x48d95f8).
  * `com_adaptiveTick` "0" (0x4884948), `com_adaptiveTickImmediateMode` "0" (0x48d95f0).
  * `com_adaptiveTickMinHz` "30" (0x48d95d8), `com_adaptiveTickMaxHz` "30" (0x48d95d0).
  * The SP cfgs (`default.cfg`, `default_sp.cfg`, `nx_*.cfg`) set none of these. Only `default_dedicatedServer.cfg` sets `com_fixedtic 1` / `com_adaptiveTick 0`. The lowercase `com_fixedtic` string is only a name the cfg parser looks up; it is not a second registration that matters here.
* The game frame loop is in 0x1ecfa80 (local game, called from 0x1ed36dc) and in its twin 0x1ed0f00. The net client version, 0x1ece280, has the same residual logic but no adaptive branch:
  * `frameMs = int(vfunc_0x240(0) / timescale)`.
  * `residual += clamp(Sys_Milliseconds() - last, 0, 50)`.
  * If `com_fixedTic != 0`:
    * Not adaptive: 0x1f2b630 resets the tick object (`*(main+0x487fc40)`) to {30 Hz, 1/30, 33 ms}, calls `SetGameHz(30)` (vfunc 0xa8), and sets `residual = 0`.
  * Then a **do-while** loop runs a game frame (0x1ece760), does `residual -= frameMs+1`, and repeats while `residual > frameMs`.
* So there is always **at least one 33 ms game frame per render frame**:
  * Game speed = fps / 30, which is 1.5-2x at 45-60 fps and 0.5x at 15 fps. That matches what we saw.
  * The Switch presents at interval 2 (30 fps), so there it is real time. Our `DM_PRESENT_INTERVAL=display` ignores the interval.
* `com_fixedTic 0` does not help above 30 fps. The do-while still runs at least one frame per render frame, and the residual only adds catch-up frames when fps is below 30.
* The tick clock is correct:
  * SDK `GetSystemTickFrequency` (sdk+0x31b5e0) is the constant 19200000.
  * The `mrs cntpct_el0` gate (hle.c) and svc 0x1E return `guest_ns()*12/625` = 19.2 MHz from CLOCK_MONOTONIC.
  * main itself reads no cntpct/cntfrq.

## The engine's own fix: adaptive tick

This branch only exists inside the fixedTic branch. It is taken when:

* `com_adaptiveTick` is on,
* the game state is 6 (in game),
* the tick object byte `+0x24` (allowHzChange) is set. The map sets it at 0x4c6abc / 0x4d64dc, `ae_setAllowHzChange` toggles it, and 0x4f15c8 clears it.

With `com_adaptiveTickImmediateMode` on, 0x1ecf940 runs each frame. Its state is in static `S = main+0x66bd7c0` (+0x90 last µs, +0xa0 carry):

* `us = clamp(now_us - last, 1e6/MaxHz, 1e6/MinHz) * 0.96 + carry`
* `hz = ceil(1e6/us)`, then `SetGameHz(hz)`
* `carry = us - 1e6/hz`

So each game frame lasts as long as the render frame took. The Switch shipped Min = Max = 30, so the adaptive tick never actually changed anything there. The HUD (0x1f0b6xx, shown for `com_showFPS` 1-2) prints `Tick-Adaptive %2dHz` / `Tick-Fixed %2dHz`. The non-immediate mode (AdjHz, SettleMS, MaxRate) steps the Hz up and down using the residual, which is coarser.

Note the `*0.96` factor: game time may run a few percent slower than real time, unless frameMs rounding makes up for it. The diagnostic below measures this.

## Fix (dm/host/hle_args.c)

This is on by default. `DM_DOOM_ADAPTIVE_TICK=0` restores the original behaviour.

```
+set com_fixedTic 1 +set com_adaptiveTick 1 +set com_adaptiveTickImmediateMode 1
+set com_adaptiveTickMinHz $DM_DOOM_TICK_MIN(30) +set com_adaptiveTickMaxHz $DM_DOOM_TICK_MAX(60)
```

* MaxHz 60 matches our present cap (`present_auto_hz`, 60).
* Below 30 fps the game slows down, as on the Switch. To try sub-30 tics, set `DM_DOOM_TICK_MIN=20`.
* `g_argv` was raised from 64 to 128 slots, since the defaults now use about 60.
* Menus and states where allowHzChange is off still use the fixed 30 Hz tic. Their UI animation runs at fps/30.
* Fallback that is exact everywhere but caps at 30 fps: `DM_PRESENT_INTERVAL=game`, which follows the game's interval 2.

## Diagnostic (dm/host/hle_doom_tick.c, called per present from hle_os.c `dm_guest_frame_tick`)

With `DM_DOOM_TICKLOG=1`, it logs once per wall second:

```
[tick] 1.00s wall: 48 presents (48.0 fps), 48 adaptive frames (last gameHz 50); game time ~960 ms => speed 0.96x | com_fixedTic=1 com_adaptiveTick=1 ...
```

* It replays the 0x1ecf940 formula from S+0x90 / S+0xa0. A present without an adaptive call counts as one 1000/30 ms tic.
* It refuses to run if `main+0x36139c3` is not "com_fixedTic", i.e. on a different build.

## Cheap on-device test

1. `DM_DOOM_TICKLOG=1 DM_DOOM_ARGS="+set com_showFPS 1"`. In gameplay the HUD should show `Tick-Adaptive NNHz` with NN close to the fps, and the log should show `speed ~0.95-1.0x`.
2. Run the same build with `DM_DOOM_ADAPTIVE_TICK=0`. Expect `Tick-Fixed 30Hz` and `speed ~ fps/30` (about 1.6x at 48 fps).
3. Stopwatch check: time something with a fixed duration, such as a door opening or the reload animation, in both modes.
