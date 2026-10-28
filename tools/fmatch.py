# Map base-game (v0, with symbols) functions to the update build (stripped) by masked instruction match.
# Usage: python tools/fmatch.py <regex on mangled/demangled name> [max]
import json, re, struct, sys, bisect
B = 'work/exefs-base/main.raw'; U = 'work/exefs-update/main.raw'
b = open(B, 'rb').read(); u = open(U, 'rb').read()
syms = sorted((x['value'], x['name']) for x in json.load(open('work/exefs-base/main.symbols.json'))['exports'] if x['type'] == 2 and x['value'])

def mask(w):
    if (w & 0x9f000000) in (0x90000000, 0x10000000): return w & 0x9f00001f      # adrp / adr
    if (w & 0x7c000000) == 0x14000000: return w & 0xfc000000                     # b / bl
    if (w & 0xff000010) == 0x54000000: return w & 0xff00001f                     # b.cond
    if (w & 0x7e000000) in (0x34000000, 0x36000000): return w & 0xff00001f       # cbz/cbnz/tbz/tbnz
    if (w & 0x3b000000) == 0x39000000 and (w >> 22) & 3 == 1: return w & 0xffc0001f if False else w  # ldr/str uimm: keep
    return w

def sig(buf, a, n):
    return [mask(struct.unpack_from('<I', buf, a + 4 * i)[0]) for i in range(n)]

# index update by masked first 3 words (aligned 4)
import os; LOOSE = os.environ.get('LOOSE') == '1'
rx = re.compile(sys.argv[1]); mx = int(sys.argv[2]) if len(sys.argv) > 2 else 20
want = [(v, n) for v, n in syms if rx.search(n)][:mx]
if not want: sys.exit('no match')
import numpy as np
uw = np.frombuffer(u[:len(u) // 4 * 4], dtype='<u4')
def masked_arr(w):
    w = w.copy()
    m1 = ((w & 0x9f000000) == 0x90000000) | ((w & 0x9f000000) == 0x10000000); w[m1] &= 0x9f00001f
    m2 = (w & 0x7c000000) == 0x14000000; w[m2] &= 0xfc000000
    m3 = (w & 0xff000010) == 0x54000000; w[m3] &= 0xff00001f
    m4 = ((w & 0x7e000000) == 0x34000000) | ((w & 0x7e000000) == 0x36000000); w[m4] &= 0xff00001f
    if LOOSE: m5 = (w & 0x3b000000) == 0x39000000; w[m5] &= 0xffc003ff   # ldr/str unsigned offset: ignore the offset (vtable slots move)
    return w
um = masked_arr(uw)
for v, n in want:
    i = bisect.bisect_right([s for s, _ in syms], v)
    size = (syms[i][0] - v) if i < len(syms) else 64
    k = max(4, min(size // 4, 24))
    s = masked_arr(np.array(sig(b, v, k), dtype='<u4'))
    c = np.nonzero(um[:len(um) - k] == s[0])[0]
    hits = [int(j) * 4 for j in c if (um[j:j + k] == s).all()]
    print(f'{v:#09x} -> {",".join(hex(h) for h in hits[:4]) or "?"}{" (+%d)" % (len(hits) - 4) if len(hits) > 4 else ""}  k={k}  {n[:110]}')
