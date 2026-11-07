# [pvr] DM_GFX_FENCE_GUARD 源码门（BUGS_AND_FIXES 2.38）：槽栅栏只经 fguard 访问，记录线程的 vkQueueSubmit 在 fguard::Lock 下。
# [pvr] DM_GFX_FENCE_GUARD source gate (BUGS_AND_FIXES 2.38): slot fences are touched only through fguard (status / wait / reset), and
# every vkQueueSubmit that signals a slot fence runs under fguard::Lock, so a new raw call cannot bring back the cross-thread fence race
# that the PowerVR 1.15 driver failed on. Not a compile; xbg_game.cpp only (vulkan_present.cpp's fences are its own thread's).
from pathlib import Path
import re
root=Path(__file__).resolve().parents[4]
src=(root/'gfx/vulkan/engine/xbg_game.cpp').read_text(encoding='utf-8')
code=re.sub(r"/\*.*?\*/|//[^\n]*", "", src, flags=re.S)   # comments out; string literals stay (no fence call is in one)

def spans(name):
    """[start, end) of every top-level `namespace <name> {` block."""
    out=[]
    for m in re.finditer(r'namespace\s+%s\s*\{'%name, code):
        depth=0
        for i in range(m.end()-1, len(code)):
            if code[i]=='{':depth+=1
            elif code[i]=='}':
                depth-=1
                if not depth:out.append((m.start(), i+1));break
    return out
fg=spans('fguard')
assert len(fg)==2, 'expected the fguard base block and the helper block, got %d'%len(fg)
def inside(pos,blocks):return any(a<=pos<b for a,b in blocks)

# 1. vkGetFenceStatus: only inside fguard.
bad=[code.count('\n',0,m.start())+1 for m in re.finditer(r'\bvkGetFenceStatus\s*\(', code) if not inside(m.start(),fg)]
assert not bad, 'raw vkGetFenceStatus outside fguard near lines %s'%bad

# 2. vkWaitForFences: fguard, the Mali retry wrapper (dm_wait_for_fences) and the self-test's private kat_fence only.
for m in re.finditer(r'\bvkWaitForFences\s*\(([^;]*)', code):
    line=code.count('\n',0,m.start())+1;args=m.group(1)
    ok=inside(m.start(),fg) or 'kat_fence' in args or args.replace(' ','')=='d,n,f,all,timeout)'   # the last: dm_wait_for_fences itself
    assert ok, 'raw vkWaitForFences near line %d: %s'%(line,args[:80])

# 3. vkResetFences: no slot fence outside fguard.
for m in re.finditer(r'\bvkResetFences\s*\(([^;]*)', code):
    if inside(m.start(),fg):continue
    assert 'fence' not in m.group(1).replace('kat_fence','').replace('wfence',''), 'raw vkResetFences of a slot fence near line %d'%(code.count('\n',0,m.start())+1)

# 4. Every vkQueueSubmit that passes a slot fence (s.fence / q.fence) runs inside a { fguard::Lock g; ... } scope.
subs=list(re.finditer(r'\bvkQueueSubmit\s*\(\s*cfg\.queue\s*,\s*1\s*,\s*&si\s*,\s*(s|q)\.fence\s*\)', code))
assert len(subs)>=4, 'expected the record thread (2) and the direct (2) slot submits, got %d'%len(subs)
for m in subs:
    before=code[max(0,m.start()-120):m.start()]
    assert re.search(r'\{\s*fguard::Lock\s+g\s*;[^{}]*$', before), 'slot vkQueueSubmit without fguard::Lock near line %d'%(code.count('\n',0,m.start())+1)

# 5. The pool-reset worker and the legacy waits wait through fguard::wait, and the worker's failure goes to pool_reset_failed.
assert len(re.findall(r'fguard::wait\(', code))>=4, 'fguard::wait call sites missing'
assert 'pool_reset_failed(s)' in code and 'fence_fatal(s,fr,waited,timeout,"retire")' in code

# 6. The family defaults are read where the other per-family policies are.
assert 'xbg_fence_guard_default(gpu_policy)' in code and 'xbg_crumbs_default(gpu_policy)' in code
print('PASS: slot fences only through fguard, slot submits under fguard::Lock, worker / retire / legacy waits guarded (not a compile)')
