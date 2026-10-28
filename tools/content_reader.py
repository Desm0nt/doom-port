"""Read-only NSP / NSZ (uncompressed entries) / XCI and NCA readers. Keys remain in memory; formats follow hactool nca/bktr/ivfc/hfs0.
Own code of the DOOM Mobile author (first written for an earlier Switch port), MIT like the rest of the repository."""
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import re,struct,hashlib,bisect
U32=lambda b,o:struct.unpack_from('<I',b,o)[0]
U64=lambda b,o:struct.unpack_from('<Q',b,o)[0]
def dec(key,mode,b):
 d=Cipher(algorithms.AES(key),mode).decryptor();return d.update(b)+d.finalize()
def exact(f,o,n):
 f.seek(o);b=f.read(n)
 if len(b)!=n:raise ValueError('Short read')
 return b
def pfs(read,size):
 h=read(0,16);assert h[:4]==b'PFS0';n,ss=struct.unpack_from('<II',h,4);assert n<100000
 base=16+n*24+ss;assert base<=size
 t=read(16,n*24+ss);out=[]
 for i in range(n):
  off,sz,no=struct.unpack_from('<QQI',t,i*24);assert no<ss and base+off+sz<=size
  name=t[n*24+no:].split(b'\0')[0].decode();assert name and '/' not in name and '\\' not in name and name not in ('.','..')
  out.append((name,base+off,sz))
 return out
def hfs(read,off,size):
 h=read(off,16);assert h[:4]==b'HFS0';n,ss=struct.unpack_from('<II',h,4);assert n<100000
 base=off+16+n*0x40+ss;t=read(off+16,n*0x40+ss);out=[]
 for i in range(n):
  o,sz,no=struct.unpack_from('<QQI',t,i*0x40);assert no<ss and o+sz<=size-(base-off)
  out.append((t[n*0x40+no:].split(b"\0")[0].decode(),base+o,sz))
 return out
def entries_of(f,size):
 """Top-level files of a package: PFS0 (NSP / NSZ) or the secure partition of an XCI (HFS0)."""
 read=lambda o,n:exact(f,o,n)
 if read(0,4)==b'PFS0':return pfs(read,size)
 if size>0x200 and read(0x100,4)==b'HEAD':
  ro,rs=struct.unpack_from('<QQ',read(0x130,16));sec=[e for e in hfs(read,ro,size-ro) if e[0]=='secure']
  if not sec:raise ValueError('XCI without a secure partition')
  return hfs(read,sec[0][1],sec[0][2])
 raise ValueError('not an NSP, NSZ or XCI package')
class Package:
 def __init__(self,path,keypath):
  self.path=Path(path);self.f=self.path.open('rb');self.entries=entries_of(self.f,self.path.stat().st_size)
  self.keys={k:bytes.fromhex(v) for k,v in re.findall(r'^\s*(\w+)\s*=\s*([0-9a-fA-F]+)\s*$',Path(keypath).read_text(),re.M)}
  self.tickets={}
  for name,o,n in self.entries:
   if name.endswith('.tik'):
    t=exact(self.f,o,n);self.tickets[t[0x2a0:0x2b0]]=t[0x180:0x190]
 def ncas(self):
  for name,o,n in self.entries:
   if name.endswith('.nca'):yield Nca(self,name,o,n)
class Nca:
 def __init__(self,p,name,o,n):
  self.p,self.name,self.offset,self.size=p,name,o,n;e=exact(p.f,o,0xc00)
  h=b''.join(dec(p.keys['header_key'],modes.XTS(s.to_bytes(16,'big')),e[s*512:(s+1)*512]) for s in range(6));assert h[512:516]==b'NCA3' and U64(h,0x208)==n
  self.header=h;self.type=h[0x205];self.title=U64(h,0x210);rev=max(0,max(h[0x206],h[0x220])-1);rid=h[0x230:0x240]
  if any(rid):self.key=dec(p.keys[f'titlekek_{rev:02x}'],modes.ECB(),p.tickets[rid])
  else:self.key=dec(p.keys[f'key_area_key_{["application","ocean","system"][h[0x207]]}_{rev:02x}'],modes.ECB(),h[0x320:0x330])
  self.sections=[]
  for i in range(4):
   start,end=struct.unpack_from('<II',h,0x240+16*i)
   if not start:continue
   sh=h[0x400+i*512:0x600+i*512];assert hashlib.sha256(sh).digest()==h[0x280+i*32:0x2a0+i*32]
   assert start<end and end*512<=n
   self.sections.append(Section(self,i,start*512,(end-start)*512,sh))
