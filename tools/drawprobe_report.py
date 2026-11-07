# Usage: python -I tools/drawprobe_report.py <logcat.txt> [frame]
# Parses "NVN DRAWPROBE" lines (DM_GFX_DRAWPROBE), pairs each depth pre-pass draw (dw=1, df=4 NVN LEQUAL) with the G-buffer
# draw of the same mesh (df=3 NVN EQUAL, same ib/vb/n/bv) in the same frame, and prints the meshes whose G-buffer draw
# passed fewer samples than the pre-pass draw (0 = dropped), grouped by G-buffer VS, with c5 size, gate and ssbo.
import re,sys,collections
rx=re.compile(r'NVN DRAWPROBE f=(\d+) slot=(\d+) i=(\d+) vs=(\S+) ps=(\S+) n=(-?\d+) bv=(-?\d+) inst=(\d+) ib=(\S+) vb=(\S+) pass=(\d+)x(\d+) dt=(\d) dw=(\d) df=(\d+) c5=(\d+)@(\S+) ssbo=(\d) gate=(\S+) samples=(-?\d+)')
rows=[]
for l in open(sys.argv[1],encoding='utf-8',errors='replace'):
    m=rx.search(l)
    if m:
        g=m.groups();rows.append(dict(f=int(g[0]),slot=int(g[1]),i=int(g[2]),vs=g[3][:16],ps=g[4][:16],n=int(g[5]),bv=int(g[6]),inst=int(g[7]),ib=g[8],vb=g[9],w=int(g[10]),h=int(g[11]),dt=int(g[12]),dw=int(g[13]),df=int(g[14]),c5=int(g[15]),c5a=g[16],ssbo=int(g[17]),gate=g[18],s=int(g[19])))
frames=sorted(set(r['f'] for r in rows))
want=int(sys.argv[2]) if len(sys.argv)>2 else None
for f in frames:
    if want is not None and f!=want: continue
    fr=[r for r in rows if r['f']==f]
    print(f'=== frame {f}: {len(fr)} probed draws')
    pre=collections.defaultdict(list);gb=collections.defaultdict(list)
    for r in fr:
        if r['ib']=='0' or r['ib']=='0x0': continue
        k=(r['ib'],r['vb'],r['n'],r['bv'],r['w'],r['h'])
        if r['dt'] and r['dw'] and r['df']==4: pre[k].append(r)
        elif r['dt'] and not r['dw'] and r['df']==3: gb[k].append(r)
        elif r['dt'] and r['df']==8 and not r['dw']: gb[k].append(r)   # DM_GFX_DEBUG_ZALWAYS keeps df in the state; listed for completeness
    stats=collections.defaultdict(lambda:[0,0,0])   # G-buffer VS -> [pairs, dropped (0), partial (<50% of pre)]
    bad=[]
    for k,gs in gb.items():
        if k not in pre: continue
        p=max(x['s'] for x in pre[k])
        for g in gs:
            st=stats[(g['vs'],g['ssbo'])];st[0]+=1
            if p>0 and g['s']==0: st[1]+=1;bad.append((g,pre[k][-1],p))
            elif p>0 and g['s']*2<p: st[2]+=1;bad.append((g,pre[k][-1],p))
    for (vs,ss),(n,z,h) in sorted(stats.items(),key=lambda x:-x[1][0]):
        print(f'  gbuffer vs={vs} ssbo={ss} pairs={n} dropped={z} partial={h}')
    for g,pv,p in sorted(bad,key=lambda x:(x[0]['vs'],x[0]['slot'],x[0]['i'])):
        print(f"    DROP slot={g['slot']} i={g['i']} gvs={g['vs']} gps={g['ps'][:8]} prevs={pv['vs']} n={g['n']} c5={g['c5']}@{g['c5a']} ssbo={g['ssbo']}/{pv['ssbo']} gate={g['gate']} pre={p} g={g['s']}")
    unpaired=[r for k,v in gb.items() if k not in pre for r in v]
    print(f'  G-buffer draws without a pre-pass partner: {len(unpaired)}')
    # every VS that reads its bones as SSBO (or all with "all" as 3rd argument): draws and zero-sample draws per pass kind.
    # A legitimately hidden mesh also passes 0 samples: compare the same VS between an SSBO and a UBO run.
    per=collections.defaultdict(lambda:[0,0])
    for r in fr:
        if r['ssbo'] or (len(sys.argv)>3 and sys.argv[3]=='all') or r['vs'] in {g['vs'] for g,_,_ in bad}:
            q=per[(r['vs'],r['ssbo'],f"{r['w']}x{r['h']}",r['df'],r['dw'])];q[0]+=1;q[1]+=r['s']==0
    print('  per VS (vs ssbo pass df dw): draws zero_sample_draws')
    for k,(n,z) in sorted(per.items()):print('   ',*k,n,z)
