# Usage: python -I tools/capture_zdelta.py <capture dir> <vs prefix>[,<vs prefix>...]
# For a crop capture made with DM_GFX_DEBUG_ZALWAYS + DM_GFX_DEBUG_ZWRITE on the listed VS: for every draw of those VS, the
# depth crop (rt-1, D24S8: low 24 bits) before and after the draw. Prints how many pixels it rasterized (changed) and the
# histogram of (new - old) in D24 units, i.e. its own depth against the pre-pass depth of the same pixels (0 = equal).
import sys,os,struct,collections,re
# capture.txt reader (FILE / DRAW / PASS lines of a gfx-capture-now crop capture)
def load(d):
    evs=[];ev=None
    for l in open(os.path.join(d,'capture.txt'),encoding='utf-8',errors='replace'):
        if l.startswith('EVENT'):
            ev={'line':l.strip(),'vs':'','ps':'','n':0,'out':{},'pass':'','bufs':{},'tex':{},'attrs':[],'num':0};evs.append(ev)
        elif ev is None:continue
        elif l.startswith('DRAW'):
            m=re.search(r'number=(\d+) vs=(\w+) ps=(\w+).*count=(\d+)',l);ev['num']=int(m.group(1));ev['vs'],ev['ps'],ev['n']=m.group(2),m.group(3),int(m.group(4))
            ev['draw']=l.strip()
        elif l.startswith('PASS'):ev['pass']=l.strip()
        elif l.startswith('BUFFER'):
            d_=dict(re.findall(r'(\w+)=(\S+)',l));ev['bufs'][d_['label']]=d_
        elif l.startswith('TEXTURE'):
            d_=dict(re.findall(r'(\w+)=(\S+)',l));ev['tex'][d_['label']]=d_
        elif l.startswith('ATTRIBUTE'):ev['attrs'].append(l.strip())
        elif l.startswith('FILE'):
            fn=re.search(r'name=(\S+)',l).group(1)
            p=os.path.join(d,fn)
            m=re.search(r'-e\d+-(.*)\.bin',fn)
            if os.path.exists(p):ev['out'][m.group(1)]=open(p,'rb').read()
    return evs
evs=load(sys.argv[1]);pre=sys.argv[2].split(',')
prevd={};prevc={}
for i,e in enumerate(evs):
    col={k:v for k,v in e['out'].items() if k.startswith('rt0-') or k.startswith('rt1-') or k.startswith('rt2-')}
    dk=[k for k in e['out'] if k.startswith('rt-1-output')]
    cur=e['out'][dk[0]] if dk else None;prev=prevd.get(dk[0]) if dk else None
    if cur is not None and prev is not None and len(cur)==len(prev) and any(e['vs'].startswith(p) for p in pre):
        n=len(cur)//4;a=struct.unpack('<%dI'%n,prev);b=struct.unpack('<%dI'%n,cur)
        h=collections.Counter();ch=0
        for x,y in zip(a,b):
            dx,dy=x&0xffffff,y&0xffffff
            if dx!=dy: ch+=1;h[dy-dx]+=1
        top=', '.join(f'{d:+d}:{c}' for d,c in h.most_common(8))
        cc=max([sum(1 for j in range(0,len(v),4) if v[j:j+4]!=prevc[k][j:j+4]) for k,v in col.items() if k in prevc and len(prevc[k])==len(v)] or [-1])
        print(f"e{i+1} vs={e['vs'][:8]} ps={e['ps'][:8]} n={e['n']} depth_changed={ch}/{n} colour_changed={cc} delta(D24 units) {top or 'none'}")
    if cur is not None: prevd[dk[0]]=cur
    prevc.update(col)
