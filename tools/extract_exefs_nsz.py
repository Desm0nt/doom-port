"""Extract ExeFS (main, sdk, subsdk*, main.npdm) from the program NCZ inside an NSZ.

NCZ layout (nsz project): first 0x4000 bytes are the original NCA header area (encrypted),
then "NCZSECTN" + section table, then either an "NCZBLOCK" header + blocks or one zstd stream.
The decompressed stream is the *plaintext* NCA body starting at offset 0x4000, so section data
needs no AES once decompressed. The stream is read forward only; bytes before the requested
offset are discarded, so memory stays bounded even when ExeFS sits at the end of the NCA.
"""
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import argparse, hashlib, json, os, re, struct, zstandard

KEYS = Path(os.environ.get('NX_PROD_KEYS', Path.home() / '.switch' / 'prod.keys'))   # your own console keys (hactool format); never part of this repository


def pfs(read, size):
    h = read(0, 16)
    assert h[:4] == b'PFS0', h[:4]
    n, ss = struct.unpack_from('<II', h, 4)
    t = read(16, n * 24 + ss)
    base = 16 + n * 24 + ss
    out = []
    for i in range(n):
        off, sz, no = struct.unpack_from('<QQI', t, i * 24)
        out.append((t[n * 24 + no:].split(b'\0')[0].decode(), base + off, sz))
    assert all(o + s <= size for _, o, s in out)
    return out


def xts_header(key, raw):
    d = lambda s: Cipher(algorithms.AES(key), modes.XTS(s.to_bytes(16, 'big'))).decryptor()
    return b''.join(d(s).update(raw[s * 512:(s + 1) * 512]) for s in range(6))


class NczStream:
    """Forward-only reader over the plaintext NCA body (absolute NCA offsets >= 0x4000)."""

    def __init__(self, f, body_off):
        f.seek(body_off)
        magic = f.read(8)
        assert magic == b'NCZSECTN', magic
        count = struct.unpack('<Q', f.read(8))[0]
        self.sections = [struct.unpack('<QQQQ16s16s', f.read(0x40)) for _ in range(count)]
        pos = f.tell()
        if f.read(8) == b'NCZBLOCK':
            raise NotImplementedError('NCZBLOCK mode: add block-table decoding')
        f.seek(pos)
        self.z = zstandard.ZstdDecompressor().stream_reader(f, read_size=1 << 22)
        self.pos = 0x4000          # absolute offset of buf[0]
        self.buf = bytearray()

    def read(self, off, n):
        assert off >= self.pos, 'NczStream is forward-only'
        drop = off - self.pos
        while drop > len(self.buf):        # skip without keeping
            drop -= len(self.buf)
            self.pos += len(self.buf)
            self.buf = bytearray(self._chunk(min(drop, 1 << 26)))
        del self.buf[:drop]
        self.pos = off
        while len(self.buf) < n:
            self.buf.extend(self._chunk(n - len(self.buf)))
        return bytes(self.buf[:n])

    def _chunk(self, n):
        c = self.z.read(max(n, 1 << 22))
        if not c:
            raise EOFError('NCZ stream ended early')
        return c


def extract(nsz, dest, keys_path=KEYS):
    """ExeFS members of the program NCZ in `nsz` -> `dest`; returns the report dict."""
    keys = {k: bytes.fromhex(v) for k, v in re.findall(r'^\s*(\w+)\s*=\s*([0-9a-fA-F]+)\s*$', Path(keys_path).read_text(), re.M)}
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    with open(nsz, 'rb') as f:
        entries = pfs(lambda o, n: (f.seek(o), f.read(n))[1], Path(nsz).stat().st_size)
        report = {'package': Path(nsz).name, 'entries': [(n, s) for n, _, s in entries], 'modules': {}}
        for name, off, size in entries:
            if not name.endswith('.ncz'):
                continue
            f.seek(off)
            h = xts_header(keys['header_key'], f.read(0xc00))
            assert h[0x200:0x204] == b'NCA3'
            report['title_id'] = f'{struct.unpack_from("<Q", h, 0x210)[0]:016x}'
            report['content_type'] = h[0x205]
            secs = []
            for i in range(4):
                s, e = struct.unpack_from('<II', h, 0x240 + 16 * i)
                if s:
                    secs.append((i, s * 512, (e - s) * 512, h[0x400 + i * 512:0x600 + i * 512]))
            report['sections'] = [(i, st, sz, fsh[2], fsh[3], fsh[4]) for i, st, sz, fsh in secs]
            z = NczStream(f, off + 0x4000)
            for i, st, sz, fsh in sorted(secs, key=lambda s: s[1]):
                if fsh[3] != 2:  # 2 = PFS0 (ExeFS / logo); RomFS handled elsewhere
                    continue
                ho, hs, po, ps = struct.unpack_from('<QQQQ', fsh, 0x30)
                ht = z.read(st + ho, hs)
                assert hashlib.sha256(ht).digest() == fsh[8:40], 'hash table mismatch'
                data = z.read(st + po, ps)
                bs = struct.unpack_from('<I', fsh, 0x28)[0]
                for j in range(0, ps, bs):
                    assert hashlib.sha256(data[j:j + bs]).digest() == ht[j // bs * 32:j // bs * 32 + 32], 'pfs block hash'
                members = pfs(lambda o, n: data[o:o + n], len(data))
                if 'main' not in [m[0] for m in members]:
                    continue
                for mn, mo, ms in members:
                    blob = data[mo:mo + ms]
                    (dest / mn).write_bytes(blob)
                    report['modules'][mn] = {'size': ms, 'sha256': hashlib.sha256(blob).hexdigest()}
                    print(mn, ms)
            break
    (dest / 'extract-report.json').write_text(json.dumps(report, indent=1))
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nsz')
    ap.add_argument('dest')
    a = ap.parse_args()
    extract(a.nsz, a.dest)


if __name__ == '__main__':
    main()
