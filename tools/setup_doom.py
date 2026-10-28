#!/usr/bin/env python3
"""DOOM Mobile: one command from your own DOOM dumps to the folder the app reads.

  setup_doom.py check [--work DIR]
      Report what is installed, what has been prepared and the next step. Changes nothing.
  setup_doom.py <base .nsp/.nsz/.xci> <update .nsp/.nsz> <prod.keys> [--work DIR] [--push SERIAL] [--dry-run]
      [--keep-nsp] [--skip-shaders] [--steps exefs,patch,romfs,shaders]
      Check that the packages are DOOM (base 0100416004C00000, update 0100416004C00800 v262144, main build id
      2847991952b3d7ab...), then: ExeFS -> offline patch (dm/tools/patch_nso.py, DM_TARGET=android) -> RomFS -> shader
      translation -> the phone folder <work>/DOOM Mobile/{romfs, dm/out, dm/shaders}. Every step is skipped when its
      output is already complete; delete an output to redo its step. --dry-run identifies the inputs and prints the plan.
  setup_doom.py push SERIAL [--work DIR]
      Copy <work>/DOOM Mobile to /sdcard/Download/DOOM Mobile on the phone over adb (only missing / size-mismatched files,
      so an interrupted push resumes) and grant org.doom.port "All files access".

Packages and keys are only read, never copied, uploaded or written anywhere; the keys stay in memory. NSZ packages are
expanded to plain NSPs in <work>/nsp (needed for the RomFS) and those are deleted afterwards unless --keep-nsp.
Default work directory: $DM_DOOM_WORK or <repository>/work. Needs ~70 GB free with NSZ input (~25 GB at the end).
"""
import argparse, json, os, shutil, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
sys.path.insert(0, str(TOOLS))

BASE_TID, UPDATE_TID, UPDATE_VERSION = 0x0100416004C00000, 0x0100416004C00800, 262144
BUILD_ID = '2847991952b3d7abd228624fcd3b0779'
APP_ID, PHONE_DIR = 'org.doom.port', '/sdcard/Download/DOOM Mobile'
MODULE_FILES = [f'{m}.{e}' for m in ('main', 'sdk') for e in ('patched.bin', 'fixups.tsv', 'gates.tsv')]
WIN = os.name == 'nt'


class Fail(Exception):
    pass


def say(msg): print(f'== {msg}', flush=True)


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
        return all((self.out / f).is_file() for f in MODULE_FILES)

    def romfs_ok(self):
        return self.romfs_manifest.is_file() and self.romfs.is_dir()

    def shader_count(self):
        return len(list(self.shaders.glob('*.xgs'))) if self.shaders.is_dir() else 0

    def shaders_ok(self):
        return (self.shaders / '_vk_build.json').is_file() and self.shader_count() > 0


def build_id(nso):
    try:
        with open(nso, 'rb') as f:
            h = f.read(0x60)
    except OSError:
        return None
    return h[0x40:0x50].hex() if h[:4] == b'NSO0' else None


# ---------------------------------------------------------------- inputs
def identify(path, keys):
    """Title id, version and type of every content meta (cnmt) in a package; keys stay in memory."""
    from content_reader import Package
    p = Package(path, keys)
    metas = []
    for nca in p.ncas():
        if nca.type != 1:   # meta
            continue
        data, ents = nca.sections[0].pfs()
        for name, o, s in ents:
            if name.endswith('.cnmt'):
                tid, ver = struct.unpack_from('<QI', data, o)
                metas.append({'title': tid, 'version': ver, 'type': data[o + 12], 'nca': nca.name})
    p.f.close()
    return metas


