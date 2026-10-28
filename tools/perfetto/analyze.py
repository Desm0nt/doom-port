"""Summarize a DOOM perfetto trace: GPU / CPU frequency, per-thread run / runnable / sleep time of the game process.
Usage: python tools/perfetto/analyze.py work/logs/NAME/doom.pftrace [process-name-substring]"""
import sys
from perfetto.trace_processor import TraceProcessor

path = sys.argv[1]
proc = sys.argv[2] if len(sys.argv) > 2 else 'doom'
tp = TraceProcessor(trace=path)

def q(sql):
    return list(tp.query(sql))

span = q("select (max(ts)-min(ts))/1e9 as s from sched")
print('trace seconds', round(span[0].s, 2) if span else '?')

print('\n== GPU frequency (MHz; the counter is kHz), time-weighted')
g=q("""select c.ts, c.value from counter c join counter_track t on c.track_id=t.id where t.name='gpufreq' order by c.ts""")
end=q("select max(ts) e from sched")[0].e
if g:
    tot=0;acc=0;hist={}
    for k,r in enumerate(g):
        nts=g[k+1].ts if k+1<len(g) else end
        d=max(0,nts-r.ts);tot+=d;acc+=d*r.value/1000;hist[int(r.value/1000)]=hist.get(int(r.value/1000),0)+d
    print(f'  avg {acc/max(tot,1):.0f} MHz over {tot/1e9:.2f} s; time share:',', '.join(f'{f}:{100*d/tot:.0f}%' for f,d in sorted(hist.items(),reverse=True)))

print('\n== CPU frequency per cpu (MHz, time-weighted avg)')
for r in q("""with f as (select c.ts, c.value, t.cpu, lead(c.ts) over (partition by t.cpu order by c.ts) nts
                         from counter c join cpu_counter_track t on c.track_id=t.id where t.name='cpufreq')
              select cpu, sum(value*(nts-ts))/sum(nts-ts)/1000 as avg, max(value)/1000 as mx from f where nts is not null group by cpu order by cpu"""):
    print(f'  cpu{r.cpu}: avg {r.avg:.0f} max {r.mx:.0f}')

print(f'\n== Threads of "{proc}" (seconds): Running / Runnable(waiting CPU) / Sleeping / D-state')
rows = q(f"""select t.tid, t.name as tname,
       sum(case when ts.state='Running' then ts.dur else 0 end)/1e9 run,
       sum(case when ts.state in ('R','R+') then ts.dur else 0 end)/1e9 runnable,
       sum(case when ts.state='S' then ts.dur else 0 end)/1e9 sleep,
       sum(case when ts.state in ('D','DK') then ts.dur else 0 end)/1e9 dstate
     from thread_state ts join thread t using(utid) join process p using(upid)
     where p.name like '%{proc}%' group by t.tid order by run desc limit 30""")
for r in rows:
    print(f'  {r.tid:6d} {str(r.tname)[:22]:22s} run {r.run:6.2f}  runnable {r.runnable:6.2f}  sleep {r.sleep:6.2f}  D {r.dstate:5.2f}')

print('\n== Process CPU total (s) and per-CPU spread')
for r in q(f"""select s.cpu, sum(s.dur)/1e9 secs from sched s join thread t using(utid) join process p using(upid)
               where p.name like '%{proc}%' group by s.cpu order by s.cpu"""):
    print(f'  cpu{r.cpu}: {r.secs:.2f}')
