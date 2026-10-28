# Resolve C++ vtables through RELATIVE relocations + Itanium RTTI (works on the stripped update build).
# usage: python tools/vtables.py base|update <class mangled length-prefixed name, e.g. 13idCommonLocal> [slots]
import struct, sys, collections

def load(which):
    raw = bytearray(open(f'work/exefs-{which}/main.raw', 'rb').read())
    mod0 = struct.unpack_from('<I', raw, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', raw, mod0 + 4)[0]
    tags = collections.defaultdict(list); o = dyn
    while True:
        t, v = struct.unpack_from('<qQ', raw, o); o += 16
        if t == 0: break
        tags[t].append(v)
    rela, relasz = tags[7][0], tags[8][0]
    symtab = tags[6][0]
    for i in range(relasz // 24):
        off, info, add = struct.unpack_from('<QQq', raw, rela + 24 * i)
        t = info & 0xffffffff
        if off + 8 > len(raw): continue
        if t == 1027: struct.pack_into('<Q', raw, off, add & (2**64 - 1))
        elif t == 257:   # ABS64 against a defined symbol (base build exports everything)
            _, _, _, shndx, val, _ = struct.unpack_from('<IBBHQQ', raw, symtab + (info >> 32) * 24)
            if shndx: struct.pack_into('<Q', raw, off, (val + add) & (2**64 - 1))
    return bytes(raw)

def vtable(raw, cls):
    name = cls.encode() + b'\0'
    s = raw.find(b'\0' + name) + 1
    assert s > 0, 'type name not found'
    tn = struct.pack('<Q', s)
    res = []
    i = -1
    while True:   # typeinfo objects: [vptr][name ptr]...
        i = raw.find(tn, i + 1)
        if i < 0: break
        if i % 8: continue
        ti = i - 8
        tip = struct.pack('<Q', ti); j = -1
        while True:   # vtable: [offset-to-top][typeinfo][slot0...]
            j = raw.find(tip, j + 1)
            if j < 0: break
            if j % 8 == 0 and struct.unpack_from('<q', raw, j - 8)[0] == 0: res.append(j + 8)
    return res

if __name__ == '__main__':
    raw = load(sys.argv[1]); n = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    for vt in vtable(raw, sys.argv[2]):
        print(f'vtable {vt:#x}')
        for k in range(n):
            p = struct.unpack_from('<Q', raw, vt + 8 * k)[0]
            if p == 0 or p > len(raw): break
            print(f'  [{k * 8:#05x}] {p:#x}')
