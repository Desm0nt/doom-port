#!/usr/bin/env python3
"""ASTC HDR investigation and self-test vectors for the DOOM ASTC overlay (docs/BUGS_AND_FIXES.md 2.37). Developer tool, not setup.

  astc_hdr_selftest_gen.py formats ROMFS
      histogram of the BIM texture formats in the RomFS index (is any BC6H texture of another (signed) format code?)
  astc_hdr_selftest_gen.py stream ROMFS OVERLAY [--limit N] [--ldr] | astc_hdr_scan [--picks picks.bin]
      every BC6H texture the overlay re-encoded: per mip and face its ASTC 4x4 HDR blocks, astcenc's decode of them and the
      decode of the source BC6H blocks (the engine's CPU decoder port), on stdout for tools/astc_hdr_scan.cpp (which decodes the
      blocks again with the independent decoder engine/xbg_astc_ref_decode.h); --ldr also streams the ASTC LDR blocks (BC7 / BC3)
  astc_hdr_selftest_gen.py ldrstream ROMFS OVERLAY [--limit N] [--only bc4,bc5] | astc_ldr_scan
      the LDR substitutes (BC3 / BC7 -> ASTC 4x4, BC4 -> EAC R11, BC5 -> EAC RG11) with their source blocks, for
      tools/astc_ldr_scan.cpp (independent spec decoders: is each substitute the same picture as its source?)
  astc_hdr_selftest_gen.py onlyindex ROMFS OVERLAY bc3,bc4,bc5,bc7
      OVERLAY/only-<fmt>/base/gameresources_003.pindex: only that format substituted (DM_GFX_ASTC_ONLY=<fmt> on the phone; copy the
      only-<fmt> folder next to data/, 14.7 MB each)
  astc_hdr_selftest_gen.py header PICKS OUT.h
      the embedded self-test vectors (dm/gfx/vulkan/engine/xbg_astc_hdr_data.h): the real blocks astc_hdr_scan picked plus
      blocks encoded here with the overlay's own encoder settings (astcenc 4x4, HDR_RGB_LDR_A, -medium) from BC6H blocks with
      extreme values; expected values = astcenc's decode (engine/tests/astc_hdr_test.cpp checks them with the independent decoder).

Run with the setup venv (work/venv-astc: astc-encoder-py, numpy, texture2ddecoder). OVERLAY = the tool's output folder
(work/astc_overlay_out: data/base/gameresources_003.{patch,pindex}).
"""
import os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import astc_overlay_doom as ov   # noqa: E402

MAGIC = b'AHS1'


def entries_of(path):
    return ov.parse_index(open(path, 'rb').read())


def moved_records(romfs, overlay, names=None):
    """[(orig (patch, off, size, zsize), new (patch, off, size, zsize))] of every record the overlay re-encoded (unique);
    names (a dict) gets the first index entry name of each original record."""
    a = entries_of(os.path.join(romfs, 'base', ov.PINDEX))
    b = entries_of(os.path.join(overlay, 'data', 'base', ov.PINDEX))
    if len(a) != len(b): raise SystemExit('overlay index does not match the RomFS index')
    seen, out = set(), []
    for x, y in zip(a, b):
        ko = (x['patch'], x['off'], x['size'], x['zsize']); kn = (y['patch'], y['off'], y['size'], y['zsize'])
        if ko == kn or ko in seen: continue
        seen.add(ko); out.append((ko, kn))
        if names is not None: names[ko] = x['name']
    return out


def read_from(base, patch, off, size, zsize):
    with open(os.path.join(base, ov.CONTAINERS[patch]), 'rb') as f:
        f.seek(off); z = f.read(zsize)
    return z if size == zsize else zlib.decompressobj(-15).decompress(z)


def cmd_formats(romfs):
    base = os.path.join(romfs, 'base')
    ents = entries_of(os.path.join(base, ov.PINDEX))
    files = {p: open(os.path.join(base, n), 'rb') for p, n in ov.CONTAINERS.items()}
    hist, seen = {}, set()
    for e in ents:
        if e['type'] not in ov.BIM_TYPES or e['size'] < 62: continue
        k = (e['patch'], e['off'], e['zsize'])
        if k in seen: continue
        seen.add(k)
        h = ov.bim_head(files[e['patch']], e['off'], e['size'], e['zsize'])
        if h[4:8] != b'\x07MIB': continue
        typ = struct.unpack_from('>I', h, 8)[0]; fmt = struct.unpack_from('<I', h, 0x20)[0]
        hist[(fmt, typ)] = hist.get((fmt, typ), 0) + 1
    for (fmt, typ), n in sorted(hist.items()):
        name = ov.FORMATS.get(fmt, ('?',))[0]
        print(f'BIM format {fmt:3d} ({name:5s}) type {typ} ({"cube" if typ == 2 else "2d" if typ == 0 else typ}): {n} textures')


