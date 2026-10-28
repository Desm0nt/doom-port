"""Rebuild the original NSP from an NSZ: every .ncz entry is decompressed and re-encrypted back into the exact .nca.

NCZ body (after the raw 0x4000-byte header area) is the NCA in plaintext; the NCZSECTN table lists the encrypted
ranges (offset, size, crypto type, key, counter). Types 3 and 4 are re-encrypted with AES-CTR using the stored
counter nonce and offset/16, which is the inverse of what nsz did. The result is checked later by content readers
(section header hashes, IVFC levels), so a wrong byte cannot pass silently.
Usage: ncz2nsp.py <in.nsz> <out.nsp>
"""
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import struct, sys, zstandard

CHUNK = 1 << 24


def pfs_entries(f):
    f.seek(0)
    h = f.read(16)
    assert h[:4] == b'PFS0'
    n, ss = struct.unpack_from('<II', h, 4)
    t = f.read(n * 24 + ss)
    base = 16 + n * 24 + ss
    out = []
    for i in range(n):
        off, sz, no = struct.unpack_from('<QQI', t, i * 24)
        out.append([t[n * 24 + no:].split(b'\0')[0].decode(), base + off, sz])
    return out


def ncz_size(f, off):
    """Original NCA size from the (still encrypted) header is not readable without keys; take it from the section table end."""
    f.seek(off + 0x4000)
    assert f.read(8) == b'NCZSECTN'
    count = struct.unpack('<Q', f.read(8))[0]
    secs = [struct.unpack('<QQQQ16s16s', f.read(0x40)) for _ in range(count)]
    return secs, f.tell()


def write_nca(f, off, out, total):
    """Stream one NCZ into `out`; returns bytes written (must equal the NCA size)."""
    f.seek(off)
    out.write(f.read(0x4000))
    secs, body = ncz_size(f, off)
    f.seek(body)
    if f.read(8) == b'NCZBLOCK':
        raise NotImplementedError('NCZBLOCK')
    f.seek(body)
    z = zstandard.ZstdDecompressor().stream_reader(f, read_size=CHUNK)
    pos = 0x4000
    enc = {}
    for s_off, s_size, ctype, _, key, ctr in secs:
        if ctype in (3, 4):
            enc[(s_off, s_off + s_size)] = (key, ctr[:8])
        else:
            assert ctype in (0, 1), f'crypto type {ctype}'
    ranges = sorted(enc)
    end = max([e for _, e in ranges] + [pos])
    while pos < end:
        want = min(CHUNK, end - pos)
        data = bytearray()
        while len(data) < want:
            c = z.read(want - len(data))
            if not c:
                break
            data.extend(c)
        if not data:
            break
        n = len(data)
        for lo, hi in ranges:
            a, b = max(lo, pos), min(hi, pos + n)
            if a >= b:
                continue
            key, nonce = enc[(lo, hi)]
            skip = a % 16
            iv = nonce + ((a - skip) // 16).to_bytes(8, 'big')
            e = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor()
            seg = bytes(skip) + bytes(data[a - pos:b - pos])
            data[a - pos:b - pos] = e.update(seg)[skip:]
        out.write(data)
        pos += n
        print(f'\r  {pos / 2**30:7.2f} GiB', end='', flush=True)
    # The zstd frame must end exactly at the last section; bytes after it belong to the next PFS0 entry.
    try:
        rest = z.read(1)
    except zstandard.ZstdError:
        rest = b''
    assert not rest, 'NCZ stream longer than the last section'
    print()
    return pos


def main():
    convert(sys.argv[1], sys.argv[2])


def convert(src, dst):
    src, dst = Path(src), Path(dst)
    with src.open('rb') as f:
        ents = pfs_entries(f)
        # first pass: sizes of rebuilt NCAs (needed for the PFS0 header) = end of the last section
        sizes = []
        for name, off, sz in ents:
            if name.endswith('.ncz'):
                secs, _ = ncz_size(f, off)
                sizes.append(max(o + s for o, s, *_ in secs))
            else:
                sizes.append(sz)
        names = [n[:-4] + '.nca' if n.endswith('.ncz') else n for n, _, _ in ents]
        strtab = b''.join(n.encode() + b'\0' for n in names)
        strtab += bytes((-(16 + 24 * len(names) + len(strtab))) % 0x20)
        hdr = bytearray(b'PFS0' + struct.pack('<II', len(names), len(strtab)) + bytes(4))
        o = no = 0
        for n, s in zip(names, sizes):
            hdr += struct.pack('<QQII', o, s, no, 0)
            o += s
            no += len(n) + 1
        hdr += strtab
        tmp = dst.with_suffix('.partial')
        with tmp.open('wb') as out:
            out.write(hdr)
            for (name, off, sz), size in zip(ents, sizes):
                print(name, size, flush=True)
                if name.endswith('.ncz'):
                    w = write_nca(f, off, out, size)
                    assert w == size, f'{name}: wrote {w}, expected {size}'
                else:
                    f.seek(off)
                    left = sz
                    while left:
                        b = f.read(min(CHUNK, left))
                        out.write(b)
                        left -= len(b)
        tmp.replace(dst)


if __name__ == '__main__':
    main()
