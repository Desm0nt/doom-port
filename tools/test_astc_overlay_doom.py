#!/usr/bin/env python3
"""Host tests of tools/astc_overlay_doom.py (the ASTC / ETC2 / EAC overlay; engine side: engine/tests/astc_overlay_test.cpp).

  python tools/test_astc_overlay_doom.py            unit tests + an end-to-end run on a synthetic mini RomFS (temp folder)
  python tools/test_astc_overlay_doom.py --bcdec    also: 300k random BC6H blocks (every mode) against bcdec.h compiled with
                                                    gcc under WSL (the engine's CPU decoder), bit for bit

Needs the packages of tools/requirements-astc.txt (e.g. work/venv-astc). Reads no game data.
"""
import hashlib, json, os, shutil, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import astc_overlay_doom as T
import numpy as np

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1; print(f'FAIL: {what}')


# bcdec_bc6h_half(isSigned = 0) of these blocks (one per mode code incl. a reserved one, and three blocks of the game's light
# probes): sha256 prefix and sum of the 48 output halves
BC6H_VECTORS = (
    ('60a18d9d931176ecc10d26948632cd53', '0e8c9c8fadc6f572', 756722),
    ('8de7d9708bac7672bb381a4fd87fa80e', 'c4934833de1d8b5b', 634076),
    ('a2e5b8abeeab14cef599cc052535d5ce', 'c73d75c1d82aa12c', 1139241),
    ('e610c2fa0a67ce0d8f02264785762462', '1fac89334c6ac8ae', 731842),
    ('4a36e4307fa901a76df710c04edc083c', '0c9e31de898975c1', 450601),
    ('2e6b49e2c23348df4abd744e5c8bbd04', '5c9c5c1b5ae3d796', 855362),
    ('92758c5cbdd130e147d9d82f7c5b7010', '0eb0c9e9c53c48fd', 702144),
    ('b68a4200a5627b0c90b818268101caff', '3651b00287ce35dc', 690952),
    ('7a1a0bdfd5adf8158373d1d7731f4dd8', '55ee12201f09d36f', 884503),
    ('1e7fa01dd0a174645397c6a0adc1367e', '33cb7e2ca050164c', 705797),
    ('632fb109fa75dc800fadcc557469c96f', 'e86ce1733494ec12', 939388),
    ('a7b5404808b9586fbf8a6343304464f9', 'c4c73e0e993dd74f', 929217),
    ('cb1638d795b5a8a29ff5f9eef71f6360', '8067b4bbaeb625e0', 958964),
    ('cf23399232b39168ce358d21b527970b', '4bbb6535e5437ea7', 584966),
    ('b35d34c7f36d1eb5cdc25ec5599f4e00', '2ea9ab9198d16380', 0),
    ('6b156118a270c20931247859ab67cf57', '87e2e05afe403701', 462914),
    ('0b16632492f6dc72abfd61ec30b82075', 'c0f8be7cbe421e5a', 455770),
    ('72671859be47d7be66dd01fea6a66402', 'd665f579193a3ea1', 254897),
)


def test_bc6h_vectors():
    blocks = np.array([np.frombuffer(bytes.fromhex(h), np.uint8) for h, _, _ in BC6H_VECTORS])
    out = T.bc6h_unsigned_to_half(blocks)
    for (h, digest, total), px in zip(BC6H_VECTORS, out):
        got = px.reshape(48).astype('<u2')
        check(hashlib.sha256(got.tobytes()).hexdigest()[:16] == digest and int(got.sum()) == total, f'BC6H block {h} differs from bcdec')


