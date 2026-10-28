"""Parse id Tech 6 (DOOM 2016) .index / .pindex: big-endian header '\x05SER', entry = id, 3 strings (u32 LE len),
u64 offset, u32 size, u32 zsize, u32 flags, u8 patch file number (all BE)."""
import struct, sys, collections


def parse(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'\x05SER'
    n = struct.unpack_from('>I', b, 0x20)[0]
    o = 0x24
    out = []
    for _ in range(n):
        eid = struct.unpack_from('>I', b, o)[0]; o += 4
        s = []
        for _ in range(3):
            l = struct.unpack_from('<I', b, o)[0]; o += 4
            s.append(b[o:o + l].decode('latin1')); o += l
        off, size, zsize, flags, patch = struct.unpack_from('>QIIIB', b, o); o += 21
        out.append(dict(id=eid, type=s[0], name=s[1], name2=s[2], off=off, size=size, zsize=zsize, flags=flags, patch=patch))
    assert o == len(b), (o, len(b))
    return out


if __name__ == '__main__':
    e = parse(sys.argv[1])
    print(len(e), 'entries')
    c = collections.Counter(x['type'] for x in e)
    sz = collections.Counter()
    for x in e: sz[x['type']] += x['zsize']
    for t, k in c.most_common(): print(f'{t:28} {k:6} {sz[t] / 2**20:9.1f} MB')
    print('patch numbers', collections.Counter(x['patch'] for x in e).most_common())