def cmd_stream(romfs, overlay, limit, ldr):
    np = ov._numpy()
    base, obase = os.path.join(romfs, 'base'), os.path.join(overlay, 'data', 'base')
    out = sys.stdout.buffer
    out.write(MAGIC)
    n = 0
    for ko, kn in moved_records(romfs, overlay):
        d0 = read_from(base, *ko)
        lay = ov.bim_layout(d0)
        if lay is None: continue
        fmt = lay[0]
        name = ov.FORMATS[fmt][0]
        if name not in ('bc6h', 'bc7', 'bc3'): continue
        if name != 'bc6h' and not ldr: continue
        d1 = read_from(obase, *kn)
        recs0, recs1 = lay[5], ov.bim_layout(d1)[5]
        if name == 'bc6h':
            out.write(struct.pack('<III', 1, n, len(recs0)))
            for (_, rw, rh, do0, sz), (_, _, _, do1, _) in zip(recs0, recs1):
                bw, bh = (rw + 3) // 4, (rh + 3) // 4
                blocks = d1[do1:do1 + sz]
                src = ov.decode_bc('bc6h', d0[do0:do0 + sz], bw * 4, bh * 4)                 # (H, W, 4) float16
                got = ov.decode_substitute('bc6h', blocks, bw * 4, bh * 4, 'medium')        # astcenc
                out.write(struct.pack('<III', bw * 4, bh * 4, bw * bh)); out.write(blocks)
                out.write(np.ascontiguousarray(got).view(np.uint16).tobytes())
                out.write(np.ascontiguousarray(src).view(np.uint16).tobytes())
        else:
            out.write(struct.pack('<III', 2, n, len(recs1)))
            for (_, rw, rh, do1, sz) in recs1:
                bw, bh = (rw + 3) // 4, (rh + 3) // 4
                out.write(struct.pack('<III', bw * 4, bh * 4, bw * bh)); out.write(d1[do1:do1 + sz])
        n += 1
        if n % 200 == 0: print(f'streamed {n} textures', file=sys.stderr, flush=True)
        if limit and n >= limit: break
    out.write(struct.pack('<III', 0, n, 0))
    out.flush()
    print(f'streamed {n} textures', file=sys.stderr)


LDR_CODES = {'bc3': 1, 'bc4': 2, 'bc5': 3, 'bc7': 4}


def cmd_onlyindex(romfs, overlay, fmts):
    """For on-device bisection (DM_GFX_ASTC_ONLY, docs/BUGS_AND_FIXES.md 2.37): OVERLAY/only-<fmt>/base/gameresources_003.pindex, the
    RomFS index with only the records of one BC format pointing at the overlay's re-encoded blocks (in data/base/..._003.patch,
    already on the phone); every other texture keeps its original BC record. 14.7 MB each, nothing else to copy."""
    idx_path = os.path.join(romfs, 'base', ov.PINDEX)
    buf = open(idx_path, 'rb').read()
    entries = ov.parse_index(buf)
    base = os.path.join(romfs, 'base')
    recs = moved_records(romfs, overlay)
    fmt_of = {}
    for ko, kn in recs:
        lay = ov.bim_layout(read_from(base, *ko))
        fmt_of[ko] = ov.FORMATS[lay[0]][0] if lay else None
    for f in fmts:
        moved = {(ko[0], ko[1], ko[3]): kn for ko, kn in recs if fmt_of[ko] == f}
        data = ov.rewrite_index(buf, entries, moved)
        out = os.path.join(overlay, 'only-' + f, 'base'); os.makedirs(out, exist_ok=True)
        open(os.path.join(out, ov.PINDEX), 'wb').write(data)
        print(f'{out}/{ov.PINDEX}: {len(moved)} records of {f} point at the overlay ({len(data)} bytes)')