def test_bc6h_bcdec(n=300000):
    """Random blocks (all 18 mode codes) through bcdec.h under WSL gcc; skipped without WSL."""
    src = HERE.parent / 'src' / 'dm' / 'gfx' / 'vulkan' / 'engine' / 'third_party' / 'bcdec' / 'bcdec.h'
    tmp = Path(tempfile.mkdtemp(prefix='bc6t'))
    try:
        shutil.copy(src, tmp / 'bcdec.h')
        (tmp / 'ref.c').write_text('#define BCDEC_IMPLEMENTATION\n#include "bcdec.h"\n#include <stdio.h>\n#include <stdlib.h>\n'
            'int main(int c,char**v){FILE*f=fopen(v[1],"rb");fseek(f,0,2);long n=ftell(f)/16;fseek(f,0,0);unsigned char*b=malloc(n*16);'
            'if(fread(b,16,n,f)!=(size_t)n)return 1;fclose(f);unsigned short*o=malloc(n*96);for(long i=0;i<n;i++)bcdec_bc6h_half(b+i*16,o+i*48,12,0);'
            'f=fopen(v[2],"wb");fwrite(o,96,n,f);fclose(f);return 0;}\n')
        rng = np.random.default_rng(7)
        b = rng.integers(0, 256, (n, 16), dtype=np.uint8)
        codes = np.array([0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15, 19, 23, 27, 31])[rng.integers(0, 18, n)]
        b[:, 0] = (b[:, 0] & 0xE0) | np.where(codes < 2, (b[:, 0] & 0x1C) | codes, codes).astype(np.uint8)
        b.tofile(tmp / 'in.bin')
        wsl = str(tmp).replace('\\', '/')
        if len(wsl) > 1 and wsl[1] == ':': wsl = f'/mnt/{wsl[0].lower()}{wsl[2:]}'
        r = subprocess.run(['wsl', '-e', 'bash', '-lc', f"cd '{wsl}' && gcc -O2 -o ref ref.c && ./ref in.bin out.bin"], capture_output=True, text=True)
        if r.returncode: print(f'  (skipped the bcdec comparison: WSL gcc not available: {r.stderr.strip()[:200]})'); return
        ref = np.fromfile(tmp / 'out.bin', np.uint16).reshape(-1, 16, 3)
        got = T.bc6h_unsigned_to_half(b)
        bad = int((got != ref).any(axis=(1, 2)).sum())
        check(bad == 0, f'BC6H: {bad} of {n} random blocks differ from bcdec')
        print(f'  bcdec comparison: {n} random BC6H blocks, {bad} differ')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def make_index(entries):
    """Synthetic .pindex v5 bytes: entries = [(type, name, off, size, zsize, flags, patch)]."""
    body = struct.pack('>I', len(entries))
    for i, (typ, name, off, size, zsize, flags, patch) in enumerate(entries):
        body += struct.pack('>I', 1000 + i)
        for s in (typ, name, ''):
            b = s.encode('latin1'); body += struct.pack('<I', len(b)) + b
        body += struct.pack('>QIIIB', off, size, zsize, flags, patch)
    head = b'\x05SER' + struct.pack('>I', len(body)) + bytes(24)
    return head + body


def test_index_roundtrip():
    buf = make_index([('image', 'a', 4, 100, 50, 0, 0), ('file', 'b', 54, 10, 10, 0x10, 3), ('image', 'a2', 4, 100, 50, 0, 0)])
    e = T.parse_index(buf)
    check(len(e) == 3 and e[1]['flags'] == 0x10 and e[1]['patch'] == 3 and e[0]['off'] == 4, 'parse_index fields')
    new = T.rewrite_index(buf, e, {(0, 4, 50): (3, 999999999999, 120, 60)})
    check(len(new) == len(buf), 'rewrite_index keeps the length')
    e2 = T.parse_index(new)
    check([(x['patch'], x['off'], x['size'], x['zsize'], x['flags']) for x in e2] ==
          [(3, 999999999999, 120, 60, 0), (3, 54, 10, 10, 0x10), (3, 999999999999, 120, 60, 0)], 'rewrite_index moves every entry of the blob')
    check([(x['id'], x['type'], x['name']) for x in e2] == [(x['id'], x['type'], x['name']) for x in e], 'rewrite_index keeps ids / names')
    bad = bytearray(buf); bad[7] ^= 1
    try: T.parse_index(bytes(bad)); check(False, 'parse_index accepts a wrong size field')
    except T.Fail: pass


