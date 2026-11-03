#!/usr/bin/env python3
"""DOOM Mobile: from your own DOOM dumps to the folder the app reads, in one command.

  setup                     (no file arguments, e.g. a double-click on setup.bat)
      Looks for the base game, the update v262144 and prod.keys in put-game-files-here/ (next to the setup scripts) and
      in the current directory, under any file names, and prepares everything.
  setup FILE_OR_FOLDER... [--work DIR] [--push [SERIAL]] [--dry-run] [--yes] [--keep-nsp] [--skip-shaders]
        [--steps exefs,patch,romfs,shaders]
      The same with explicit files (or folders to search), in any order. Packages: NSP, NSZ, XCI, XCZ, also split dumps
      (a folder of 00, 01, ... parts, or name.xc0 / name.ns0 with .xc1 ... next to it); one package may hold both the base
      game and the update. Base and update are told apart by the title id in their content meta (CNMT), not by the name.
      Then: ExeFS -> offline patch (dm/tools/patch_nso.py, DM_TARGET=android) -> RomFS -> shader translation -> the phone
      folder <work>/DOOM Mobile/{romfs, dm/out, dm/shaders}. Every step is skipped when its output is already complete;
      delete an output to redo its step. --dry-run identifies the inputs, checks the tools and the free space and prints
      the plan. --yes answers yes to every question (download .NET, copy to the phone).
  setup check [FILE_OR_FOLDER...] [--work DIR]
      Report the tools, the game files found, what has been prepared and the next step. Changes nothing.
  setup push [SERIAL] [--work DIR]
      Copy <work>/DOOM Mobile to /sdcard/Download/DOOM Mobile on the phone over adb (only missing / size-mismatched files,
      so an interrupted push resumes) and grant org.doom.port "All files access". SERIAL may be left out when exactly one
      phone is connected.

Supported: DOOM (2016) for Nintendo Switch, base 0100416004C00000 + update 0100416004C00800 v262144 (main build id
2847991952b3d7ab...). Packages and keys are only read, never copied, uploaded or written anywhere; the keys stay in
memory. Compressed packages (NSZ / XCZ) get their program expanded to plain NSPs in <work>/nsp (needed for the RomFS);
those are deleted afterwards unless --keep-nsp. Default work directory: $DM_DOOM_WORK or <repository>/work.
The .NET 10 SDK (shader translator build) is taken from DOTNET, tools/.runtime/dotnet, ~/.dotnet or PATH; when it is
missing, setup offers to install it into tools/.runtime/dotnet with Microsoft's dotnet-install script (no admin rights,
nothing system-wide). setup.bat / setup.sh provide Python and its packages the same way (tools/.runtime).
"""
import argparse, os, re, shutil, struct, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
RUNTIME = TOOLS / '.runtime'
DROP = ROOT / 'put-game-files-here'
sys.path.insert(0, str(TOOLS))

BASE_TID, UPDATE_TID, UPDATE_VERSION = 0x0100416004C00000, 0x0100416004C00800, 262144
BUILD_ID = '2847991952b3d7abd228624fcd3b0779'
APP_ID, PHONE_DIR = 'org.doom.port', '/sdcard/Download/DOOM Mobile'
APK_URL = 'https://github.com/Desm0nt/doom-port/releases/latest/download/DOOM-Mobile-arm64.apk'
MODULE_FILES = [f'{m}.{e}' for m in ('main', 'sdk') for e in ('patched.bin', 'fixups.tsv', 'gates.tsv')]
PY_PACKAGES = (('cryptography', 'decrypting your packages'), ('zstandard', 'NSZ / XCZ input'), ('lz4', 'NSO modules'),
               ('capstone', 'the offline patcher'))
PKG_RE = re.compile(r'\.(nsp|nsz|xci|xcz|xc0|ns0)$', re.I)
META_TYPES = {0x80: 'base game', 0x81: 'update', 0x82: 'DLC', 0x83: 'delta update'}
ROMFS_BYTES = 23 << 30          # the merged RomFS is ~22 GB
SMALL_BYTES = 3 << 29           # ExeFS, patched modules, shader jobs, translator build, bundles (~1.5 GB)
WIN = os.name == 'nt'
GB = 2**30
RUN_SETUP = 'run setup (double-click setup.bat on Windows, ./setup.sh on Linux / macOS)'

STAGES = [('Finding and checking your game files', 'a few seconds'),
          ('Checking the tools and the free space', 'seconds; the first time also the .NET download'),
          ('Extracting the game program (ExeFS)', '1-5 min'),
          ('Patching the program for Android', '1-5 min'),
          ('Extracting the game data (RomFS, ~22 GB)', '20-60 min, longest with NSZ / XCZ'),
          ('Translating the shaders', '5-20 min; the first run also downloads build packages from NuGet')]


class Fail(Exception):
    pass


def say(msg): print(f'== {msg}', flush=True)


def stage(n):
    name, est = STAGES[n - 1]
    print(f'\n[{n}/{len(STAGES)}] {name}' + (f'  (usually {est})' if est else ''), flush=True)
    return time.monotonic()


def took(t0):
    s = int(time.monotonic() - t0)
    return f'{s // 60} min {s % 60:02d} s' if s >= 60 else f'{s} s'


def gb(n): return f'{n / GB:.1f} GB'