def cmd_ldrstream(romfs, overlay, limit, only):
    """The LDR substitutes for tools/astc_ldr_scan.cpp ('AHL1'): per texture its format, name and per mip / face the source BC
    blocks, the substitute blocks (ASTC 4x4 LDR / EAC R11 / EAC RG11) and, for BC3 / BC7, the source decoded to RGBA8 by
    texture2ddecoder (BC4 / BC5 the scan decodes itself). The scan decodes the substitutes with its own spec decoders, so neither
    astcenc nor etcpak nor texture2ddecoder's ETC / ASTC decoders are trusted."""
    base, obase = os.path.join(romfs, 'base'), os.path.join(overlay, 'data', 'base')
    out = sys.stdout.buffer
    out.write(b'AHL1')
    names = {}
    n = 0
    for ko, kn in moved_records(romfs, overlay, names):
        d0 = read_from(base, *ko)
        lay = ov.bim_layout(d0)
        if lay is None: continue
        name = ov.FORMATS[lay[0]][0]
        if name not in LDR_CODES or (only and name not in only): continue
        d1 = read_from(obase, *kn)
        recs0, recs1 = lay[5], ov.bim_layout(d1)[5]
        nm = names[ko].encode('latin1')[:255]
        out.write(struct.pack('<III', LDR_CODES[name], n, len(recs0))); out.write(bytes([len(nm)])); out.write(nm)
        out.write(struct.pack('<IIII', lay[1], lay[2], lay[3], lay[4]))
        for (_, rw, rh, do0, sz), (_, _, _, do1, _) in zip(recs0, recs1):
            bw, bh = (rw + 3) // 4, (rh + 3) // 4
            out.write(struct.pack('<IIII', bw * 4, bh * 4, bw * bh, sz)); out.write(d0[do0:do0 + sz]); out.write(d1[do1:do1 + sz])
            if name in ('bc3', 'bc7'):
                out.write(ov.decode_bc(name, d0[do0:do0 + sz], bw * 4, bh * 4).tobytes())
        n += 1
        if n % 200 == 0: print(f'streamed {n} textures', file=sys.stderr, flush=True)
        if limit and n >= limit: break
    out.write(struct.pack('<III', 0, n, 0))
    out.flush()
    print(f'streamed {n} textures', file=sys.stderr)


# ---------------------------------------------------------------- self-test vectors
def bc6h_mode11_block(e0, e1):
    """A BC6H mode 10 (raw 10-bit endpoints, one subset, 4-bit indices: code 0b00011) block with all indices = texel ramp 0..15.
    e0 / e1: (r, g, b) 10-bit unsigned endpoint values (unquantized as the decoder does)."""
    bits = []
    def put(v, n):
        for i in range(n): bits.append((v >> i) & 1)
    put(0b00011, 5)
    put(e0[0], 10); put(e0[1], 10); put(e0[2], 10)
    put(e1[0], 10); put(e1[1], 10); put(e1[2], 10)
    # 63 index bits: anchor (texel 0) has 3 bits, the others 4
    idx = list(range(16))
    put(idx[0] & 7, 3)
    for i in range(1, 16): put(idx[i], 4)
    assert len(bits) == 128
    v = 0
    for i, b in enumerate(bits): v |= b << i
    return v.to_bytes(16, 'little')


def synthetic_blocks():
    """(label, 16-byte BC6H block) with extreme values: the brightest BC6H can hold (65504), ramps across the range, tiny values."""
    out = []
    M = 1023
    out.append(('bc6h max 65504 flat', bc6h_mode11_block((M, M, M), (M, M, M))))
    out.append(('bc6h ramp 0..65504', bc6h_mode11_block((0, 0, 0), (M, M, M))))
    out.append(('bc6h ramp per channel', bc6h_mode11_block((M, 0, 512), (0, M, 900))))
    out.append(('bc6h near max', bc6h_mode11_block((1000, 1010, 1020), (M, M, M))))
    out.append(('bc6h tiny', bc6h_mode11_block((0, 0, 0), (2, 3, 1))))
    out.append(('bc6h small ramp', bc6h_mode11_block((1, 1, 1), (40, 60, 80))))
    out.append(('bc6h zero', bc6h_mode11_block((0, 0, 0), (0, 0, 0))))
    return out


def astc_cem11_block(v, weights):
    """A hand-built ASTC 4x4 block: one partition, endpoint mode 11 (HDR RGB direct) with the 6 colour values v (range 256),
    a 4x4 weight grid of range 4 (block mode 0x42), weights 0..3 per texel. No encoder makes these; they pin the decoder's
    behaviour at the top of the LNS range (endpoint exponent 31: the spec clamps the result to 0x7BFF, a decoder without the clamp
    returns Inf / NaN)."""
    bits = [0] * 128
    def put(at, val, n):
        for i in range(n): bits[at + i] = (val >> i) & 1
    put(0, 0x42, 11); put(11, 0, 2); put(13, 11, 4)
    for i, x in enumerate(v): put(17 + 8 * i, x, 8)
    for i, w in enumerate(weights):              # the weight stream runs from bit 127 downwards
        for j in range(2): bits[127 - (2 * i + j)] = (w >> j) & 1
    val = 0
    for i, b in enumerate(bits): val |= b << i
    return val.to_bytes(16, 'little')


