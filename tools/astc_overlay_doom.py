#!/usr/bin/env python3
"""DOOM Mobile: ASTC / ETC2 / EAC texture overlay for phones without BC textures (Mali, PowerVR, Xclipse). Run by setup.

  astc_overlay_doom.py ROMFS OUT [--jobs N] [--quality medium] [--store] [--force] [--check]

The game's file textures (BIM v7 records inside base/gameresources*.resources / *.patch) are BC1/3/4/5/6H/7. GPUs without
BC sampled them only after decoding to uncompressed formats (4-8x the VRAM; docs/BUGS_AND_FIXES.md 2.24). This tool re-encodes
every BC block stream once, on the PC, into a format of the SAME block size, so the byte layout of each texture does not change:

  BC3, BC7 -> ASTC 4x4 (LDR)          16 bytes / 4x4      astcenc (astc-encoder-py), -medium; -thorough below 42 dB
  BC6H     -> ASTC 4x4 HDR            16 bytes / 4x4      astcenc, HDR RGB + LDR alpha, -medium (cube maps only)
  BC1      -> ETC2 RGB8               8 bytes / 4x4       etcpak, only with --bc1-etc2 (median 36 dB: BC1 stays on GPU decode)
  BC4      -> EAC R11                 8 bytes / 4x4       etcpak
  BC5      -> EAC RG11                16 bytes / 4x4      etcpak

The game then loads, swizzles and copies those blocks into its atlases (decal BC7 8192^2, transSort BC5, light BC4) and the
light-probe cube array (BC6H) exactly as before; the engine only creates those images in the matching ASTC / ETC2 / EAC
VkFormat (xbg_astc_overlay.h). The virtual-texture caches are filled at run time by the game's own transcoder with real BC
blocks and stay on the GPU BC decoder; the textures the engine treats as real BC by shape are left untouched here too
(xbg_astc_keep_bc: single-level >= 4096x4096, BC6H that is not a cube map, BC3 arrays).

Output in OUT (by setup: <work>/DOOM Mobile/astc, i.e. Download/DOOM Mobile/astc on the phone; DM_ROMFS_OVERLAY layers):
  data/base/gameresources_003.patch    a byte copy of the RomFS's _003.patch with the re-encoded records appended
  data/base/gameresources_003.pindex   the RomFS index with every re-encoded record pointing there (patch 3)
  nohdr/base/gameresources_003.pindex  the same without BC6H (devices without ASTC HDR keep BC6H -> GPU decode)
  manifest.json                        written last: version, encoders and settings, counts, sizes, source identity
  quality.tsv                          per texture: format, size, PSNR of mip 0 against the decoded BC (HDR: log2 RMSE)
The appended records use the game's own container format (raw deflate with a sync-flush tail like the originals, or stored
when that is not smaller); the game opens the very same file names as before, nothing else in the RomFS changes.

The RomFS is only read. The output is deterministic for the same input, encoders and zlib (work is spread over --jobs worker
processes, results are written in a fixed order). An interrupted run resumes from its journal (OUT/.journal); a finished one
is skipped while manifest.json matches this script (its SHA-256), the encoder versions, the settings and the source RomFS.
--force rebuilds, --check only reports whether OUT is up to date (exit 0) or not (exit 1).
Dependencies (pinned in tools/requirements-astc.txt, installed by setup into its private venv): astc-encoder-py (MIT; ARM
astc-encoder, Apache-2.0), etcpak (MIT; wolfpld/etcpak, BSD-3-Clause), texture2ddecoder (MIT), numpy (BSD-3-Clause).
"""
import argparse, hashlib, json, os, struct, sys, time, zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

OVERLAY_FORMAT, OVERLAY_VERSION = 'doom-astc-overlay', 1
CONTAINERS = {0: 'gameresources.resources', 1: 'gameresources.patch', 2: 'gameresources_002.patch', 3: 'gameresources_003.patch'}
PINDEX = 'gameresources_003.pindex'
HOST_PATCH = 3                                  # re-encoded records are appended to a copy of this container
BIM_TYPES = ('image', 'decalatlas', 'transsortatlas', 'lightatlas', 'file')   # index entry types that hold BIM v7 textures
# BIM format code -> (name, BC block bytes, target)
FORMATS = {10: ('bc1', 8, 'etc2'), 11: ('bc3', 16, 'astc'), 22: ('bc6h', 16, 'astc_hdr'), 23: ('bc7', 16, 'astc'),
           24: ('bc4', 8, 'eac_r'), 25: ('bc5', 16, 'eac_rg')}
ORDER = ('bc1', 'bc3', 'bc4', 'bc5', 'bc6h', 'bc7')
# BC1 is not substituted by default: etcpak's ETC2 RGB8 of the decoded BC1 measured a median of 36 dB over the 906 BC1 textures,
# 357 below 35 dB (classic-DOOM pixel art of the secret levels 17-25 dB, dirt masks ~20 dB); BC1 is standalone textures only (no
# atlas), a few MB a level, so it stays on the GPU decoder. --bc1-etc2 turns it on.
DEFAULT_FORMATS = ('bc3', 'bc4', 'bc5', 'bc6h', 'bc7')
RETRY_DB = 42.0                                  # ASTC LDR mip 0 below this at the preset: encoded again at -thorough


def variants_of(formats):
    f = tuple(x for x in ORDER if x in formats)
    return {'data': f, 'nohdr': tuple(x for x in f if x != 'bc6h')}
QUALITY = {'fastest': 0, 'fast': 10, 'medium': 60, 'thorough': 98}
PACKAGES = ('astc-encoder-py', 'etcpak', 'texture2ddecoder', 'numpy')


class Fail(Exception):
    pass