def check_inputs(base, update, keys):
    for x in (base, update, keys):
        if not Path(x).is_file(): raise Fail(f'not found: {x}')
    for x in (base, update):
        if Path(x).suffix.lower() in ('.xcz',): raise Fail(f'{x}: XCZ is not supported; decompress it to XCI first (nsz -D)')
    try:
        bm, um = identify(base, keys), identify(update, keys)
    except KeyError as e:
        raise Fail(f'prod.keys has no {e.args[0]}: dump the keys of your own console again (e.g. with Lockpick_RCM)')
    except (ValueError, AssertionError) as e:
        raise Fail(f'cannot read the package ({e or "format / key mismatch"}); is it a valid NSP / NSZ / XCI with matching keys?')
    if not any(m['title'] == BASE_TID and m['type'] == 0x80 for m in bm):
        found = ', '.join(f"{m['title']:016X} v{m['version']}" for m in bm) or 'no content meta'
        raise Fail(f'{base}: not the DOOM base game {BASE_TID:016X} (found: {found})')
    upd = [m for m in um if m['title'] == UPDATE_TID and m['type'] == 0x81]
    if not upd:
        found = ', '.join(f"{m['title']:016X} v{m['version']}" for m in um) or 'no content meta'
        raise Fail(f'{update}: not the DOOM update {UPDATE_TID:016X} (found: {found})')
    if upd[0]['version'] != UPDATE_VERSION:
        raise Fail(f"{update}: DOOM update v{upd[0]['version']}; only v{UPDATE_VERSION} is supported "
                   '(the patches and addresses are made for that build)')
    return bm, upd[0]


def is_nsz(path):
    return Path(path).suffix.lower() == '.nsz'


# ---------------------------------------------------------------- steps
def step_exefs(L, update, keys):
    if L.exefs_ok(): say('ExeFS: present'); return
    say('ExeFS: extracting main / sdk from the update')
    L.exefs.mkdir(parents=True, exist_ok=True)
    if is_nsz(update):
        import extract_exefs_nsz
        extract_exefs_nsz.extract(update, L.exefs, keys)
    else:
        from content_reader import Package
        p = Package(update, keys)
        nca = next(n for n in p.ncas() if n.type == 0)
        for s in nca.sections:
            if s.h[3] != 2: continue
            data, members = s.pfs()
            if 'main' not in [m[0] for m in members]: continue
            for name, o, n in members: (L.exefs / name).write_bytes(data[o:o + n])
    bid = build_id(L.exefs / 'main')
    if bid != BUILD_ID:
        raise Fail(f'main build id {bid} is not the supported {BUILD_ID} (DOOM update v{UPDATE_VERSION})')


def step_patch(L):
    if L.out_ok(): say('dm/out: patched modules present'); return
    L.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DM_TARGET='android', PYTHONUTF8='1')
    for m in ('main', 'sdk'):
        say(f'patch: {m} (dm/tools/patch_nso.py, DM_TARGET=android)')
        run([sys.executable, ROOT / 'dm' / 'tools' / 'patch_nso.py', L.exefs / m, L.out, m], env=env)
    if not L.out_ok(): raise Fail('patch_nso.py finished without its outputs')


def step_romfs(L, base, update, keys, keep_nsp):
    if L.romfs_ok(): say('RomFS: present'); return
    pkgs = []
    for name, src in (('base', base), ('update', update)):
        if is_nsz(src):
            dst = L.nsp / f'{name}.nsp'
            if not dst.is_file():
                say(f'NSZ -> NSP: {Path(src).name} -> {dst}')
                import ncz2nsp
                L.nsp.mkdir(parents=True, exist_ok=True)
                ncz2nsp.convert(src, dst)
            src = dst
        pkgs.append(src)
    say('RomFS: verifying (IVFC) and extracting base + update (~22 GB)')
    import romfs_doom
    if L.romfs.exists(): shutil.rmtree(L.romfs)
    tmp = L.work / 'romfs-manifest.partial'
    romfs_doom.extract(pkgs[0], pkgs[1], keys, L.romfs, tmp)
    tmp.replace(L.romfs_manifest)
    if not keep_nsp and L.nsp.is_dir():
        shutil.rmtree(L.nsp); say('removed the intermediate NSPs (--keep-nsp keeps them)')


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


def dotnet():
    return os.environ.get('DOTNET') or shutil.which('dotnet')