def constructed_blocks():
    ramp = [i % 4 for i in range(16)]
    return [
        # majcomp 3 (direct 12-bit endpoints): e1 = (0xFF0, 0xFE0, 0xFE0) -> LNS 0xFF00 / 0xFE00 (exponent 31: clamped to 65504)
        ('LNS exponent 31 (clamp to 0x7BFF)', astc_cem11_block([0x80, 0xFF, 0x70, 0xFE, 0x80 | 0x30, 0x80 | 0x7F], ramp)),
        # just below: e1 = (0xF70, 0xF60, 0xF60) -> the largest finite values without the clamp
        ('LNS just below exponent 31', astc_cem11_block([0x60, 0xF7, 0x50, 0xF6, 0x80 | 0x20, 0x80 | 0x7B], ramp)),
    ]


def astc_hdr_decode(blk):
    np = ov._numpy()
    got = ov.decode_substitute('bc6h', blk, 4, 4, 'medium')
    return np.ascontiguousarray(got).view(np.uint16).reshape(16, 4)


def cmd_header(picks_path, out_path):
    np = ov._numpy()
    vecs = []   # (label, astc block bytes)
    data = open(picks_path, 'rb').read()
    if data[:4] != b'AHP1': raise SystemExit('not a picks file of astc_hdr_scan')
    n = struct.unpack_from('<I', data, 4)[0]
    at = 8
    for _ in range(n):
        ln = data[at]; label = data[at + 1:at + 1 + ln].decode(); at += 1 + ln
        vecs.append(('real: ' + label, data[at:at + 16])); at += 16
    for label, bc in synthetic_blocks():
        img = ov.decode_bc('bc6h', bc, 4, 4)
        blk = ov.encode('bc6h', img, 'medium')
        vecs.append(('encoded: ' + label, blk))
    for label, blk in constructed_blocks():
        vecs.append(('constructed: ' + label, blk))
    lines = []
    lines.append('#pragma once')
    lines.append('// Generated by tools/astc_hdr_selftest_gen.py header (docs/BUGS_AND_FIXES.md 2.37), do not edit. ASTC 4x4 HDR test blocks of the')
    lines.append('// BC6H -> ASTC overlay self-test (xbg_astc_hdr.h): real blocks of work/astc_overlay_out picked by tools/astc_hdr_scan.cpp')
    lines.append('// (every endpoint mode / partition count / plane layout, the brightest and darkest texels, void extents) and blocks')
    lines.append('// encoded with the overlay\'s settings (astcenc 4x4 HDR_RGB_LDR_A -medium) from BC6H blocks with extreme values, plus two')
    lines.append('// hand-built blocks at the top of the LNS range (exponent 31: the spec clamps to 0x7BFF; without the clamp: Inf / NaN).')
    lines.append('// expected: FP16 bits per texel (row-major 4x4) and channel RGBA, from astcenc; engine/tests/astc_hdr_test.cpp decodes')
    lines.append('// every block again with the independent decoder xbg_astc_ref_decode.h and requires the same bits.')
    lines.append('#include <stdint.h>')
    lines.append(f'enum {{ XBG_ASTC_HDR_TEST_BLOCKS = {len(vecs)} }};')
    lines.append('static const struct { const char *label; uint8_t block[16]; uint16_t expected[16][4]; } xbg_astc_hdr_tests[XBG_ASTC_HDR_TEST_BLOCKS] = {')
    for label, blk in vecs:
        exp = astc_hdr_decode(blk)
        if not np.all(np.isfinite(exp.view(np.float16))): raise SystemExit(f'{label}: astcenc decodes it to Inf / NaN')
        b = ','.join(f'0x{x:02x}' for x in blk)
        e = ','.join('{' + ','.join(f'0x{int(v):04x}' for v in exp[i]) + '}' for i in range(16))
        lines.append(f'    {{"{label}", {{{b}}},')
        lines.append(f'     {{{e}}}}},')
    lines.append('};')
    open(out_path, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print(f'{out_path}: {len(vecs)} blocks')


def main(argv):
    if len(argv) >= 2 and argv[0] == 'formats': return cmd_formats(argv[1])
    if len(argv) >= 3 and argv[0] == 'stream':
        limit = int(argv[argv.index('--limit') + 1]) if '--limit' in argv else 0
        return cmd_stream(argv[1], argv[2], limit, '--ldr' in argv)
    if len(argv) >= 3 and argv[0] == 'header': return cmd_header(argv[1], argv[2])
    if len(argv) >= 4 and argv[0] == 'onlyindex': return cmd_onlyindex(argv[1], argv[2], argv[3].split(','))
    if len(argv) >= 3 and argv[0] == 'ldrstream':
        limit = int(argv[argv.index('--limit') + 1]) if '--limit' in argv else 0
        only = argv[argv.index('--only') + 1].split(',') if '--only' in argv else None
        return cmd_ldrstream(argv[1], argv[2], limit, only)
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]) or 0)
