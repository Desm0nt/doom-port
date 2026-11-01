# Game files: what you need and where they go

DOOM Mobile ships **no game content**. It runs the game code from *your own* copy of DOOM (2016) for Nintendo Switch.
You dump the game from a Switch you own, prepare it on your PC with one command and copy the result to the phone.
Nothing here downloads game files, and your packages and keys never leave your machine.

**Players: follow the step-by-step guide [docs/INSTALL.md](../INSTALL.md) (Russian: [docs/INSTALL.ru.md](../INSTALL.ru.md)).**
This page has the technical details.

## What you need

| Item | Notes |
|---|---|
| DOOM base game, title `0100416004C00000` | NSP, NSZ, XCI or XCZ dump of your own cartridge / eShop copy |
| DOOM update `0100416004C00800`, **v262144** | NSP, NSZ, XCI or XCZ (a package holding both the base game and the update counts as both). The port, its patches and addresses are made for this update (main build id `2847991952b3d7abd228624fcd3b0779…`, nnSdk 6.4.0); other versions are refused. |
| `prod.keys` of your own console | Needed to decrypt your packages; read in memory only, never copied |
| ~50 GB free on the PC with NSZ / XCZ input (~25 GB with NSP / XCI), ~23 GB at the end; ~22 GB on the phone | checked before anything is written |
| Python 3.10+ with `cryptography`, `zstandard`, `lz4`, `capstone<6` | `setup.bat` / `setup.sh` provide them in `tools/.runtime` (asked first); by hand: `pip install cryptography zstandard lz4 "capstone<6"` |
| .NET 10 SDK | for the shader translator (it targets `net10.0`). Found via `DOTNET`, `tools/.runtime/dotnet`, `~/.dotnet` or `PATH`; when missing, setup offers Microsoft's `dotnet-install` script into `tools/.runtime/dotnet` (no admin rights) |
| SPIRV-Tools (`spirv-opt`, `spirv-val`) | optional: only the `spirv-val` check of every translated shader. Found on `PATH`, via `SPIRV_OPT` / `SPIRV_VAL` or in the Android NDK's `shader-tools/` (`ANDROID_HOME` / `ANDROID_NDK_HOME`). Without them the bundles are byte-identical (the capability trim is `py_trim` either way unless the `spirv-opt` in use has `--trim-capabilities`) |
| adb | only for copying over USB debugging (`push`) |

Containers: NSP and NSZ (PFS0), XCI and XCZ (the secure partition, also with the 0x1000-byte key area in front), and
split dumps (a folder of `00`, `01`, … parts, or `name.xc0` / `name.ns0` with `.xc1` … next to it). NCZ entries may be
solid (one zstd stream, the nsz default for NSZ) or block-compressed (`NCZBLOCK`, the nsz default for XCZ).

**Dumping (on your Switch, homebrew):** dump the base game and the update with
[nxdumptool](https://github.com/DarkMatterCore/nxdumptool) (NSP, or XCI for a cartridge), and your console keys with
[Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM) (`prod.keys`). NSZ / XCZ files made with `nsz` work as well.
NSP dumps need a common ticket (nxdumptool: *remove console specific data*); a personalized ticket is refused with a message.

## One command

```sh
./setup.sh                                         # files from put-game-files-here/ (2 levels deep) or the current folder
./setup.sh <files or folders, any order> [--work DIR] [--push [SERIAL]] [--dry-run] [--yes]
./setup.sh check [files...]                        # tools, game files found, what is prepared, next step; changes nothing
./setup.sh push [SERIAL]                           # copy the finished folder over adb (resumes)
```

On Windows: `setup.bat` with the same arguments (a double-click runs the first form, files dropped onto it the second);
quote paths with spaces. `python tools/setup_doom.py ...` works too once the Python packages are there.

**The launchers.** `setup.bat` runs `tools/bootstrap.ps1` first: it reuses `tools/.runtime/venv` or `tools/.runtime/python`
when they already have the packages (no network needed), otherwise makes a venv on top of an installed Python 3.10+
(`PYTHON`, `py -3`, `python`) or, when there is none, downloads python.org's official NuGet package (`python` on
nuget.org, a complete Python without installer or registry entries) into `tools/.runtime/python`, and then installs the
packages from PyPI (wheels only). `setup.sh` makes `tools/.runtime/venv` with `python3` (`PYTHON`, `python3.13` …
`python3.10`, `python3`) and prints the distribution's command when Python or its venv module is missing. Every
download is asked first; `DOOM_SETUP_YES=1` (or `--yes` for the questions of `setup_doom.py`) answers yes.
`tools/.runtime` is git-ignored; deleting it removes everything the launchers and setup installed.

What it does, skipping every step whose output is already complete (delete an output to redo it):

1. **Checks the tools and the inputs**: the .NET 10 SDK (offered when missing), then every package's content meta
   (CNMT): the base must be `0100416004C00000`, the update `0100416004C00800` v262144, found by title id in any order
   or in one combined package; anything else stops with a message listing the title ids and versions it found (other
   games and DLC are listed and ignored). Then the free space for the remaining steps (the RomFS plus the expanded
   programs of NSZ / XCZ input). `--dry-run` stops here and prints the plan.
2. **ExeFS** of the update (`main`, `sdk`, …) → `work/exefs-update`, and checks main's build id.
3. **Offline patch** of `main` and `sdk` for Android (`dm/tools/patch_nso.py`, `DM_TARGET=android`) → `dm/out`.
4. **RomFS** of base + update, IVFC-verified (`tools/romfs_doom.py` with the readers in `tools/content_reader.py`; no
   hactool needed). With NSZ / XCZ input the program NCA of the base and of the update is first expanded to an NSP in
   `work/nsp` (only that NCA and the tickets), which is removed afterwards (`--keep-nsp` keeps it).
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

At the end setup prints the folder, its size and the next steps, and offers to copy it when adb sees exactly one phone.

## Copy to the phone

- With adb: `./setup.sh push [serial]` (or `--push` on the main command). It copies only missing or size-mismatched
  files, so an interrupted copy resumes, and grants the app *All files access*
  (`appops set org.doom.port MANAGE_EXTERNAL_STORAGE allow`) when the app is installed.
- By hand: copy `work/DOOM Mobile` to the phone's `Download` folder over USB, start the app and allow *All files
  access* when it asks.

Saves, the game's own settings and logs stay in the app's private storage.

## The steps by hand

The same pipeline with the individual tools, for development (paths are the defaults `setup.sh` uses):

```sh
python tools/ncz2nsp.py base.nsz work/nsp/base.nsp            # only for NSZ / XCZ; same for the update
python tools/extract_exefs_nsz.py update.nsz work/exefs-update  # NSZ / XCZ; NSP / XCI: setup_doom.py reads the NCA directly
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
`PATH` or in the NDK's `shader-tools/`) to the shader step, and `DOTNET_ROOT` (the folder of that dotnet, so the
translator's apphost finds a private .NET); set them yourself when running the tools by hand. On Windows the translator
is `dmsh_xlate.exe`, elsewhere `dmsh_xlate`.