def ask(question, yes):
    """Yes / no question, default yes. --yes (or DOOM_SETUP_YES=1) answers it; without a terminal the answer is no."""
    if yes or os.environ.get('DOOM_SETUP_YES') == '1':
        print(f'{question} [Y/n] y (--yes)'); return True
    if not (sys.stdin and sys.stdin.isatty()):
        print(f'{question} [Y/n] -> no (no terminal to ask; run with --yes to accept)'); return False
    try:
        return input(f'{question} [Y/n] ').strip().lower() in ('', 'y', 'yes', 'д', 'да')
    except EOFError:
        return False


class Progress:
    """One updating console line, at most a few times per second."""
    def __init__(self): self.t = 0.0

    def __call__(self, text, final=False):
        now = time.monotonic()
        if final or now - self.t > 0.5:
            self.t = now
            print(f'\r  {text}    ', end='\n' if final else '', flush=True)


class Layout:
    def __init__(self, work):
        self.work = Path(work).resolve()
        self.phone = self.work / 'DOOM Mobile'
        self.romfs, self.out, self.shaders = self.phone / 'romfs', self.phone / 'dm' / 'out', self.phone / 'dm' / 'shaders'
        self.exefs, self.nsp, self.jobs = self.work / 'exefs-update', self.work / 'nsp', self.work / 'shaders-jobs'
        self.xlate = self.work / 'build' / 'dmsh_xlate_doom'
        self.romfs_manifest = self.work / 'romfs-manifest.json'

    def exefs_ok(self):
        return build_id(self.exefs / 'main') == BUILD_ID and (self.exefs / 'sdk').is_file()

    def out_ok(self):
        # Complete AND made by the patcher in this checkout: build_src.sha256 (written by patch_nso.py) lists the hashes of the
        # patcher sources, so a newer release with new patches regenerates dm/out on the next setup run.
        if not all((self.out / f).is_file() for f in MODULE_FILES): return False
        try: rec = dict(reversed(l.split()) for l in (self.out / 'build_src.sha256').read_text().splitlines() if l.strip())
        except (OSError, ValueError): return False
        import hashlib
        for name in ('patch_nso.py', 'nso_code.py'):
            src = ROOT / 'dm' / 'tools' / name
            if src.is_file() and rec.get(name) != hashlib.sha256(src.read_bytes()).hexdigest(): return False
        return True

    def romfs_ok(self):
        return self.romfs_manifest.is_file() and self.romfs.is_dir()

    def shader_count(self):
        return len(list(self.shaders.glob('*.xgs'))) if self.shaders.is_dir() else 0

    def shaders_ok(self):
        return (self.shaders / '_vk_build.json').is_file() and self.shader_count() > 0

    def complete(self):
        return self.out_ok() and self.romfs_ok() and self.shaders_ok()


def build_id(nso):
    try:
        with open(nso, 'rb') as f:
            h = f.read(0x60)
    except OSError:
        return None
    return h[0x40:0x50].hex() if h[:4] == b'NSO0' else None


def free_bytes(path):
    p = Path(path)
    while not p.exists() and p != p.parent: p = p.parent
    return shutil.disk_usage(p).free


def folder_bytes(path):
    return sum(p.stat().st_size for p in Path(path).rglob('*') if p.is_file()) if Path(path).is_dir() else 0


# ---------------------------------------------------------------- finding the inputs
def looks_like_keys(p):
    """A small text file with header_key = ... (prod.keys, also prod.keys.txt / keys.txt)."""
    try:
        if not p.is_file() or p.stat().st_size > 1 << 20 or PKG_RE.search(p.name): return False
        return re.search(r'^\s*header_key\s*=\s*[0-9a-fA-F]{64}\s*$', p.read_text(errors='replace'), re.M | re.I) is not None
    except OSError:
        return False


def is_package_path(p):
    from content_reader import split_parts
    if p.is_dir(): return bool(split_parts(p))
    return bool(PKG_RE.search(p.name))


def search(folders, depth=0):
    """Packages and key files inside the folders; `depth` levels of subfolders are searched too."""
    pkgs, keys = [], []
    for d in folders:
        try:
            items = sorted(d.iterdir(), key=lambda q: q.name.lower()) if d.is_dir() else []
        except OSError:                         # e.g. a folder of another user in the current directory
            continue
        for p in items:
            try:
                if is_package_path(p): pkgs.append(p)
                elif p.is_file() and looks_like_keys(p): keys.append(p)
                elif p.is_dir() and depth > 0:
                    p2, k2 = search([p], depth - 1); pkgs += p2; keys += k2
            except OSError:                     # unreadable entry: not ours
                continue
    return pkgs, keys


def unique(paths):
    seen, out = set(), []
    for p in paths:
        r = str(Path(p).resolve()).lower() if WIN else str(Path(p).resolve())
        if r not in seen: seen.add(r); out.append(Path(p))
    return out


def gather(args):
    """(packages, key files, where we looked) from explicit paths, or from put-game-files-here/ and the current directory."""
    if args:
        pkgs, keys, folders = [], [], []
        for a in map(Path, args):
            if not a.exists(): raise Fail(f'not found: {a}')
            if a.is_dir() and not is_package_path(a): folders.append(a)
            elif a.is_file() and looks_like_keys(a): keys.append(a)
            elif a.is_file() and a.name.lower() in ('title.keys', 'dev.keys'):
                raise Fail(f'{a}: this is {a.name}; setup needs prod.keys (the console keys, with header_key in it)')
            else: pkgs.append(a)
        p2, k2 = search(folders)
        return unique(pkgs + p2), unique(keys + k2), folders or [Path.cwd()]
    folders = unique([DROP, Path.cwd()])
    p1, k1 = search([DROP], depth=2)            # the drop folder also with the game's own folders inside it
    p2, k2 = search([Path.cwd()])
    return unique(p1 + p2), unique(k1 + k2), folders


