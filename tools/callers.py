# callers of a base/update address via direct bl/b scan. usage: callers.py base|update addr
import sys, json, bisect, numpy as np
w = sys.argv[1]; t = int(sys.argv[2], 16)
raw = open(f'work/exefs-{w}/main.raw', 'rb').read()
a = np.frombuffer(raw[:len(raw)//4*4], dtype='<u4')
idx = np.arange(len(a), dtype=np.int64)
isbl = (a & 0x7c000000) == 0x14000000
imm = (a & 0x03ffffff).astype(np.int64); imm = np.where(imm & 0x02000000, imm - 0x04000000, imm)
hit = np.nonzero(isbl & ((idx + imm) * 4 == t))[0]
fn = []
if w == 'base':
    S = json.load(open('work/exefs-base/main.symbols.json'))['exports']
    fn = sorted((x['value'], x['name']) for x in S if x['type'] == 2 and x['value'])
fv = [v for v, _ in fn]
for h in hit:
    x = int(h) * 4; s = ''
    if fn: i = bisect.bisect_right(fv, x) - 1; s = f'{fn[i][1][:100]}+{x - fn[i][0]:#x}'
    print(hex(x), 'bl' if a[h] >> 31 else 'b', s)
