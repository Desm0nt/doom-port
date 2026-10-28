"""DOOM RomFS (base + BKTR update) via the NSP/XCI/NCA readers in content_reader.py.
Usage: romfs_doom.py list            -> work/romfs-list.tsv (path, size)
       romfs_doom.py extract <dest>  -> files + work/romfs-manifest.json (IVFC verified first)
(packages: work/nsp/base.nsp + work/nsp/update.nsp; tools/setup_doom.py calls extract() with its own paths)
"""
import os, sys, json, struct
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from content_reader import Package, Patched, verify_ivfc

W = Path(__file__).resolve().parents[1] / 'work'
KEYS = str(Path(os.environ.get('NX_PROD_KEYS', Path.home() / '.switch' / 'prod.keys')))   # your own console keys; never part of this repository


def program(nsp, keys=KEYS):
    nca = next(n for n in Package(nsp, keys).ncas() if n.type == 0)
    return next(s for s in nca.sections if s.h[3] == 3)


def romfs_view(s, verify):
    if verify:
        off, size, _ = verify_ivfc(s)
    else:
        lv = [struct.unpack_from('<QQI', s.h, 0x18 + i * 24) for i in range(6)]
        off, size = lv[-1][0], lv[-1][1]
    read = lambda o, n: s.read(off + o, n)
    h = struct.unpack('<10Q', read(0, 80))
    assert h[0] == 80
    return read, h


def walk(read, h):
    dirs, files = read(h[3], h[4]), read(h[7], h[8])
    out = []

    def rec(di, parent):
        _, sib, child, fi, _, ns = struct.unpack_from('<6I', dirs, di)
        folder = f'{parent}/{dirs[di + 24:di + 24 + ns].decode()}' if ns else parent
        while fi != 0xffffffff:
            _, nxt, fo, fs, _, fns = struct.unpack_from('<IIQQII', files, fi)
            out.append((f'{folder}/{files[fi + 32:fi + 32 + fns].decode()}', h[9] + fo, fs))
            fi = nxt
        if child != 0xffffffff: rec(child, folder)
        if sib != 0xffffffff: rec(sib, parent)
    rec(0, '')
    return out


def extract(base_pkg, update_pkg, keys, dest, manifest):
    """IVFC-verify the merged RomFS, then write every file under `dest` and a manifest (path, size, sha256)."""
    import hashlib
    read, h = romfs_view(Patched(program(update_pkg, keys), program(base_pkg, keys)), True)
    man = []
    for p, o, s in walk(read, h):
        t = Path(dest) / p.lstrip('/')
        t.parent.mkdir(parents=True, exist_ok=True)
        d = hashlib.sha256()
        with t.open('wb') as f:
            for pos in range(0, s, 1 << 24):
                b = read(o + pos, min(1 << 24, s - pos)); f.write(b); d.update(b)
        man.append({'path': p, 'size': s, 'sha256': d.hexdigest()})
        print(p, s, flush=True)
    Path(manifest).write_text(json.dumps(man, indent=1))
    return man


def main():
    base, upd = program(W / 'nsp/base.nsp'), program(W / 'nsp/update.nsp')
    patched = Patched(upd, base)
    print('BKTR mappings', len(patched.reloc), 'subsections', len(patched.subs), flush=True)
    if sys.argv[1] == 'list':
        read, h = romfs_view(patched, False)
        ents = walk(read, h)
        (W / 'romfs-list.tsv').write_text(''.join(f'{p}\t{s}\n' for p, _, s in ents))
        print(len(ents), 'files', sum(s for *_, s in ents) / 2**30, 'GiB')
    else:
        extract(W / 'nsp/base.nsp', W / 'nsp/update.nsp', KEYS, sys.argv[2], W / 'romfs-manifest.json')


if __name__ == '__main__':
    main()
