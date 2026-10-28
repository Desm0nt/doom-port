# Game-derived information in this repository

DOOM Mobile contains **no game code and no game data**: no executables or parts of them, no RomFS / resource files,
no shaders (binary, GLSL or translated), no textures, audio, video, text or fonts, and no keys. The player supplies
the game ([GAME_FILES.md](GAME_FILES.md)) and every game-derived output (patched modules, extracted RomFS, translated
shaders) is produced on the player's machine into the git-ignored `work/` directory.

To make the original code run on Android, the repository does contain *facts about* the game, found by analysing the
author's own copy: addresses, short instruction encodings, identifiers and layouts. This page lists them so their
extent is clear. They apply only to DOOM for Switch, update v262144 (main build id `2847991952b3d7abd228624fcd3b0779…`,
nnSdk 6.4.0); the rewriter checks the build id and the original bytes before it changes anything.

| What | Where | Extent |
|---|---|---|
| **Patch sites ("cuts")**: module offset, the original 4-byte instruction word(s) and the replacement, with a one-line description naming the game function | `dm/tools/patch_nso.py` (`CUTS` / DOOM section) | 17 entries: system UI "never showing", title storage disabled path offline, EULA / legal screen accepted, events from any input device, no MULTIPLAYER button in the main menu, Chromatic Aberration option rebound to dynamic resolution (`rs_enable`) |
| **Two menu strings** replaced in place | `dm/tools/patch_nso.py` | The localisation keys `#str_menu_system_field_chromatic_aberration_label` / `_help` (original bytes, as identifiers) are overwritten with our own English text "Dynamic Resolution" / its help line |
| **Observation gates**: offsets and the original first instruction of a few game functions (menu / SWF event handling, event queue, frame loop, dialog add, sign-in lookup) | `dm/tools/patch_nso.py` (`OBS`), `dm/host/dm.h` (gate numbers 23-34) | Read-only diagnostics (`DM_INPUT_OBS=1`) |
| **Rewrite-site classes** (x18 sites, word-offset jump tables, retry windows, `sub sp` alignment, `mov sp, x18`) | computed at patch time; a few addresses are quoted in comments of `patch_nso.py`, `dm/verify/equiv.py` | Comments only |
| **Struct offsets and gate addresses** for gyro aiming and aim assist (player / weapon aim-assist data, six-axis state reads) | `dm/host/hle_hid.c` | Offsets into game objects and addresses of the checks, so the host can feed the phone's gyro and set aim-assist strength |
| **SDK 6.4 internals**: offsets of nnSdk functions and fields (ThreadType name pointer, audio renderer system, account constructor, heap readouts…) | `dm/host/hle_*.c` comments and constants | Offsets / field positions, used to call or mirror the SDK's own code |
| **Game cvar names** passed as `+set` arguments | `dm/host/hle_args.c`, `docs/doom/SWITCHES.md` | e.g. `rs_forceFractionX/Y`, `rs_enable`, `r_geometryCulling`, `r_smallTriangleCulling`, `joy_*`, `com_adaptiveTick*`, `aa_DESIGN_override` |
| **NVN API**: function names and the layout of NVN objects as the game uses them | `dm/gfx/nvn/` | NVN is Nintendo's graphics API; names and layouts follow the public patterns of homebrew / emulator projects (Ryujinx, deko3d, nxnative) |
| **Shader identifiers**: 16-hex-digit keys (sha256 of a shader's control section) of individual game shaders and their roles (e.g. the GPU culling geometry shaders, TAA, global fog) | `dm/gfx/vulkan/engine/xbg_game.cpp`, `xbg_spirv_*.cpp`, docs | Identifiers only; the shaders themselves are never included |
| **Resource container format** of id Tech 6 (`.index` / `.pindex` layout, `generated/nx/*.sb` container) | `tools/doom_index.py`, `tools/doom_shaders.py`, `tools/doom_shader_jobs.py` | Parsers; no resource data |
| **Analysis notes**: function names, addresses, call chains, a handful of paraphrased shader lines describing a bug | `docs/research/*.md`, `docs/doom/*.md` | Text analyses; the longest quoted fragments are a few pseudo-code lines (e.g. `research/fog_fix.md`, `research/gs_cull_fix.md`) |

Function and type names (e.g. `idMenuManager_Shell::HandleEvent`) come from the symbols the Switch executables export.

What the repository does **not** contain, and the tooling never writes into the source tree: `*.nsp / *.nsz / *.nca /
*.xci`, ExeFS (`main`, `sdk`, `*.raw`, `main.npdm`), `*.patched.bin`, RomFS files (`*.resources`, `*.patch`,
`*.pindex`, `*.bik`, …), shader binaries or `.xgs` bundles, console keys.

The base d2r-mobile tree carries its own D2R-derived tables (for example `dm/gfx/nvn/shader_map.tsv`); they are
described by upstream's [`docs/DERIVED.md`](../DERIVED.md) and are not used by the DOOM build.