# ---------------------------------------------------------------- index (pindex v5)
def parse_index(buf):
    """id Tech 6 .pindex v5: '\\x05SER', BE size (= file - 0x20) @4, BE entry count @0x20; entry = BE id, 3 x (LE u32 len + str),
    BE u64 off, u32 size, u32 zsize, u32 flags, u8 patch. Returns entries with `at` = byte offset of the off field (21 bytes)."""
    if buf[:4] != b'\x05SER': raise Fail('not an id Tech 6 index (no 05 SER magic)')
    if struct.unpack_from('>I', buf, 4)[0] != len(buf) - 0x20: raise Fail('index size field does not match the file')
    n = struct.unpack_from('>I', buf, 0x20)[0]
    o, out = 0x24, []
    for _ in range(n):
        eid = struct.unpack_from('>I', buf, o)[0]; o += 4
        s = []
        for _ in range(3):
            ln = struct.unpack_from('<I', buf, o)[0]; o += 4
            s.append(buf[o:o + ln].decode('latin1')); o += ln
        off, size, zsize, flags, patch = struct.unpack_from('>QIIIB', buf, o)
        out.append(dict(id=eid, type=s[0], name=s[1], off=off, size=size, zsize=zsize, flags=flags, patch=patch, at=o)); o += 21
    if o != len(buf): raise Fail('trailing bytes after the last index entry')
    return out


def rewrite_index(buf, entries, moved):
    """A copy of the index bytes with the entries whose (patch, off, zsize) is in `moved` pointing at the new record:
    moved[(patch, off, zsize)] = (new patch, new off, new size, new zsize). Same length, everything else byte-identical."""
    out = bytearray(buf)
    for e in entries:
        m = moved.get((e['patch'], e['off'], e['zsize']))
        if m is None: continue
        patch, off, size, zsize = m
        struct.pack_into('>QIIIB', out, e['at'], off, size, zsize, e['flags'], patch)
    return bytes(out)


# ---------------------------------------------------------------- BIM v7
def bim_layout(d):
    """BIM v7: 42-byte header (u32 timestamp, 07 'MIB', BE type (0 2D, 2 cube), width, height, depth, mips, u32, LE format @0x20,
    ...), then per mip and face (mip-major) a BE 20-byte record (mip, face, w, h, size) followed by `size` bytes of linear
    blocks. Returns (format, type, width, height, mips, [(record offset, w, h, data offset, size)]) or None if not BIM v7."""
    if len(d) < 62 or d[4:8] != b'\x07MIB': return None
    typ, w, h, depth, mips = struct.unpack_from('>5I', d, 8)
    fmt = struct.unpack_from('<I', d, 0x20)[0]
    faces = 6 if typ == 2 else 1
    recs, o = [], 42
    for _ in range(mips * faces):
        if o + 20 > len(d): raise Fail('BIM record past the end')
        _, _, rw, rh, sz = struct.unpack_from('>5I', d, o)
        recs.append((o, rw, rh, o + 20, sz)); o += 20 + sz
    if o != len(d): raise Fail(f'BIM size mismatch ({o} != {len(d)})')
    return fmt, typ, w, h, mips, recs


def keep_bc(name, typ, w, h, mips):
    """Mirror of xbg_astc_keep_bc in the engine: shapes the engine leaves on real BC, so their blocks must stay BC."""
    if mips == 1 and w >= 4096 and h >= 4096: return 'vt-cache-shape'
    if name == 'bc6h' and typ != 2: return 'bc6h-not-cube'
    return None


def skeleton_hash(d, recs):
    """Hash of everything in a BIM record except the block data (header + mip records): must not change by re-encoding."""
    h = hashlib.sha1(d[:42])
    for ro, _, _, _, _ in recs: h.update(d[ro:ro + 20])
    return h.hexdigest()


# ---------------------------------------------------------------- BC6H (unsigned) decoder, numpy port of bcdec_bc6h_half
_BC6_FIELDS = {n: i for i, n in enumerate('r0 g0 b0 r1 g1 b1 r2 g2 b2 r3 g3 b3 d'.split())}


def _bc6_reads(spec):
    out = []
    for item in spec.split():
        p = item.split(':')
        out.append((_BC6_FIELDS[p[0]], int(p[1]), int(p[2]) if len(p) > 2 else 0, len(p) > 3))
    return out


