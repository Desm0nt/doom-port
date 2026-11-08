# [pvr] DM_GFX_COMPILE_LOCK source gate (BUGS_AND_FIXES 2.47): on PowerVR the driver compiler is not thread-safe, and it runs in
# vkCreate*Pipelines, vkCreateShaderModule and inside some vkCmd*. Every vkCreateShaderModule / vkCreateGraphicsPipelines /
# vkCreateComputePipelines of xbg_game.cpp is inside a drvc::Lock / gpguard::Lock scope on the same line, the record thread's exec takes
# drvc::Lock, gpguard's mutex is drvc::mu, and the present blit / clear take DrvcLock. Not a compile.
from pathlib import Path
import re
root=Path(__file__).resolve().parents[4]
src=(root/'gfx/vulkan/engine/xbg_game.cpp').read_text(encoding='utf-8')
code=re.sub(r"/\*.*?\*/|//[^\n]*", "", src, flags=re.S)
bad=[]
for m in re.finditer(r'\bvkCreate(ShaderModule|GraphicsPipelines|ComputePipelines)\s*\(', code):
    line_start=code.rfind('\n',0,m.start())+1;before=code[line_start:m.start()]
    # the nearest lock on the same line must still be open: count braces between it and the call
    k=max(before.rfind('drvc::Lock'),before.rfind('gpguard::Lock'))
    if k<0 or before[k:].count('{')-before[k:].count('}')<0 or '{' not in before[:k][-40:]:
        bad.append(code.count('\n',0,m.start())+1)
assert not bad, 'driver compile outside the compile lock near lines %s'%bad
assert 'std::mutex &create_mu=drvc::mu;' in code, 'gpguard must share drvc::mu'
assert re.search(r'drvc::Lock compile\(compile_locked\(h->op\)\);', code), 'vkrec::exec must take the compile lock'
assert 'drvc::Lock drvc_g(true);call;' in code, 'VKREC_DIRECT must take the compile lock'
pres=(root/'android/vulkan_present.cpp').read_text(encoding='utf-8')
assert '{DrvcLock compile;vkCmdBlitImage(' in pres and '{DrvcLock compile;vkCmdClearColorImage(' in pres, 'present blit / clear must take DrvcLock'
print('PASS: every module / pipeline creation under the compile lock, dm-record packets and present blit / clear take it (not a compile)')
