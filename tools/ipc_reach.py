"""Which of main's imports can reach Horizon IPC inside the SDK?

Static call graph of sdk: functions = exported FUNC symbols (sdk exports nearly everything), edges = direct
bl / b targets inside each function. A function is an IPC root if it contains svc 0x1f/0x21/0x22/0x40/0x41
(ConnectToNamedPort / SendSyncRequest(+WithUserBuffer) / CreateSession / AcceptSession). Indirect calls (blr) are
not followed, so the result is a lower bound; together with runtime 'SVC 0x1f/0x21' logs it is enough to decide
which imports need a host takeover. Imports already taken over (HLE_FORCE / HLE names in dm/host) are marked.
Usage: ipc_reach.py  -> work/ipc-reach.tsv
"""
import bisect, glob, json, os, re, struct, collections, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1].as_posix()
W = f'{ROOT}/work'
raw = open(f'{W}/exefs-update/sdk.raw', 'rb').read()
text_end = struct.unpack_from('<I', open(f'{W}/exefs-update/sdk', 'rb').read(), 0x18)[0]
ex = sorted({(x['value'], x['name']) for x in json.load(open(f'{W}/exefs-update/sdk.symbols.json'))['exports']
             if x['type'] == 2 and 0 < x['value'] < text_end})
starts = [v for v, _ in ex]
names = {}
for v, n in ex: names.setdefault(v, n)
starts = sorted(names)
IPC_SVC = {0x1f, 0x21, 0x22, 0x40, 0x41}

def func_of(a):
    if a in plt: return plt[a]
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 else None

# Calls between exported functions go through PLT stubs (adrp x16; ldr x17,[x16,#lo]; add; br x17) -> GOT slot
# -> JUMP_SLOT relocation -> symbol. Resolve stub address -> definition address.
def plt_map():
    mod0 = struct.unpack_from('<I', raw, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', raw, mod0 + 4)[0]
    tags = {}
    o = dyn
    while True:
        t, v = struct.unpack_from('<qQ', raw, o); o += 16
        if t == 0: break
        tags.setdefault(t, v)
    symtab, strtab, jmprel, pltsz = tags[6], tags[5], tags[23], tags[2]
    by_sym = {}
    for i in range(pltsz // 24):
        off, info, _ = struct.unpack_from('<QQq', raw, jmprel + 24 * i)
        nm, _, _, shndx, val, _ = struct.unpack_from('<IBBHQQ', raw, symtab + 24 * (info >> 32))
        if shndx: by_sym[off] = val
    out = {}
    for o in range(0, text_end - 12, 4):
        w0, w1 = struct.unpack_from('<II', raw, o)
        if w0 & 0x9F00001F == 0x90000010 and w1 & 0xFFC003FF == 0xF9400211:   # adrp x16 ; ldr x17,[x16,#imm]
            immlo, immhi = (w0 >> 29) & 3, (w0 >> 5) & 0x7FFFF
            page = ((immhi << 2 | immlo) ^ 0x100000) - 0x100000
            got = (o & ~0xFFF) + (page << 12) + ((w1 >> 10) & 0xFFF) * 8
            if got in by_sym: out[o] = by_sym[got]
    return out
plt = plt_map()

calls = collections.defaultdict(set); roots = set()
for i, s in enumerate(starts):
    e = starts[i + 1] if i + 1 < len(starts) else text_end
    for o in range(s, e, 4):
        w = struct.unpack_from('<I', raw, o)[0]
        if w & 0xFC000000 in (0x94000000, 0x14000000):          # bl / b imm26
            imm = w & 0x3FFFFFF
            if imm & 0x2000000: imm -= 0x4000000
            t = o + 4 * imm
            if 0 <= t < text_end and not (s <= t < e):
                f = func_of(t)
                if f is not None: calls[s].add(f)
        elif w & 0xFFE0001F == 0xD4000001 and (w >> 5) & 0xFFFF in IPC_SVC:
            roots.add(s)
# IPC is reached through virtual service proxies (blr), invisible to the bl/b graph; seed by name instead.
ROOT_NAME = re.compile(r'(2sf\d*4hipc|N2nn2sm|2sm10Initialize|ServiceObject|ShimLibrary|6detail3ipc|HipcSimple|InitializeHipc|GetServiceSession|CreateHipc|4hipc|ProxyObject|ServiceResolution)')
roots |= {v for v in starts if ROOT_NAME.search(names[v])}

# reverse reachability from roots
rev = collections.defaultdict(set)
for a, bs in calls.items():
    for b in bs: rev[b].add(a)
# Abort / assert / log paths reach the lm service but never run in a healthy game: do not propagate through them.
STOP = re.compile(r'(4diag|nndetailDiag|AbortImpl|OnAssertionFailure|LogImpl|__cxa_|_Unwind|terminate)')
for v in starts:
    if STOP.search(names[v]): rev.pop(v, None); roots.discard(v)
for a in list(rev):
    rev[a] = {c for c in rev[a] if not STOP.search(names.get(c, ''))}
reach = set(roots); todo = list(roots)
while todo:
    f = todo.pop()
    for c in rev[f]:
        if c not in reach: reach.add(c); todo.append(c)

src = ''.join(open(f, encoding='utf-8', errors='ignore').read() for f in glob.glob(f'{ROOT}/dm/host/*.c')
              + glob.glob(f'{ROOT}/dm/gfx/**/*.c', recursive=True))
by_name = {n: v for v, n in ex}
rows = []
for x in json.load(open(f'{W}/exefs-update/main.symbols.json'))['imports']:
    n = x['name']; v = by_name.get(n)
    if v is None: continue
    rows.append((n, v in reach, n in src))
dm = subprocess.run([os.environ.get('LLVM_CXXFILT', 'llvm-cxxfilt')],
                    input='\n'.join(r[0] for r in rows), capture_output=True, text=True).stdout.split('\n')
with open(f'{W}/ipc-reach.tsv', 'w', encoding='utf-8') as f:
    f.write('ipc\thle\tmangled\tdemangled\n')
    for (n, ipc, hle), d in zip(rows, dm):
        f.write(f'{int(ipc)}\t{int(hle)}\t{n}\t{d}\n')
need = [(n, d) for (n, ipc, hle), d in zip(rows, dm) if ipc and not hle]
print(f'sdk functions {len(starts)}, IPC roots {len(roots)}, reaching IPC {len(reach)}')
print(f'main imports resolved by sdk {len(rows)}; reach IPC {sum(r[1] for r in rows)}; of those not taken over: {len(need)}')
for n, d in need: print('  ', d)