# mode code -> (mode index, the reads after the mode bits in stream order as field:bits[:shift[:reversed]]): bcdec.h
BC6_MODES = {
    0b00: (0, 'g2:1:4 b2:1:4 b3:1:4 r0:10 g0:10 b0:10 r1:5 g3:1:4 g2:4 g1:5 b3:1 g3:4 b1:5 b3:1:1 b2:4 r2:5 b3:1:2 r3:5 b3:1:3 d:5'),
    0b01: (1, 'g2:1:5 g3:1:4 g3:1:5 r0:7 b3:1 b3:1:1 b2:1:4 g0:7 b2:1:5 b3:1:2 g2:1:4 b0:7 b3:1:3 b3:1:5 b3:1:4 r1:6 g2:4 g1:6 g3:4 b1:6 b2:4 r2:6 r3:6 d:5'),
    0b00010: (2, 'r0:10 g0:10 b0:10 r1:5 r0:1:10 g2:4 g1:4 g0:1:10 b3:1 g3:4 b1:4 b0:1:10 b3:1:1 b2:4 r2:5 b3:1:2 r3:5 b3:1:3 d:5'),
    0b00110: (3, 'r0:10 g0:10 b0:10 r1:4 r0:1:10 g3:1:4 g2:4 g1:5 g0:1:10 g3:4 b1:4 b0:1:10 b3:1:1 b2:4 r2:4 b3:1 b3:1:2 r3:4 g2:1:4 b3:1:3 d:5'),
    0b01010: (4, 'r0:10 g0:10 b0:10 r1:4 r0:1:10 b2:1:4 g2:4 g1:4 g0:1:10 b3:1 g3:4 b1:5 b0:1:10 b2:4 r2:4 b3:1:1 b3:1:2 r3:4 b3:1:4 b3:1:3 d:5'),
    0b01110: (5, 'r0:9 b2:1:4 g0:9 g2:1:4 b0:9 b3:1:4 r1:5 g3:1:4 g2:4 g1:5 b3:1 g3:4 b1:5 b3:1:1 b2:4 r2:5 b3:1:2 r3:5 b3:1:3 d:5'),
    0b10010: (6, 'r0:8 g3:1:4 b2:1:4 g0:8 b3:1:2 g2:1:4 b0:8 b3:1:3 b3:1:4 r1:6 g2:4 g1:5 b3:1 g3:4 b1:5 b3:1:1 b2:4 r2:6 r3:6 d:5'),
    0b10110: (7, 'r0:8 b3:1 b2:1:4 g0:8 g2:1:5 g2:1:4 b0:8 g3:1:5 b3:1:4 r1:5 g3:1:4 g2:4 g1:6 g3:4 b1:5 b3:1:1 b2:4 r2:5 b3:1:2 r3:5 b3:1:3 d:5'),
    0b11010: (8, 'r0:8 b3:1:1 b2:1:4 g0:8 b2:1:5 g2:1:4 b0:8 b3:1:5 b3:1:4 r1:5 g3:1:4 g2:4 g1:5 b3:1 g3:4 b1:6 b2:4 r2:5 b3:1:2 r3:5 b3:1:3 d:5'),
    0b11110: (9, 'r0:6 g3:1:4 b3:1 b3:1:1 b2:1:4 g0:6 g2:1:5 b2:1:5 b3:1:2 g2:1:4 b0:6 g3:1:5 b3:1:3 b3:1:5 b3:1:4 r1:6 g2:4 g1:6 g3:4 b1:6 b2:4 r2:6 r3:6 d:5'),
    0b00011: (10, 'r0:10 g0:10 b0:10 r1:10 g1:10 b1:10'),
    0b00111: (11, 'r0:10 g0:10 b0:10 r1:9 r0:1:10 g1:9 g0:1:10 b1:9 b0:1:10'),
    0b01011: (12, 'r0:10 g0:10 b0:10 r1:8 r0:2:10:r g1:8 g0:2:10:r b1:8 b0:2:10:r'),
    0b01111: (13, 'r0:10 g0:10 b0:10 r1:4 r0:6:10:r g1:4 g0:6:10:r b1:4 b0:6:10:r'),
}
BC6_BITS = ((10, 7, 11, 11, 11, 9, 8, 8, 8, 6, 10, 11, 12, 16),   # endpoint w precision per mode index
            (5, 6, 5, 4, 4, 5, 6, 5, 5, 6, 10, 9, 8, 4),        # delta precision R / G / B
            (5, 6, 4, 5, 4, 5, 5, 6, 5, 6, 10, 9, 8, 4),
            (5, 6, 4, 4, 5, 5, 5, 5, 6, 6, 10, 9, 8, 4))
# two-subset shapes (bit i = subset of pixel i, row-major) and the anchor pixel of subset 1 (BC6H / BC7 tables)
_SHAPES2 = (0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0,
            0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0,
            0x718E, 0x399C)
_ANCHOR2 = (15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2)
_np = None
_bc6_tables = None


def _numpy():
    global _np
    if _np is None:
        import numpy
        _np = numpy
    return _np


def _bc6_init():
    global _bc6_tables
    if _bc6_tables is None:
        np = _numpy()
        part = np.array([[(_SHAPES2[p] >> i) & 1 for i in range(16)] for p in range(32)], np.int64)
        ib2 = np.array([[2 if i in (0, _ANCHOR2[p]) else 3 for i in range(16)] for p in range(32)], np.int64)
        io2 = 82 + np.concatenate([np.zeros((32, 1), np.int64), np.cumsum(ib2, 1)[:, :-1]], 1)
        ib1 = np.array([3] + [4] * 15, np.int64)
        io1 = 65 + np.concatenate([[0], np.cumsum(ib1)[:-1]])
        modes = {c: (mi, _bc6_reads(s)) for c, (mi, s) in BC6_MODES.items()}
        _bc6_tables = dict(part=part, ib2=ib2, io2=io2, ib1=ib1, io1=io1, modes=modes,
                           w3=np.array([0, 9, 18, 27, 37, 46, 55, 64], np.int64),
                           w4=np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int64))
    return _bc6_tables


def _bits(lo, hi, pos, n):
    """Bits [pos, pos + n) of the 128-bit values (lo, hi) (uint64 arrays); pos / n are scalars or broadcastable arrays."""
    np = _numpy()
    pos = np.asarray(pos, np.int64); n = np.asarray(n, np.int64); z = np.uint64(0)
    v = (np.where(pos < 64, lo >> np.clip(pos, 0, 63).astype(np.uint64), z)
         | np.where((pos > 0) & (pos < 64), hi << np.clip(64 - pos, 1, 63).astype(np.uint64), z)
         | np.where(pos >= 64, hi >> np.clip(pos - 64, 0, 63).astype(np.uint64), z))
    return (v & ((np.uint64(1) << n.astype(np.uint64)) - np.uint64(1))).astype(np.int64)


