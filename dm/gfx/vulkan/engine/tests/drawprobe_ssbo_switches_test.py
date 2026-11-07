# DM_GFX_DRAWPROBE / DM_GFX_DEBUG_ZWRITE / DM_GFX_BONES_SSBO_{NODYN,COPY,PAD} (BUGS_AND_FIXES 2.34): source checks of the
# integration sites (not a compile, not a driver run) plus the frame selection rule re-run in Python.
# Checks: every switch defaults off (getenv only); the probe query never nests in a guest samples-passed run, ends right after
# the draw call inside the same pass, its pool is reset at slot start (outside a pass) and read after the slot's fence; the
# NODYN guard is part of UBO_DYN (one place decides dynamic-ness for keys, layouts and writes) and keeps such sets out of the
# cross-frame memo; COPY/PAD copies exactly ssbo_range bytes, and the descriptor range of a bones SSBO is ssbo_range.
from pathlib import Path
import re
src=(Path(__file__).resolve().parent.parent/'xbg_game.cpp').read_text(encoding='utf-8')
def body(start):
    i=src.index(start);j=src.index('{',i);d=0
    for k in range(j,len(src)):
        if src[k]=='{':d+=1
        elif src[k]=='}':
            d-=1
            if d==0:return src[i:k+1]
    raise AssertionError(start)
# defaults off
assert 'int drawprobe_on=-1;' in src
en=body('bool drawprobe_enabled(')
assert 'getenv("DM_GFX_DRAWPROBE")' in en and 'drawprobe_on=n>=1;' in en
assert 'int bones_ssbo_nodyn=0,bones_ssbo_copy=0;size_t bones_ssbo_pad=0;' in src
dec=body('void bones_ssbo_decide(')
for k in ('DM_GFX_BONES_SSBO_NODYN','DM_GFX_BONES_SSBO_COPY','DM_GFX_BONES_SSBO_PAD'):assert 'getenv("%s")'%k in dec,k
assert '!(pad&(pad-1))' in dec   # power of two only, else 0 (off)
zw=body('bool zwrite_listed(')
assert 'getenv("DM_GFX_DEBUG_ZWRITE")' in zw and '!zl.empty()' in zw
# frame selection, same arithmetic as drawprobe_selected
sel=body('bool drawprobe_selected(')
assert 'frame<drawprobe_from' in sel and 'k<drawprobe_count' in sel and '%drawprobe_period==0' in sel
def selected(frame,frm,per,cnt):
    if frame<frm:return False
    k=(frame-frm)//per
    return k<cnt and (frame-frm)%per==0
assert [f for f in range(0,40) if selected(f,10,5,3)]==[10,15,20]
assert [f for f in range(0,40) if selected(f,7,1,1)]==[7]
# probe in draw(): begun only without an open guest occlusion run, ended right after the draw call
dr=body('void draw(int prim,int first,int count')
b=dr.index('vkrec::CmdBeginQuery(c.command,ps_.probe_pool')
g=dr.rindex('if(ps_.probe && c.command==ps_.cmd && !open_occlusion.cmd',0,b)
e=dr.index('if(probe_q>=0)vkrec::CmdEndQuery(c.command,slots[slot_index].probe_pool')
last_draw=dr.index('else vkrec::CmdDraw(c.command,count,instances,first,first_instance);')
assert g<b<last_draw<e<dr.index('DRAW_MARK(6)'),'probe begin/draw/end order'
assert dr.index('if(new_run){end_occlusion_query();begin_occlusion_query(')<g   # the guest run (if any) is open before the check
# pool reset at slot start, results after the fence
rs=body('Slot &recording_slot(')
assert 'vkrec::CmdResetQueryPool(s.cmd,s.probe_pool,0,drawprobe_query_limit)' in rs and 'cfg.enabled_core.occlusionQueryPrecise' in rs
rt=body('void retire_slot(')
assert rt.index('vkWaitForFences')<rt.index('vkGetQueryPoolResults(cfg.device,s.probe_pool')
assert 'VK_QUERY_RESULT_WITH_AVAILABILITY_BIT' in rt[rt.index('s.probe_pool'):]
# NODYN in the single dynamic-ness macro, and kept out of the cross-frame memo
m=re.search(r'#define UBO_DYN\(mode,b\)(.*?)\n(.*?)\n',src)
assert m and 'bones_ssbo_nodyn && ((b).flags&XBG_BIND_SSBO)' in m.group(1)+m.group(2)
assert 'if(bones_ssbo_nodyn && memo_dyn_ok)' in src and 'dm.persistent=xmemo_enabled && memo_dyn_ok' in src
# COPY / PAD
sn=body('Buffer ssbo_snapshot(')
assert 'if(bones_ssbo_copy){' in sn and 'stream_buffer(ssbo_range(copy),' in sn
assert 'n=bind_ssbo(b)?ssbo_range(copy):uniform_range(copy);' in src
rg=body('size_t ssbo_range(')
assert 'if(bones_ssbo_pad)n=(n+bones_ssbo_pad-1)&~(bones_ssbo_pad-1);' in rg
# DM_GFX_DEBUG_ZEQ_LE: only EQUAL tests without depth write of listed VS; bias only when set
i=src.index('getenv("DM_GFX_DEBUG_ZEQ_LE")');zq=src[i:src.index('DM_GFX_DEBUG_ZEQ_LE / _BIAS',i)]
assert '!zl.empty()' in zq and '!ds.depthWriteEnable && ds.depthCompareOp==VK_COMPARE_OP_EQUAL' in zq and 'if(zb!=0){rs.depthBiasEnable=VK_TRUE;' in zq
assert src.index('ds.depthCompareOp=VkCompareOp(state.ds.df-1)')<i<src.index('ci.pDepthStencilState=&ds')
print('PASS: drawprobe / zwrite / bones SSBO variants: defaults off, query order, memo guard, ranges (source checks, not a compile)')