class Title:
    """One content meta (CNMT) found in a package."""
    def __init__(self, pkg, tid, version, mtype, contents):
        self.pkg, self.tid, self.version, self.mtype = pkg, tid, version, mtype
        self.contents = contents                     # [(content id hex, content type, size)]
        prog = [c for c in contents if c[1] == 1]
        self.program = None                          # entry name of the program NCA / NCZ in the package
        self.program_size = prog[0][2] if prog else 0
        self.program_id = prog[0][0] if prog else '?'
        if prog:
            for ext in ('.nca', '.ncz'):
                if prog[0][0] + ext in pkg.names: self.program = prog[0][0] + ext

    @property
    def compressed(self): return bool(self.program) and self.program.endswith('.ncz')

    def describe(self):
        game = 'DOOM ' if self.tid & ~0xfff == BASE_TID else ''
        return f"{game}{META_TYPES.get(self.mtype, f'type {self.mtype:#x}')} {self.tid:016X} v{self.version}"


class Scanned:
    def __init__(self, path, kind, names, titles):
        self.path, self.kind, self.names, self.titles = path, kind, names, titles


def cnmt_titles(pkg_scanned, data, o):
    tid, ver = struct.unpack_from('<QI', data, o)
    mtype = data[o + 12]
    ext, count = struct.unpack_from('<HH', data, o + 14)
    contents = []
    for i in range(count):
        r = o + 0x20 + ext + i * 0x38
        contents.append((data[r + 0x20:r + 0x30].hex(), data[r + 0x36], int.from_bytes(data[r + 0x30:r + 0x35], 'little')))
    return Title(pkg_scanned, tid, ver, mtype, contents)


def scan(path, keys):
    """Container kind and every content meta in a package. Errors become Fail with a plain explanation."""
    from content_reader import Package, Nca
    try:
        p = Package(path, keys)
    except (ValueError, AssertionError, OSError, struct.error) as e:
        why = '' if str(e).startswith('not an NSP') else f' ({e or "unknown layout"})'
        raise Fail(f'{path.name}: not an NSP / NSZ / XCI / XCZ package, or a damaged one{why}')
    try:
        names = {n for n, _, _ in p.entries}
        packed = any(n.endswith('.ncz') for n in names)
        kind = ('XCZ' if packed else 'XCI') if p.kind == 'xci' else ('NSZ' if packed else 'NSP')
        sc = Scanned(path, kind, names, [])
        for name, o, n in p.entries:
            if not name.endswith('.nca'): continue   # nsz compresses only program / data NCAs, never the content meta
            nca = Nca(p, name, o, n)
            if nca.type != 1: continue            # content meta
            data, ents = nca.sections[0].pfs()
            for cname, co, _ in ents:
                if cname.endswith('.cnmt'): sc.titles.append(cnmt_titles(sc, data, co))
        return sc
    except KeyError as e:
        k = e.args[0] if e.args else '?'
        raise Fail(f'{path.name}: your prod.keys has no {k}. Dump the keys again from your own console with an up-to-date '
                   'Lockpick_RCM (keys from an older firmware lack the newer key generations).')
    except ValueError as e:
        raise Fail(f'{path.name}: {e}')
    except AssertionError:
        raise Fail(f'{path.name}: the content does not decrypt with this prod.keys (wrong or damaged keys, or a damaged dump)')
    finally:
        p.close()


def resolve(pkgs, key_files, show=True):
    """Identify every package; returns (base Title, update Title, keys path). Fails with what was found otherwise."""
    if not key_files:
        raise Fail('prod.keys not found. Put the prod.keys of your own console next to the game files '
                   '(dump it with Lockpick_RCM on your Switch).')
    if not pkgs:
        raise Fail('no game packages found (NSP / NSZ / XCI / XCZ).')
    scanned, keys, last = [], None, None
    for kf in sorted(key_files, key=lambda k: k.name.lower() != 'prod.keys'):   # prod.keys first, then the others
        try:
            scanned = [scan(p, kf) for p in pkgs]; keys = kf; break
        except Fail as e:
            last = e
    if keys is None: raise last
    titles = [t for s in scanned for t in s.titles]
    if show:
        print(f'  keys: {keys.name}')
        for s in scanned:
            print(f'  {s.path.name}: {s.kind}, ' + (', '.join(t.describe() for t in s.titles) or 'no content meta'))
    found = '; '.join(f'{s.path.name}: ' + (', '.join(t.describe() for t in s.titles) or 'nothing') for s in scanned)
    titles.sort(key=lambda t: t.program is None)   # a complete copy first when the same title is there twice
    base = next((t for t in titles if t.tid == BASE_TID and t.mtype == 0x80), None)
    upd = next((t for t in titles if t.tid == UPDATE_TID and t.mtype == 0x81 and t.version == UPDATE_VERSION), None)
    if base is None:
        raise Fail(f'the DOOM base game ({BASE_TID:016X}) is not among the files. Found: {found}')
    if upd is None:
        other = [t for t in titles if t.tid == UPDATE_TID and t.mtype == 0x81]
        if other:
            v = other[0].version
            raise Fail(f'your DOOM update is v{v} (release {v >> 16}); only v{UPDATE_VERSION} (release {UPDATE_VERSION >> 16}) '
                       f'is supported: the patches are made for that exact build. Dump update v{UPDATE_VERSION}.')
        raise Fail(f'the DOOM update {UPDATE_TID:016X} v{UPDATE_VERSION} is not among the files. Found: {found}')
    for t, what in ((base, 'base game'), (upd, 'update')):
        if not t.program:
            raise Fail(f'{t.pkg.path.name}: the {what} program content ({t.program_id}) is missing '
                       'from the package (incomplete dump?)')
        check_program(t, keys)
    return base, upd, keys


