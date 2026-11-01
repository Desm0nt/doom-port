# Installing DOOM Mobile — step by step

Русская версия: [INSTALL.ru.md](INSTALL.ru.md).

DOOM Mobile runs **your own copy** of DOOM (2016) for Nintendo Switch on an Android phone. This project contains no
game files and never shares game files or console keys: you dump them from a Switch you own. A setup program on your
PC turns your dump into a game folder (~22 GB), you copy that folder to the phone and install the app.

The whole thing takes about an hour, most of it waiting.

## What you need

**From your Switch** (see step 1):

| What | Details |
|---|---|
| DOOM base game | title id `0100416004C00000`, from your cartridge or eShop copy |
| DOOM update **v262144** | title id `0100416004C00800`. Only this exact update works (the port is made for its code). Dump tools usually put it in the file name, e.g. `...[0100416004C00800][v262144].nsp` |
| `prod.keys` | the keys of **your own** console |

The game and the update can be **NSP, NSZ, XCI or XCZ**, in any combination. An XCI that already contains the update
counts as both. Split dumps from a FAT32 SD card work too (a folder with parts `00`, `01`, … or `name.xc0`, `name.xc1`, …).
File names do not matter: setup recognises the files by their content.

**A PC** with Windows 10/11 (64-bit), Linux or macOS:

- about **50 GB of free disk space** while preparing (70 GB to be safe); about 23 GB stay at the end;
- internet on the first run: setup downloads a few helper tools (Python packages ~15 MB, Microsoft .NET ~300 MB, and
  Python itself ~15 MB on Windows if you have none). It asks before each download and puts them only inside its own
  folder. Later runs work offline.

**A phone:**

- Android 12 or newer, 64-bit;
- **Snapdragon with an Adreno GPU recommended** (tested on Snapdragon 8 Gen 3 and 8 Gen 2; other GPUs are untested, see
  *Vulkan drivers* / *Other GPUs* in the [README](../README.md));
- 8 GB of RAM or more (12 GB+ is more comfortable);
- about **23 GB free** in internal storage;
- a USB cable (or another way to copy a 22 GB folder to the phone).

## Step 1. Dump your game and keys (on your Switch)

You need homebrew on your Switch for this. This guide does not cover how to get it.