def bc6h_unsigned_to_half(blocks):
    """(n, 16) uint8 BC6H UF16 blocks -> (n, 16, 3) uint16 binary16 RGB, pixels row-major in the block. Bit-exact with
    bcdec_bc6h_half(isSigned = 0) (the engine's CPU decoder; tools/test_astc_overlay_doom.py), reserved modes give 0."""
    np = _numpy(); T = _bc6_init()
    b = np.ascontiguousarray(blocks, np.uint8).reshape(-1, 16)
    lo = b[:, :8].copy().view('<u8').ravel(); hi = b[:, 8:].copy().view('<u8').ravel()
    out = np.zeros((len(b), 16, 3), np.uint16)
    m2 = (lo & np.uint64(3)).astype(np.int64)
    code = np.where(m2 > 1, (lo & np.uint64(31)).astype(np.int64), m2)
    for c, (mi, reads) in T['modes'].items():
        sel = np.nonzero(code == c)[0]
        if not len(sel): continue
        L, H = lo[sel], hi[sel]
        f = np.zeros((13, len(sel)), np.int64)
        pos = 2 if c < 2 else 5
        for fi, nb, sh, rev in reads:
            v = _bits(L, H, pos, nb); pos += nb
            if rev:
                r = np.zeros_like(v)
                for k in range(nb): r |= ((v >> k) & 1) << (nb - 1 - k)
                v = r
            f[fi] |= v << sh
        two = mi < 10
        ne = 4 if two else 2
        e = [f[0:3].copy(), f[3:6].copy(), f[6:9].copy(), f[9:12].copy()]
        b0 = BC6_BITS[0][mi]
        if mi not in (9, 10):                      # deltas: sign-extend, then add the base endpoint (transform_inverse)
            for i in range(1, ne):
                for ch in range(3):
                    nbits = BC6_BITS[1 + ch][mi]; s = 1 << (nbits - 1)
                    e[i][ch] = ((e[i][ch] & ((s << 1) - 1)) ^ s) - s
                e[i] = (e[i] + e[0]) & ((1 << b0) - 1)
        unq = []
        for i in range(ne):
            v = e[i]
            unq.append(v if b0 >= 15 else np.where(v == 0, 0, np.where(v == (1 << b0) - 1, 0xFFFF, ((v << 16) + 0x8000) >> b0)))
        if two:
            part = f[12]
            idx = _bits(L[:, None], H[:, None], T['io2'][part], T['ib2'][part]); sub = T['part'][part]; w = T['w3'][idx]
        else:
            idx = _bits(L[:, None], H[:, None], T['io1'][None, :], T['ib1'][None, :]); sub = None; w = T['w4'][idx]
        for ch in range(3):
            a = unq[0][ch][:, None]; bb = unq[1][ch][:, None]
            if two:
                a = np.where(sub == 0, a, unq[2][ch][:, None]); bb = np.where(sub == 0, bb, unq[3][ch][:, None])
            val = (a * (64 - w) + bb * w + 32) >> 6
            out[sel, :, ch] = ((val * 31) >> 6).astype(np.uint16)
    return out


def blocks_to_image(px, bw, bh, ch):
    """(bw * bh, 16, ch) pixels in block order -> (bh * 4, bw * 4, ch) image."""
    return px.reshape(bh, bw, 4, 4, ch).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, ch)


# ---------------------------------------------------------------- encoding (worker processes)
_ctx = {}
_files = {}


def _astc(kind, quality):
    key = (kind, quality)
    if key not in _ctx:
        import astc_encoder as ae
        prof = ae.ASTCProfile.HDR_RGB_LDR_A if kind == 'hdr' else ae.ASTCProfile.LDR
        _ctx[key] = (ae, ae.ASTCContext(ae.ASTCConfig(prof, 4, 4, 1, QUALITY[quality]), 1), ae.ASTCSwizzle())
    return _ctx[key]


