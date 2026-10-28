"""Summarize A/B runs: python tools/ab_report.py NAME... (work/logs/ab/NAME). Gameplay PERF samples (draws > 300): median fps, GPU ms,
submit ms, passes; holes = share of pure-black pixels in the G-buffer dump (texture serial 2, RGBA8 1280x720); crash marker."""
import re, sys, os, statistics as st
import numpy as np

for name in sys.argv[1:]:
    d = os.path.join('work/logs/ab', name)
    try: log = open(os.path.join(d, 'logcat.txt'), encoding='utf-8', errors='replace').read()
    except FileNotFoundError: print(name, 'missing'); continue
    rows = []
    for m in re.finditer(r'NVN PERF v=1 .*', log):
        kv = dict(re.findall(r'(\w+)=([0-9.]+)', m.group(0)))
        if float(kv.get('draws', 0)) > 300: rows.append(kv)
    def med(k): return st.median(float(r[k]) for r in rows) if rows else float('nan')
    holes = float('nan')
    g = os.path.join(d, 'gbuf.raw')
    if os.path.exists(g) and os.path.getsize(g) == 1280 * 720 * 4:
        a = np.fromfile(g, np.uint8).reshape(720, 1280, 4)[:, :, :3]
        holes = 100.0 * np.mean(np.all(a == 0, axis=2))
    crash = 'CRASH' if '[dm] CRASH' in log or 'Fatal signal' in log else ''
    print(f'{name:22s} samples {len(rows):2d}  fps {med("fps"):5.1f}  gpu {med("gpu_ms"):5.1f} ms  submit {med("submit_cpu_ms"):5.1f} ms  passes {med("passes"):5.0f}  holes {holes:5.2f}%  {crash}')