class Section:
 def __init__(self,nca,index,start,size,h):self.nca,self.index,self.start,self.size,self.h=nca,index,start,size,h
 def read(self,o,n,ctr=None):
  assert 0<=o<=self.size and 0<=n<=self.size-o
  at=self.start+o;skip=at%16;b=exact(self.nca.p.f,self.nca.offset+at-skip,n+skip)
  if self.h[4]==1:return b[skip:]
  assert self.h[4] in (3,4)
  prefix=self.h[0x140:0x148][::-1]
  if ctr is not None:prefix=prefix[:4]+ctr.to_bytes(4,'big')
  return dec(self.nca.key,modes.CTR(prefix+(at//16).to_bytes(8,'big')),b)[skip:]
 def pfs(self):
  ho,hs,po,ps=struct.unpack_from('<QQQQ',self.h,0x30);ht=self.read(ho,hs);assert hashlib.sha256(ht).digest()==self.h[8:40]
  data=self.read(po,ps);bs=U32(self.h,0x28)
  for j in range(0,ps,bs):assert hashlib.sha256(data[j:j+bs]).digest()==ht[j//bs*32:j//bs*32+32]
  return data,pfs(lambda o,n:data[o:o+n],len(data))
class Patched:
 def __init__(self,patch,base):
  self.patch,self.base,self.h=patch,base,patch.h
  ro,rs=struct.unpack_from('<QQ',self.h,0x100);so,ss=struct.unpack_from('<QQ',self.h,0x120)
  assert self.h[0x110:0x114]==b'BKTR' and self.h[0x130:0x134]==b'BKTR'
  assert ro+rs==so and so+ss==patch.size
  def entries(data,fmt,stride):
   count=U32(data,4);assert (count+1)*0x4000<=len(data);out=[]
   for i in range(count):
    o=(i+1)*0x4000;num=U32(data,o+4);assert num*stride<=0x3ff0
    out.extend(struct.unpack_from(fmt,data,o+16+j*stride) for j in range(num))
   assert out and out[0][0]==0 and all(a[0]<b[0] for a,b in zip(out,out[1:]))
   return out,U64(data,8)
  self.reloc,self.size=entries(patch.read(ro,rs),'<QQI',20)
  subs,total=entries(patch.read(so,ss),'<QII',16);assert total==so
  self.subs=[(o,c) for o,_,c in subs if o<ro]+[(ro,U32(self.h,0x140))]
  self.rstarts=[x[0] for x in self.reloc];self.sstarts=[x[0] for x in self.subs]
 def physical(self,o,n):
  out=[]
  while n:
   i=bisect.bisect_right(self.sstarts,o)-1;assert i>=0
   end=self.sstarts[i+1] if i+1<len(self.subs) else self.patch.size;k=min(n,end-o);assert k>0
   out.append(self.patch.read(o,k,self.subs[i][1]));o+=k;n-=k
  return b''.join(out)
 def read(self,o,n):
  assert 0<=o<=self.size and n<=self.size-o;out=[]
  while n:
   i=bisect.bisect_right(self.rstarts,o)-1;assert i>=0
   v,p,ispatch=self.reloc[i];end=self.rstarts[i+1] if i+1<len(self.reloc) else self.size;k=min(n,end-o);assert k>0 and ispatch in (0,1)
   out.append(self.physical(p+o-v,k) if ispatch else self.base.read(p+o-v,k));o+=k;n-=k
  return b''.join(out)
def verify_ivfc(s):
 assert s.h[8:12]==b'IVFC';levels=[struct.unpack_from('<QQI',s.h,0x18+i*24) for i in range(6)];counts=[]
 for i,(off,size,exp) in enumerate(levels):
  assert 5<=exp<=24;bs=1<<exp
  expected=s.h[0xc8:0xe8] if i==0 else s.read(levels[i-1][0],levels[i-1][1]);count=(size+bs-1)//bs;assert count*32<=len(expected)
  for batch in range(0,size,bs*256):
   data=s.read(off+batch,min(bs*256,size-batch))
   for j in range(0,len(data),bs):
    b=data[j:j+bs];digest=hashlib.sha256(b+b'\0'*(bs-len(b))).digest();idx=(batch+j)//bs
    if digest!=expected[idx*32:idx*32+32]:raise ValueError(f'IVFC mismatch level {i} block {idx}')
  counts.append(count);print('IVFC',i,'OK',count,flush=True)
 return levels[-1][0],levels[-1][1],counts
