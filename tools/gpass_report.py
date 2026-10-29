#!/usr/bin/env python3
"""Summarize the per-pass GPU profile (NVN GPASS lines) of a logcat.

Enable on device with dm/env `DM_GFX_PASS_PROF=1` (= "300 600 0": from present 300, one frame every 600) or
`DM_GFX_PASS_PROF=300 120 2400` (one frame every 120 presents during presents 300..2700; 0 = no end), or the older marker file dm/gfx-pass-profile.

  python tools/gpass_report.py work/logs/play/<run>/logcat.txt [--top 25] [--min-us 2000]

Prints per profiled frame the totals by class (GS depth pre-pass, shadow, main shading, post, compute) and, over all
frames, the most expensive pass signatures (size, attachments, first/last shader pair). --min-us keeps only frames whose
pass+compute sum is at least that many microseconds (e.g. 20000 for the slow open-scene frames).
"""
import argparse, collections, re, sys

GS_CULL = {'703f1fcc3cda7204', '351c0bfc62457935', '78c87a041d7f7005', '919c10a8430cedb8', 'ee0022fdb45318d5', 'ef97a2735bfdeae6'}
LINE = re.compile(r'NVN GPASS frame=(\d+) slot=(\d+) ring=\d+ i=(\d+) kind=(\w+) us=(\d+) (.*)')

def classify(kind, rest):
    if kind == 'compute':
        return 'compute'
    m = re.match(r'(\d+)x(\d+) att=(\d+) fmt=(\d+) draws=(\d+)', rest)
    w, h, att, fmt, draws = (int(x) for x in m.groups()) if m else (0, 0, 0, 0, 0)
    keys = set(re.findall(r'\d:([0-9a-f]{16})', rest))
    if keys & GS_CULL:
        return 'gs-prepass'   # still running the culling GS (DM_GFX_GS_PASSTHRU off or culling on)
    if w >= 2048 and w == h:
        return 'shadow'
    if att == 1 and w >= 1280 and h >= 720 and draws >= 20:
        return 'gs-prepass'   # the same depth pre-pass with the GS dropped (DM_GFX_GS_PASSTHRU=1)
    if w >= 1280 and h >= 720 and draws >= 20:
        return 'main'
    return 'post/small'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--min-us', type=int, default=0)
    a = ap.parse_args()
    frames = collections.defaultdict(list)
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if m:
            fr, slot, i, kind, us, rest = m.groups()
            frames[int(fr)].append((int(slot), int(i), kind, int(us), rest.strip()))
    if not frames:
        print('no NVN GPASS pass lines (DM_GFX_PASS_PROF not set?)')
        return 1
    sig_us = collections.Counter(); sig_n = collections.Counter(); sig_draws = collections.Counter(); used = 0
    end_n = collections.Counter(); end_us = collections.Counter(); one_draw = collections.Counter()   # perf4: end=L<line> of each pass
    print('frame   total_ms  ' + '  '.join(f'{c:>11}' for c in ('gs-prepass', 'shadow', 'main', 'post/small', 'compute')) + '  passes draws')
    for fr in sorted(frames):
        recs = frames[fr]
        total = sum(r[3] for r in recs)
        if total < a.min_us:
            continue
        used += 1
        cls = collections.Counter(); draws = 0
        for slot, i, kind, us, rest in recs:
            c = classify(kind, rest); cls[c] += us
            m = re.match(r'(\d+x\d+ att=\d+ fmt=\d+) draws=(\d+) verts=\d+ first=(\S+) last=(\S+)', rest)
            sig = f'{kind} {m.group(1)} first={m.group(3)} last={m.group(4)}' if m else f'{kind} {rest[:90]}'
            d = int(m.group(2)) if m else 0; draws += d
            sig_us[sig] += us; sig_n[sig] += 1; sig_draws[sig] += d
            e = re.search(r' end=L(\d+)', rest)
            if e and kind == 'pass':
                end_n[e.group(1)] += 1; end_us[e.group(1)] += us
                if d <= 1: one_draw[e.group(1)] += 1
        print(f'{fr:6} {total/1000:9.2f}  ' + '  '.join(f'{cls[c]/1000:11.2f}' for c in ('gs-prepass', 'shadow', 'main', 'post/small', 'compute')) + f'  {len(recs):6} {draws:5}')
    if not used:
        print('no frame above --min-us'); return 1
    print(f'\ntop pass signatures over {used} frame(s): mean ms per frame, passes per frame, draws per pass')
    for sig, us in sig_us.most_common(a.top):
        print(f'{us/used/1000:7.2f} ms  x{sig_n[sig]/used:5.1f}  draws/pass {sig_draws[sig]/max(1,sig_n[sig]):6.1f}  {sig[:200]}')
    if end_n:
        print('\npass ends by call site (end=L<line> of this build): passes per frame, of them with <=1 draw, mean ms per frame')
        for line, n in end_n.most_common(12):
            print(f'  L{line:>5}  x{n/used:6.1f}  <=1 draw x{one_draw[line]/used:6.1f}  {end_us[line]/used/1000:6.2f} ms')
    return 0

if __name__ == '__main__':
    sys.exit(main())