def dotnet_sdk_ok():
    """The translator targets net10.0 (dmsh_xlate.csproj): a .NET 10+ SDK is required."""
    d = dotnet()
    if not d: return False
    try:
        out = subprocess.run([d, '--list-sdks'], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return any(l.split('.')[0].isdigit() and int(l.split('.')[0]) >= 10 for l in out.splitlines())


def step_shaders(L):
    if L.shaders_ok(): say(f'dm/shaders: {L.shader_count()} bundles present'); return
    for tool in ('spirv-opt', 'spirv-val'):
        if not spirv_tool(tool): raise Fail(f'{tool} not found (SPIRV-Tools; the Android NDK ships it in shader-tools/)')
    if not dotnet_sdk_ok(): raise Fail('a .NET 10 SDK is required to build the shader translator (https://dot.net; or set DOTNET=<dotnet executable>)')
    env = dict(os.environ, PYTHONUTF8='1', SPIRV_OPT=spirv_tool('spirv-opt'), SPIRV_VAL=spirv_tool('spirv-val'), DOTNET=dotnet())
    if not (L.jobs / 'jobs.tsv').is_file():
        say('shaders: jobs from the RomFS (tools/doom_shader_jobs.py)')
        run([sys.executable, TOOLS / 'doom_shader_jobs.py', L.romfs, L.jobs], env=env)
    exe = L.xlate / 'bin' / ('dmsh_xlate.exe' if WIN else 'dmsh_xlate')
    if not exe.is_file():
        say('shaders: building the translator (tools/build_dmsh_xlate_doom.py)')
        run([sys.executable, TOOLS / 'build_dmsh_xlate_doom.py', L.xlate], env=env)
    say('shaders: translating to SPIR-V bundles (dm/gfx/tools/dmsh_vk_doom.py)')
    L.shaders.mkdir(parents=True, exist_ok=True)
    run([sys.executable, ROOT / 'dm' / 'gfx' / 'tools' / 'dmsh_vk_doom.py', L.jobs, L.shaders], env=dict(env, XLATE=str(exe)))
    say(f'dm/shaders: {L.shader_count()} bundles')


def run(cmd, env=None):
    cmd = [str(c) for c in cmd]
    r = subprocess.run(cmd, env=env)
    if r.returncode: raise Fail(f'failed ({r.returncode}): {" ".join(cmd)}')


# ---------------------------------------------------------------- push
def adb_cmd(serial):
    return [os.environ.get('ADB') or shutil.which('adb') or 'adb', '-s', serial]


def push(L, serial):
    if not (L.out_ok() and L.romfs_ok() and L.shaders_ok()):
        raise Fail(f'{L.phone} is not complete yet; run the preparation first (setup_doom.py check)')
    adb = adb_cmd(serial)
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
        say(f'{APP_ID} is not installed yet: install the APK, then allow "All files access" when the app asks')


# ---------------------------------------------------------------- check
def check(L):
    nxt = []
    def item(good, text, fix=None):
        print(f"  {'OK ' if good else '-- '} {text}")
        if not good and fix: nxt.append(fix)
    say('1. Python packages')
    import importlib.util as iu
    for mod, why in (('cryptography', 'decrypting your packages'), ('zstandard', 'NSZ input'), ('lz4', 'NSO modules'),
                     ('capstone', 'the offline patcher')):
        item(iu.find_spec(mod) is not None, f'{mod} ({why})', f'{sys.executable} -m pip install {mod}')
    say('2. Tools')
    item(dotnet_sdk_ok(), f'.NET 10 SDK: {dotnet() or "dotnet missing"} (builds the shader translator)', 'install the .NET 10 SDK (https://dot.net), or set DOTNET=<dotnet executable>')
    for t in ('spirv-opt', 'spirv-val'):
        item(bool(spirv_tool(t)), f'{t}: {spirv_tool(t) or "missing"}', 'install SPIRV-Tools or the Android NDK (shader-tools/), or set SPIRV_OPT / SPIRV_VAL')
    adb = os.environ.get('ADB') or shutil.which('adb')
    print(f"  {'OK ' if adb else '.. '} adb: {adb or 'not found (only needed for push)'}")
    say(f'3. Work directory {L.work}')
    probe = L.work
    while not probe.exists() and probe != probe.parent: probe = probe.parent
    free = shutil.disk_usage(probe).free / 2**30
    need = 0 if L.romfs_ok() else 70
    item(free >= need, f'{free:.0f} GB free ({need} GB needed for the remaining steps with NSZ input)', 'free disk space or use --work on another disk')
    prep = '<base .nsp/.nsz/.xci> <update .nsp/.nsz> <prod.keys>'
    item(L.exefs_ok(), f'ExeFS of update v{UPDATE_VERSION} (build id {BUILD_ID[:16]}...)', f'./setup.sh {prep}')
    item(L.out_ok(), 'dm/out: patched main / sdk', f'./setup.sh {prep}')
    item(L.romfs_ok(), 'romfs: extracted and verified', f'./setup.sh {prep}')
    item(L.shaders_ok(), f'dm/shaders: {L.shader_count()} SPIR-V bundles', f'./setup.sh {prep}')
    if L.nsp.is_dir(): print(f'  ..  intermediate NSPs in {L.nsp} (removed after the RomFS step)')
    if nxt:
        say(f'Next: {nxt[0]}')
        return 1
    say(f'Ready: copy {L.phone} to {PHONE_DIR} on the phone, or ./setup.sh push <adb serial>')
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    default_work = os.environ.get('DM_DOOM_WORK') or ROOT / 'work'
    if not argv or argv[0] in ('-h', '--help'):
        print(__doc__); return 0
    if argv[0] in ('check', 'push'):
        ap = argparse.ArgumentParser(prog=f'setup_doom.py {argv[0]}')
        if argv[0] == 'push': ap.add_argument('serial')
        ap.add_argument('--work', default=default_work)
        a = ap.parse_args(argv[1:])
        L = Layout(a.work)
        if argv[0] == 'check': return check(L)
        push(L, a.serial); return 0
    ap = argparse.ArgumentParser(prog='setup_doom.py', description='Prepare DOOM Mobile game files from your own dumps.')
    ap.add_argument('base'); ap.add_argument('update'); ap.add_argument('keys')
    ap.add_argument('--work', default=default_work)
    ap.add_argument('--push', metavar='SERIAL')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--keep-nsp', action='store_true')
    ap.add_argument('--skip-shaders', action='store_true')
    ap.add_argument('--steps', default='exefs,patch,romfs,shaders', help='comma list of steps to run (default: all)')
    a = ap.parse_args(argv)
    L = Layout(a.work)
    bm, um = check_inputs(a.base, a.update, a.keys)
    say(f"base: DOOM {BASE_TID:016X} v{next(m['version'] for m in bm if m['title'] == BASE_TID)} ({Path(a.base).name})")
    say(f"update: DOOM {UPDATE_TID:016X} v{um['version']} ({Path(a.update).name}) -- supported")
    plan = [('ExeFS + build id check', L.exefs_ok()), ('patch main / sdk -> dm/out', L.out_ok()),
            ('RomFS -> romfs' + (' (NSZ -> NSP first)' if is_nsz(a.base) or is_nsz(a.update) else ''), L.romfs_ok()),
            ('shaders -> dm/shaders', L.shaders_ok() or a.skip_shaders)]
    for name, done in plan: print(f"  {'skip' if done else 'run '}  {name}")
    print(f'  output: {L.phone}')
    if a.push: print(f'  then push to {a.push}:{PHONE_DIR}')
    if a.dry_run: return 0
    steps = set(a.steps.split(','))
    if 'exefs' in steps: step_exefs(L, a.update, a.keys)
    if 'patch' in steps: step_patch(L)
    if 'romfs' in steps: step_romfs(L, a.base, a.update, a.keys, a.keep_nsp)
    if 'shaders' in steps and not a.skip_shaders: step_shaders(L)
    say(f'done: {L.phone}')
    if a.push: push(L, a.push)
    else: say(f'copy it to {PHONE_DIR} on the phone, or: ./setup.sh push <adb serial>')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print(f'\nSTOP: {e}', file=sys.stderr); sys.exit(2)
    except KeyboardInterrupt:
        print('\ninterrupted; run the same command again to continue', file=sys.stderr); sys.exit(130)
