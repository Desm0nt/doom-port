// [gs-fix] port-common/modules/gs-fix (KB PNH-B-03): the engine side of the geometry-shader rewrites in xbg_gsfix_spv.{h,c}
// (adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later: vka_shader.c gs_words / gs_fill / vka_shader_gs_link_module,
// vka_draw.c claim words, vka_pso.c). Included once into xbg_game.cpp right before load_shader (needs Shader, Slot, Arena, drvc).
//
// DM_GFX_GS_FIX (boot) is a bit mask, unset = the default:
//   1 claim (default on): a geometry shader that writes memory makes those writes in one run per primitive, as Maxwell runs it once
//     (Adreno runs it once per declared output vertex: D16's culling GS appended every visible triangle 3 times and ran past its index
//     range, gs_cull_fix.md rounds 2-7; NieR's IndexBuildIndependent_* the same). Its draws bind the claim words at set 0, the stage's
//     next free binding (D16 / NieR: 33 / 21), from the slot's own arena (Slot::gsclaim), word 0 = the draw's tag. Instanced and
//     indirect draws get tag 0: drawn as before (each run writes), logged once.
//   2 fill (default on only on Qualcomm's own driver, driverID 8): a run that would emit nothing emits one degenerate primitive (the
//     stock driver's binning and drawing passes must agree on the primitive count; peanuts: a GPU hang). D16's and NieR's GS always
//     emit: the filler never runs there, the rewrite only adds a flag store per vertex.
//   4 link (default on): at pipeline time, a geometry module gets outputs for the pixel inputs it does not write, copied from the
//     vertex shader's outputs (the stock driver refuses such a pipeline with -13). Our shipped packs link the GS to its next stage
//     offline, so nothing is added there (one map lookup per GS draw).
// 0 = everything as before (the modules, layouts and draws byte for byte the old ones).
#pragma once
#include "xbg_gsfix_spv.h"