def make_bim(fmt, w, h, mips, cube, payload_fn, ts=0x12345678):
    """Synthetic BIM v7 record (layout of bim_layout); payload_fn(mip, face, w, h) -> block bytes."""
    typ = 2 if cube else 0
    hdr = struct.pack('>I', ts) + b'\x07MIB' + struct.pack('>5I', typ, w, h, 0, mips) + bytes(4) + struct.pack('<I', fmt) + b'\x05\x00\x00\x00\x01\x00'
    assert len(hdr) == 42
    out = bytearray(hdr)
    for m in range(mips):
        mw, mh = max(1, w >> m), max(1, h >> m)
        for f in range(6 if cube else 1):
            data = payload_fn(m, f, mw, mh)
            out += struct.pack('>5I', m, f, mw, mh, len(data)) + data
    return bytes(out)


def rgba_test_image(w, h, seed):
    y, x = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), np.uint8)
    img[..., 0] = (x * 255 // max(1, w - 1)); img[..., 1] = (y * 255 // max(1, h - 1)); img[..., 2] = (seed * 37 + x + y) % 256
    img[..., 3] = 255 - img[..., 0] // 2
    return img


def bc_payload(name):
    import etcpak
    enc = {'bc1': etcpak.compress_bc1, 'bc3': etcpak.compress_bc3, 'bc4': etcpak.compress_bc4, 'bc5': etcpak.compress_bc5,
           'bc7': lambda d, w, h: etcpak.compress_bc7(d, w, h, None)}[name]
    def fn(m, f, w, h):
        W, H = (w + 3) // 4 * 4, (h + 3) // 4 * 4
        return enc(rgba_test_image(W, H, m * 7 + f).tobytes(), W, H)
    return fn


def bc6h_payload(m, f, w, h):
    """Mode 11 (one region, 10.10 endpoints, unsigned) blocks with smooth endpoints: valid HDR data in [0, ~4]."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    out = bytearray()
    for i in range(bw * bh):
        e0 = [(100 + 13 * i + 41 * c + 7 * f) % 900 + 50 for c in range(3)]; e1 = [(e + 200 + 31 * m) % 1023 for e in e0]
        v = 0b00011
        pos = 5
        for val in e0 + e1: v |= val << pos; pos += 10
        for p in range(16):
            n = 3 if p == 0 else 4
            v |= ((p * 5 + i) % (1 << n)) << pos; pos += n
        out += v.to_bytes(16, 'little')
    return bytes(out)


def test_convert_formats():
    for name, fmt in (('bc1', 10), ('bc3', 11), ('bc4', 24), ('bc5', 25), ('bc7', 23)):
        for w, h, mips, cube in ((64, 32, 5, False), (16, 16, 3, True), (24, 8, 1, False)):
            d = make_bim(fmt, w, h, mips, cube, bc_payload(name))
            lay = T.bim_layout(d)
            check(lay is not None and lay[0] == fmt and len(lay[5]) == mips * (6 if cube else 1), f'bim_layout {name} {w}x{h}')
            new, st = T.convert_record(d, 'fast', 6)
            check(len(new) == len(d), f'{name} {w}x{h}: same length')
            check(T.skeleton_hash(new, T.bim_layout(new)[5]) == T.skeleton_hash(d, lay[5]), f'{name} {w}x{h}: header and mip records unchanged')
            check(new != d, f'{name}: blocks re-encoded')
            limit = {'bc4': 40, 'bc5': 38}.get(name, 28)      # garbage (blocks read as another format) measures ~10 dB
            check(st['q'] is not None and st['q'] > limit, f'{name} {w}x{h}: quality {st["q"]} dB <= {limit}')
    # BC6H cube
    d = make_bim(22, 16, 16, 3, True, bc6h_payload)
    new, st = T.convert_record(d, 'fast', 6)
    check(len(new) == len(d) and st['q'] is not None and st['q'] < 0.2, f'bc6h: log2 rmse {st["q"]}')


def test_eac_r_half():
    """etcpak's EAC R11 output is twice the size; encode() keeps the first half, which must decode to the image."""
    img = np.zeros((32, 64, 4), np.uint8); img[..., 0] = np.add.outer(np.arange(32) * 3, np.arange(64) * 2) % 256; img[..., 3] = 255
    b = T.encode('bc4', img, 'fast')
    check(len(b) == 16 * 8 * 8, f'EAC R11 size {len(b)}')
    back = T.decode_substitute('bc4', b, 64, 32, 'fast')
    check(np.abs(back[..., 0].astype(int) - img[..., 0]).max() <= 4, 'EAC R11 decode of the kept half')


def test_deflate():
    raw = bytes(range(256)) * 400
    z = T.deflate(raw, 6)
    check(z is not None and z.endswith(b'\x00\x00\xff\xff'), 'deflate ends with a sync flush like the game records')
    check(zlib.decompressobj(-15).decompress(z) == raw, 'deflate round trip')
    check(T.deflate(os.urandom(4096), 6) is None, 'incompressible records are stored')


def test_keep_bc():
    check(T.keep_bc('bc3', 0, 4096, 4096, 1) == 'vt-cache-shape', 'single-level 4096^2 stays BC')
    check(T.keep_bc('bc3', 0, 4096, 1024, 1) is None, 'a 4096x1024 single-level file texture is substituted')
    check(T.keep_bc('bc6h', 0, 128, 128, 7) == 'bc6h-not-cube', 'BC6H 2D stays BC')
    check(T.keep_bc('bc6h', 2, 128, 128, 7) is None, 'BC6H cube is substituted')


def write_romfs(root, records):
    """Mini RomFS: base/gameresources{.resources,.patch,_002.patch,_003.patch} + _003.pindex holding `records`
    = [(type, name, container, bytes, compress)]; a record may be listed twice (same blob, two entries)."""
    base = Path(root) / 'base'; base.mkdir(parents=True)
    data = {p: bytearray(b'\x05SER' + (bytes(12) if p == 0 else b'')) for p in range(4)}
    ents, placed = [], {}
    for typ, name, p, rec, comp in records:
        key = (p, rec, comp)
        if key not in placed:
            z = T.deflate(rec, 6) if comp else None
            z = z if z is not None else rec
            placed[key] = (len(data[p]), len(rec), len(z)); data[p] += z
        off, size, zsize = placed[key]
        ents.append((typ, name, off, size, zsize, 0x10 if typ == 'file' else 0, p))
    for p, n in T.CONTAINERS.items(): (base / n).write_bytes(bytes(data[p]))
    (base / T.PINDEX).write_bytes(make_index(ents))
    return ents


def test_end_to_end():
    tmp = Path(tempfile.mkdtemp(prefix='astcov'))
    try:
        bc7 = make_bim(23, 64, 64, 4, False, bc_payload('bc7'))
        bc4 = make_bim(24, 32, 32, 3, False, bc_payload('bc4'))
        bc6 = make_bim(22, 16, 16, 2, True, bc6h_payload)
        bc6_2d = make_bim(22, 16, 16, 1, False, bc6h_payload)           # never in the game: must stay BC (engine keeps it on BC)
        bc1 = make_bim(10, 32, 32, 2, False, bc_payload('bc1'))          # BC1: left alone by default
        rgba = make_bim(3, 8, 8, 1, False, lambda m, f, w, h: bytes(w * h * 4))
        recs = [('image', 'tex/bc7', 0, bc7, True), ('decalatlas', 'tex/bc7', 0, bc7, True), ('lightatlas', 'l/bc4', 1, bc4, False),
                ('file', 'probe_compressed.bimage', 2, bc6, True), ('file', 'x.bimage', 3, bc6_2d, True), ('image', 'b1', 3, bc1, True),
                ('image', 'rgba', 0, rgba, True), ('model', 'm', 1, b'not a texture' * 9, True)]
        ents = write_romfs(tmp / 'romfs', recs)
        out = tmp / 'out'
        check(T.run(str(tmp / 'romfs'), str(out), 2, 'fast', 6, False, False) == 0, 'run')
        man = json.loads((out / 'manifest.json').read_text())
        check(man['format'] == 'doom-astc-overlay' and man['version'] == 1, 'manifest format / version')
        check(man['counts'] == {'bc7': 1, 'bc4': 1, 'bc6h': 1}, f'counts {man["counts"]}')
        check(man['kept_bc_by_shape'] == {'bc6h-not-cube': 1}, f'kept {man["kept_bc_by_shape"]}')
        check(man['engine_formats_data'] == 'bc3 bc4 bc5 bc6h bc7' and man['engine_formats_nohdr'] == 'bc3 bc4 bc5 bc7', 'engine formats')
        check(man['entries_rewritten'] == {'data': 4, 'nohdr': 3}, f'rewritten {man["entries_rewritten"]}')
        for rel, size in man['files'].items(): check((out / rel).stat().st_size == size, f'file size {rel}')
        src3 = (tmp / 'romfs' / 'base' / T.CONTAINERS[3]).read_bytes()
        patch = (out / 'data' / 'base' / T.CONTAINERS[3]).read_bytes()
        check(patch[:len(src3)] == src3, 'the overlay container starts with the original _003.patch byte for byte')
        orig_idx = (tmp / 'romfs' / 'base' / T.PINDEX).read_bytes()
        for v, moved_names in (('data', {'tex/bc7', 'l/bc4', 'probe_compressed.bimage'}), ('nohdr', {'tex/bc7', 'l/bc4'})):
            idx = (out / v / 'base' / T.PINDEX).read_bytes()
            check(len(idx) == len(orig_idx), f'{v}: index length')
            for e0, e in zip(T.parse_index(orig_idx), T.parse_index(idx)):
                if e['name'] in moved_names:
                    check(e['patch'] == 3 and e['off'] >= len(src3) and e['size'] == e0['size'], f'{v}: {e["name"]} moved to the new record')
                    z = patch[e['off']:e['off'] + e['zsize']]
                    d = z if e['size'] == e['zsize'] else zlib.decompressobj(-15).decompress(z)
                    o = {'tex/bc7': bc7, 'l/bc4': bc4, 'probe_compressed.bimage': bc6}[e['name']]
                    check(len(d) == len(o) and d[:42] == o[:42] and d != o, f'{v}: {e["name"]} re-encoded record')
                else:
                    check((e['patch'], e['off'], e['size'], e['zsize']) == (e0['patch'], e0['off'], e0['size'], e0['zsize']), f'{v}: {e["name"]} untouched')
        # up to date: --check passes, a second run does nothing
        check(T.run(str(tmp / 'romfs'), str(out), 2, 'fast', 6, False, True) == 0, '--check after a run')
        before = {p: p.stat().st_mtime_ns for p in out.rglob('*') if p.is_file()}
        T.run(str(tmp / 'romfs'), str(out), 2, 'fast', 6, False, False)
        check(before == {p: p.stat().st_mtime_ns for p in out.rglob('*') if p.is_file()}, 'an up-to-date overlay is not rewritten')
        # deterministic: another worker count gives the same bytes
        out2 = tmp / 'out2'
        T.run(str(tmp / 'romfs'), str(out2), 1, 'fast', 6, False, False)
        for rel in man['files']: check((out / rel).read_bytes() == (out2 / rel).read_bytes(), f'deterministic {rel}')
        # stale: a changed RomFS index makes --check fail
        (tmp / 'romfs' / 'base' / T.PINDEX).write_bytes(make_index(ents[:-1]))
        check(T.run(str(tmp / 'romfs'), str(out), 2, 'fast', 6, False, True) == 1, '--check notices another RomFS')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    tests = [test_bc6h_vectors, test_index_roundtrip, test_convert_formats, test_eac_r_half, test_deflate, test_keep_bc, test_end_to_end]
    if '--bcdec' in sys.argv: tests.append(test_bc6h_bcdec)
    for t in tests:
        print(t.__name__, flush=True); t()
    print('test_astc_overlay_doom: ' + (f'{failures} FAILURES' if failures else 'all passed'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
