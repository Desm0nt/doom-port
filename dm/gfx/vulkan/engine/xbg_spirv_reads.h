// [tileopt] Which descriptor bindings a SPIR-V module actually reads (DM_GFX_UBO_DYNAMIC=2, xbg_vendor_policy.h). Pure, no device calls,
// so tests/vendor_policy_test.cpp runs it on hand-made modules.
//
// A binding counts as read when a variable decorated with it is used in a function body as the base pointer of OpLoad, OpAccessChain /
// OpInBoundsAccessChain / OpPtrAccessChain, OpArrayLength, OpCopyObject or OpCopyMemory (source), or is passed to OpFunctionCall. Only
// those operand positions are looked at, so a literal that happens to equal the variable's id never makes an unread binding look read
// (the unsafe direction for the PowerVR rule: a dynamic UBO the stage does not read). A module this does not understand (bad header,
// truncated instruction, more than one function) reports ok=false, and the caller then treats every binding as unread for that rule (no dynamic offsets).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <set>
#include <vector>

struct XbgSpirvReads{bool ok=false;std::set<uint32_t> read,declared;};   // binding numbers (any descriptor set)

inline XbgSpirvReads xbg_spirv_binding_reads(const uint32_t *w,size_t n){
    XbgSpirvReads out;
    if(!w || n<5 || w[0]!=0x07230203u)return out;
    const uint32_t bound=w[3];if(!bound || bound>(1u<<22))return out;
    std::vector<int64_t> binding_of(bound,-1);std::vector<uint8_t> is_var(bound,0);
    bool in_function=false;unsigned functions=0;std::vector<uint32_t> used;
    for(size_t i=5;i<n;){
        const uint32_t wc=w[i]>>16,op=w[i]&0xffffu;
        if(!wc || i+wc>n)return out;
        if(op==71 /* OpDecorate */ && wc>=4 && w[i+2]==33 /* Binding */ && w[i+1]<bound)binding_of[w[i+1]]=int64_t(w[i+3]);
        else if(op==59 /* OpVariable */ && wc>=4 && w[i+2]<bound)is_var[w[i+2]]=1;
        else if(op==54 /* OpFunction */){in_function=true;++functions;}
        else if(in_function){
            auto use=[&](size_t k){if(k<wc && w[i+k]<bound)used.push_back(w[i+k]);};
            switch(op){
            case 61:  /* OpLoad: type, result, pointer */ use(3);break;
            case 65: case 66: case 67: case 70: /* access chains (70 OpInBoundsPtrAccessChain): type, result, base */ use(3);break;
            case 68:  /* OpArrayLength: type, result, structure */ use(3);break;
            case 83:  /* OpCopyObject: type, result, operand */ use(3);break;
            case 63:  /* OpCopyMemory: target, source */ use(2);break;
            case 57:  /* OpFunctionCall: type, result, function, arguments... */ for(size_t k=4;k<wc;k++)use(k);break;
            default:break;}
        }
        i+=wc;
    }
    // More than one function: a read in a function the entry point never calls would count; the driver compiles only what is
    // reachable, so such a module is not understood (ok=false), the safe answer.
    if(functions!=1)return out;
    for(uint32_t id=0;id<bound;id++)if(is_var[id] && binding_of[id]>=0)out.declared.insert(uint32_t(binding_of[id]));
    for(uint32_t id:used)if(is_var[id] && binding_of[id]>=0)out.read.insert(uint32_t(binding_of[id]));
    out.ok=true;return out;
}
