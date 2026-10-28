"""Decompress NSO modules to <mod>.raw and dump imports/exports via MOD0 -> .dynamic."""
import collections, json, re, struct, sys
from pathlib import Path
import lz4.block


def nso_raw(nso):
    """Return (image bytes, info) with segments placed at their memory offsets."""
    b = nso.read_bytes()
    assert b[:4] == b'NSO0'
    flags = struct.unpack_from('<I', b, 0xc)[0]
    img = bytearray()
    segs = []
    for i, (ho, hs) in enumerate([(0x10, 0x60), (0x20, 0x64), (0x30, 0x68)]):
        foff, moff, dsz = struct.unpack_from('<III', b, ho)
        raw = b[foff:foff + struct.unpack_from('<I', b, hs)[0]]
        d = lz4.block.decompress(raw, uncompressed_size=dsz) if flags & (1 << i) else raw
        img[len(img):moff] = bytes(max(0, moff - len(img)))
        img[moff:moff + dsz] = d
        segs.append((moff, dsz))
    info = {'build_id': b[0x40:0x60].hex(), 'segments': segs, 'bss': struct.unpack_from('<I', b, 0x3c)[0]}
    return bytes(img), info


def parse(raw):
    mod0 = struct.unpack_from('<I', raw, 4)[0]
    assert raw[mod0:mod0 + 4] == b'MOD0'
    dyn = mod0 + struct.unpack_from('<i', raw, mod0 + 4)[0]
    tags = collections.defaultdict(list)
    o = dyn
    while True:
        t, v = struct.unpack_from('<qQ', raw, o)
        o += 16
        if t == 0:
            break
        tags[t].append(v)
    symtab, strtab, syment = tags[6][0], tags[5][0], tags.get(11, [24])[0]
    # symbol count from DT_HASH nchain
    nsym = struct.unpack_from('<I', raw, tags[4][0] + 4)[0] if 4 in tags else (strtab - symtab) // syment
    cstr = lambda off: raw[off:raw.index(b'\0', off)].decode()
    imports, exports = [], []
    for i in range(1, nsym):
        nm, info, other, shndx, val, size = struct.unpack_from('<IBBHQQ', raw, symtab + i * syment)
        (imports if shndx == 0 else exports).append({'name': cstr(strtab + nm), 'bind': info >> 4, 'type': info & 15, 'value': val})
    return {'imports': imports, 'exports': exports, 'needed': [cstr(strtab + v) for v in tags.get(1, [])],
            'has_init': 12 in tags, 'relocs': {'rela': tags.get(8, [0])[0] // 24, 'jmprel': tags.get(2, [0])[0] // 24}}


def namespace(n):
    m = re.match(r'_ZN(K?)2nn(\d+)', n)
    if m:
        return 'nn::' + n[m.end():m.end() + int(m.group(2))]
    if n.startswith('nvn'):
        return 'nvn'
    if n.startswith('_Z'):
        return 'c++'
    return 'c'


if __name__ == '__main__':
    d = Path(sys.argv[1])
    for m in sys.argv[2:]:
        raw, info = nso_raw(d / m)
        (d / f'{m}.raw').write_bytes(raw)
        r = parse(raw)
        r['nso'] = info
        (d / f'{m}.symbols.json').write_text(json.dumps(r, indent=1))
        c = collections.Counter(namespace(x['name']) for x in r['imports'])
        print(m, info['build_id'][:32], 'imports', len(r['imports']), 'exports', len(r['exports']), 'relocs', r['relocs'])
        print('  ', dict(c.most_common()))
