// [mtb] DM_GFX_RECORD_THREAD packet protocol (xbg_game.cpp namespace vkrec), offline. The vkrec block is cut out of xbg_game.cpp by the
// Makefile (vkrec_block.inc) and compiled against mock vkCmd* / vkQueueSubmit that append every call with all its arguments (arrays by
// value) to a trace. The same scripted command stream (every wrapper, ring wraps, oversized and pNext packets that fall back to a drain,
// submits with tickets, present hand-offs, A/B switches at slot starts) must give the same trace
//   - direct (mode 0, the calls themselves), - mode 2 (packets replayed inline), - mode 1 (the dm-record thread),
// and wait_submitted / sync must hold their ordering. Linux (WSL): make t_vkrec && ./t_vkrec
#include <vulkan/vulkan.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>

// ---- what the block uses from xbg_game.cpp ----
struct Cfg{void (*log)(void*,const char*);void *userdata;VkQueue queue;} cfg{};
uint64_t timing_clock(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
void xbg_logf(const char *fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);va_end(a);printf("\n");}
[[noreturn]] void xbg_fatalf(const char *fmt,...){va_list a;va_start(a,fmt);printf("FATAL: ");vprintf(fmt,a);va_end(a);printf("\n");exit(3);}
bool g_ab_off=false;inline bool mt_off(const char*){return g_ab_off;}
#define VK(call) do{if((call)!=VK_SUCCESS){printf("VK failed: %s\n",#call);exit(4);}}while(0)
// The trace: every Vulkan call in execution order. One thread at a time touches it (the protocol orders them).
std::string T;std::atomic<int> present_jobs{0};
template<class X>void put(const X &x){T.append(reinterpret_cast<const char*>(&x),sizeof x);}
template<class X>void putn(const X *x,uint32_t n){put(n);if(n)T.append(reinterpret_cast<const char*>(x),sizeof(X)*n);}
void tag(const char *s){T.append(s);T.push_back('|');}
extern "C" {
int dm_present_handoff_prepare(void){return 1;}
int dm_present_async_m(VkImage s,VkFormat f,uint32_t w,uint32_t h,const int32_t crop[4],int32_t iv,int mirror){tag("present");put(s);put(f);put(w);put(h);put(iv);put(mirror);if(crop)putn(crop,4);++present_jobs;return 0;}
uint64_t dm_present_wait_submitted(void){return 0;}
int dm_present_last_error(void){return 0;}
void dm_present_queue_lock(void){}
void dm_present_queue_unlock(void){}
__attribute__((weak)) unsigned dm_present_pin_mask(void);   // weak, as in the engine
unsigned dm_present_pin_mask(void){return 0;}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer c,const VkCommandBufferBeginInfo *b){tag("begin");put(c);put(b->flags);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer c){tag("end");put(c);return VK_SUCCESS;}
std::atomic<uint64_t> queue_submits{0};
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q,uint32_t n,const VkSubmitInfo *si,VkFence f){tag("submit");put(q);put(f);for(uint32_t i=0;i<n;i++)putn(si[i].pCommandBuffers,si[i].commandBufferCount);++queue_submits;return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im){
    tag("barrier");put(c);put(s);put(d);put(f);put(nm);for(uint32_t i=0;i<nm;i++){put(m[i].srcAccessMask);put(m[i].dstAccessMask);}put(nb);for(uint32_t i=0;i<nb;i++){put(b[i].buffer);put(b[i].offset);put(b[i].size);put(b[i].srcAccessMask);}
    put(ni);for(uint32_t i=0;i<ni;i++){put(im[i].image);put(im[i].oldLayout);put(im[i].newLayout);put(im[i].subresourceRange);put(im[i].srcAccessMask);put(im[i].dstAccessMask);}}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer c,VkQueryPool p,uint32_t i,VkQueryControlFlags f){tag("bq");put(c);put(p);put(i);put(f);}
VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer c,VkQueryPool p,uint32_t i){tag("eq");put(c);put(p);put(i);}
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer c,VkQueryPool p,uint32_t a,uint32_t n){tag("rq");put(c);put(p);put(a);put(n);}
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits s,VkQueryPool p,uint32_t i){tag("ts");put(c);put(s);put(p);put(i);}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer c,VkQueryPool p,uint32_t a,uint32_t n,VkBuffer d,VkDeviceSize o,VkDeviceSize s,VkQueryResultFlags f){tag("cqp");put(c);put(p);put(a);put(n);put(d);put(o);put(s);put(f);}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo *b,VkSubpassContents k){tag("brp");put(c);put(b->renderPass);put(b->framebuffer);put(b->renderArea);putn(b->pClearValues,b->clearValueCount);put(k);}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer c){tag("erp");put(c);}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer c,VkBuffer s,VkBuffer d,uint32_t n,const VkBufferCopy *r){tag("cb");put(c);put(s);put(d);putn(r,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer c,VkBuffer b,VkImage i,VkImageLayout l,uint32_t n,const VkBufferImageCopy *r){tag("b2i");put(c);put(b);put(i);put(l);putn(r,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage i,VkImageLayout l,VkBuffer b,uint32_t n,const VkBufferImageCopy *r){tag("i2b");put(c);put(i);put(l);put(b);putn(r,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy *r){tag("ci");put(c);put(s);put(sl);put(d);put(dl);putn(r,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageBlit *r,VkFilter f){tag("blit");put(c);put(s);put(sl);put(d);put(dl);putn(r,n);put(f);}
VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageResolve *r){tag("res");put(c);put(s);put(sl);put(d);put(dl);putn(r,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue *v,uint32_t n,const VkImageSubresourceRange *r){tag("ccol");put(c);put(i);put(l);put(*v);putn(r,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearDepthStencilValue *v,uint32_t n,const VkImageSubresourceRange *r){tag("cds");put(c);put(i);put(l);put(*v);putn(r,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer c,uint32_t na,const VkClearAttachment *a,uint32_t nr,const VkClearRect *r){tag("catt");put(c);putn(a,na);putn(r,nr);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint bp,VkPipeline p){tag("bp");put(c);put(bp);put(p);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint bp,VkPipelineLayout l,uint32_t f,uint32_t ns,const VkDescriptorSet *s,uint32_t nd,const uint32_t *d){tag("bds");put(c);put(bp);put(l);put(f);putn(s,ns);putn(d,nd);}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer c,uint32_t f,uint32_t n,const VkViewport *v){tag("vp");put(c);put(f);putn(v,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer c,uint32_t f,uint32_t n,const VkRect2D *r){tag("sc");put(c);put(f);putn(r,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer c,uint32_t f,uint32_t n,const VkBuffer *b,const VkDeviceSize *o){tag("vb");put(c);put(f);putn(b,n);putn(o,n);}
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t){tag("ib");put(c);put(b);put(o);put(t);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s){tag("dii");put(c);put(b);put(o);put(n);put(s);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s){tag("di");put(c);put(b);put(o);put(n);put(s);}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer c,uint32_t a,uint32_t b,uint32_t d,int32_t e,uint32_t f){tag("dx");put(c);put(a);put(b);put(d);put(e);put(f);}
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer c,uint32_t a,uint32_t b,uint32_t d,uint32_t e){tag("d");put(c);put(a);put(b);put(d);put(e);}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){tag("disp");put(c);put(x);put(y);put(z);}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o){tag("dispi");put(c);put(b);put(o);}
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags s,uint32_t o,uint32_t n,const void *v){tag("pc");put(c);put(l);put(s);put(o);putn(static_cast<const uint8_t*>(v),n);}
}
void VKAPI_CALL mock_dicnt(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkBuffer cb,VkDeviceSize co,uint32_t n,uint32_t s){tag("dicnt");put(c);put(b);put(o);put(cb);put(co);put(n);put(s);}
void VKAPI_CALL mock_dcnt(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkBuffer cb,VkDeviceSize co,uint32_t n,uint32_t s){tag("dcnt");put(c);put(b);put(o);put(cb);put(co);put(n);put(s);}

#include "vkrec_block.inc"

