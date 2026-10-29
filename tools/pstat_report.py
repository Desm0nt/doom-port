#!/usr/bin/env python3
"""Summarize driver pipeline statistics (NVN PSTAT lines, VK_KHR_pipeline_executable_properties).

Enable on device with dm/env `DM_GFX_PIPE_STATS=1` (= "3000 1800": a 60-frame usage window at present 3000 and every
1800 presents after it) or `DM_GFX_PIPE_STATS="<from> <period>"`; the older marker file gfx-pipe-stats also works.

  python tools/pstat_report.py work/logs/play/<run>/logcat.txt [--top 25] [--stage fragment]

Prints, for the pipelines with the most draws per frame (max over windows), the statistics of the executable whose name
contains --stage (default: fragment; Qualcomm names executables by stage), so register count, instruction counts,
max waves/invocations and spills of the hottest shaders sit side by side.
"""
import argparse, collections, re, sys

USE = re.compile(r'NVN PSTAT use pipeline=(\S+) draws_per_frame=([\d.]+)(?: window=\d+)? what=(.*)')
STAT = re.compile(r'NVN PSTAT pipeline=(\S+) what=(.*?) exec=(\d+) stages=(\S+) name=(\S+) subgroup=(\d+)(.*)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--stage', default='frag')
    a = ap.parse_args()
    draws = collections.defaultdict(float); what = {}; stats = collections.defaultdict(list)
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = USE.search(line)
        if m:
            p = m.group(1); draws[p] = max(draws[p], float(m.group(2))); what[p] = m.group(3).strip(); continue
        m = STAT.search(line)
        if m:
            kv = dict(x.split('=', 1) for x in m.group(7).split() if '=' in x)
            lst = stats[m.group(1)]
            if lst and lst[-1][0] == m.group(5) and not (set(kv) & set(lst[-1][3])):
                lst[-1][3].update(kv)   # continuation chunk of the same executable (lines are split at ~700 chars)
            else:
                lst.append((m.group(5), int(m.group(4), 16), int(m.group(6)), kv))
            what.setdefault(m.group(1), m.group(2))
    if not stats:
        print('no NVN PSTAT lines (DM_GFX_PIPE_STATS not set, or the driver lacks VK_KHR_pipeline_executable_properties)')
        return 1
    order = sorted(stats, key=lambda p: -draws.get(p, 0))
    keys = collections.Counter()
    for p in order[:a.top]:
        for name, _, _, kv in stats[p]:
            if a.stage.lower() in name.lower():
                keys.update(kv.keys())
    pref = ['Instruction_count_all', 'ALU_instruction_count_32bit', 'ALU_instruction_count_16bit', 'Texture_read_instruction_count',
            'Overall_register_footprint_per_shader_instance', 'Half_precision_register_footprint_per_shader_instance', 'Scratch_memory_usage_per_shader_instance']
    cols = [k for k in pref if k in keys] + [k for k, _ in keys.most_common(30) if k not in pref and ('wave' in k.lower() or 'invoc' in k.lower() or 'spill' in k.lower())]
    if not cols: cols = [k for k, _ in keys.most_common(10)]
    print('draws/f  ' + '  '.join(f'{c[:14]:>14}' for c in cols) + '  shaders')
    for p in order[:a.top]:
        ex = [e for e in stats[p] if a.stage.lower() in e[0].lower()] or stats[p][-1:]
        name, _, sg, kv = ex[0]
        print(f'{draws.get(p, 0):7.1f}  ' + '  '.join(f'{kv.get(c, "-")[:14]:>14}' for c in cols) + f'  sg={sg} {what.get(p, "")[:110]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
