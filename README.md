# DOOM Mobile

**A native Android port of DOOM (2016) for Nintendo Switch.** The game's own AArch64 program runs directly on the
phone's CPU; a small host provides the Switch system services, and the NVN graphics API is replayed on Vulkan.
You bring the game: this repository contains no game content.

Русский: [ниже](#по-русски).

> **Tested so far only on Adreno 750** (Snapdragon 8 Gen 3, Xiaomi 14 Ultra) with the stock Qualcomm driver. Other GPUs
> and devices are untested. Mesa Turnip is optional (settings → custom Vulkan driver) and runs slower there.

## Screenshots

Captured on the phone (Adreno 750, Qualcomm driver). The counter in the top-left corner is the port's own FPS / frame
time / GPU time overlay (it can be turned off in the settings).

**On-screen controls** (shown while no gamepad is connected):

| | |
|---|---|
| ![Desert, on-screen controls](docs/images/touch_desert.jpg) | ![Fight, on-screen controls](docs/images/touch_combat.jpg) |

**With a gamepad** (Bluetooth / USB): the on-screen controls hide themselves and come back when it disconnects.

| | |
|---|---|
| ![Mars surface, gamepad](docs/images/desert.jpg) | ![Fight with an imp, gamepad](docs/images/combat.jpg) |
| ![Canyon, gamepad](docs/images/canyon.jpg) | ![Main menu in Russian, on-screen controls](docs/images/touch_controls.jpg) |

## Port settings (gear button)

The gear at the top of the screen opens the port's own settings, on top of the game's menus:

![Port settings dialog](docs/images/settings.jpg)

| Setting | Default | What it does |
|---|---|---|
| DOOM dynamic resolution | off | lets the game lower the 3D resolution under load (also in the game's Video menu) |
| Turnip (Mesa) driver | off | uses a Turnip driver instead of the stock Qualcomm one (slower on Adreno 750 for now) |
| Game language | system | text and voice: system language, Russian or English (next launch) |
| Aim assist | 1 | strength of the game's aim assist; "Как на Switch" keeps the Switch behaviour (next launch) |
| Rumble | off | controller / phone vibration (next launch; off to save battery) |
| FPS and frame time counter | on | the overlay in the top-left corner |
| 3D resolution | 100% (720p) | 85% / 75% / 67%; the HUD stays at 720p |
| Sharpening (FSR RCAS) | off | weak / medium / strong |
| Look sensitivity | 1.0 | swipe look and the look while FIRE is held |
| Move stick dead-zone compensation | none | offsets the game's own move-stick dead zone |

Graphics settings apply at the next launch, control settings at once. The dialog is in Russian for now.

## What it is, and what it is not

**It is:**

- The original game code, **executing natively** on the phone — no emulated CPU, no JIT. Switch and Android phones
  share the AArch64 instruction set; an offline rewriter changes only the few instructions that cannot run unchanged
  on Linux (x18 use, TLS reads, system calls, counter reads) into calls to the host.
- An HLE host for the Horizon OS services DOOM uses (threads, memory, file system, audio renderer, input, sockets…),
  and an NVN → Vulkan recorder and engine tuned for Adreno GPUs.
- A phone port: on-screen controls with mouse-like touch look, game controllers, gyro aiming, aim assist, rumble,
  resolution scaling and sharpening, the game's own languages (including Russian) following the phone's locale.

**It is not:**

- An emulator, cloud gaming or streaming.
- **A source of game content.** There is no game program, data, shader, text or key in this repository, and the APK
  contains none. You dump DOOM from a Switch you own and convert it on your PC ([docs/doom/GAME_FILES.md](docs/doom/GAME_FILES.md)).
  What the repository knows about the game (patch addresses, cvar names, shader identifiers…) is listed in
  [docs/doom/DERIVED.md](docs/doom/DERIVED.md).
- Online. The MULTIPLAYER entry is removed from the main menu; the campaign runs fully offline.
- Affiliated with id Software, Bethesda, ZeniMax, Panic Button or Nintendo.

## Current state

Target device: **Snapdragon 8 Gen 3 / Adreno 750** (Xiaomi 14 Ultra) — the only device tested so far. Other
Snapdragon devices may work; other GPUs are untested.

- The campaign is playable from the main menu through gameplay at 1280×720 (the Switch's handheld resolution).
- Performance on Adreno 750 with the stock driver: **~45 fps typical**, ~35-43 fps in the big desert scenes; with
  chunked submit (default) frame pacing holds a steady 30-60 fps. The game's adaptive tick keeps game speed equal to
  real time at any frame rate.
- Input: on-screen pad (left stick, mouse-like look on the right half, FIRE that also looks while held, R3),
  Bluetooth / USB controllers (the on-screen pad hides while one is connected), gyro aiming, aim assist with adjustable
  strength, rumble.
- Settings (gear button): 3D render resolution, the game's dynamic resolution, RCAS sharpening, Turnip / custom Vulkan
  driver, game language, aim assist strength, frame counter. The in-game Video menu's *Chromatic Aberration* entry is
  repurposed as *Dynamic Resolution*.

### Known issues

- The game's GPU triangle culling (`r_geometryCulling`, `r_smallTriangleCulling`) is **off by default**: on our GPU
  path it dropped visible triangles. Turning it on (`DM_DOOM_GEOMCULL=1`) is for debugging only.
- Dynamic resolution is off by default (it looked worse than a fixed lower resolution); enable it in the Video menu.
- Only update **v262144** is supported; the patches and addresses are tied to its build id.
- Mesa Turnip works through libadrenotools but is optional and slower than the stock driver on Adreno 750.
- Many renderer fixes are still behind switches for A/B testing; see [docs/doom/SWITCHES.md](docs/doom/SWITCHES.md).

## Building

Requirements: Android SDK with NDK r29, CMake ≥ 3.22 and Ninja, JDK 17+ and Gradle, Python 3. Clone with submodules:

```sh
git clone --recursive <this repository>
# or, in an existing clone:
git submodule update --init --recursive
```

Configure and build the native host for DOOM (`DM_GAME=doom`; the default `d2r` builds the original D2R host):

```sh
cmake -S dm/android -B work/build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release -DDM_GAME=doom
```

Then build, sign and (optionally) install the APK — `build.bat` on Windows (wraps the PowerShell script), `pwsh`
elsewhere:

```sh
build.bat [-Install <adb serial>]                 # Windows
pwsh tools/build_apk.ps1 [-Install <adb serial>]  # Linux / macOS with PowerShell
```

The script reads every tool path from the environment (`ANDROID_HOME`, `JAVA_HOME`, `GRADLE`, `PYTHON`) and signs with
`DM_KEYSTORE` (default: the Android debug keystore in `~/.android`). Output: `work/DOOM-port-debug.apk`, app id
`org.doom.port`. `work/` is git-ignored and holds every build output and every game-derived file.

## Installing

1. Dump your own copy (base game + update v262144) and your console's `prod.keys` — e.g. with nxdumptool and
   Lockpick_RCM on your Switch.
2. Prepare the game files with one command (it checks the inputs are the supported DOOM version, extracts ExeFS and
   RomFS, patches the code for Android and translates the shaders):

   ```sh
   ./setup.sh check                                                # what is missing; changes nothing
   ./setup.sh <base .nsp/.nsz/.xci> <update .nsp/.nsz> <prod.keys> [--push <adb serial>]
   ```

   On Windows: `setup.bat` with the same arguments. Details: [docs/doom/GAME_FILES.md](docs/doom/GAME_FILES.md).
3. Install the APK and copy `work/DOOM Mobile` to the phone's `Download` folder (or let `--push` / `./setup.sh push
   <serial>` do it over adb).
4. Start *DOOM Mobile* and allow *All files access* when asked.

## Repository layout

| Path | What |
|---|---|
| `dm/` | The host (from d2r-mobile): loader, HLE (`dm/host`), NVN recorder and Vulkan engine (`dm/gfx`), Android app (`dm/android`), offline rewriter (`dm/tools/patch_nso.py`) and its verifiers (`dm/verify`). DOOM code is behind `DM_GAME_DOOM`. |
| `setup.sh`, `setup.bat`, `build.bat` | Game file preparation (`tools/setup_doom.py`) and APK build entry points |
| `tools/` | DOOM tooling: game file extraction, shader jobs, APK build, device test and analysis scripts |
| `third_party/libadrenotools` | git submodule (BSD-2-Clause) |
| `docs/doom/` | Port documentation: state, recon, switches, game files, derived information |
| `docs/research/` | Analysis notes behind individual fixes |
| `docs/upstream/` | d2r-mobile's original README and contribution guide; the other files in `docs/` are upstream's D2R documentation |

## Credits

DOOM Mobile stands on the work of others — thank you:

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** by jhaizhou (MIT) — the base of this port: native
  NSO loader and rewriter, HLE host, NVN → Vulkan engine, shader translation chain and Android app.
- **[nxnative](https://github.com/seedds/nxnative)** by seedds (MIT) — threads and SVCs, SDK 6.x services, audio
  renderer, NVN entry points, timestamp units, patcher and translator pieces.
- **[nx2android](https://github.com/shirai91/nx2android)** / nx2apk (MIT) — window acquire-sync fix (1c6177d),
  Android haptics, ADPF, gamepad code.
- **[BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid)** and
  **[LASRecompAndroid](https://github.com/mamcjr/LASRecompAndroid)** by mamcjr, and
  **[LASRecomp](https://github.com/peiikai/LASRecomp)** by Peiikai (MIT) — the Turnip / adrenotools driver loading
  approach, chunked submit and sync-lag ideas, UBWC / format-list and Turnip notes.
- **[libadrenotools](https://github.com/bylaws/libadrenotools)** by Billy Laws (BSD-2-Clause).
- **Ryujinx** shader translator (Ryujinx Team and Contributors, MIT), vendored by d2r-mobile in `dm/third_party/ryujinx`.
- **[AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)** RCAS (MIT).
- Mesa **Turnip** (MIT) and the Khronos **Vulkan validation layers** (Apache-2.0) — used, not redistributed.

Details per file: [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).

DOOM © id Software LLC, a ZeniMax Media company. DOOM, id, id Software and id Tech are trademarks of ZeniMax Media
Inc.; the Switch version was developed with Panic Button. Nintendo Switch and NVN are trademarks of Nintendo. This is an
unofficial, non-commercial fan project with no affiliation to any of them. Play it with a copy you own.

## License

MIT — see [LICENSE](LICENSE). The d2r-mobile copyright notice is kept; third-party components keep their own licenses.

---

## По-русски

**DOOM Mobile** — нативный порт DOOM (2016) для Nintendo Switch на Android. Оригинальный AArch64-код игры исполняется
прямо на процессоре телефона (без эмулятора и JIT); небольшой хост подменяет системные сервисы Switch, а графический API
NVN воспроизводится через Vulkan. Основа — проект [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) (MIT).

- **Игры в репозитории нет.** Нужна своя копия DOOM для Switch (база + обновление v262144) и свои ключи консоли;
  подготовка файлов описана в [docs/doom/GAME_FILES.md](docs/doom/GAME_FILES.md) (англ.).
- Проверено пока только на Adreno 750 (Snapdragon 8 Gen 3, Xiaomi 14 Ultra); другие GPU и устройства не проверялись,
  Turnip — опционально и медленнее. На нём: обычно ~45 fps, ~35-43 в больших сценах пустыни,
  стабильные 30-60 с chunked submit.
- Экранный геймпад с обзором «как мышью», геймпады, гироскоп, aim assist, вибрация, русский язык (язык игры берётся из
  системы или задаётся в настройках), масштаб 3D-разрешения и резкость RCAS.
- Шестерёнка вверху экрана открывает настройки порта: динамическое разрешение, драйвер Turnip, язык игры, сила
  помощи в прицеливании, вибрация (по умолчанию выключена), счётчик FPS, разрешение 3D, резкость RCAS,
  чувствительность обзора и компенсация мёртвой зоны стика (см. таблицу «Port settings» выше).
- Известные ограничения: GPU-куллинг треугольников игры выключен (на нашем пути он терял видимые треугольники),
  динамическое разрешение выключено по умолчанию, поддерживается только обновление v262144, мультиплеер убран.
- Подготовка файлов игры одной командой: `./setup.sh <база> <обновление> <prod.keys> [--push <serial>]`
  (на Windows — `setup.bat`), проверка без изменений — `./setup.sh check`.
- Сборка: `cmake -S dm/android -B work/build-android … -DDM_GAME=doom`, затем `build.bat` / `tools/build_apk.ps1` (см. «Building»).

Подробности о состоянии порта, переключателях и истории исправлений — в [docs/doom/](docs/doom/) и [docs/research/](docs/research/).