def check_program(t, keys):
    """Open the program NCA now (title key from the ticket, key generation; for a plain NCA also its ExeFS hash table), so
    that a missing ticket or key, or a wrong title key, stops setup at stage 1 instead of in the middle of the work."""
    from content_reader import Package, Nca
    p = Package(t.pkg.path, keys)
    try:
        name, o, n = next(e for e in p.entries if e[0] == t.program)
        if name.endswith('.ncz'):                # compressed body: the NCA size comes from its (original) header
            from extract_exefs_nsz import xts_header
            p.f.seek(o)
            n = struct.unpack_from('<Q', xts_header(p.keys['header_key'], p.f.read(0xc00)), 0x208)[0]
        nca = Nca(p, name, o, n)
        if name.endswith('.nca'):
            for s in nca.sections:
                if s.h[3] == 2:                  # PFS0 (ExeFS): its hash table must match the section header
                    ho, hs = struct.unpack_from('<QQ', s.h, 0x30)
                    import hashlib
                    assert hashlib.sha256(s.read(ho, hs)).digest() == s.h[8:40]
                    break
    except KeyError as e:
        k = e.args[0] if e.args else '?'
        raise Fail(f'{t.pkg.path.name}: your prod.keys has no {k} (needed for the {t.describe()} program). Dump the keys '
                   'again from your own console with an up-to-date Lockpick_RCM (keys from an older firmware lack the newer '
                   'key generations).')
    except ValueError as e:
        raise Fail(f'{t.pkg.path.name}: {e}')
    except (AssertionError, struct.error):
        raise Fail(f'{t.pkg.path.name}: the {t.describe()} program does not decrypt with this prod.keys and the ticket in '
                   'the package (wrong keys, a wrong or missing ticket, or a damaged dump)')
    finally:
        p.close()


# ---------------------------------------------------------------- steps
def step_exefs(L, upd, keys):
    if L.exefs_ok(): print('  already done'); return
    L.exefs.mkdir(parents=True, exist_ok=True)
    if upd.compressed:
        import extract_exefs_nsz
        extract_exefs_nsz.extract(upd.pkg.path, L.exefs, keys, entry=upd.program)
    else:
        from content_reader import Package
        p = Package(upd.pkg.path, keys)
        try:
            nca = next(n for n in p.ncas() if n.name == upd.program)
            for s in nca.sections:
                if s.h[3] != 2: continue
                data, members = s.pfs()
                if 'main' not in [m[0] for m in members]: continue
                for name, o, n in members: (L.exefs / name).write_bytes(data[o:o + n])
        finally:
            p.close()
    bid = build_id(L.exefs / 'main')
    if bid != BUILD_ID:
        raise Fail(f'main build id {bid} is not the supported {BUILD_ID} (DOOM update v{UPDATE_VERSION})')


def step_patch(L):
    if L.out_ok(): print('  already done'); return
    L.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DM_TARGET='android', PYTHONUTF8='1')
    for m in ('main', 'sdk'):
        print(f'  patching {m} (dm/tools/patch_nso.py, DM_TARGET=android)', flush=True)
        run([sys.executable, ROOT / 'dm' / 'tools' / 'patch_nso.py', L.exefs / m, L.out, m], env=env)
    if not L.out_ok(): raise Fail('patch_nso.py finished without its outputs')


def step_romfs(L, base, upd, keys, keep_nsp):
    if L.romfs_ok(): print('  already done'); return
    srcs = []
    for role, t in (('base', base), ('update', upd)):
        if t.compressed:
            dst = L.nsp / f'{role}.nsp'
            if not dst.is_file():
                print(f'  expanding the {role} program from {t.pkg.kind} ({gb(t.program_size)}) -> {dst}', flush=True)
                import ncz2nsp
                L.nsp.mkdir(parents=True, exist_ok=True)
                bar, done = Progress(), [0]
                def prog(n, total=t.program_size):
                    done[0] += n; bar(f'{gb(done[0])} of {gb(total)}')
                ncz2nsp.convert(t.pkg.path, dst, only={t.program}, progress=prog)
                bar(f'{gb(done[0])} of {gb(t.program_size)}', final=True)
            srcs.append((dst, t.program[:-4] + '.nca'))
        else:
            srcs.append((t.pkg.path, t.program))
    import romfs_doom
    if L.romfs.exists(): shutil.rmtree(L.romfs)
    tmp = L.work / 'romfs-manifest.partial'
    bar = Progress()
    print('  verifying the data (IVFC hashes), then writing the files', flush=True)
    romfs_doom.extract(srcs[0][0], srcs[1][0], keys, L.romfs, tmp, base_nca=srcs[0][1], update_nca=srcs[1][1],
                       verify_progress=lambda d, t: bar(f'verified {gb(d)} of {gb(t)}'),
                       file_progress=lambda i, n, d, t: bar(f'file {i} of {n}, {gb(d)} of {gb(t)}', final=i == n))
    tmp.replace(L.romfs_manifest)
    if not keep_nsp and L.nsp.is_dir():
        import gc; gc.collect()                  # let the readers close their files (Windows cannot delete open files)
        shutil.rmtree(L.nsp, ignore_errors=True)
        print('  removed the temporary NSPs (--keep-nsp keeps them)' if not L.nsp.exists() else
              f'  could not remove the temporary NSPs in {L.nsp}; delete them by hand')


