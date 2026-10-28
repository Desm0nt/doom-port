import sys,json,bisect
from capstone import *
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
which=sys.argv[1]; a=int(sys.argv[2],16); n=int(sys.argv[3],0) if len(sys.argv)>3 else 0x200
raw=open(f'work/exefs-{which}/main.raw','rb').read()
fn=[]
if which=='base':
    S=json.load(open('work/exefs-base/main.symbols.json'))['exports']
    fn=sorted((x['value'],x['name']) for x in S if x['type']==2 and x['value'])
fv=[v for v,_ in fn]
def nm(x):
    if not fn: return ''
    i=bisect.bisect_right(fv,x)-1; return f"{fn[i][1][:90]}+{x-fn[i][0]:#x}"
c=0
for i in md.disasm(raw[a:a+n],a):
    s=f'  {i.address:x} {i.mnemonic} {i.op_str}'
    if i.mnemonic in('bl','b') and i.op_str.startswith('#'): s+='  ; '+nm(int(i.op_str[1:],16))
    print(s); c+=1
    if i.mnemonic=='ret' and len(sys.argv)<=4: break