// ---- the scripted stream ----
template<class H>H hd(uint64_t v){return reinterpret_cast<H>(uintptr_t(v));}
uint32_t rnd_state=1;uint32_t rnd(){rnd_state=rnd_state*1664525u+1013904223u;return rnd_state>>8;}
struct Run{std::string trace;uint64_t fallbacks,syncs_n,presents;};
// One "slot": begin, a mix of every command, optional oversized / pNext packets, end, submit (direct or packet), maybe a present.
void slot(VkCommandBuffer c,VkFence fence,int k,bool big){
    using namespace vkrec;
    slot_start(false);begin(c);
    for(int i=0;i<40+int(rnd()%40);i++){
        switch(rnd()%27){
        case 0:{VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};m.srcAccessMask=rnd();m.dstAccessMask=rnd();VkImageMemoryBarrier ib[3]{};for(auto &b:ib){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.image=hd<VkImage>(rnd());b.oldLayout=VkImageLayout(rnd()%8);b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.subresourceRange={1,0,rnd()%4,0,1};}
            VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.buffer=hd<VkBuffer>(rnd());bb.size=rnd();CmdPipelineBarrier(c,rnd(),rnd(),0,rnd()%2,&m,rnd()%2,&bb,rnd()%4,ib);break;}
        case 1:CmdBeginQuery(c,hd<VkQueryPool>(rnd()),rnd()%256,1);CmdEndQuery(c,hd<VkQueryPool>(rnd()),rnd()%256);break;
        case 2:CmdResetQueryPool(c,hd<VkQueryPool>(rnd()),0,rnd()%256);break;
        case 3:CmdWriteTimestamp(c,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,hd<VkQueryPool>(rnd()),rnd()%64);break;
        case 4:CmdCopyQueryPoolResults(c,hd<VkQueryPool>(rnd()),0,rnd()%256,hd<VkBuffer>(rnd()),rnd(),8,1);break;
        case 5:{VkClearValue cv[9]{};for(auto &v:cv)v.color.uint32[0]=rnd(),v.color.uint32[3]=rnd();VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=hd<VkRenderPass>(rnd());b.framebuffer=hd<VkFramebuffer>(rnd());b.renderArea.extent={rnd()%4096,rnd()%4096};
            b.clearValueCount=rnd()%10;b.pClearValues=b.clearValueCount?cv:nullptr;CmdBeginRenderPass(c,&b,VK_SUBPASS_CONTENTS_INLINE);CmdEndRenderPass(c);break;}
        case 6:{VkBufferCopy r[4];for(auto &x:r)x={rnd(),rnd(),rnd()};CmdCopyBuffer(c,hd<VkBuffer>(rnd()),hd<VkBuffer>(rnd()),1+rnd()%4,r);break;}
        case 7:{VkBufferImageCopy r[3]{};for(auto &x:r){x.bufferOffset=rnd();x.imageSubresource={1,rnd()%10,0,1};x.imageExtent={rnd()%512,rnd()%512,1};}CmdCopyBufferToImage(c,hd<VkBuffer>(rnd()),hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1+rnd()%3,r);break;}
        case 8:{VkBufferImageCopy r{};r.bufferOffset=rnd();r.imageSubresource={1,0,0,1};r.imageExtent={rnd()%512,1,1};CmdCopyImageToBuffer(c,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_GENERAL,hd<VkBuffer>(rnd()),1,&r);break;}
        case 9:{VkImageCopy r[2]{};for(auto &x:r){x.srcSubresource={1,0,0,1};x.dstSubresource={1,rnd()%3,0,1};x.extent={rnd()%64,rnd()%64,1};}CmdCopyImage(c,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_GENERAL,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_GENERAL,1+rnd()%2,r);break;}
        case 10:{VkImageBlit r{};r.srcSubresource={1,0,0,1};r.dstSubresource=r.srcSubresource;r.srcOffsets[1]={int32_t(rnd()%99),int32_t(rnd()%99),1};r.dstOffsets[1]={9,9,1};CmdBlitImage(c,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&r,VkFilter(rnd()%2));break;}
        case 11:{VkImageResolve r{};r.srcSubresource={1,0,0,1};r.dstSubresource=r.srcSubresource;r.extent={rnd()%640,rnd()%480,1};CmdResolveImage(c,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&r);break;}
        case 12:{VkClearColorValue v{};v.float32[0]=float(rnd()%7);VkImageSubresourceRange r{1,0,1,0,rnd()%6+1};CmdClearColorImage(c,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&v,1,&r);break;}
        case 13:{VkClearDepthStencilValue v{float(rnd()%3),rnd()%256};VkImageSubresourceRange r{2,0,1,0,1};CmdClearDepthStencilImage(c,hd<VkImage>(rnd()),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&v,1,&r);break;}
        case 14:{VkClearAttachment a[2]{};a[0].aspectMask=1;a[0].colorAttachment=rnd()%8;a[0].clearValue.color.uint32[1]=rnd();a[1].aspectMask=2;a[1].clearValue.depthStencil={0.5f,rnd()};VkClearRect r{{{0,0},{rnd()%100,rnd()%100}},0,1};CmdClearAttachments(c,1+rnd()%2,a,1,&r);break;}
        case 15:CmdBindPipeline(c,VkPipelineBindPoint(rnd()%2),hd<VkPipeline>(rnd()));break;
        case 16:{VkDescriptorSet s=hd<VkDescriptorSet>(rnd());uint32_t d[16];for(auto &x:d)x=rnd();CmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,hd<VkPipelineLayout>(rnd()),0,1,&s,rnd()%17,d);break;}
        case 17:{VkViewport v{0,0,float(rnd()%1280),float(rnd()%720),0,1};CmdSetViewport(c,0,1,&v);VkRect2D r{{0,0},{rnd()%1280,rnd()%720}};CmdSetScissor(c,0,1,&r);break;}
        case 18:{VkBuffer b[3];VkDeviceSize o[3];for(int j=0;j<3;j++){b[j]=hd<VkBuffer>(rnd());o[j]=rnd();}CmdBindVertexBuffers(c,rnd()%16,1+rnd()%3,b,o);break;}
        case 19:CmdBindIndexBuffer(c,hd<VkBuffer>(rnd()),rnd(),VkIndexType(rnd()%2));CmdDrawIndexed(c,rnd(),1+rnd()%3,rnd(),int32_t(rnd())-100,rnd()%4);break;
        case 20:CmdDraw(c,rnd(),1,rnd(),0);break;
        case 21:CmdDrawIndexedIndirect(c,hd<VkBuffer>(rnd()),rnd(),1,20);CmdDrawIndirect(c,hd<VkBuffer>(rnd()),rnd(),1,16);break;
        case 22:CmdDispatch(c,rnd()%64,rnd()%64,1);CmdDispatchIndirect(c,hd<VkBuffer>(rnd()),rnd());break;
        case 23:{uint8_t pc[16];for(auto &x:pc)x=uint8_t(rnd());CmdPushConstants(c,hd<VkPipelineLayout>(rnd()),VK_SHADER_STAGE_FRAGMENT_BIT,0,4*(1+rnd()%4),pc);break;}
        case 24:{VkBuffer a=hd<VkBuffer>(rnd()),b=hd<VkBuffer>(rnd());VkDeviceSize o=rnd(),co=rnd();uint32_t n=rnd()%9;
            if(vkrec::on)vkrec::CmdDrawIndirectCount(vkrec::K_DRAW_IDX_IND_CNT,reinterpret_cast<void*>(&mock_dicnt),c,a,o,b,co,n,20);else mock_dicnt(c,a,o,b,co,n,20);
            if(vkrec::on)vkrec::CmdDrawIndirectCount(vkrec::K_DRAW_IND_CNT,reinterpret_cast<void*>(&mock_dcnt),c,b,co,a,o,n,16);else mock_dcnt(c,b,co,a,o,n,16);break;}
        case 25:{VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};static int chain;m.pNext=(rnd()%8==0)?&chain:nullptr;m.srcAccessMask=rnd();CmdPipelineBarrier(c,1,1,0,1,&m,0,nullptr,0,nullptr);break;}   // pNext: drained, then direct
        case 26:if(big){std::vector<VkImageMemoryBarrier> ib(2500,VkImageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER});for(auto &b:ib)b.image=hd<VkImage>(rnd());CmdPipelineBarrier(c,1,1,0,0,nullptr,0,nullptr,uint32_t(ib.size()),ib.data());}break;   // > ring/8: drained, then direct
        }
    }
    end(c);
    if(vkrec::on)(void)submit(1,&c,fence,true);else{VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&c;VK(vkQueueSubmit(cfg.queue,1,&si,fence));}
    if(k%3==2){int32_t crop[4]={1,2,int32_t(rnd()%99),3};if(vkrec::on)vkrec::present(hd<VkImage>(rnd()),VK_FORMAT_R8G8B8A8_UNORM,1280,720,crop,1);else{vkrec::sync(vkrec::W_PRESENT);dm_present_async_m(hd<VkImage>(rnd()),VK_FORMAT_R8G8B8A8_UNORM,1280,720,crop,1,dm_present_handoff_prepare());}}
}
Run script(int mode,bool ab,int slots){
    T.clear();rnd_state=12345;vkrec::mode=mode;vkrec::on=false;vkrec::window_reset();const uint64_t f0=vkrec::fallbacks;
    for(int k=0;k<slots;k++){
        if(ab)g_ab_off=(k/5)&1;   // A/B: switch every 5 slots, only at slot starts
        slot(hd<VkCommandBuffer>(0x1000+k%3),hd<VkFence>(0x2000+k%3),k,k%17==4);
        if(k%7==3)vkrec::sync(vkrec::W_FINISH);   // like a legacy finish
    }
    vkrec::sync(vkrec::W_SHUTDOWN);g_ab_off=false;
    uint64_t s=0;for(auto x:vkrec::syncs)s+=x;return {T,vkrec::fallbacks-f0,s,uint64_t(present_jobs.load())};
}
int fails=0;
void diff(const char *what,const std::string &a,const std::string &b){size_t i=0;while(i<a.size() && i<b.size() && a[i]==b[i])++i;if(i==a.size() && i==b.size())return;
    auto ctx=[&](const std::string &s){std::string o;for(size_t k=i>160?i-160:0;k<std::min(s.size(),i+160);k++){const char ch=s[k];o.push_back(ch>=32 && ch<127?ch:'.');}return o;};
    printf("%s: first difference at byte %zu of %zu/%zu\n  A: %s\n  B: %s\n",what,i,a.size(),b.size(),ctx(a).c_str(),ctx(b).c_str());}