def spirv_tool(name):
    env = os.environ.get('SPIRV_OPT' if name == 'spirv-opt' else 'SPIRV_VAL')
    if env: return env
    found = shutil.which(name)
    if found: return found
    for sdk in (os.environ.get('ANDROID_NDK_HOME'), os.environ.get('ANDROID_NDK_ROOT')):
        if sdk:
            for c in Path(sdk).glob(f'shader-tools/*/{name}*'): return str(c)
    home = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if home:
        for c in sorted(Path(home).glob(f'ndk/*/shader-tools/*/{name}*'), reverse=True): return str(c)
    return None


# ---------------------------------------------------------------- .NET
DOTNET_DIR = RUNTIME / 'dotnet'
DOTNET_EXE = 'dotnet.exe' if WIN else 'dotnet'


def sdk_major(d):
    """Highest .NET SDK major version of a dotnet executable (0 when it does not run)."""
    try:
        out = subprocess.run([str(d), '--list-sdks'], capture_output=True, text=True, timeout=60,
                             env=dotnet_env(d)).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    return max([int(l.split('.')[0]) for l in out.splitlines() if l.split('.')[0].isdigit()] + [0])


def find_dotnet():
    """A dotnet with a .NET 10+ SDK (the translator targets net10.0): DOTNET, tools/.runtime/dotnet, ~/.dotnet, PATH."""
    for c in (os.environ.get('DOTNET'), DOTNET_DIR / DOTNET_EXE, Path.home() / '.dotnet' / DOTNET_EXE, shutil.which('dotnet')):
        if c and Path(c).is_file() and sdk_major(c) >= 10: return str(c)
    return None


def dotnet_env(d, base=None):
    env = dict(base or os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')
    if d:
        env['DOTNET'] = str(d)
        env['DOTNET_ROOT'] = str(Path(d).resolve().parent)   # the translator's apphost finds its runtime there
        if not WIN and Path(d).resolve().parent == DOTNET_DIR.resolve() and 'DOTNET_SYSTEM_GLOBALIZATION_INVARIANT' not in env:
            env['DOTNET_SYSTEM_GLOBALIZATION_INVARIANT'] = '1'   # a private SDK must not depend on the distro's libicu
    return env


def download(url, dst):
    """Download with Python; falls back to curl (e.g. a macOS Python without its certificates installed)."""
    import urllib.request
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(dst, 'wb') as f:
            shutil.copyfileobj(r, f)
        return
    except Exception as e:
        err = e
    if shutil.which('curl') and subprocess.run(['curl', '-fsSL', '-o', str(dst), url]).returncode == 0: return
    raise Fail(f'download failed: {url} ({err}); check the internet connection and run setup again')


def install_dotnet(yes):
    print(f'  The shader step needs the .NET 10 SDK. Setup can install a private copy from Microsoft (dotnet-install script,\n'
          f'  ~300 MB download, ~800 MB on disk) into {DOTNET_DIR}. Nothing is installed system-wide, no admin rights needed.')
    if not ask('  Download and install the .NET 10 SDK there now?', yes):
        raise Fail('the .NET 10 SDK is needed to translate the shaders. Run setup again and answer Y, install it yourself '
                   'from https://dotnet.microsoft.com/download/dotnet/10.0 (SDK), or set DOTNET=<path to dotnet>.')
    script = RUNTIME / ('dotnet-install.ps1' if WIN else 'dotnet-install.sh')
    download('https://dot.net/v1/' + script.name, script)
    if WIN:
        cmd = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script), '-Channel', '10.0',
               '-InstallDir', str(DOTNET_DIR), '-NoPath']
    else:
        cmd = ['bash', str(script), '--channel', '10.0', '--install-dir', str(DOTNET_DIR), '--no-path']
    print('  installing (a few minutes)...', flush=True)
    if subprocess.run(cmd).returncode != 0 or sdk_major(DOTNET_DIR / DOTNET_EXE) < 10:
        raise Fail(f'the .NET SDK install into {DOTNET_DIR} failed (see above). Run setup again, or install the .NET 10 SDK '
                   'from https://dotnet.microsoft.com/download/dotnet/10.0')
    return str(DOTNET_DIR / DOTNET_EXE)


