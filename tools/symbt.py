"""Symbolize 'bt:' lines (sdk+0x.., main+0x..) from a dmhost log using MOD0 symbols (sdk exports; main: base v0 exports)."""
import bisect, json, re, sys
from pathlib import Path
W = (Path(__file__).resolve().parents[1] / 'work').as_posix()
tabs = {}
for mod, path in (('sdk', f'{W}/exefs-update/sdk.symbols.json'), ('main', f'{W}/exefs-update/main.symbols.json')):
    ex = sorted((x['value'], x['name']) for x in json.load(open(path))['exports'] if x['type'] == 2 and x['value'])
    tabs[mod] = ([v for v, _ in ex], [n for _, n in ex])
def sym(mod, off):
    vs, ns = tabs[mod]
    i = bisect.bisect_right(vs, off) - 1
    return f'{ns[i]}+{off - vs[i]:#x}' if i >= 0 and mod == 'sdk' else f'{mod}+{off:#x}'
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    if 'bt:' in line or 'Break' in line or 'UNIMPL' in line:
        print(line.rstrip())
        for m, o in re.findall(r'(sdk|main)\+0x([0-9a-f]+)', line):
            print('    ', m, '+0x' + o, sym(m, int(o, 16)))
