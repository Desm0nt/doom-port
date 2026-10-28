"""Convert xbg debug frame dumps (<serial>_n<nvn>_v<vk>_<w>x<h>x<d>.raw) to PNGs. Usage: dump2png.py <dir>
Float formats are shown as-is clamped to [0,1] and also with a simple x/(1+x) tonemap; 3D textures as a strip of slices."""
import os, re, sys, struct
import numpy as np
from PIL import Image

def f11(v):   # 6-bit mantissa / 5-bit exponent unsigned float
    e = (v >> 6) & 31; m = v & 63
    return np.where(e == 0, m / 64.0 * 2.0 ** -14, np.where(e == 31, 65504.0, (1 + m / 64.0) * 2.0 ** (e.astype(np.int32) - 15)))
def f10(v):
    e = (v >> 5) & 31; m = v & 31
    return np.where(e == 0, m / 32.0 * 2.0 ** -14, np.where(e == 31, 65504.0, (1 + m / 32.0) * 2.0 ** (e.astype(np.int32) - 15)))

def decode(vk, raw, n):
    a = np.frombuffer(raw, dtype=np.uint8)
    if vk in (37, 43, 44, 50):             # RGBA8 unorm / srgb, BGRA8
        p = a.reshape(n, 4).astype(np.float32) / 255
        return p[:, [2, 1, 0, 3]] if vk in (44, 50) else p, False
    if vk == 64:                           # A2B10G10R10 unorm
        v = np.frombuffer(raw, dtype='<u4')
        return np.stack([(v & 1023) / 1023, ((v >> 10) & 1023) / 1023, ((v >> 20) & 1023) / 1023, (v >> 30) / 3], 1), False
    if vk == 122:                          # B10G11R11 ufloat
        v = np.frombuffer(raw, dtype='<u4')
        return np.stack([f11(v & 2047), f11((v >> 11) & 2047), f10(v >> 22), np.ones(n)], 1), True
    if vk == 97:                           # RGBA16F
        return np.frombuffer(raw, dtype='<f2').reshape(n, 4).astype(np.float32), True
    if vk == 83:                           # RG16F
        p = np.frombuffer(raw, dtype='<f2').reshape(n, 2).astype(np.float32)
        return np.concatenate([p, np.zeros((n, 1)), np.ones((n, 1))], 1), True
    if vk == 76:                           # R16F
        p = np.frombuffer(raw, dtype='<f2').astype(np.float32)
        return np.stack([p, p, p, np.ones(n)], 1), True
    if vk == 100:                          # R32F
        p = np.frombuffer(raw, dtype='<f4')
        return np.stack([p, p, p, np.ones(n)], 1), True
    if vk == 109:                          # RGBA32F
        return np.frombuffer(raw, dtype='<f4').reshape(n, 4), True
    if vk == 9:                            # R8
        p = a.astype(np.float32) / 255
        return np.stack([p, p, p, np.ones(n)], 1), False
    if vk == 16:                           # RG8
        p = a.reshape(n, 2).astype(np.float32) / 255
        return np.concatenate([p, np.zeros((n, 1)), np.ones((n, 1))], 1), False
    return None, False

d = sys.argv[1]
for f in sorted(os.listdir(d)):
    m = re.match(r'(\d+)_n([0-9a-f]+)_v(\d+)_(\d+)x(\d+)x(\d+)(_present)?\.raw$', f)
    if not m: continue
    vk, w, h, z = int(m[3]), int(m[4]), int(m[5]), int(m[6])
    raw = open(os.path.join(d, f), 'rb').read()
    px, hdr = decode(vk, raw, w * h * z)
    if px is None: print('skip', f, 'vk', vk); continue
    px = np.nan_to_num(np.asarray(px, dtype=np.float32), nan=0, posinf=1e4)
    stats = 'min %s max %s mean %s' % (np.round(px[:, :3].min(0), 3), np.round(px[:, :3].max(0), 3), np.round(px[:, :3].mean(0), 3))
    rgb = px[:, :3] / (1 + px[:, :3]) if hdr else px[:, :3]
    img = (np.clip(rgb, 0, 1) * 255).astype(np.uint8).reshape(z, h, w, 3)
    img = np.concatenate(list(img), axis=1) if z > 1 else img[0]
    Image.fromarray(img).save(os.path.join(d, f[:-4] + '.png'))
    print(f, stats)