def step_shaders(L, dn):
    if L.shaders_ok(): print(f'  already done ({L.shader_count()} shader bundles)'); return
    env = dotnet_env(dn, dict(os.environ, PYTHONUTF8='1'))
    opt, val = spirv_tool('spirv-opt'), spirv_tool('spirv-val')
    if opt: env['SPIRV_OPT'] = opt
    if val: env['SPIRV_VAL'] = val
    if not val: print('  (SPIRV-Tools not found: the optional spirv-val check of each shader is skipped; same output)')
    env.setdefault('J', str(max(2, min(16, os.cpu_count() or 8))))
    if not (L.jobs / 'jobs.tsv').is_file():
        print('  reading the shaders from the RomFS (tools/doom_shader_jobs.py)', flush=True)
        run([sys.executable, TOOLS / 'doom_shader_jobs.py', L.romfs, L.jobs], env=env)
    exe = L.xlate / 'bin' / ('dmsh_xlate.exe' if WIN else 'dmsh_xlate')
    if not exe.is_file():
        print('  building the shader translator (tools/build_dmsh_xlate_doom.py; needs internet the first time)', flush=True)
        run([sys.executable, TOOLS / 'build_dmsh_xlate_doom.py', L.xlate], env=env)
    print('  translating to SPIR-V bundles (dm/gfx/tools/dmsh_vk_doom.py)', flush=True)
    L.shaders.mkdir(parents=True, exist_ok=True)
    run([sys.executable, ROOT / 'dm' / 'gfx' / 'tools' / 'dmsh_vk_doom.py', L.jobs, L.shaders], env=dict(env, XLATE=str(exe)))
    print(f'  {L.shader_count()} shader bundles')


def run(cmd, env=None):
    cmd = [str(c) for c in cmd]
    r = subprocess.run(cmd, env=env)
    if r.returncode: raise Fail(f'failed ({r.returncode}): {" ".join(cmd)}')


# ---------------------------------------------------------------- phone
def adb_exe():
    c = os.environ.get('ADB') or shutil.which('adb')
    if c: return c
    home = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if home:
        p = Path(home) / 'platform-tools' / ('adb.exe' if WIN else 'adb')
        if p.is_file(): return str(p)
    return None