#define CHECK(c,...) do{if(!(c)){++fails;printf("FAIL %s:%d: ",__FILE__,__LINE__);printf(__VA_ARGS__);printf("\n");}}while(0)
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    setenv("DM_GFX_RECORD_RING_MB","1",1);   // a small ring: wraps every few slots, packets above 128 KB fall back
    cfg.queue=hd<VkQueue>(0x77);
    vkrec::init(1);   // allocates the ring (mode set per run below)
    const int slots=400;
    Run d=script(0,false,slots);
    printf("direct: %zu trace bytes\n",d.trace.size());
    // mode 2: packets replayed inline
    // mode 2 first: the engine never mixes modes, and once the dm-record thread runs it would also consume mode-2 packets
    Run m2=script(2,false,slots);uint64_t w2=vkrec::wpos;diff("mode 2",d.trace,m2.trace);
    CHECK(m2.trace==d.trace,"mode 2 trace differs from direct (%zu vs %zu bytes)",m2.trace.size(),d.trace.size());
    CHECK(m2.fallbacks>0,"no fallback exercised");CHECK(w2>vkrec::cap,"ring did not wrap (%llu bytes written)",(unsigned long long)w2);
    // A/B switching at slot boundaries
    Run dab=script(0,true,slots),m2ab=script(2,true,slots);diff("mode 2 A/B",dab.trace,m2ab.trace);
    CHECK(m2ab.trace==dab.trace,"mode 2 A/B trace differs");
    // mode 1: the dm-record thread, repeated (races)
    Run m1{};int bad1=0,bad1ab=0;
    for(int rep=0;rep<12;rep++){
        m1=script(1,false,slots);if(m1.trace!=d.trace){if(!bad1++)diff("mode 1",d.trace,m1.trace);}
        Run m1ab=script(1,true,slots);if(m1ab.trace!=dab.trace){if(!bad1ab++)diff("mode 1 A/B",dab.trace,m1ab.trace);}}
    CHECK(!bad1,"mode 1 trace differs from direct in %d/12 runs",bad1);CHECK(!bad1ab,"mode 1 A/B trace differs in %d/12 runs",bad1ab);
    // tickets: a submission's ticket is waited for from another thread while the producer keeps recording
    {vkrec::mode=1;vkrec::on=true;T.clear();std::atomic<int> seen{0};const uint64_t base=vkrec::sub_enq;
     std::thread w([&]{for(uint64_t t=base+1;t<=base+200;t++){if(!vkrec::wait_submitted(t,UINT64_MAX)){printf("wait_submitted failed\n");break;}if(vkrec::sub_done.load()<t)break;++seen;}});
     for(int k=0;k<200;k++){VkCommandBuffer c=hd<VkCommandBuffer>(0x1000+k%3);vkrec::begin(c);for(int i=0;i<50;i++)vkrec::CmdDraw(c,3,1,0,0);vkrec::end(c);(void)vkrec::submit(1,&c,hd<VkFence>(0x2000+k%3),false);}
     w.join();vkrec::sync(vkrec::W_SHUTDOWN);CHECK(seen.load()==200,"tickets seen %d/200",seen.load());
     CHECK(!vkrec::wait_submitted(vkrec::sub_enq+1,2000000),"wait_submitted of a future ticket did not time out");}
    printf("fallbacks direct-run %llu, mode2 %llu, mode1 %llu; syncs %llu; ring %llu KB, written %llu KB; record thread executed %llu packets\n",(unsigned long long)d.fallbacks,(unsigned long long)m2.fallbacks,(unsigned long long)m1.fallbacks,(unsigned long long)m1.syncs_n,
        (unsigned long long)(vkrec::cap>>10),(unsigned long long)(vkrec::wpos>>10),(unsigned long long)vkrec::executed.load());
    printf(fails?"t_vkrec: %d FAILED\n":"t_vkrec: PASS\n",fails);fflush(stdout);_exit(fails?1:0);   // the record thread is never joined (as in the engine)
}
