"""Rebuild the original NSP from an NSZ or XCZ: every .ncz entry is decompressed and re-encrypted back into the exact .nca.

NCZ body (after the raw 0x4000-byte header area) is the NCA in plaintext; the NCZSECTN table lists the encrypted
ranges (offset, size, crypto type, key, counter). Types 3 and 4 are re-encrypted with AES-CTR using the stored
counter nonce and offset/16, which is the inverse of what nsz did. The body is either one zstd stream (solid, the nsz
default for NSZ) or an NCZBLOCK table of independently compressed blocks (nsz -B, the default for XCZ); NczBody reads
both. The result is checked later by content readers (section header hashes, IVFC levels), so a wrong byte cannot pass
silently. The source may be an NSZ (PFS0) or an XCZ (the secure partition of an XCI); plain .nca entries are copied.
Usage: ncz2nsp.py <in.nsz|in.xcz> <out.nsp>
"""
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import struct, sys, zstandard

sys.path.insert(0, str(Path(__file__).resolve().parent))
from content_reader import open_package, entries_of

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


def nca_size(f, off):
    """Size of the NCA an NCZ entry expands to."""
    secs, _ = ncz_size(f, off)
    return max(o + s for o, s, *_ in secs)


class NczBody:
    """Forward-only reader over the plaintext NCA body of one NCZ entry (NCA offsets from 0x4000 on).
    read(n) returns up to n bytes, b'' at the end. Solid: one zstd stream; NCZBLOCK: header (magic, version, type,
    unused, block size exponent, block count, decompressed size), u32 compressed size per block, then the blocks; a
    block whose stored size is not smaller than its plain size is stored uncompressed (nsz BlockDecompressorReader)."""

    def __init__(self, f, off):
        self.sections, body = ncz_size(f, off)
        self.f = f
        f.seek(body)
        if f.read(8) == b'NCZBLOCK':
            _ver, _typ, _unused, exp, count, self.total = struct.unpack('<BBBBIQ', f.read(16))
            self.bsize = 1 << exp
            self.sizes = struct.unpack(f'<{count}I', f.read(4 * count))
            self.at = f.tell()
            self.block = 0
            self.buf = b''
            self.z = None
        else:
            f.seek(body)
            self.z = zstandard.ZstdDecompressor().stream_reader(f, read_size=CHUNK)

    def _next_block(self):
        i = self.block
        if i >= len(self.sizes):
            return b''
        plain = min(self.bsize, self.total - i * self.bsize)
        self.f.seek(self.at)
        raw = self.f.read(self.sizes[i])
        if len(raw) != self.sizes[i]:
            raise EOFError('NCZBLOCK data ends early')
        self.at += self.sizes[i]
        self.block += 1
        data = raw if self.sizes[i] >= plain else zstandard.ZstdDecompressor().decompress(raw, max_output_size=plain)
        if len(data) != plain:
            raise ValueError(f'NCZBLOCK block {i}: {len(data)} bytes, expected {plain}')
        return data

    def read(self, n):
        if self.z is not None:
            return self.z.read(n)
        out = bytearray()
        while len(out) < n:
            if not self.buf:
                self.buf = self._next_block()
                if not self.buf:
                    break
            k = n - len(out)
            out += self.buf[:k]
            self.buf = self.buf[k:]
        return bytes(out)

    def at_end(self):
        """True when nothing follows the data read so far (a solid stream must end at the last section)."""
        if self.z is None:
            return not self.buf and self.block >= len(self.sizes)
        try:
            return not self.z.read(1)
        except zstandard.ZstdError:
            return True   # the reader ran into the next entry's bytes: the frame itself is complete


def write_nca(f, off, out, total, progress=None):
    """Stream one NCZ into `out`; returns bytes written (must equal the NCA size)."""
    f.seek(off)
    out.write(f.read(0x4000))
    z = NczBody(f, off)
    pos = 0x4000
    enc = {}
    for s_off, s_size, ctype, _, key, ctr in z.sections:
        if ctype in (3, 4):
            enc[(s_off, s_off + s_size)] = (key, ctr[:8])
        else:
            assert ctype in (0, 1), f'crypto type {ctype}'
    ranges = sorted(enc)
    end = max([o + s for o, s, *_ in z.sections] + [pos])
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
        if progress:
            progress(n)
        else:
            print(f'\r  {pos / 2**30:7.2f} / {total / 2**30:.2f} GiB', end='', flush=True)
    # The NCZ must end exactly at the last section; bytes after it belong to the next entry.
    assert z.at_end(), 'NCZ stream longer than the last section'
    if not progress:
        print()
    return pos


def main():
    convert(sys.argv[1], sys.argv[2])


def convert(src, dst, only=None, progress=None):
    """NSZ / XCZ (or a split dump of one) -> NSP. `only`: entry names (.nca / .ncz) to keep; tickets and certificates
    are always kept. progress(bytes) is called while NCZ entries are expanded (default: a GiB counter on stdout)."""
    dst = Path(dst)
    f, fsize = open_package(src)
    try:
        ents = [list(e) for e in entries_of(f, fsize)]
        if only is not None:
            ents = [e for e in ents if e[0] in only or e[0].endswith(('.tik', '.cert'))]
        # first pass: sizes of rebuilt NCAs (needed for the PFS0 header) = end of the last section
        sizes = [nca_size(f, off) if name.endswith('.ncz') else sz for name, off, sz in ents]
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
                if not progress:
                    print(name, size, flush=True)
                if name.endswith('.ncz'):
                    w = write_nca(f, off, out, size, progress)
                    assert w == size, f'{name}: wrote {w}, expected {size}'
                else:
                    f.seek(off)
                    left = sz
                    while left:
                        b = f.read(min(CHUNK, left))
                        out.write(b)
                        left -= len(b)
        tmp.replace(dst)
    finally:
        f.close()


if __name__ == '__main__':
    main()