namespace gsfix {
enum : unsigned { CLAIM = 1, FILL = 2, LINK = 4 };
unsigned mask = 0;uint32_t driver_id = 0;
std::atomic<uint64_t> claimed{0},refused{0},filled{0},linked{0};
uint64_t draws_claimed = 0,draws_open = 0;uint32_t next_tag = 0,draw_words = 0;

// Boot, before the warm-up threads rewrite shaders (xbg_game_initialize, beside bones_ssbo_decide).
inline void decide(VkPhysicalDevice phys){
    VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&dp;
    vkGetPhysicalDeviceProperties2(phys,&p2);driver_id=uint32_t(dp.driverID);
    const unsigned def=CLAIM|LINK|(driver_id==VK_DRIVER_ID_QUALCOMM_PROPRIETARY?FILL:0u);
    const char *e=getenv("DM_GFX_GS_FIX");mask=e && *e?unsigned(strtoul(e,nullptr,0))&7u:def;
    claimed=0;refused=0;filled=0;linked=0;draws_claimed=draws_open=0;next_tag=0;draw_words=0;
    xbg_logf("NVN gs-fix: DM_GFX_GS_FIX=%#x%s (1 claim: a geometry shader's memory writes once per primitive; 2 fill: a run that emits nothing emits a degenerate primitive; 4 link: outputs for the pixel inputs it does not write; default %#x) driverID=%u",
             mask,e && *e?"":" (default)",def,driver_id);
    pc_note("gs-fix",mask!=0,"DM_GFX_GS_FIX=%#x: claim %s, fill %s, link %s (driverID %u; PNH-B-03)",mask,mask&CLAIM?"on":"off",mask&FILL?"on":"off",mask&LINK?"on":"off",driver_id);
}
// The claim binding of a geometry module: our translator numbers the set-0 bindings slot * 6 + stage, so the stage's next number
// above its own bindings is free in every stage. -1 when a binding does not follow that numbering.
inline int claim_binding(const std::vector<xbg_shader_binding> &bs,uint32_t stage){
    uint32_t next=stage;for(const auto &b:bs){if(b.binding%6!=stage)return -1;next=std::max(next,b.binding+6);}return int(next);}
// SPIR-V at load (load_shader and warm::load_module, right after lone_rewrite): claim, then fill. Returns the claim binding (-1: none).
inline int rewrite(std::vector<uint32_t> &w,uint32_t stage,const std::vector<xbg_shader_binding> &bs,const char *key,const char *loader){
    if(stage!=3 || !(mask&(CLAIM|FILL)))return -1;
    static std::atomic<unsigned> told{0};char err[256]="";int cb=-1;
    if(mask&CLAIM){const int b=claim_binding(bs,stage);
        if(b<0){++refused;if(told++<32)xbg_logf("NVN gs-fix claim shader=%s loader=%s: its bindings are not numbered slot*6+stage: drawn as it is",key,loader);}
        else{uint32_t *o=nullptr;size_t on=0;const int n=xbg_gsfix_claim(w.data(),w.size(),0,uint32_t(b),&o,&on,err,sizeof err);
            if(n<0){++refused;if(told++<32)xbg_logf("NVN gs-fix claim shader=%s loader=%s: not put under a claim (%s): drawn as it is, its memory writes repeated by the device's runs of each primitive",key,loader,err);}
            else if(n>0 && o){w.assign(o,o+on);cb=b;++claimed;if(told++<32)xbg_logf("NVN gs-fix claim shader=%s loader=%s: %d memory writes made by one run per primitive (claim words at set 0 binding %d)",key,loader,n,b);}
            free(o);}}
    if(mask&FILL){size_t on=0;uint32_t *o=xbg_gsfix_fill(w.data(),w.size(),&on,err,sizeof err);
        if(o){w.assign(o,o+on);++filled;if(told++<32)xbg_logf("NVN gs-fix fill shader=%s loader=%s: a run that emits nothing emits a degenerate primitive",key,loader);}
        else if(err[0] && told++<32)xbg_logf("NVN gs-fix fill shader=%s loader=%s: not filled (%s)",key,loader,err);
        free(o);}
    return cb;
}
// load_shader, after storage_writes: the claim binding joins the module's bindings (source XBG_SOURCE_GSCLAIM, not a guest buffer: out of
// storage_writes), and the interface signatures the link compares (vertex / geometry outputs, pixel inputs; the geometry words kept).
inline void note_shader(Shader &s,const std::vector<uint32_t> &w,int cb){
    s.gsfix_claim=cb;
    if(cb>=0){xbg_shader_binding b{};b.binding=uint32_t(cb);b.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;b.count=1;b.source=XBG_SOURCE_GSCLAIM;b.sampler_slot=UINT32_MAX;s.bindings.push_back(b);s.storage_writes.erase(uint32_t(cb));}
    if(!(mask&LINK))return;
    if(s.stage==0 || s.stage==3)xbg_gsfix_io(w.data(),w.size(),3,s.gsfix_io);else if(s.stage==4)xbg_gsfix_io(w.data(),w.size(),1,s.gsfix_io);
    if(s.stage==3)s.gsfix_words=w;
}
// draw(): the claim words this draw takes (descriptors -> region). Primitive IDs restart with each instance and an indirect draw's count is
// the GPU's, so those draws take tag 0 (every run writes, as before).
struct Draw{
    template<class Topology> Draw(const Shader *gs,uint32_t count,uint32_t instances,bool indirect,Topology topology){
        draw_words=0;if(!gs || gs->gsfix_claim<0)return;
        if(indirect || instances>1){++draws_open;static std::atomic<unsigned> told{0};if(told++<4)xbg_logf("NVN gs-fix: geometry shader %s writes memory in an %s draw: drawn without the claim, its writes repeated by the device's runs of each primitive",gs->key.c_str(),indirect?"indirect":"instanced");return;}
        draw_words=std::max<uint32_t>(1,xbg_gsfix_prims(uint32_t(topology()),count));}
    ~Draw(){draw_words=0;}};
// descriptors(): the claim words of the draw being recorded, from the recording slot's own arena (zeroed once when a chunk is made, never
// cleared after: it holds 0 or the tags of earlier draws, none of them this draw's), word 0 = this draw's tag (0: no claim, one word).
inline Buffer region(Slot &s){
    if(!s.gsclaim.zero_new){s.gsclaim.zero_new=true;s.gsclaim.chunk_bytes=4ull<<20;}
    const uint32_t words=draw_words?draw_words+1:1;Buffer b=s.gsclaim.alloc(size_t(words)*4);
    uint32_t tag=0;if(draw_words){if(++next_tag==0)next_tag=1;tag=next_tag;++draws_claimed;}
    memcpy(b.mapped,&tag,4);b.flush();return b;}
// draw(): the geometry module for this vertex / pixel pair, with outputs for the pixel inputs it does not write (VK_NULL_HANDLE cached
// when none are missing: the base module). Made once per pair; not on the warm-up list (no bundle key).
inline VkShaderModule link_module(Shader &gs,const Shader *vs,const Shader *fs,VkShaderModule base){
    if(!(mask&LINK) || !fs || gs.gsfix_words.empty())return base;
    auto it=gs.gsfix_links.find({vs,fs});if(it!=gs.gsfix_links.end())return it->second?it->second:base;
    VkShaderModule m=VK_NULL_HANDLE;const uint32_t need=xbg_gsfix_link_need(gs.gsfix_io,fs->gsfix_io);
    if(need){static const uint8_t none[32]{};uint32_t cp=0,nl=0;size_t on=0;char err[256]="";static std::atomic<unsigned> told{0};
        uint32_t *o=xbg_gsfix_link(gs.gsfix_words.data(),gs.gsfix_words.size(),vs?vs->gsfix_io:none,fs->gsfix_io,&cp,&nl,&on,err,sizeof err);
        if(o){VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=on*4;ci.pCode=o;VkResult r;{drvc::Lock compile(true);r=vkCreateShaderModule(cfg.device,&ci,nullptr,&m);}
            if(r!=VK_SUCCESS)m=VK_NULL_HANDLE;else ++linked;
            if(told++<16)xbg_logf("NVN gs-fix link: geometry shader %s with vs %s ps %s: outputs added for the pixel inputs it does not write: locations %#x copied from the vertex shader, %#x null%s",
                                  gs.key.c_str(),vs?vs->key.c_str():"-",fs->key.c_str(),cp,nl,m?"":" (module not made: the base module)");}
        else if(told++<16)xbg_logf("NVN gs-fix link: geometry shader %s with ps %s: pixel inputs %#x not written, not added (%s)",gs.key.c_str(),fs->key.c_str(),need,err);
        free(o);}
    gs.gsfix_links.emplace(std::make_pair(vs,fs),m);return m?m:base;
}
}   // namespace gsfix