1. Dump the **base game** and the **update v262144** with
   [nxdumptool](https://github.com/DarkMatterCore/nxdumptool):
   - an installed game or eShop copy → *NSP*; a cartridge → *XCI* (dump the installed update separately as NSP);
   - turn on **"remove console specific data"** for NSP dumps (otherwise the update cannot be read on a PC).
2. Dump your console keys with [Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM). You get `prod.keys`
   (on the SD card in `switch/`).
3. Copy the dumps and `prod.keys` to your PC.

Do not download game files or keys from the internet, and do not share yours.

## Step 2. Download DOOM Mobile setup

1. Open <https://github.com/Desm0nt/doom-port>, click the green **Code** button → **Download ZIP**
   (direct link: <https://github.com/Desm0nt/doom-port/archive/refs/heads/main.zip>).
2. Extract the ZIP to a disk with enough free space, into a short path such as `D:\DOOM-setup`.
   You get a folder `doom-port-main` with `setup.bat`, `setup.sh` and a folder `put-game-files-here`.

## Step 3. Put your files into `put-game-files-here`

Copy (or move) the three things into the `put-game-files-here` folder:

```
doom-port-main/
├── setup.bat
├── setup.sh
└── put-game-files-here/
    ├── DOOM ... [0100416004C00000] ... .nsp   (or .nsz / .xci / .xcz — the base game)
    ├── DOOM ... [0100416004C00800][v262144] ... .nsp   (the update)
    └── prod.keys
```

You may also put the folder from your dump tool inside it as it is (setup looks two folders deep).

## Step 4. Run setup

### Windows

1. **Double-click `setup.bat`.**
   If Windows warns about a file from the internet, choose **Run** (or **More info → Run anyway**).
2. Answer the questions with **Y** and Enter (Enter alone also means yes):
   - *Download a private Python?* — only asked when your PC has no Python 3.10+;
   - *Install the Python packages?*
   - *Download and install the .NET 10 SDK?*

   Everything goes into `tools\.runtime` inside the setup folder. Nothing is installed system-wide, no admin rights are
   needed. Deleting the folder removes it all.
3. **Wait.** Setup shows six numbered stages with progress:

   | Stage | Usually takes |
   |---|---|
   | 1. Finding and checking your game files | seconds |
   | 2. Checking the tools and the free space | seconds (first time: the .NET download) |
   | 3. Extracting the game program | 1–5 min |
   | 4. Patching the program for Android | 1–5 min |
   | 5. Extracting the game data (~22 GB) | 20–60 min |
   | 6. Translating the shaders | 5–20 min |

   It checks the free space before it starts and stops early with a clear message when something is wrong. If you
   close the window or the PC turns off, just run setup again: finished stages are skipped.
4. At the end it shows where the finished folder is and what to do next. The window stays open until you press a key.

Instead of using `put-game-files-here` you can also drag the three files onto `setup.bat`.

### Linux / macOS

1. You need Python 3.10 or newer with its `venv` module. If you do not have it:
   - Debian / Ubuntu: `sudo apt install python3 python3-venv python3-pip`
   - Fedora: `sudo dnf install python3 python3-pip`
   - Arch: `sudo pacman -S python python-pip`
   - macOS: `brew install python@3.12`, or the installer from <https://www.python.org/downloads/macos/>
2. Open a terminal in the `doom-port-main` folder and run:

   ```sh
   bash setup.sh
   ```

3. Answer the questions with **Y** and wait (same stages and times as on Windows). Python packages go into
   `tools/.runtime/venv`, .NET into `tools/.runtime/dotnet`; nothing is installed system-wide.

You can also give the files directly, in any order: `bash setup.sh ~/dumps/doom-base.nsz ~/dumps/doom-update.nsp ~/prod.keys`.

### Not sure what is missing?

Run `setup.bat check` (Windows, in a terminal in the setup folder) or `bash setup.sh check`. It lists the tools, the
game files it finds, what is already prepared and the next step, and changes nothing.

## Step 5. Copy the game folder to the phone

The finished folder is **`work/DOOM Mobile`** inside the setup folder (setup prints the full path and its size,
about 22 GB).

1. Connect the phone with a USB cable and choose **File transfer** on the phone.
2. Open the phone's **internal storage** → **Download** on the PC.
3. Copy the whole **`DOOM Mobile`** folder there. It must end up exactly like this:

   ```
   Internal storage/Download/DOOM Mobile/romfs/...
   Internal storage/Download/DOOM Mobile/dm/out/...
   Internal storage/Download/DOOM Mobile/dm/shaders/...
   ```

   Not on an SD card, and not `Download/DOOM Mobile/DOOM Mobile/...`.

Copying ~22 GB over USB takes 10–40 minutes. On macOS use [Android File Transfer](https://www.android.com/filetransfer/)
or OpenMTP; on Linux the file manager usually shows the phone directly.

**With adb (optional, for advanced users):** with USB debugging on, setup offers to copy the folder itself at the end
when it sees the phone. Later you can run `setup.bat push` / `bash setup.sh push`; an interrupted copy resumes.

## Step 6. Install the app

On the phone, open this link and install the APK:

**<https://github.com/Desm0nt/doom-port/releases/latest/download/DOOM-Mobile-arm64.apk>**

(Release notes and older versions: <https://github.com/Desm0nt/doom-port/releases>.) Android asks you to allow
installing apps from your browser or file manager the first time.

## Step 7. First launch

1. Start **DOOM Mobile**.
2. It asks for **All files access** (it needs it to read `Download/DOOM Mobile`). Turn it on for DOOM Mobile and go back.
3. The first start and the first loads take longer than later ones. The gear button at the top opens the port's
   settings (resolution, frame rate limit, driver, controls).

Saves and settings stay in the app's own storage. Installing a newer APK over the old one keeps them.

## Updating

- **New app version:** install the new APK over the old one. The game folder on the phone stays.
- If the release notes say the game folder must be prepared again: download the new setup ZIP, delete
  `work/DOOM Mobile/dm` in your old setup folder (or start fresh), run setup again and copy the new `dm` folder to the phone.

## Troubleshooting / FAQ

**"your DOOM update is v… ; only v262144 is supported"**
You dumped another update. Only v262144 works, because the port's patches are made for that exact program. Dump update
v262144.

**"the DOOM base game / the update … is not among the files"**
Setup lists every file it found with its title id and version. Check that you copied the right files into
`put-game-files-here` (DLC and other games are ignored).

**"prod.keys not found" / "your prod.keys has no …" / "does not decrypt with this prod.keys"**
Put `prod.keys` next to the game files. Use the keys of the console you dumped from, made with an up-to-date
Lockpick_RCM; keys from an older firmware lack newer key generations. `title.keys` is a different file. A file that
Windows shows as `prod.keys` but is really `prod.keys.txt` is fine.

**"personalized ticket" / "no ticket for rights id"**
Dump the NSP again with nxdumptool's *remove console specific data* option turned on (and keep the ticket).

**"not enough free space"**
Free some space, or put the work folder on another disk. In a terminal in the setup folder:
`setup.bat --work D:\doom-work` (Windows) or `bash setup.sh --work /mnt/big/doom-work`.

**A download failed (Python, packages or .NET)**
Check the internet connection (and proxy / antivirus) and run setup again; it continues where it stopped. You can also
install them yourself: Python 3.12 from <https://www.python.org/downloads/> ("Add python.exe to PATH"), the .NET 10 SDK
from <https://dotnet.microsoft.com/download/dotnet/10.0>.

**The app says the game files, modules or shaders are missing**
The folder is not where the app looks or the copy is incomplete. It must be `Download/DOOM Mobile` in internal storage
with `romfs` and `dm` directly inside it. Copy it again (or only the missing part). Make sure *All files access* is on
(Android settings → Apps → DOOM Mobile → Permissions / Special app access).

**The first start or a level load is slow**
Normal the first time: the game reads its data and the graphics driver prepares the shaders. Later starts are faster.
On 8–12 GB phones close other apps before playing.

**Black screen, yellow / white screen, or the app closes**
1. Open the gear dialog → *Vulkan driver* → try *System* (or the other choice), restart the app.
2. Check that the phone is supported (Snapdragon / Adreno recommended; see the README).
3. Report it with the log: install [Android platform-tools](https://developer.android.com/tools/releases/platform-tools),
   enable USB debugging, then run `adb logcat -d -s dm > doom-log.txt` right after the problem (the game must still be running or have just stopped). Open an issue at
   <https://github.com/Desm0nt/doom-port/issues> with the phone model, Android version and the log.

**Can I delete things on the PC afterwards?**
Yes. Once the folder is on the phone you may delete `work` (keep it if you want to copy again) and `tools/.runtime`
(the helper tools; setup downloads them again when needed). Your dumps are never changed.

Technical details of the preparation (what each step does, running the tools by hand): [doom/GAME_FILES.md](doom/GAME_FILES.md).