def adb_devices(adb):
    """[(serial, state)] of the connected devices; [] when adb does not answer."""
    try:
        out = subprocess.run([adb, 'devices'], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [tuple(l.split('\t')[:2]) for l in out.splitlines()[1:] if '\t' in l]


def pick_device(adb, serial):
    if serial: return serial
    devs = adb_devices(adb)
    ready = [s for s, st in devs if st == 'device']
    if len(ready) == 1: return ready[0]
    if not devs: raise Fail('no phone found over adb: connect it with USB debugging enabled (or copy the folder by hand)')
    if not ready: raise Fail('the phone is not authorized: unlock it and accept the "Allow USB debugging" prompt, then retry')
    raise Fail('several phones are connected; name one: setup push <serial> (' + ', '.join(ready) + ')')


def push(L, serial):
    if not L.complete():
        raise Fail(f'{L.phone} is not complete yet; run the preparation first (setup check shows what is missing)')
    adb = adb_exe()
    if not adb: raise Fail('adb not found (Android platform-tools); copy the folder by hand instead, see docs/INSTALL.md')
    serial = pick_device(adb, serial)
    adb = [adb, '-s', serial]
    if subprocess.run(adb + ['get-state'], capture_output=True, text=True).stdout.strip() != 'device':
        raise Fail(f'adb device {serial} not reachable')
    for local in (L.out, L.shaders, L.romfs):
        rel = local.relative_to(L.phone).as_posix()
        remote = f'{PHONE_DIR}/{rel}'
        listing = subprocess.run(adb + ['shell', f"cd '{remote}' 2>/dev/null && find . -type f -exec stat -c '%s %n' {{}} +"],
                                 capture_output=True, text=True).stdout
        have = {}
        for line in listing.splitlines():
            size, _, name = line.strip().partition(' ')
            if size.isdigit(): have[name[2:] if name.startswith('./') else name] = int(size)
        todo = [p for p in sorted(local.rglob('*')) if p.is_file() and have.get(p.relative_to(local).as_posix()) != p.stat().st_size]
        say(f'push {rel}: {len(todo)} of {sum(1 for p in local.rglob("*") if p.is_file())} files to copy')
        dirs = set()
        for p in todo:
            rp = f'{remote}/{p.relative_to(local).as_posix()}'
            d = rp.rsplit('/', 1)[0]
            if d not in dirs:
                subprocess.run(adb + ['shell', f"mkdir -p '{d}'"], check=True); dirs.add(d)
            print(f'  {p.relative_to(L.phone).as_posix()} ({p.stat().st_size / 2**20:.1f} MB)', flush=True)
            if subprocess.run(adb + ['push', str(p), rp], capture_output=True).returncode:
                raise Fail(f'adb push failed: {p} (run again to resume)')
    if APP_ID in subprocess.run(adb + ['shell', 'pm', 'list', 'packages', APP_ID], capture_output=True, text=True).stdout:
        subprocess.run(adb + ['shell', 'appops', 'set', APP_ID, 'MANAGE_EXTERNAL_STORAGE', 'allow'])
        say(f'{APP_ID}: All files access granted')
    else:
        say(f'the app is not installed yet: install the APK ({APK_URL}), then allow "All files access" when it asks')


def next_steps(L, offer_push, yes):
    size = folder_bytes(L.phone)
    print(f'\n== Done. The game folder is ready: {L.phone} ({gb(size)})')
    adb = adb_exe() if offer_push else None
    if adb:
        ready = [s for s, st in adb_devices(adb) if st == 'device']
        if len(ready) == 1:
            model = subprocess.run([adb, '-s', ready[0], 'shell', 'getprop', 'ro.product.model'], capture_output=True,
                                   text=True).stdout.strip()
            if ask(f'A phone is connected over USB ({model or ready[0]}). Copy the folder to it now ({gb(size)}, '
                   'can take 10-30 min)?', yes):
                push(L, ready[0])
                print('\nNext: install the APK if you have not yet, start DOOM Mobile, allow "All files access". Have fun!')
                return
    print(f'''
What to do next:
  1. Copy the whole "DOOM Mobile" folder into the phone's Download folder (internal storage), so that the phone has
       Download/DOOM Mobile/romfs   and   Download/DOOM Mobile/dm
     (USB cable + file manager is fine; the phone needs ~{gb(size + GB)} free).
  2. Install the app on the phone: {APK_URL}
  3. Start DOOM Mobile and allow "All files access" when it asks.
  Step-by-step guide and troubleshooting: docs/INSTALL.md (Russian: docs/INSTALL.ru.md)''')


# ---------------------------------------------------------------- check
def missing_packages():
    import importlib.util as iu
    return [m for m, _ in PY_PACKAGES if iu.find_spec(m) is None]


def check(L, files):
    nxt = []
    def item(good, text, fix=None):
        print(f"  {'OK ' if good else '-- '} {text}")
        if not good and fix: nxt.append(fix)
    say('1. Python')
    item(sys.version_info >= (3, 10), f'Python {sys.version.split()[0]}: {sys.executable}', 'Python 3.10 or newer is needed (setup.bat / setup.sh offer one)')
    import importlib.util as iu
    for mod, why in PY_PACKAGES:
        item(iu.find_spec(mod) is not None, f'{mod} ({why})', f'run setup.bat / ./setup.sh (it installs the packages), or: "{sys.executable}" -m pip install cryptography zstandard lz4 "capstone<6"')
    say('2. Tools')
    dn = find_dotnet()
    item(bool(dn) or L.shaders_ok(), f'.NET 10 SDK: {dn or "not found (setup offers to install a private copy into tools/.runtime/dotnet)"}')
    for t in ('spirv-opt', 'spirv-val'):
        print(f"  {'OK ' if spirv_tool(t) else '.. '} {t}: {spirv_tool(t) or 'not found (optional: only checks the translated shaders)'}")
    adb = adb_exe()
    print(f"  {'OK ' if adb else '.. '} adb: {adb or 'not found (optional: only for copying to the phone over USB debugging)'}")
    say('3. Game files')
    pkgs, keys, where = gather(files)
    if not files: print('  looked in: ' + ', '.join(str(w) for w in where))
    ok_files = L.complete()
    if missing_packages():
        print('  .. (the Python packages above are needed to identify the files)')
    elif not pkgs and not keys:
        item(ok_files, f'no game files found; put the base game, update v{UPDATE_VERSION} and prod.keys into {DROP}',
             f'put the game files and prod.keys into {DROP}, then run setup again')
    else:
        try:
            base, upd, kf = resolve(pkgs, keys)
            item(True, f'base game: {base.pkg.path.name}; update v{UPDATE_VERSION}: {upd.pkg.path.name}; keys: {kf.name}')
        except Fail as e:
            item(ok_files, str(e), 'fix the game files (see the message above), then run setup again')
    say(f'4. Work folder {L.work}')
    free = free_bytes(L.work)
    need = 0 if L.romfs_ok() else ROMFS_BYTES + SMALL_BYTES
    item(free >= need, f'{gb(free)} free ({gb(need)}+ needed for the remaining steps; up to ~50 GB with NSZ / XCZ input)',
         'free some disk space, or use --work <folder on another disk>')
    item(L.exefs_ok(), f'ExeFS of update v{UPDATE_VERSION} (build id {BUILD_ID[:16]}...)', RUN_SETUP)
    item(L.out_ok(), 'dm/out: patched main / sdk', RUN_SETUP)
    item(L.romfs_ok(), 'romfs: extracted and verified', RUN_SETUP)
    item(L.shaders_ok(), f'dm/shaders: {L.shader_count()} SPIR-V bundles', RUN_SETUP)
    if L.nsp.is_dir(): print(f'  ..  temporary NSPs in {L.nsp} (removed after the RomFS step)')
    if nxt:
        say(f'Next: {nxt[0]}')
        return 1
    say(f'Ready: copy {L.phone} to Download/DOOM Mobile on the phone ({PHONE_DIR}), or: setup push')
    return 0


# ---------------------------------------------------------------- prepare
def prepare(a, L):
    steps = set(a.steps.split(','))
    shaders_needed = 'shaders' in steps and not a.skip_shaders and not L.shaders_ok()
    miss = missing_packages()
    if sys.version_info < (3, 10) or miss:
        raise Fail(f'Python 3.10+ with {", ".join(miss) or "the packages"} is needed; start setup through setup.bat / '
                   f'./setup.sh (they install them), or: "{sys.executable}" -m pip install cryptography zstandard lz4 "capstone<6"')

    t0 = stage(1)
    pkgs, keys, where = gather(a.files)
    if not pkgs and not keys and not a.files:
        raise Fail(f'no game files found. Put your DOOM base game, the update v{UPDATE_VERSION} (NSP / NSZ / XCI / XCZ, any '
                   f'file names) and your prod.keys into\n  {DROP}\nand run setup again (or drop the files onto setup.bat). '
                   f'Also searched: {Path.cwd()}')
    try:
        base, upd, kf = resolve(pkgs, keys)
    except Fail as e:
        if a.files: raise
        raise Fail(f'{e}\n  (searched: ' + ', '.join(str(w) for w in where) + ')')
    print(f'  base game: {base.pkg.path.name} ({base.pkg.kind}), DOOM v{base.version}')
    print(f'  update:    {upd.pkg.path.name} ({upd.pkg.kind}), v{upd.version} -- the supported version')
    print(f'  checked in {took(t0)}')

    t0 = stage(2)
    print(f'  Python {sys.version.split()[0]} with its packages: OK')
    need = SMALL_BYTES
    if 'romfs' in steps and not L.romfs_ok():
        need += max(0, ROMFS_BYTES - folder_bytes(L.romfs))   # a partial RomFS is deleted first
        for role, t in (('base', base), ('update', upd)):
            if t.compressed and not (L.nsp / f'{role}.nsp').is_file(): need += t.program_size
    free = free_bytes(L.work)
    print(f'  output: {L.phone}')
    print(f'  disk space: {gb(free)} free, about {gb(need)} needed while preparing (~23 GB stays at the end)')
    if free < need:
        raise Fail(f'not enough free space on the disk of {L.work}: {gb(free)} free, ~{gb(need)} needed. Free some space, '
                   'or put the work folder on another disk: setup --work <folder> (keep the game files where they are).')
    dn = find_dotnet()
    if dn: print(f'  .NET 10 SDK: {dn}')
    elif shaders_needed and not a.dry_run: dn = install_dotnet(a.yes)
    elif shaders_needed: print('  .NET 10 SDK: not found; setup will offer to install it (tools/.runtime/dotnet)')
    else: print('  .NET 10 SDK: not needed (shaders done or skipped)')
    print(f"  SPIRV-Tools: {spirv_tool('spirv-val') or 'not found (optional)'}")
    plan = [('ExeFS', L.exefs_ok() or 'exefs' not in steps), ('patch', L.out_ok() or 'patch' not in steps),
            ('RomFS' + (' (compressed input is expanded first)' if base.compressed or upd.compressed else ''),
             L.romfs_ok() or 'romfs' not in steps), ('shaders', not shaders_needed)]
    print('  plan: ' + ', '.join(f"{n} {'(done/skipped)' if d else ''}".strip() for n, d in plan))
    if a.dry_run:
        say('dry run: inputs, tools and space checked; nothing was written'); return 0

    t0 = stage(3)
    if 'exefs' in steps: step_exefs(L, upd, kf); print(f'  done in {took(t0)}')
    else: print('  skipped (--steps)')
    t0 = stage(4)
    if 'patch' in steps: step_patch(L); print(f'  done in {took(t0)}')
    else: print('  skipped (--steps)')
    t0 = stage(5)
    if 'romfs' in steps: step_romfs(L, base, upd, kf, a.keep_nsp); print(f'  done in {took(t0)}')
    else: print('  skipped (--steps)')
    t0 = stage(6)
    if shaders_needed:
        step_shaders(L, dn or find_dotnet() or install_dotnet(a.yes)); print(f'  done in {took(t0)}')
    else: print('  already done' if L.shaders_ok() else '  skipped')
    if not L.complete():
        say(f'finished the requested steps; the folder is not complete yet: {L.phone}'); return 0
    if a.push:
        push(L, None if a.push == 'auto' else a.push); return 0
    next_steps(L, False, a.yes)
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    default_work = os.environ.get('DM_DOOM_WORK') or ROOT / 'work'
    if argv and argv[0] in ('-h', '--help', 'help'):
        print(__doc__); return 0
    if argv and argv[0] in ('check', 'push'):
        ap = argparse.ArgumentParser(prog=f'setup {argv[0]}')
        if argv[0] == 'push': ap.add_argument('serial', nargs='?')
        else: ap.add_argument('files', nargs='*')
        ap.add_argument('--work', default=default_work)
        ap.add_argument('--yes', '-y', action='store_true')
        a = ap.parse_args(argv[1:])
        L = Layout(a.work)
        if argv[0] == 'check': return check(L, a.files)
        push(L, a.serial); return 0
    ap = argparse.ArgumentParser(prog='setup', description='Prepare DOOM Mobile game files from your own dumps.')
    ap.add_argument('files', nargs='*', help='base game, update v262144 and prod.keys in any order (or folders to search); '
                                             'none: put-game-files-here/ and the current folder')
    ap.add_argument('--work', default=default_work)
    ap.add_argument('--push', metavar='SERIAL', nargs='?', const='auto')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--yes', '-y', action='store_true')
    ap.add_argument('--keep-nsp', action='store_true')
    ap.add_argument('--skip-shaders', action='store_true')
    ap.add_argument('--steps', default='exefs,patch,romfs,shaders', help='comma list of steps to run (default: all)')
    a = ap.parse_args(argv)
    L = Layout(a.work)
    print('DOOM Mobile setup: prepares the game folder for the phone from your own DOOM (Switch) dumps.')
    return prepare(a, L)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print(f'\nSTOP: {e}', file=sys.stderr); sys.exit(2)
    except KeyboardInterrupt:
        print('\ninterrupted; run setup again to continue where it stopped', file=sys.stderr); sys.exit(130)
    except Exception as e:                      # never end with a bare traceback: say what to do with it
        import traceback; traceback.print_exc()
        why = f'{e.strerror}: {e.filename}' if isinstance(e, OSError) and e.strerror and e.filename else f'{type(e).__name__}: {e}'
        print(f'\nSTOP: unexpected error ({why}). Run setup again (finished steps are kept); if it happens again, open an '
              'issue at https://github.com/Desm0nt/doom-port/issues with the text above.', file=sys.stderr); sys.exit(1)