def decode_bc(name, data, W, H):
    """BC blocks of a W x H (multiples of 4) image -> numpy image: RGBA8 (bc1/3/7), (H, W, 4) uint8 with R / RG in place
    (bc4/5), or (H, W, 4) float16 RGB + alpha 1 (bc6h)."""
    np = _numpy()
    if name == 'bc6h':
        px = bc6h_unsigned_to_half(np.frombuffer(data, np.uint8).reshape(-1, 16))
        img = np.empty((H, W, 4), np.uint16)
        img[..., :3] = blocks_to_image(px, W // 4, H // 4, 3); img[..., 3] = 0x3C00
        return img.view(np.float16)
    import texture2ddecoder as t2d
    fn = {'bc1': t2d.decode_bc1, 'bc3': t2d.decode_bc3, 'bc7': t2d.decode_bc7, 'bc4': t2d.decode_bc4, 'bc5': t2d.decode_bc5}[name]
    bgra = np.frombuffer(fn(data, W, H), np.uint8).reshape(H, W, 4)
    img = np.ascontiguousarray(bgra[..., [2, 1, 0, 3]])
    if name == 'bc4': img[..., 1:3] = 0; img[..., 3] = 255
    if name == 'bc5': img[..., 2] = 0; img[..., 3] = 255
    if name == 'bc1': img[..., 3] = 255          # opaque (ETC2 RGB8); punch-through blocks are counted by the caller
    return img


def encode(name, img, quality):
    """Image from decode_bc -> blocks of the substitute format."""
    W, H = img.shape[1], img.shape[0]
    target = FORMATS[[k for k, v in FORMATS.items() if v[0] == name][0]][2]
    if target in ('astc', 'astc_hdr'):
        ae, ctx, sw = _astc('hdr' if target == 'astc_hdr' else 'ldr', quality)
        im = ae.ASTCImage(ae.ASTCType.F16 if target == 'astc_hdr' else ae.ASTCType.U8, W, H, 1, img.tobytes())
        return ctx.compress(im, sw)
    import etcpak
    fn = {'etc2': etcpak.compress_etc2_rgb, 'eac_r': etcpak.compress_eac_r, 'eac_rg': etcpak.compress_eac_rg}[target]
    out = fn(img.tobytes(), W, H)
    if target == 'eac_r':
        # etcpak 0.9.15 sizes the EAC R11 output like RG11 (16 bytes a block): the blocks are the first half, in order, the rest is
        # unused buffer (tools/test_astc_overlay_doom.py checks the decode of exactly this half)
        n = (W // 4) * (H // 4) * 8
        if len(out) == 2 * n: out = out[:n]
    return out


def decode_substitute(name, blocks, W, H, quality):
    """Substitute-format blocks -> image comparable with decode_bc's (for the quality report and the tests)."""
    np = _numpy()
    target = FORMATS[[k for k, v in FORMATS.items() if v[0] == name][0]][2]
    if target in ('astc', 'astc_hdr'):
        ae, ctx, sw = _astc('hdr' if target == 'astc_hdr' else 'ldr', quality)
        hdr = target == 'astc_hdr'
        im = ctx.decompress(blocks, ae.ASTCImage(ae.ASTCType.F16 if hdr else ae.ASTCType.U8, W, H, 1), sw)
        return np.frombuffer(im.data, np.float16 if hdr else np.uint8).reshape(H, W, 4)
    import texture2ddecoder as t2d
    fn = {'etc2': t2d.decode_etc2, 'eac_r': t2d.decode_eacr, 'eac_rg': t2d.decode_eacrg}[target]
    bgra = np.frombuffer(fn(blocks, W, H), np.uint8).reshape(H, W, 4)
    img = np.ascontiguousarray(bgra[..., [2, 1, 0, 3]])
    if target == 'eac_r': img[..., 1:3] = 0; img[..., 3] = 255
    if target == 'eac_rg': img[..., 2] = 0; img[..., 3] = 255
    return img


def quality_of(name, src, got):
    """PSNR (dB) over the format's channels for LDR, log2-RMSE (stops) of RGB for HDR."""
    np = _numpy()
    if name == 'bc6h':
        a = np.log2(src[..., :3].astype(np.float64) + 1e-3); b = np.log2(got[..., :3].astype(np.float64) + 1e-3)
        return float(np.sqrt(np.mean((a - b) ** 2)))
    ch = {'bc4': 1, 'bc5': 2, 'bc1': 3}.get(name, 4)
    mse = float(np.mean((src[..., :ch].astype(np.float64) - got[..., :ch]) ** 2))
    return 99.0 if mse == 0 else float(10 * np.log10(255.0 ** 2 / mse))


def bc1_punchthrough(data):
    """Number of BC1 blocks in 3-colour mode that use index 3 (transparent black when sampled as BC1 RGBA)."""
    np = _numpy()
    blk = np.frombuffer(data, np.uint8).reshape(-1, 8)
    c0 = blk[:, 0].astype(np.uint16) | (blk[:, 1].astype(np.uint16) << 8)
    c1 = blk[:, 2].astype(np.uint16) | (blk[:, 3].astype(np.uint16) << 8)
    idx = blk[:, 4:8].copy().view('<u4').ravel()
    has3 = np.zeros(len(idx), bool)
    for k in range(16): has3 |= ((idx >> (2 * k)) & 3) == 3
    return int(((c0 <= c1) & has3).sum())


def deflate(raw, level):
    """The containers' own record format: raw deflate ending in a sync flush (00 00 FF FF, no final block); None when not
    smaller than 97% of the input (then the record is stored: size == zsize)."""
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    z = c.compress(raw) + c.flush(zlib.Z_SYNC_FLUSH)
    return z if len(z) < len(raw) * 0.97 else None


def read_record(base, patch, off, size, zsize):
    f = _files.get((base, patch))
    if f is None:
        f = _files[(base, patch)] = open(os.path.join(base, CONTAINERS[patch]), 'rb')
    f.seek(off); z = f.read(zsize)
    if len(z) != zsize: raise Fail(f'{CONTAINERS[patch]}: short read at {off}')
    d = z if size == zsize else zlib.decompressobj(-15).decompress(z)
    if len(d) != size: raise Fail(f'{CONTAINERS[patch]}: record at {off} inflates to {len(d)} bytes, index says {size}')
    return d


def convert_record(d, quality, level, measure=True):
    """One BIM record -> (new record bytes, stats). The output has the same length, header and mip records; only the block
    data of every mip / face is re-encoded."""
    fmt, typ, w, h, mips, recs = bim_layout(d)
    name, bpb, _ = FORMATS[fmt]
    out = bytearray(d)
    st = dict(fmt=name, w=w, h=h, mips=mips, cube=int(typ == 2), px=0, punch=0, q=None, preset=quality)
    for i, (ro, rw, rh, do, sz) in enumerate(recs):
        bw, bh = (rw + 3) // 4, (rh + 3) // 4
        if sz != bw * bh * bpb: raise Fail(f'{name} mip record {rw}x{rh}: {sz} bytes, expected {bw * bh * bpb}')
        data = d[do:do + sz]
        if name == 'bc1': st['punch'] += bc1_punchthrough(data)
        img = decode_bc(name, data, bw * 4, bh * 4)
        enc = encode(name, img, quality)
        if len(enc) != sz: raise Fail(f'{name} -> substitute: {len(enc)} bytes for {sz}')
        out[do:do + sz] = enc
        st['px'] += rw * rh
        if measure and i == 0:
            st['q'] = quality_of(name, img, decode_substitute(name, enc, bw * 4, bh * 4, quality))
    return bytes(out), st


def job(args):
    """Worker: (base, patch, off, size, zsize, quality, level, retry) -> (record bytes as stored, size, stats). An ASTC LDR texture
    whose mip 0 stays below `retry` dB at `quality` is encoded again at -thorough and the better of the two kept (noise LUTs,
    dirt masks, VFX sheets: about one in ten BC7 / BC3 textures; same result whatever the worker count)."""
    base, patch, off, size, zsize, quality, level, retry = args
    d = read_record(base, patch, off, size, zsize)
    st_skel = skeleton_hash(d, bim_layout(d)[5])
    new, st = convert_record(d, quality, level)
    if retry and st['fmt'] in ('bc3', 'bc7') and st['q'] is not None and st['q'] < retry and QUALITY[quality] < QUALITY['thorough']:
        new2, st2 = convert_record(d, 'thorough', level)
        st2['retried'] = 1
        if st2['q'] > st['q']: new, st = new2, st2
        else: st['retried'] = 1
    st['skel'] = st_skel
    z = deflate(new, level) if level else None
    return (z if z is not None else new), len(new), st


# ---------------------------------------------------------------- planning
def bim_head(f, off, size, zsize, n=64):
    f.seek(off)
    if size == zsize: return f.read(min(n, size))
    d = zlib.decompressobj(-15); out = b''; tail = b''
    while len(out) < n:
        c = f.read(4096)
        if not c and not tail: break
        out += d.decompress(tail + c, n - len(out)); tail = d.unconsumed_tail
        if d.eof or not c: break
    return out[:n]


def plan(base, entries, progress=None, formats=ORDER):
    """Unique BC BIM records the tool converts: [(patch, off, size, zsize, name, type, w, h, mips)] and the ones left as BC by
    shape (keep_bc). Formats not in `formats` are left as they are (not substituted by the engine either)."""
    files = {p: open(os.path.join(base, n), 'rb') for p, n in CONTAINERS.items()}
    seen, jobs, skipped = set(), [], {}
    try:
        cand = [e for e in entries if e['type'] in BIM_TYPES and e['size'] >= 62]
        for i, e in enumerate(cand):
            if progress and i % 500 == 0: progress(f'scanning the index: {i} of {len(cand)} texture candidates')
            k = (e['patch'], e['off'], e['zsize'])
            if k in seen: continue
            seen.add(k)
            if e['patch'] not in files: raise Fail(f'index entry {e["name"]} is in container {e["patch"]}, which this tool does not know')
            h = bim_head(files[e['patch']], e['off'], e['size'], e['zsize'])
            if h[4:8] != b'\x07MIB': continue
            typ, w, hh, _, mips = struct.unpack_from('>5I', h, 8)
            fmt = struct.unpack_from('<I', h, 0x20)[0]
            if fmt not in FORMATS: continue
            name = FORMATS[fmt][0]
            if name not in formats: continue
            why = keep_bc(name, typ, w, hh, mips)
            if why: skipped[why] = skipped.get(why, 0) + 1; continue
            jobs.append((e['patch'], e['off'], e['size'], e['zsize'], name, typ, w, hh, mips))
    finally:
        for f in files.values(): f.close()
    # largest first (load balance), ties by position: a fixed order, so the output is the same whatever the worker count
    jobs.sort(key=lambda j: (-j[2], j[0], j[1]))
    return jobs, skipped


# ---------------------------------------------------------------- driver
def sha256_file(p, chunk=1 << 22):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(chunk), b''): h.update(b)
    return h.hexdigest()


def encoder_versions():
    from importlib import metadata
    v = {}
    for p in PACKAGES:
        try: v[p] = metadata.version(p)
        except metadata.PackageNotFoundError: v[p] = None
    v['zlib'] = zlib.ZLIB_RUNTIME_VERSION
    return v


def missing_packages():
    import importlib.util as iu
    return [p for p, m in zip(PACKAGES, ('astc_encoder', 'etcpak', 'texture2ddecoder', 'numpy')) if iu.find_spec(m) is None]


class Line:
    """One updating progress line on a terminal; without one (a log file) a plain line every 15 s."""
    def __init__(self):
        self.t = 0.0
        try: self.tty = sys.stdout.isatty()
        except (AttributeError, ValueError): self.tty = False

    def __call__(self, text, final=False):
        now = time.monotonic()
        if final or now - self.t > (0.5 if self.tty else 15):
            self.t = now
            if self.tty: print(f'\r  {text}    ', end='\n' if final else '', flush=True)
            else: print(f'  {text}', flush=True)


def build_key(tool_sha, versions, settings, source):
    return hashlib.sha256(json.dumps([OVERLAY_VERSION, tool_sha, versions, settings, source], sort_keys=True).encode()).hexdigest()


def overlay_status(out, key):
    """(up to date, why not)."""
    m = Path(out) / 'manifest.json'
    try: man = json.loads(m.read_text())
    except (OSError, ValueError): return False, 'no manifest'
    if man.get('format') != OVERLAY_FORMAT or man.get('version') != OVERLAY_VERSION: return False, 'other format / version'
    if man.get('key') != key: return False, 'made by another tool version, encoder, setting or RomFS'
    for rel, size in man.get('files', {}).items():
        p = Path(out) / rel
        if not p.is_file() or p.stat().st_size != size: return False, f'{rel} missing or of the wrong size'
    return True, ''


def run(romfs, out, jobs_n, quality, level, force, check, limit=None, formats=DEFAULT_FORMATS, retry=RETRY_DB):
    t_start = time.monotonic()
    VARIANTS = variants_of(formats)
    base = os.path.join(romfs, 'base')
    pindex = os.path.join(base, PINDEX)
    for p in [pindex] + [os.path.join(base, n) for n in CONTAINERS.values()]:
        if not os.path.isfile(p): raise Fail(f'not found: {p} (RomFS incomplete?)')
    tool_sha = sha256_file(__file__)
    versions = encoder_versions()
    settings = dict(astc_block='4x4', astc_quality=quality, astc_retry_thorough_below_db=retry, astc_hdr='4x4 HDR_RGB_LDR_A',
                    etc='etcpak etc2_rgb / eac_r / eac_rg', formats=' '.join(VARIANTS['data']), deflate=level, host_patch=HOST_PATCH, limit=limit)
    host = os.path.join(base, CONTAINERS[HOST_PATCH])
    source = dict(pindex=f'base/{PINDEX}', pindex_size=os.path.getsize(pindex), pindex_sha256=sha256_file(pindex),
                  patch=f'base/{CONTAINERS[HOST_PATCH]}', patch_size=os.path.getsize(host))
    key = build_key(tool_sha, versions, settings, source)
    ok, why = overlay_status(out, key)
    if check:
        print(f'astc overlay {out}: ' + ('up to date' if ok else f'not up to date ({why})'))
        return 0 if ok else 1
    if ok and not force:
        print(f'  already up to date ({out})'); return 0
    miss = missing_packages()
    if miss: raise Fail(f'missing Python packages: {", ".join(miss)} (pip install -r tools/requirements-astc.txt)')
    out = Path(out)
    for v in VARIANTS: (out / v / 'base').mkdir(parents=True, exist_ok=True)
    man_path = out / 'manifest.json'
    if man_path.exists(): man_path.unlink()          # never a manifest next to files that are being rewritten
    idx_buf = open(pindex, 'rb').read()
    entries = parse_index(idx_buf)
    bar = Line()
    jobs, skipped = plan(base, entries, bar, VARIANTS['data'])
    if limit: jobs = jobs[:limit]
    total_bytes = sum(j[2] for j in jobs)
    bar(f'{len(jobs)} BC textures to re-encode ({total_bytes / 1e6:.0f} MB of blocks and headers)' +
        (f'; left as BC by shape: {skipped}' if skipped else ''), final=True)

    patch_out = out / 'data' / 'base' / CONTAINERS[HOST_PATCH]
    partial = patch_out.with_name(patch_out.name + '.partial')
    journal = out / '.journal'
    done = []                                        # [(job index, new off, size, zsize, stats)]
    if not force and journal.is_file() and partial.is_file():
        lines = journal.read_text().splitlines()
        if lines and lines[0] == key:
            for ln in lines[1:]:
                try: i, off, sz, zs, st = ln.split('\t', 4); done.append((int(i), int(off), int(sz), int(zs), json.loads(st)))
                except ValueError: break
            end = done[-1][1] + done[-1][3] if done else source['patch_size']
            if partial.stat().st_size < end: done = []
            else:
                with open(partial, 'r+b') as f: f.truncate(end)
                print(f'  resuming: {len(done)} of {len(jobs)} textures already done')
    if not done:
        print(f'  copying {CONTAINERS[HOST_PATCH]} ({source["patch_size"] / 1e6:.0f} MB) as the base of the overlay container', flush=True)
        with open(host, 'rb') as src, open(partial, 'wb') as dst:
            while True:
                b = src.read(1 << 24)
                if not b: break
                dst.write(b)
        journal.write_text(key + '\n')
    if os.path.getsize(partial) < source['patch_size']: raise Fail('the copy of the container is short')

    t0 = time.monotonic()
    nxt = len(done)
    bytes_done = sum(jobs[i][2] for i, *_ in done)
    jf = open(journal, 'a')
    pf = open(partial, 'ab')
    pending = {}
    try:
        with ProcessPoolExecutor(max_workers=jobs_n) as ex:
            submit_at = nxt
            window = max(4, jobs_n * 3)
            while nxt < len(jobs):
                while submit_at < len(jobs) and submit_at - nxt < window:
                    j = jobs[submit_at]
                    pending[submit_at] = ex.submit(job, (base, j[0], j[1], j[2], j[3], quality, level, retry)); submit_at += 1
                blob, size, st = pending.pop(nxt).result()
                if size != jobs[nxt][2]: raise Fail('re-encoded record has another size')
                off = pf.tell()
                pf.write(blob)
                pf.flush()
                done.append((nxt, off, size, len(blob), st))
                jf.write(f'{nxt}\t{off}\t{size}\t{len(blob)}\t{json.dumps(st, sort_keys=True)}\n')
                if nxt % 64 == 0: jf.flush()
                bytes_done += jobs[nxt][2]; nxt += 1
                el = time.monotonic() - t0
                eta = el / max(1, bytes_done) * (total_bytes - bytes_done) if bytes_done else 0
                bar(f'textures {nxt} of {len(jobs)}, {bytes_done / 1e6:.0f} of {total_bytes / 1e6:.0f} MB, {el:.0f} s, about {eta:.0f} s left',
                    final=nxt == len(jobs))
    except BaseException:
        for f in pending.values(): f.cancel()
        raise
    finally:
        jf.close(); pf.close()
    print(f'  encoded in {time.monotonic() - t0:.0f} s', flush=True)

    # check every new record by reading it back: size, and header / mip records identical to the original's
    vb = Line()
    with open(partial, 'rb') as f:
        for k, (i, off, size, zsize, st) in enumerate(done):
            f.seek(off); z = f.read(zsize)
            d = z if size == zsize else zlib.decompressobj(-15).decompress(z)
            if len(d) != size: raise Fail(f'read-back of record {i}: {len(d)} bytes, expected {size}')
            lay = bim_layout(d)
            if lay is None or skeleton_hash(d, lay[5]) != st['skel']: raise Fail(f'read-back of record {i}: header or mip records changed')
            if k % 200 == 0: vb(f'verified {k} of {len(done)} records')
    vb(f'verified {len(done)} of {len(done)} records', final=True)

    moved = {}
    for i, off, size, zsize, st in done:
        p, o, s, z = jobs[i][:4]
        moved[(p, o, z)] = (HOST_PATCH, off, size, zsize, st['fmt'])
    files = {}
    partial.replace(patch_out)
    files[f'data/base/{CONTAINERS[HOST_PATCH]}'] = patch_out.stat().st_size
    for v, fmts in VARIANTS.items():
        mv = {k: m[:4] for k, m in moved.items() if m[4] in fmts}
        data = rewrite_index(idx_buf, entries, mv)
        tgt = out / v / 'base' / PINDEX
        tmp = tgt.with_name(tgt.name + '.tmp'); tmp.write_bytes(data); tmp.replace(tgt)
        files[f'{v}/base/{PINDEX}'] = len(data)
    rewritten = {v: sum(1 for e in entries if (e['patch'], e['off'], e['zsize']) in moved and moved[(e['patch'], e['off'], e['zsize'])][4] in f)
                 for v, f in VARIANTS.items()}

    counts, bc_bytes, px, punch, qual = {}, {}, {}, 0, {}
    with open(out / 'quality.tsv', 'w') as q:
        q.write('index\tformat\twidth\theight\tmips\tcube\tbytes\tquality(dB | hdr log2 rmse)\tpreset\n')
        for i, off, size, zsize, st in done:
            f = st['fmt']; counts[f] = counts.get(f, 0) + 1; bc_bytes[f] = bc_bytes.get(f, 0) + size; px[f] = px.get(f, 0) + st['px']
            punch += st['punch']
            if st['q'] is not None: qual.setdefault(f, []).append(st['q'])
            q.write(f'{i}\t{f}\t{st["w"]}\t{st["h"]}\t{st["mips"]}\t{st["cube"]}\t{size}\t{"" if st["q"] is None else round(st["q"], 3)}\t{st.get("preset", "")}\n')
    med = {f: round(sorted(v)[len(v) // 2], 2) for f, v in qual.items()}
    worst = {f: round(max(v) if f == 'bc6h' else min(v), 2) for f, v in qual.items()}
    if punch: print(f'  WARNING: {punch} BC1 blocks with punch-through alpha became opaque (ETC2 RGB8)')
    secs = round(time.monotonic() - t_start, 1)
    man = dict(format=OVERLAY_FORMAT if not limit else OVERLAY_FORMAT + '-partial', version=OVERLAY_VERSION,
               tool='tools/astc_overlay_doom.py', tool_sha256=tool_sha, key=key, encoders=versions, settings=settings,
               source=source, variants={v: ' '.join(f) for v, f in VARIANTS.items()}, files=files,
               textures=len(done), entries_rewritten=rewritten, counts=counts, bytes_bc=bc_bytes, megapixels={f: round(v / 1e6, 1) for f, v in px.items()},
               quality_median=med, quality_worst=worst, astc_thorough_retries=sum(1 for *_, st in done if st.get('retried')), bc1_punchthrough_blocks=punch, kept_bc_by_shape=skipped, seconds=secs,
               created=time.strftime('%Y-%m-%d %H:%M:%S'),
               # flat keys the engine reads (xbg_astc_overlay.h): what each variant substitutes, files with sizes, source sizes
               engine_formats_data=' '.join(VARIANTS['data']), engine_formats_nohdr=' '.join(VARIANTS['nohdr']),
               engine_files=' '.join(f'{k}={v}' for k, v in sorted(files.items())),
               engine_source=f'{source["pindex"]}={source["pindex_size"]} {source["patch"]}={source["patch_size"]}')
    tmp = out / 'manifest.json.tmp'
    tmp.write_text(json.dumps(man, indent=1, sort_keys=False) + '\n'); tmp.replace(out / 'manifest.json')
    try: journal.unlink()
    except OSError: pass
    total = sum(files.values())
    per = ', '.join(f'{f} {counts.get(f, 0)}' for f in VARIANTS['data'])
    print(f'  {len(done)} textures ({per}); overlay {total / 1e9:.2f} GB; quality median {med}; {secs:.0f} s in all', flush=True)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description='ASTC / ETC2 / EAC overlay of the DOOM RomFS textures for GPUs without BC.')
    ap.add_argument('romfs', help='the extracted RomFS (with base/gameresources_003.pindex)')
    ap.add_argument('out', help='output folder, e.g. "<work>/DOOM Mobile/astc"')
    ap.add_argument('--jobs', '-j', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--quality', choices=sorted(QUALITY), default='medium', help='astcenc preset (default medium)')
    ap.add_argument('--store', action='store_true', help='store the records uncompressed (bigger, no inflate when loading)')
    ap.add_argument('--force', action='store_true', help='rebuild even when up to date (and do not resume)')
    ap.add_argument('--check', action='store_true', help='only report whether OUT is up to date (exit 0) or not (exit 1)')
    ap.add_argument('--bc1-etc2', action='store_true', help='also substitute BC1 with ETC2 RGB8 (off: median 36 dB with etcpak)')
    ap.add_argument('--no-retry', action='store_true', help=f'no -thorough second pass for ASTC textures below {RETRY_DB:g} dB')
    ap.add_argument('--limit', type=int, help='development: only the first N textures (the manifest is then not usable by the engine)')
    a = ap.parse_args(argv)
    return run(a.romfs, a.out, max(1, a.jobs), a.quality, 0 if a.store else 6, a.force, a.check, a.limit,
               DEFAULT_FORMATS + (('bc1',) if a.bc1_etc2 else ()), 0.0 if a.no_retry else RETRY_DB)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print(f'\nastc_overlay_doom: STOP: {e}', file=sys.stderr); sys.exit(2)
    except KeyboardInterrupt:
        print('\ninterrupted; run again to continue where it stopped', file=sys.stderr); sys.exit(130)
