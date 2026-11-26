// [pvr-cb-barrier] port-common module pvr-cb-barrier (KB FN-63). Idea from GameNative wrapper-gamenative (GameNative/mesa
// wrapper-25 src/vulkan/wrapper/wrapper_device.c: WRAPPER_CMDBUF_ENTRY_BARRIER, on by default for VK_DRIVER_ID_IMAGINATION_PROPRIETARY:
// a full memory barrier at the start of every primary command buffer); our own code.
//
// What it does: right after vkBeginCommandBuffer of every primary command buffer the engine records, one
//   vkCmdPipelineBarrier(ALL_COMMANDS -> ALL_COMMANDS, MEMORY_WRITE -> MEMORY_READ | MEMORY_WRITE)
// so nothing of a command buffer starts before the writes of everything submitted earlier on the queue are visible. The engine's
// own barriers already order what Vulkan requires; this is for the PowerVR proprietary driver, whose cross-submission visibility
// GameNative found it needed for games that worked elsewhere. A candidate fix for unexplained corruption / flicker / stale data on
// PowerVR, not a confirmed fix of one of ours. Cost: one full barrier per command buffer (a few per frame), i.e. no overlap of a new
// command buffer's first work with the previous one's tail.
//
// DM_GFX_PVR_CB_BARRIER (boot, read at the first command buffer):
//   unset / 0  off (default; the command buffers are recorded exactly as before)
//   1          on when the GPU is a PowerVR (vendor 0x1010) — what safe mode step 1 sets on PowerVR (D16 xbg_safe_mode.h)
//   2          on every GPU (to run the path on an Adreno / Mali phone)
// Header-only C++17; thread-safe (the decision is one atomic int, computed by whichever thread begins the first command buffer).
#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#ifdef XBG_CB_BARRIER_REPORT
extern "C" void pc_note(const char *id,int applied,const char *fmt,...);   // [pc] port-common core/pc.h
#endif

inline std::atomic<int> xbg_cb_barrier_state{-1};   // -1 = not decided yet, 0 = off, 1 = on
inline std::atomic<unsigned long long> xbg_cb_barrier_count{0};   // command buffers that got the barrier (diagnostics)

inline int xbg_cb_barrier_mode(){const char *e=getenv("DM_GFX_PVR_CB_BARRIER");if(!e || !*e)return 0;const int v=atoi(e);return v<0?0:v>2?2:v;}
inline bool xbg_cb_barrier_decide(int mode,uint32_t vendor_id){return mode==2 || (mode==1 && vendor_id==0x1010u);}

inline bool xbg_cb_barrier_on(uint32_t vendor_id){
    int s=xbg_cb_barrier_state.load(std::memory_order_relaxed);
    if(s>=0)return s!=0;
    const int mode=xbg_cb_barrier_mode();
    s=xbg_cb_barrier_decide(mode,vendor_id)?1:0;
    int expect=-1;
    if(xbg_cb_barrier_state.compare_exchange_strong(expect,s)){
#ifdef XBG_CB_BARRIER_REPORT
        pc_note("pvr-cb-barrier",s,"%s; FN-63",s?(mode==2?"DM_GFX_PVR_CB_BARRIER=2: full barrier at the start of every command buffer, any GPU"
                                                   :"PowerVR, DM_GFX_PVR_CB_BARRIER=1 (safe mode or the player): full barrier at the start of every command buffer")
                                              :mode==1?"DM_GFX_PVR_CB_BARRIER=1 but not a PowerVR GPU":"off (DM_GFX_PVR_CB_BARRIER unset / 0)");
#endif
        return s!=0;
    }
    return expect!=0;
}

inline void xbg_cb_barrier_record(VkCommandBuffer c){
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,nullptr,0,nullptr);
    xbg_cb_barrier_count.fetch_add(1,std::memory_order_relaxed);
}

// Drop-in for vkBeginCommandBuffer at the engine's primary command buffer sites (never for a secondary one: a barrier there would be
// inside the caller's render pass). Same result value.
inline VkResult xbg_cb_begin(VkCommandBuffer c,const VkCommandBufferBeginInfo *b,uint32_t vendor_id){
    const VkResult r=vkBeginCommandBuffer(c,b);
    if(r==VK_SUCCESS && xbg_cb_barrier_on(vendor_id))xbg_cb_barrier_record(c);
    return r;
}
