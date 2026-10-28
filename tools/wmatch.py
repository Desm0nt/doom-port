# find a base code window (addr, nwords) in update with loose masking. usage: wmatch.py addr n
import sys, numpy as np
b = open('work/exefs-base/main.raw', 'rb').read(); u = open('work/exefs-update/main.raw', 'rb').read()
def m(w):
    w = w.copy()
    m1 = ((w & 0x9f000000) == 0x90000000) | ((w & 0x9f000000) == 0x10000000); w[m1] &= 0x9f00001f
    m2 = (w & 0x7c000000) == 0x14000000; w[m2] &= 0xfc000000
    m3 = (w & 0xff000010) == 0x54000000; w[m3] &= 0xff00001f
    m4 = ((w & 0x7e000000) == 0x34000000) | ((w & 0x7e000000) == 0x36000000); w[m4] &= 0xff00001f
    m5 = (w & 0x3b000000) == 0x39000000; w[m5] &= 0xffc003ff
    m6 = (w & 0x7fc00000) == 0x29400000; w[m6] &= 0xffc07fff   # ldp/stp signed offset: ignore offset
    return w
a = int(sys.argv[1], 16); n = int(sys.argv[2])
s = m(np.frombuffer(b[a:a + 4 * n], dtype='<u4'))
uw = m(np.frombuffer(u[:len(u) // 4 * 4], dtype='<u4'))
c = np.nonzero(uw[:len(uw) - n] == s[0])[0]
hits = [int(j) * 4 for j in c if (uw[j:j + n] == s).all()]
print(len(hits), [hex(h) for h in hits[:10]])
