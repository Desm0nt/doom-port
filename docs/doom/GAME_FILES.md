# Game files: what you need and where they go

DOOM Mobile ships **no game content**. It runs the game code from *your own* copy of DOOM (2016) for Nintendo Switch.
You dump the game from a Switch you own, prepare it on your PC with one command and copy the result to the phone.
Nothing here downloads game files, and your packages and keys never leave your machine.

## What you need

| Item | Notes |
|---|---|
| DOOM base game, title `0100416004C00000` | NSP, NSZ or XCI dump of your own cartridge / eShop copy |
| DOOM update `0100416004C00800`, **v262144** | NSP or NSZ. The port, its patches and addresses are made for this update (main build id `2847991952b3d7abd228624fcd3b0779…`, nnSdk 6.4.0); other versions are refused. |
| `prod.keys` of your own console | Needed to decrypt your packages; read in memory only, never copied |
| ~70 GB free on the PC with NSZ input (~45 GB with NSP / XCI), ~25 GB at the end; ~22 GB on the phone | |
| Python 3.10+ with `cryptography`, `zstandard`, `lz4`, `capstone` | `pip install cryptography zstandard lz4 capstone` |
| .NET 10 SDK and SPIRV-Tools (`spirv-opt`, `spirv-val`) | for the shader translator (it targets `net10.0`; `DOTNET=<dotnet executable>` picks a non-default install); the Android NDK's `shader-tools/` are found automatically via `ANDROID_HOME` / `ANDROID_NDK_HOME` |
| adb | only for `--push` |

**Dumping (on your Switch, homebrew):** dump the base game and the update with
[nxdumptool](https://github.com/DarkMatterCore/nxdumptool) (NSP, or XCI for a cartridge), and your console keys with
[Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM) (`prod.keys`). NSZ dumps made with `nsz` work as well.

## One command

```sh
./setup.sh check                                   # what is installed / prepared and the next step; changes nothing
./setup.sh <base .nsp/.nsz/.xci> <update .nsp/.nsz> <prod.keys> [--push <adb serial>]
```

On Windows: `setup.bat ...` with the same arguments (it finds Python through the `py` launcher or `PATH`, or `PYTHON`,
and tells you which pip packages are missing); quote paths with spaces. `python tools/setup_doom.py ...` works too.

What it does, skipping every step whose output is already complete (delete an output to redo it):

1. **Checks the inputs**: the base must be `0100416004C00000`, the update `0100416004C00800` v262144; anything else stops
   with a message. `--dry-run` stops here and prints the plan.
2. **ExeFS** of the update (`main`, `sdk`, …) → `work/exefs-update`, and checks main's build id.
3. **Offline patch** of `main` and `sdk` for Android (`dm/tools/patch_nso.py`, `DM_TARGET=android`) → `dm/out`.
4. **RomFS** of base + update, IVFC-verified (`tools/romfs_doom.py` with the readers in `tools/content_reader.py`; no
   hactool needed). NSZ input is first expanded to NSP in `work/nsp`, which is removed afterwards (`--keep-nsp` keeps it).
5. **Shaders**: DOOM's precompiled Maxwell shaders → SPIR-V bundles (`tools/doom_shader_jobs.py` →
   `tools/build_dmsh_xlate_doom.py` → `dm/gfx/tools/dmsh_vk_doom.py`) → `dm/shaders` (~860 `.xgs`).

The result is the phone folder, ready to copy:

```
work/DOOM Mobile/              → /sdcard/Download/DOOM Mobile/ on the phone
├── romfs/                     the game's RomFS (~22 GB)
└── dm/
    ├── out/                   patched main / sdk and their tables
    ├── shaders/               SPIR-V bundles
    └── env                    (optional, you create it: KEY=VALUE debug switches, see SWITCHES.md)
```

`--work DIR` (or `DM_DOOM_WORK`) puts everything somewhere else than `<repository>/work`.

## Copy to the phone

- With adb: `./setup.sh push <adb serial>` (or `--push` on the main command). It copies only missing or size-mismatched
  files, so an interrupted copy resumes, and grants the app *All files access*
  (`appops set org.doom.port MANAGE_EXTERNAL_STORAGE allow`) when the app is installed.
- By hand: copy `work/DOOM Mobile` to the phone's `Download` folder over USB, start the app and allow *All files
  access* when it asks.

Saves, the game's own settings and logs stay in the app's private storage.

## The steps by hand

The same pipeline with the individual tools, for development (paths are the defaults `setup.sh` uses):

```sh
python tools/ncz2nsp.py base.nsz work/nsp/base.nsp            # only for NSZ; same for the update
python tools/extract_exefs_nsz.py update.nsz work/exefs-update  # NSZ; NSP / XCI: setup_doom.py reads the NCA directly
DM_TARGET=android python dm/tools/patch_nso.py work/exefs-update/main "work/DOOM Mobile/dm/out" main
DM_TARGET=android python dm/tools/patch_nso.py work/exefs-update/sdk  "work/DOOM Mobile/dm/out" sdk
python dm/verify/equiv.py "work/DOOM Mobile/dm/out" main work/exefs-update/main --jobs 12   # optional proof, expect 0 differences
python tools/romfs_doom.py extract "work/DOOM Mobile/romfs"    # reads work/nsp/{base,update}.nsp
python tools/doom_shader_jobs.py "work/DOOM Mobile/romfs" work/shaders-jobs
python tools/build_dmsh_xlate_doom.py work/build/dmsh_xlate_doom
XLATE=work/build/dmsh_xlate_doom/bin/dmsh_xlate python dm/gfx/tools/dmsh_vk_doom.py work/shaders-jobs "work/DOOM Mobile/dm/shaders"
```

The key file is taken from `NX_PROD_KEYS` (default `~/.switch/prod.keys`) by the individual tools. `setup_doom.py` also sets
`PYTHONUTF8=1` (the patcher prints UTF-8), passes `DOTNET` to the translator build and `SPIRV_OPT` / `SPIRV_VAL` (found on
`PATH` or in the NDK's `shader-tools/`) to the shader step; set them yourself when running the tools by hand. On Windows the
translator is `dmsh_xlate.exe`.
