"""Build the DOOM variant of d2r-mobile's shader translator (dmsh_xlate) without touching the D2R one.

Source: dm/gfx/tools/dmsh_xlate_doom/{Program.cs, dmsh_xlate.csproj} (the DOOM translator: NVN c2 handle layout, no NV
geometry passthrough / viewport mask, stage linking from the program pairs, FP64 lowering; see its header). The D2R source
dm/gfx/tools/dmsh_xlate stays as it is, so the D2R chain version does not move. Built with dotnet against
dm/third_party/ryujinx (unmodified Ryujinx 1.3.2).
(Before 2026-10-27 this script patched a copy of the D2R Program.cs with two text substitutions; the DOOM source now carries
them and more, so there is nothing to substitute.)
Usage: build_dmsh_xlate_doom.py [out dir] (default work/build/dmsh_xlate_doom); DOTNET=<dotnet executable> (default: dotnet on PATH)
Result: <out>/bin/dmsh_xlate.exe; <out>/bin/source.sha = the embedded source sha (dmsh_xlate --source-sha), checked against the tree.
"""
import hashlib, os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'dm' / 'gfx' / 'tools' / 'dmsh_xlate_doom'
RYU = ROOT / 'dm' / 'third_party' / 'ryujinx'
FILES = ('Program.cs', 'dmsh_xlate.csproj')


def source_sha():   # same convention as dmsh_xlate --source-sha: the embedded Program.cs and csproj concatenated in that order
    return hashlib.sha256(b''.join((SRC / f).read_bytes() for f in FILES)).hexdigest()[:16]


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'work' / 'build' / 'dmsh_xlate_doom'
    dotnet = os.environ.get('DOTNET', 'dotnet')
    src = out / 'src'
    if src.exists(): shutil.rmtree(src)
    src.mkdir(parents=True)
    for f in FILES: shutil.copyfile(SRC / f, src / f)   # build a copy: obj/ and bin/ stay out of the source tree
    r = subprocess.run([dotnet, 'build', str(src / 'dmsh_xlate.csproj'), '-c', 'Release', f'-p:RyuSrc={RYU.as_posix()}',
                        '-o', str(out / 'bin'), '--artifacts-path', str(out / 'obj')], capture_output=True, text=True, errors='replace')
    print(r.stdout[-1500:], r.stderr[-1500:])
    if r.returncode: sys.exit(r.returncode)
    exe = out / 'bin' / 'dmsh_xlate.exe'
    got = subprocess.run([str(exe), '--source-sha'], capture_output=True, text=True).stdout.strip()
    if got != source_sha(): sys.exit(f'[red] built translator embeds source {got}, tree has {source_sha()}')
    (out / 'bin' / 'source.sha').write_text(got + '\n')
    print('built', exe, 'source', got)


if __name__ == '__main__':
    main()
