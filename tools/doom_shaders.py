"""Inventory / export DOOM's precompiled NVN shaders (renderProg entries generated/nx/*.sb).

The newest .pindex (gameresources_003.pindex) lists every resource with the container that holds its current
version: patch 0 = gameresources.resources, 1 = gameresources.patch, N>1 = gameresources_00N.patch.
Each .sb holds one glslc output: GPU code (0x12345678) + control section (0x98761234). The key is the same as
d2r-mobile's (sha256 of the first 0x880 control bytes, first 16 hex digits).
Usage: doom_shaders.py <romfs dir> <out dir>   -> <out>/<key>.sb + shaders.tsv
"""
import hashlib, sys, zlib
from pathlib import Path
from doom_index import parse

CTRL, GPU = bytes.fromhex('34127698'), bytes.fromhex('78563412')


def container(romfs, patch):
    return romfs / 'base' / ('gameresources.resources' if patch == 0 else
                             'gameresources.patch' if patch == 1 else f'gameresources_{patch:03d}.patch')


def main():
    romfs, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    ents = [x for x in parse(romfs / 'base' / 'gameresources_003.pindex')
            if x['type'] == 'renderProg' and x['name2'].endswith('.sb')]
    files, rows, keys = {}, [], set()
    for x in ents:
        f = files.get(x['patch']) or files.setdefault(x['patch'], container(romfs, x['patch']).open('rb'))
        f.seek(x['off'])
        raw = f.read(x['zsize'])
        d = raw if x['zsize'] == x['size'] else zlib.decompressobj(-15).decompress(raw)
        assert len(d) == x['size'], x['name2']
        stage = x['name2'].rsplit('_', 1)[-1][:-3]
        cs = [i for i in range(0, len(d) - 3, 4) if d[i:i + 4] == CTRL]
        assert cs, x['name2']           # compute programs carry no 0x12345678 (no SPH)
        for c in cs:                      # one control section per stage (vs / ps / vsps programs carry several)
            key = hashlib.sha256(d[c:c + 0x880]).hexdigest()[:16]
            if key not in keys:
                keys.add(key)
                (out / f'{key}.sb').write_bytes(d)
            rows.append(f"{key}\t{stage}\t{x['name2']}\t{x['patch']}\t{len(d)}\t{c:#x}\n")
    (out / 'shaders.tsv').write_text('key\tstage\tsource\tpatch\tsize\tctrl\n' + ''.join(rows))
    stages = {}
    for r in rows:
        s = r.split('\t')[1]; stages[s] = stages.get(s, 0) + 1
    print(len(rows), 'shader entries,', len(keys), 'unique keys, stages', stages)


if __name__ == '__main__':
    main()
