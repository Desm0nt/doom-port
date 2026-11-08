// [mtb] DM_GFX_RECORD_THREAD=1|2 (docs/research/mt_translator_design.md variant (b); docs/SWITCHES.md, BUGS_AND_FIXES 3.21). Unset / 0: every
// vkrec:: wrapper calls Vulkan directly, exactly as before (one predictable branch per call). 1: the translation thread keeps every decision
// (state, caches, hazards, snapshots, descriptor writes, slot ring, chunk cuts, retire) and appends the command-buffer work as packets with
// copied arguments to one ordered ring: every vkCmd* of the engine, vkBegin/EndCommandBuffer of the slot and prologue buffers, the slot's
// vkQueueSubmit (after the present thread's blits, under the presenter's queue lock, as before) and the present hand-off. The dm-record thread
// replays them in order into the same VkCommandBuffers. 2: the same packets replayed on the producer itself at every submit / present / drain
// (no thread): checks the packetisation and the deferred replay, so a lifetime bug shows up without a race.
// Rules (the engine side is in recording_slot / submit_slots / retire_slot / the legacy waits / poolreset / present / finish / shutdown):
//  - one producer: whoever holds the engine mutex; one consumer: dm-record (or the producer in =2). Packets of a slot precede its SUBMIT.
//  - the front never touches a command pool the back may use: per-slot pools are reset / begun only after the slot's fence (so after its
//    SUBMIT ran); a first prologue-buffer allocation, the shared pool (gfx-no-slot-pools) and switching the A/B phase off drain first.
//  - a slot fence is waited for only after its SUBMIT ran (wait_submitted: retire_slot, legacy waits, the poolreset worker).
//  - nothing a pending packet names is destroyed: the engine already defers every pipeline / pass / framebuffer / image / arena chunk it
//    drops to the retire of the recording slot (after its fence, so after every earlier packet ran); bulk teardown (shutdown, finish) drains.
//  - any direct queue operation of the front (inline present, vkQueueWaitIdle) drains first.
// The ring is never resized: a packet larger than 1/8 of it (or with a pNext chain) is executed synchronously after a drain (fallbacks=).
// [pvr] DM_GFX_FENCE_GUARD (xbg_vendor_policy.h, BUGS_AND_FIXES 2.38): how the slot fences are touched from several threads. The
// helpers (status / wait / reset) follow struct Slot (they need vkrec); the record thread's vkQueueSubmit takes the lock below too.
//  0 (default but on PowerVR): as before. 1: a slot fence is not touched before its SUBMIT ran (vkQueueSubmit's fence is externally
//  synchronized in Vulkan: a vkGetFenceStatus of a slot the record thread is submitting is a race; an unsubmitted fence is unsignalled,
//  so the poll answers VK_NOT_READY without the driver). 2 (PowerVR): also one thread at a time on any slot fence, and the waits poll
//  vkGetFenceStatus instead of sleeping in vkWaitForFences. recover (DM_GFX_FENCE_RECOVER, default 1, every GPU; only acts after a
//  driver fence error): a wait that returns VK_TIMEOUT long before its timeout (the PowerVR 1.15 driver did 1 ms after sync_wait EINVAL)
//  polls the status up to the deadline, and a pool-reset worker wait that failed on a fence retire_slot then waited for successfully
//  resets the pool on the translation thread instead of a fatal "GPU hang".
namespace fguard {
std::atomic<int> level{0};std::atomic<bool> recover{true};std::mutex mu;
std::atomic<uint64_t> unsubmitted{0},polls{0},early{0},early_recovered{0},pool_recovered{0};   // NVN PERF fence: unsubmitted / polls per window, the rest whole run
struct Lock{std::unique_lock<std::mutex> l;Lock():l(mu,std::defer_lock){if(level.load(std::memory_order_relaxed)>=2)l.lock();}};
}
// [pvr] DM_GFX_COMPILE_LOCK (xbg_vendor_policy.h, BUGS_AND_FIXES 2.47): the PowerVR compiler is not thread-safe and runs not only in
// vkCreate*Pipelines (gpguard serial) but in vkCreateShaderModule and inside vkCmd* that need driver-made programs (render-pass load /
// store, transfer, clear). mu is THE driver-compiler lock: gpguard::create_mu is this mutex, so pipeline builds, module creation, the
// dm-record packets below and the present thread's blit / clear (xbg_drvc_lock) take turns. Level 1: render-pass, copy, blit, resolve,
// clear, fill and begin / end packets; 2: every packet but submit / present (those wait for the present thread, which takes this lock).
// cur_op: the packet dm-record is executing (0 = none), printed by the crash handler (xbg_crash_rec_op).
namespace drvc {
int level=0;std::mutex mu;std::atomic<uint32_t> cur_op{0};std::atomic<uint64_t> locks{0};
struct Lock{std::unique_lock<std::mutex> l;explicit Lock(bool want):l(mu,std::defer_lock){if(want && level>0){l.lock();locks.fetch_add(1,std::memory_order_relaxed);}}};
}
extern "C" void xbg_drvc_lock(void){if(drvc::level>0)drvc::mu.lock();}
extern "C" void xbg_drvc_unlock(void){if(drvc::level>0)drvc::mu.unlock();}
namespace vkrec {
int mode=0;bool on=false;   // on: the slot being recorded goes through packets (decided when a slot starts recording: A/B target recthread)
enum Op:uint32_t{K_SKIP,K_BEGIN,K_END,K_SUBMIT,K_PRESENT,K_BARRIER,K_BEGIN_QUERY,K_END_QUERY,K_BEGIN_RP,K_END_RP,K_RESET_QP,K_TIMESTAMP,K_COPY_QP,K_COPY_BUF,
    K_COPY_B2I,K_COPY_I2B,K_COPY_IMG,K_BLIT,K_CLEAR_COLOR,K_CLEAR_DS,K_CLEAR_ATT,K_BIND_PIPE,K_BIND_SETS,K_VIEWPORT,K_SCISSOR,K_BIND_VB,K_BIND_IB,K_DRAW_IDX_IND,
    K_DRAW_IND,K_DRAW_IDX,K_DRAW,K_DISPATCH,K_DISPATCH_IND,K_PUSH,K_RESOLVE,K_DRAW_IDX_IND_CNT,K_DRAW_IND_CNT,K_FILL_BUF,K_N};   // K_FILL_BUF: [DOOM] DM_GFX_CRUMBS markers
enum Why{W_SLOT,W_ALLOC,W_BIG,W_PNEXT,W_PRESENT,W_FINISH,W_SHUTDOWN,W_N};   // drains (NVN PERF rec sync_by=slot/alloc/big/pnext/present/finish/shutdown)
// [pvr] DM_GFX_COMPILE_LOCK: the packets (and direct calls) that take drvc::mu. Never submit / present: they wait for the present thread.
inline bool compile_locked(uint32_t op){
    if(drvc::level<=0 || op==K_SKIP || op==K_SUBMIT || op==K_PRESENT)return false;
    if(drvc::level>=2)return true;
    switch(op){case K_BEGIN:case K_END:case K_BEGIN_RP:case K_END_RP:case K_COPY_BUF:case K_COPY_B2I:case K_COPY_I2B:case K_COPY_IMG:case K_BLIT:case K_RESOLVE:
        case K_CLEAR_COLOR:case K_CLEAR_DS:case K_CLEAR_ATT:case K_FILL_BUF:return true;default:return false;}
}
// The Vulkan call of each packet, for the crash report (xbg_crash_rec_op).
constexpr const char *vk_name[K_N]={"skip","vkBeginCommandBuffer","vkEndCommandBuffer","vkQueueSubmit","present hand-off","vkCmdPipelineBarrier","vkCmdBeginQuery",
    "vkCmdEndQuery","vkCmdBeginRenderPass","vkCmdEndRenderPass","vkCmdResetQueryPool","vkCmdWriteTimestamp","vkCmdCopyQueryPoolResults","vkCmdCopyBuffer",
    "vkCmdCopyBufferToImage","vkCmdCopyImageToBuffer","vkCmdCopyImage","vkCmdBlitImage","vkCmdClearColorImage","vkCmdClearDepthStencilImage","vkCmdClearAttachments",
    "vkCmdBindPipeline","vkCmdBindDescriptorSets","vkCmdSetViewport","vkCmdSetScissor","vkCmdBindVertexBuffers","vkCmdBindIndexBuffer","vkCmdDrawIndexedIndirect",
    "vkCmdDrawIndirect","vkCmdDrawIndexed","vkCmdDraw","vkCmdDispatch","vkCmdDispatchIndirect","vkCmdPushConstants","vkCmdResolveImage",
    "vkCmdDrawIndexedIndirectCount","vkCmdDrawIndirectCount","vkCmdFillBuffer"};
static_assert(vk_name[K_N-1]!=nullptr,"vk_name: one name per vkrec::Op");
extern "C" const char *xbg_crash_rec_op(void){const uint32_t v=drvc::cur_op.load(std::memory_order_relaxed);return v && v<=uint32_t(K_N)?vk_name[v-1]:nullptr;}   // host/crash.c (weak)
#define DRVC_CALL(op,call) do{drvc::Lock drvc_g(compile_locked(op));call;}while(0)
struct Hdr{uint32_t op,bytes;VkCommandBuffer cmd;};   // bytes: the whole packet, a multiple of 16
struct PQuery{VkQueryPool pool;uint32_t a,b;};struct PTimestamp{VkQueryPool pool;uint32_t stage,query;};
struct PCopyQP{VkQueryPool pool;VkBuffer dst;VkDeviceSize off,stride;uint32_t first,count,flags,pad;};
struct PBarrier{uint32_t src,dst,dep,nm,nb,ni;};
struct PBeginRP{VkRenderPass rp;VkFramebuffer fb;VkRect2D area;uint32_t nclear,contents;};
struct PCopyBuf{VkBuffer src,dst;uint32_t n,pad;};struct PB2I{VkBuffer buf;VkImage img;uint32_t layout,n;};
struct PImg2{VkImage src,dst;uint32_t sl,dl,n,filter;};   // copy image, blit, resolve
struct PClearColor{VkImage img;uint32_t layout,n;VkClearColorValue v;};struct PClearDS{VkImage img;uint32_t layout,n;VkClearDepthStencilValue v;};
struct PCount2{uint32_t a,b;};   // clear attachments (attachments, rects), viewport / scissor / vertex buffers (first, count)
struct PBindPipe{VkPipeline p;uint32_t bp,pad;};struct PBindSets{VkPipelineLayout l;uint32_t bp,first,ns,nd;};
struct PBindIB{VkBuffer b;VkDeviceSize off;uint32_t type,pad;};struct PDrawInd{VkBuffer b;VkDeviceSize off;uint32_t count,stride;};
struct PDrawIdx{uint32_t ic,inst,fi;int32_t vo;uint32_t finst,pad;};struct PDraw{uint32_t vc,inst,fv,fi;};struct PDispatch{uint32_t x,y,z,pad;};
struct PDispatchInd{VkBuffer b;VkDeviceSize off;};struct PPush{VkPipelineLayout l;uint32_t stages,off,size,pad;};
struct PDrawCnt{void *pfn;VkBuffer b;VkDeviceSize off;VkBuffer cb;VkDeviceSize coff;uint32_t max,stride;};
struct PSubmit{VkFence fence;uint64_t ticket;VkCommandBuffer cbs[2];uint32_t n,pt;};
struct PFill{VkBuffer b;VkDeviceSize off,size;uint32_t data,pad;};   // [DOOM] vkCmdFillBuffer (DM_GFX_CRUMBS)
struct PPresent{VkImage src;uint32_t fmt,w,h;int32_t interval,crop[4];uint32_t has_crop,mirror;};
static_assert(sizeof(Hdr)==16 && sizeof(PBarrier)%8==0 && sizeof(PBeginRP)%8==0 && sizeof(PCopyBuf)%8==0 && sizeof(PB2I)%8==0 && sizeof(PImg2)%8==0 && sizeof(PBindSets)%8==0 && sizeof(PCount2)%8==0,"vkrec payload alignment");
uint8_t *ring=nullptr;uint64_t cap=0,mask=0;
std::atomic<uint64_t> head{0},tail{0};   // byte positions, monotonic: published by the producer / executed by the consumer
uint64_t wpos=0,pend=0;                  // producer: next write position, size of the packet being written
std::atomic<int> state{0};               // dm-record: 0 running, 1 napping (timed wait), 2 asleep (woken by the next packet)
std::atomic<int> waiters{0};std::mutex mu;std::condition_variable cv,done_cv;
std::atomic<uint64_t> sub_done{0};uint64_t sub_enq=0;   // SUBMIT tickets: run by the consumer / handed out by the producer
std::atomic<int> err{0},err_op{0};bool dead=false;      // first failure (VkResult) and its packet; dead: the consumer skips the rest
uint64_t nap_ns=200000,idle_ns=4000000;unsigned cpus_mask=0,last_pin=~0u;std::thread *worker=nullptr;
// NVN PERF rec, per window: producer side (engine lock) and consumer side (atomics; any thread may wait for a submission)
uint64_t packets=0,bytes=0,kicks=0,syncs[W_N]={},sync_ns=0,full_waits=0,full_ns=0,fallbacks=0,slots_on=0,slots_all=0,max_lag=0;
std::atomic<uint64_t> busy_ns{0},executed{0},wakes{0},submit_ns{0},ptwait_ns{0},submits{0},sub_waits{0},sub_wait_ns{0};
const char *op_name(int op){switch(op){case K_BEGIN:return "vkBeginCommandBuffer";case K_END:return "vkEndCommandBuffer";case K_SUBMIT:return "vkQueueSubmit";case K_PRESENT:return "present hand-off";default:return "packet";}}
// The consumer must not use xbg_logf (CPU_SCOPE timing is translation-thread state): the log callback directly.
void rlog(const char *fmt,...){char text[512];va_list a;va_start(a,fmt);vsnprintf(text,sizeof text,fmt,a);va_end(a);if(cfg.log)cfg.log(cfg.userdata,text);else fprintf(stderr,"[xbg] %s\n",text);}
void fail(Op op,int r){int z=0;if(err.compare_exchange_strong(z,r?r:-1))err_op.store(int(op));dead=true;}
void notify_waiters(){if(waiters.load(std::memory_order_seq_cst)){std::lock_guard<std::mutex> l(mu);done_cv.notify_all();}}
void exec(const Hdr *h){
    const VkCommandBuffer c=h->cmd;const uint8_t *p=reinterpret_cast<const uint8_t*>(h+1);
    if(dead && h->op!=K_SUBMIT)return;   // after a failure only the tickets move (waiters wake and see err)
    struct OpMark{explicit OpMark(uint32_t op){drvc::cur_op.store(op+1,std::memory_order_relaxed);}~OpMark(){drvc::cur_op.store(0,std::memory_order_relaxed);}} mark(h->op);
    drvc::Lock compile(compile_locked(h->op));   // [pvr] DM_GFX_COMPILE_LOCK
    switch(h->op){
    case K_SKIP:break;
    case K_BEGIN:{VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;const VkResult r=vkBeginCommandBuffer(c,&b);if(r)fail(K_BEGIN,r);break;}
    case K_END:{const VkResult r=vkEndCommandBuffer(c);if(r)fail(K_END,r);break;}
    case K_SUBMIT:{const PSubmit &q=*reinterpret_cast<const PSubmit*>(p);
        if(!dead){const uint64_t t0=timing_clock();VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=q.n;si.pCommandBuffers=q.cbs;VkResult r=VK_SUCCESS;
            if(q.pt){const uint64_t w=dm_present_wait_submitted();if(w)ptwait_ns.fetch_add(w,std::memory_order_relaxed);   // [mt 2a] after every handed present's blit
                if(const int e=dm_present_last_error())fail(K_PRESENT,e);
                if(!dead){dm_present_queue_lock();{fguard::Lock g;r=vkQueueSubmit(cfg.queue,1,&si,q.fence);}dm_present_queue_unlock();if(r)fail(K_SUBMIT,r);}}
            else{{fguard::Lock g;r=vkQueueSubmit(cfg.queue,1,&si,q.fence);}if(r)fail(K_SUBMIT,r);}
            submit_ns.fetch_add(timing_clock()-t0,std::memory_order_relaxed);submits.fetch_add(1,std::memory_order_relaxed);}
        sub_done.store(q.ticket,std::memory_order_seq_cst);notify_waiters();break;}
    case K_PRESENT:{const PPresent &q=*reinterpret_cast<const PPresent*>(p);
        const int r=dm_present_async_m(q.src,VkFormat(q.fmt),q.w,q.h,q.has_crop?q.crop:nullptr,q.interval,int(q.mirror));if(r)fail(K_PRESENT,r);break;}
    case K_BARRIER:{const PBarrier &q=*reinterpret_cast<const PBarrier*>(p);const auto *m=reinterpret_cast<const VkMemoryBarrier*>(&q+1);const auto *b=reinterpret_cast<const VkBufferMemoryBarrier*>(m+q.nm);const auto *im=reinterpret_cast<const VkImageMemoryBarrier*>(b+q.nb);
        vkCmdPipelineBarrier(c,q.src,q.dst,q.dep,q.nm,q.nm?m:nullptr,q.nb,q.nb?b:nullptr,q.ni,q.ni?im:nullptr);break;}
    case K_BEGIN_QUERY:{const PQuery &q=*reinterpret_cast<const PQuery*>(p);vkCmdBeginQuery(c,q.pool,q.a,q.b);break;}
    case K_END_QUERY:{const PQuery &q=*reinterpret_cast<const PQuery*>(p);vkCmdEndQuery(c,q.pool,q.a);break;}
    case K_RESET_QP:{const PQuery &q=*reinterpret_cast<const PQuery*>(p);vkCmdResetQueryPool(c,q.pool,q.a,q.b);break;}
    case K_TIMESTAMP:{const PTimestamp &q=*reinterpret_cast<const PTimestamp*>(p);vkCmdWriteTimestamp(c,VkPipelineStageFlagBits(q.stage),q.pool,q.query);break;}
    case K_COPY_QP:{const PCopyQP &q=*reinterpret_cast<const PCopyQP*>(p);vkCmdCopyQueryPoolResults(c,q.pool,q.first,q.count,q.dst,q.off,q.stride,q.flags);break;}
    case K_BEGIN_RP:{const PBeginRP &q=*reinterpret_cast<const PBeginRP*>(p);VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=q.rp;b.framebuffer=q.fb;b.renderArea=q.area;
        b.clearValueCount=q.nclear;b.pClearValues=q.nclear?reinterpret_cast<const VkClearValue*>(&q+1):nullptr;vkCmdBeginRenderPass(c,&b,VkSubpassContents(q.contents));break;}
    case K_END_RP:vkCmdEndRenderPass(c);break;
    case K_COPY_BUF:{const PCopyBuf &q=*reinterpret_cast<const PCopyBuf*>(p);vkCmdCopyBuffer(c,q.src,q.dst,q.n,reinterpret_cast<const VkBufferCopy*>(&q+1));break;}
    case K_COPY_B2I:{const PB2I &q=*reinterpret_cast<const PB2I*>(p);vkCmdCopyBufferToImage(c,q.buf,q.img,VkImageLayout(q.layout),q.n,reinterpret_cast<const VkBufferImageCopy*>(&q+1));break;}
    case K_COPY_I2B:{const PB2I &q=*reinterpret_cast<const PB2I*>(p);vkCmdCopyImageToBuffer(c,q.img,VkImageLayout(q.layout),q.buf,q.n,reinterpret_cast<const VkBufferImageCopy*>(&q+1));break;}
    case K_COPY_IMG:{const PImg2 &q=*reinterpret_cast<const PImg2*>(p);vkCmdCopyImage(c,q.src,VkImageLayout(q.sl),q.dst,VkImageLayout(q.dl),q.n,reinterpret_cast<const VkImageCopy*>(&q+1));break;}
    case K_BLIT:{const PImg2 &q=*reinterpret_cast<const PImg2*>(p);vkCmdBlitImage(c,q.src,VkImageLayout(q.sl),q.dst,VkImageLayout(q.dl),q.n,reinterpret_cast<const VkImageBlit*>(&q+1),VkFilter(q.filter));break;}
    case K_RESOLVE:{const PImg2 &q=*reinterpret_cast<const PImg2*>(p);vkCmdResolveImage(c,q.src,VkImageLayout(q.sl),q.dst,VkImageLayout(q.dl),q.n,reinterpret_cast<const VkImageResolve*>(&q+1));break;}
    case K_CLEAR_COLOR:{const PClearColor &q=*reinterpret_cast<const PClearColor*>(p);vkCmdClearColorImage(c,q.img,VkImageLayout(q.layout),&q.v,q.n,reinterpret_cast<const VkImageSubresourceRange*>(&q+1));break;}
    case K_CLEAR_DS:{const PClearDS &q=*reinterpret_cast<const PClearDS*>(p);vkCmdClearDepthStencilImage(c,q.img,VkImageLayout(q.layout),&q.v,q.n,reinterpret_cast<const VkImageSubresourceRange*>(&q+1));break;}
    case K_CLEAR_ATT:{const PCount2 &q=*reinterpret_cast<const PCount2*>(p);const auto *a=reinterpret_cast<const VkClearAttachment*>(&q+1);vkCmdClearAttachments(c,q.a,a,q.b,reinterpret_cast<const VkClearRect*>(a+q.a));break;}
    case K_BIND_PIPE:{const PBindPipe &q=*reinterpret_cast<const PBindPipe*>(p);vkCmdBindPipeline(c,VkPipelineBindPoint(q.bp),q.p);break;}
    case K_BIND_SETS:{const PBindSets &q=*reinterpret_cast<const PBindSets*>(p);const auto *s=reinterpret_cast<const VkDescriptorSet*>(&q+1);
        vkCmdBindDescriptorSets(c,VkPipelineBindPoint(q.bp),q.l,q.first,q.ns,s,q.nd,q.nd?reinterpret_cast<const uint32_t*>(s+q.ns):nullptr);break;}
    case K_VIEWPORT:{const PCount2 &q=*reinterpret_cast<const PCount2*>(p);vkCmdSetViewport(c,q.a,q.b,reinterpret_cast<const VkViewport*>(&q+1));break;}
    case K_SCISSOR:{const PCount2 &q=*reinterpret_cast<const PCount2*>(p);vkCmdSetScissor(c,q.a,q.b,reinterpret_cast<const VkRect2D*>(&q+1));break;}
    case K_BIND_VB:{const PCount2 &q=*reinterpret_cast<const PCount2*>(p);const auto *b=reinterpret_cast<const VkBuffer*>(&q+1);vkCmdBindVertexBuffers(c,q.a,q.b,b,reinterpret_cast<const VkDeviceSize*>(b+q.b));break;}
    case K_BIND_IB:{const PBindIB &q=*reinterpret_cast<const PBindIB*>(p);vkCmdBindIndexBuffer(c,q.b,q.off,VkIndexType(q.type));break;}
    case K_DRAW_IDX_IND:{const PDrawInd &q=*reinterpret_cast<const PDrawInd*>(p);vkCmdDrawIndexedIndirect(c,q.b,q.off,q.count,q.stride);break;}
    case K_DRAW_IND:{const PDrawInd &q=*reinterpret_cast<const PDrawInd*>(p);vkCmdDrawIndirect(c,q.b,q.off,q.count,q.stride);break;}
    case K_DRAW_IDX:{const PDrawIdx &q=*reinterpret_cast<const PDrawIdx*>(p);vkCmdDrawIndexed(c,q.ic,q.inst,q.fi,q.vo,q.finst);break;}
    case K_DRAW:{const PDraw &q=*reinterpret_cast<const PDraw*>(p);vkCmdDraw(c,q.vc,q.inst,q.fv,q.fi);break;}
    case K_DISPATCH:{const PDispatch &q=*reinterpret_cast<const PDispatch*>(p);vkCmdDispatch(c,q.x,q.y,q.z);break;}
    case K_DISPATCH_IND:{const PDispatchInd &q=*reinterpret_cast<const PDispatchInd*>(p);vkCmdDispatchIndirect(c,q.b,q.off);break;}
    case K_PUSH:{const PPush &q=*reinterpret_cast<const PPush*>(p);vkCmdPushConstants(c,q.l,q.stages,q.off,q.size,&q+1);break;}
    case K_DRAW_IDX_IND_CNT:{const PDrawCnt &q=*reinterpret_cast<const PDrawCnt*>(p);reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCountKHR>(q.pfn)(c,q.b,q.off,q.cb,q.coff,q.max,q.stride);break;}
    case K_DRAW_IND_CNT:{const PDrawCnt &q=*reinterpret_cast<const PDrawCnt*>(p);reinterpret_cast<PFN_vkCmdDrawIndirectCountKHR>(q.pfn)(c,q.b,q.off,q.cb,q.coff,q.max,q.stride);break;}
    case K_FILL_BUF:{const PFill &q=*reinterpret_cast<const PFill*>(p);vkCmdFillBuffer(c,q.b,q.off,q.size,q.data);break;}   // [DOOM] DM_GFX_CRUMBS
    default:fail(Op(h->op),-1);break;}
}
// Executes every published packet on the calling thread (dm-record, or the producer in =2).
void run_avail(){
    uint64_t t=tail.load(std::memory_order_relaxed),n=0;
    for(;;){const uint64_t h=head.load(std::memory_order_acquire);if(t>=h)break;
        while(t<h){const Hdr *p=reinterpret_cast<const Hdr*>(ring+(t&mask));exec(p);t+=p->bytes;tail.store(t,std::memory_order_seq_cst);++n;}}
    executed.fetch_add(n,std::memory_order_relaxed);
}
void run_inline(){const uint64_t t0=timing_clock();run_avail();busy_ns.fetch_add(timing_clock()-t0,std::memory_order_relaxed);}
// dm-record: off the translation thread's pinned cores (it inherits that pin from its creator), or DM_GFX_RECORD_CPUS=<hex mask>.
// [DOOM] Or the CPU plan's record mask (android/cpu_topo.c) while the pin is the planned one: on one-big-cluster SoCs (SD680 4+4,
// Helio G99 2+6) the next big core after the translator's instead of "every core but the pin" (= the little cores there).
// On flagships the plan's mask is the old one (every core but the pin).
void apply_affinity(bool first){
    const unsigned pin=dm_present_pin_mask?dm_present_pin_mask():0u;if(!first && pin==last_pin)return;last_pin=pin;
    const unsigned plan=!cpus_mask && pin && dm_present_record_mask?dm_present_record_mask():0u;
    cpu_set_t set;CPU_ZERO(&set);long n=sysconf(_SC_NPROCESSORS_CONF);if(n<1 || n>32)n=n<1?8:32;
    for(long c=0;c<n;c++)if(cpus_mask?(cpus_mask>>c&1):plan?(plan>>c&1):!(pin>>c&1))CPU_SET(int(c),&set);
    if(!CPU_COUNT(&set))for(long c=0;c<n;c++)CPU_SET(int(c),&set);
    const int r=sched_setaffinity(0,sizeof set,&set);unsigned m=0;for(long c=0;c<n;c++)if(CPU_ISSET(int(c),&set))m|=1u<<c;
    rlog("NVN rec thread tid=%d affinity=%#x (translation pin %#x%s) result=%d nice=%d",int(gettid()),m,pin,cpus_mask?", DM_GFX_RECORD_CPUS":plan?", cpu plan":"",r,getpriority(PRIO_PROCESS,0));
}
void loop(){
    pthread_setname_np(pthread_self(),"dm-record");if(setpriority(PRIO_PROCESS,0,-8))setpriority(PRIO_PROCESS,0,-4);
    apply_affinity(true);uint64_t last=timing_clock();
    for(;;){
        if(tail.load(std::memory_order_relaxed)<head.load(std::memory_order_acquire)){const uint64_t t0=timing_clock();run_avail();const uint64_t t1=timing_clock();
            busy_ns.fetch_add(t1-t0,std::memory_order_relaxed);last=t1;notify_waiters();continue;}
        bool slept=false;
        {std::unique_lock<std::mutex> l(mu);const bool nap=timing_clock()-last<idle_ns;state.store(nap?1:2,std::memory_order_seq_cst);
         if(head.load(std::memory_order_seq_cst)==tail.load(std::memory_order_relaxed)){if(nap)cv.wait_for(l,std::chrono::nanoseconds(nap_ns));else{cv.wait(l);slept=true;}wakes.fetch_add(1,std::memory_order_relaxed);}
         state.store(0,std::memory_order_seq_cst);}
        if(slept)apply_affinity(false);   // the pin mask may have been set up after this thread started
    }
}
void start(){if(!worker)worker=new std::thread(loop);}   // never joined (as poolreset): it sleeps when there is nothing to record
void wake(){std::lock_guard<std::mutex> l(mu);cv.notify_one();++kicks;}
// Producer side (engine lock held).
void check_err(){if(const int e=err.load(std::memory_order_acquire)){const int op=err_op.load();if(op==K_PRESENT)xbg_fatalf("Game presentation failed: %d (record thread)",e);xbg_fatalf("%s failed on the record thread: %d",op_name(op),e);}}
void wait_tail(uint64_t target){
    for(int i=0;i<64 && tail.load(std::memory_order_acquire)<target;i++)std::this_thread::yield();
    if(tail.load(std::memory_order_acquire)>=target)return;
    waiters.fetch_add(1,std::memory_order_seq_cst);
    {std::unique_lock<std::mutex> l(mu);if(state.load(std::memory_order_seq_cst))cv.notify_one();done_cv.wait(l,[&]{return tail.load(std::memory_order_seq_cst)>=target || err.load()!=0;});}
    waiters.fetch_sub(1,std::memory_order_seq_cst);
}
// Wait until every published packet ran (mode 2: run them here). Call before anything that needs the command buffers or the queue idle.
void sync(int why){
    if(!mode)return;
    if(tail.load(std::memory_order_acquire)>=wpos){check_err();return;}
    ++syncs[why];const uint64_t t0=timing_clock();
    if(mode==2)run_inline();else{wake();wait_tail(wpos);}
    sync_ns+=timing_clock()-t0;check_err();
}
void make_room(uint64_t need){
    const uint64_t t0=timing_clock();++full_waits;
    if(mode==2)run_inline();else{wake();wait_tail(wpos+need-cap);}
    full_ns+=timing_clock()-t0;check_err();
}
// Any thread: true once the SUBMIT with this ticket ran (0 = submitted directly), false on a record-thread failure or the timeout.
bool wait_submitted(uint64_t t,uint64_t timeout_ns=UINT64_MAX){
    if(!t || sub_done.load(std::memory_order_acquire)>=t)return true;
    if(err.load())return false;
    const uint64_t t0=timing_clock();bool ok;waiters.fetch_add(1,std::memory_order_seq_cst);
    {std::unique_lock<std::mutex> l(mu);if(state.load(std::memory_order_seq_cst))cv.notify_one();
     auto pred=[&]{return sub_done.load(std::memory_order_seq_cst)>=t || err.load()!=0;};
     if(timeout_ns>=1000000000000ull)done_cv.wait(l,pred);else done_cv.wait_for(l,std::chrono::nanoseconds(timeout_ns),pred);
     ok=sub_done.load(std::memory_order_seq_cst)>=t;}
    waiters.fetch_sub(1,std::memory_order_seq_cst);sub_waits.fetch_add(1,std::memory_order_relaxed);sub_wait_ns.fetch_add(timing_clock()-t0,std::memory_order_relaxed);return ok;
}
inline uint8_t *alloc(Op op,VkCommandBuffer c,size_t payload){
    const uint64_t n=(sizeof(Hdr)+payload+15)&~uint64_t(15);if(n>cap/8)return nullptr;
    uint64_t off=wpos&mask;const uint64_t skip=off+n>cap?cap-off:0;
    if(wpos+skip+n-tail.load(std::memory_order_acquire)>cap)make_room(skip+n);
    if(skip){Hdr *k=reinterpret_cast<Hdr*>(ring+off);k->op=K_SKIP;k->bytes=uint32_t(skip);k->cmd=VK_NULL_HANDLE;wpos+=skip;bytes+=skip;off=0;}
    Hdr *h=reinterpret_cast<Hdr*>(ring+off);h->op=op;h->bytes=uint32_t(n);h->cmd=c;pend=n;return reinterpret_cast<uint8_t*>(h+1);
}
inline void commit(bool urgent=false){
    wpos+=pend;bytes+=pend;pend=0;++packets;head.store(wpos,std::memory_order_seq_cst);
    if(mode==2){if(urgent)run_inline();return;}
    const uint64_t lag=wpos-tail.load(std::memory_order_relaxed);if(lag>max_lag)max_lag=lag;if(lag>cap/4)urgent=true;
    const int st=state.load(std::memory_order_seq_cst);if(st==2 || (st==1 && urgent))wake();
}
// A slot starts recording: decide packets or direct calls for it (A/B target recthread switches only here). Switching to direct calls, or a
// pool the consumer may be using (the shared pool of gfx-no-slot-pools), drains first.
void slot_start(bool shared_pool){
    if(!mode)return;
    const bool want=!mt_off("recthread");
    if((on && !want) || shared_pool)sync(W_SLOT);
    on=want;++slots_all;if(on){++slots_on;if(mode==1)start();}
}
void begin(VkCommandBuffer c){if(!on){VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;DRVC_CALL(K_BEGIN,VK(vkBeginCommandBuffer(c,&b)));return;}alloc(K_BEGIN,c,0);commit();}
void end(VkCommandBuffer c){if(!on){DRVC_CALL(K_END,VK(vkEndCommandBuffer(c)));return;}alloc(K_END,c,0);commit();}
// The slot's queue submission (only with on); returns its ticket (Slot::rec_ticket).
uint64_t submit(uint32_t n,const VkCommandBuffer *cbs,VkFence fence,bool pt){
    PSubmit &q=*reinterpret_cast<PSubmit*>(alloc(K_SUBMIT,VK_NULL_HANDLE,sizeof(PSubmit)));const uint64_t t=++sub_enq;
    q.fence=fence;q.ticket=t;q.n=n;q.pt=pt;q.cbs[0]=cbs[0];q.cbs[1]=n>1?cbs[1]:VK_NULL_HANDLE;commit(true);return t;
}
// The present hand-off after the frame's last slot submission (only with on and the present thread): what dm_present_async took on the
// caller (core pin, ADPF frame thread, the mirror flag the engine set just before) is taken here, the job is handed over by the consumer.
void present(VkImage src,VkFormat fmt,uint32_t w,uint32_t h,const int32_t *crop,int32_t interval){
    const int mirror=dm_present_handoff_prepare();
    PPresent &q=*reinterpret_cast<PPresent*>(alloc(K_PRESENT,VK_NULL_HANDLE,sizeof(PPresent)));
    q.src=src;q.fmt=uint32_t(fmt);q.w=w;q.h=h;q.interval=interval;q.has_crop=crop!=nullptr;if(crop)memcpy(q.crop,crop,sizeof q.crop);else memset(q.crop,0,sizeof q.crop);q.mirror=uint32_t(mirror!=0);commit(true);
}
void init(int m){
    mode=m;on=false;if(!mode)return;
    if(!ring){auto env_u=[](const char *n,long d){const char *e=getenv(n);return e && *e?strtol(e,nullptr,0):d;};
        uint64_t mb=uint64_t(std::clamp<long>(env_u("DM_GFX_RECORD_RING_MB",8),1,64));cap=mb<<20;mask=cap-1;
        void *mem=nullptr;if(posix_memalign(&mem,64,size_t(cap)))mem=nullptr;ring=static_cast<uint8_t*>(mem);if(!ring){xbg_logf("NVN rec: ring of %llu MB not allocated, DM_GFX_RECORD_THREAD off",(unsigned long long)mb);mode=0;return;}
        nap_ns=uint64_t(std::clamp<long>(env_u("DM_GFX_RECORD_NAP_US",200),20,5000))*1000;cpus_mask=unsigned(env_u("DM_GFX_RECORD_CPUS",0));}
    xbg_logf("NVN rec switches: record_thread=%d (%s) ring=%lluMB nap_us=%llu cpus=%#x (A/B target recthread, group mt)",mode,mode==1?"dm-record thread":"same packets replayed inline",(unsigned long long)(cap>>20),(unsigned long long)(nap_ns/1000),cpus_mask);
}
// Wrappers: the engine's vkCmd* calls. Off: the call itself. A pNext chain or an oversized packet: drain, then the call (fallbacks=).
#define VKREC_DIRECT(why,call) do{sync(why);++fallbacks;{drvc::Lock drvc_g(true);call;}return;}while(0)   // [pvr] under DM_GFX_COMPILE_LOCK (any level)
inline void CmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im){
    if(!on){vkCmdPipelineBarrier(c,s,d,f,nm,m,nb,b,ni,im);return;}
    bool chained=false;for(uint32_t i=0;i<nm;i++)chained|=m[i].pNext!=nullptr;for(uint32_t i=0;i<nb;i++)chained|=b[i].pNext!=nullptr;for(uint32_t i=0;i<ni;i++)chained|=im[i].pNext!=nullptr;
    if(chained)VKREC_DIRECT(W_PNEXT,vkCmdPipelineBarrier(c,s,d,f,nm,m,nb,b,ni,im));
    uint8_t *p=alloc(K_BARRIER,c,sizeof(PBarrier)+nm*sizeof(*m)+nb*sizeof(*b)+ni*sizeof(*im));if(!p)VKREC_DIRECT(W_BIG,vkCmdPipelineBarrier(c,s,d,f,nm,m,nb,b,ni,im));
    PBarrier &q=*reinterpret_cast<PBarrier*>(p);q.src=s;q.dst=d;q.dep=f;q.nm=nm;q.nb=nb;q.ni=ni;uint8_t *w=p+sizeof q;
    if(nm){memcpy(w,m,nm*sizeof(*m));w+=nm*sizeof(*m);}if(nb){memcpy(w,b,nb*sizeof(*b));w+=nb*sizeof(*b);}if(ni)memcpy(w,im,ni*sizeof(*im));commit();
}
inline void CmdBeginQuery(VkCommandBuffer c,VkQueryPool pool,uint32_t i,VkQueryControlFlags f){if(!on){vkCmdBeginQuery(c,pool,i,f);return;}PQuery &q=*reinterpret_cast<PQuery*>(alloc(K_BEGIN_QUERY,c,sizeof(PQuery)));q.pool=pool;q.a=i;q.b=f;commit();}
inline void CmdEndQuery(VkCommandBuffer c,VkQueryPool pool,uint32_t i){if(!on){vkCmdEndQuery(c,pool,i);return;}PQuery &q=*reinterpret_cast<PQuery*>(alloc(K_END_QUERY,c,sizeof(PQuery)));q.pool=pool;q.a=i;q.b=0;commit();}
inline void CmdResetQueryPool(VkCommandBuffer c,VkQueryPool pool,uint32_t first,uint32_t n){if(!on){vkCmdResetQueryPool(c,pool,first,n);return;}PQuery &q=*reinterpret_cast<PQuery*>(alloc(K_RESET_QP,c,sizeof(PQuery)));q.pool=pool;q.a=first;q.b=n;commit();}
inline void CmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits stage,VkQueryPool pool,uint32_t i){if(!on){vkCmdWriteTimestamp(c,stage,pool,i);return;}PTimestamp &q=*reinterpret_cast<PTimestamp*>(alloc(K_TIMESTAMP,c,sizeof(PTimestamp)));q.pool=pool;q.stage=uint32_t(stage);q.query=i;commit();}
inline void CmdCopyQueryPoolResults(VkCommandBuffer c,VkQueryPool pool,uint32_t first,uint32_t n,VkBuffer dst,VkDeviceSize off,VkDeviceSize stride,VkQueryResultFlags f){
    if(!on){vkCmdCopyQueryPoolResults(c,pool,first,n,dst,off,stride,f);return;}PCopyQP &q=*reinterpret_cast<PCopyQP*>(alloc(K_COPY_QP,c,sizeof(PCopyQP)));q.pool=pool;q.dst=dst;q.off=off;q.stride=stride;q.first=first;q.count=n;q.flags=f;q.pad=0;commit();}
inline void CmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo *b,VkSubpassContents contents){
    if(!on){DRVC_CALL(K_BEGIN_RP,vkCmdBeginRenderPass(c,b,contents));return;}
    if(b->pNext)VKREC_DIRECT(W_PNEXT,vkCmdBeginRenderPass(c,b,contents));
    const uint32_t nc=b->pClearValues?b->clearValueCount:0;uint8_t *p=alloc(K_BEGIN_RP,c,sizeof(PBeginRP)+nc*sizeof(VkClearValue));if(!p)VKREC_DIRECT(W_BIG,vkCmdBeginRenderPass(c,b,contents));
    PBeginRP &q=*reinterpret_cast<PBeginRP*>(p);q.rp=b->renderPass;q.fb=b->framebuffer;q.area=b->renderArea;q.nclear=nc;q.contents=uint32_t(contents);if(nc)memcpy(p+sizeof q,b->pClearValues,nc*sizeof(VkClearValue));commit();
}
inline void CmdEndRenderPass(VkCommandBuffer c){if(!on){DRVC_CALL(K_END_RP,vkCmdEndRenderPass(c));return;}alloc(K_END_RP,c,0);commit();}
inline void CmdCopyBuffer(VkCommandBuffer c,VkBuffer src,VkBuffer dst,uint32_t n,const VkBufferCopy *r){
    if(!on){DRVC_CALL(K_COPY_BUF,vkCmdCopyBuffer(c,src,dst,n,r));return;}uint8_t *p=alloc(K_COPY_BUF,c,sizeof(PCopyBuf)+n*sizeof(*r));if(!p)VKREC_DIRECT(W_BIG,vkCmdCopyBuffer(c,src,dst,n,r));
    PCopyBuf &q=*reinterpret_cast<PCopyBuf*>(p);q.src=src;q.dst=dst;q.n=n;q.pad=0;if(n)memcpy(p+sizeof q,r,n*sizeof(*r));commit();}
inline void CmdCopyBufferToImage(VkCommandBuffer c,VkBuffer buf,VkImage img,VkImageLayout l,uint32_t n,const VkBufferImageCopy *r){
    if(!on){DRVC_CALL(K_COPY_B2I,vkCmdCopyBufferToImage(c,buf,img,l,n,r));return;}uint8_t *p=alloc(K_COPY_B2I,c,sizeof(PB2I)+n*sizeof(*r));if(!p)VKREC_DIRECT(W_BIG,vkCmdCopyBufferToImage(c,buf,img,l,n,r));
    PB2I &q=*reinterpret_cast<PB2I*>(p);q.buf=buf;q.img=img;q.layout=uint32_t(l);q.n=n;if(n)memcpy(p+sizeof q,r,n*sizeof(*r));commit();}
inline void CmdCopyImageToBuffer(VkCommandBuffer c,VkImage img,VkImageLayout l,VkBuffer buf,uint32_t n,const VkBufferImageCopy *r){
    if(!on){DRVC_CALL(K_COPY_I2B,vkCmdCopyImageToBuffer(c,img,l,buf,n,r));return;}uint8_t *p=alloc(K_COPY_I2B,c,sizeof(PB2I)+n*sizeof(*r));if(!p)VKREC_DIRECT(W_BIG,vkCmdCopyImageToBuffer(c,img,l,buf,n,r));
    PB2I &q=*reinterpret_cast<PB2I*>(p);q.buf=buf;q.img=img;q.layout=uint32_t(l);q.n=n;if(n)memcpy(p+sizeof q,r,n*sizeof(*r));commit();}
template<class R>inline bool img2(Op op,VkCommandBuffer c,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const R *r,uint32_t filter){
    uint8_t *p=alloc(op,c,sizeof(PImg2)+n*sizeof(R));if(!p)return false;
    PImg2 &q=*reinterpret_cast<PImg2*>(p);q.src=src;q.dst=dst;q.sl=uint32_t(sl);q.dl=uint32_t(dl);q.n=n;q.filter=filter;if(n)memcpy(p+sizeof q,r,n*sizeof(R));commit();return true;}
inline void CmdCopyImage(VkCommandBuffer c,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageCopy *r){
    if(!on){DRVC_CALL(K_COPY_IMG,vkCmdCopyImage(c,src,sl,dst,dl,n,r));return;}if(!img2(K_COPY_IMG,c,src,sl,dst,dl,n,r,0))VKREC_DIRECT(W_BIG,vkCmdCopyImage(c,src,sl,dst,dl,n,r));}
inline void CmdBlitImage(VkCommandBuffer c,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageBlit *r,VkFilter f){
    if(!on){DRVC_CALL(K_BLIT,vkCmdBlitImage(c,src,sl,dst,dl,n,r,f));return;}if(!img2(K_BLIT,c,src,sl,dst,dl,n,r,uint32_t(f)))VKREC_DIRECT(W_BIG,vkCmdBlitImage(c,src,sl,dst,dl,n,r,f));}
inline void CmdResolveImage(VkCommandBuffer c,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t n,const VkImageResolve *r){
    if(!on){DRVC_CALL(K_RESOLVE,vkCmdResolveImage(c,src,sl,dst,dl,n,r));return;}if(!img2(K_RESOLVE,c,src,sl,dst,dl,n,r,0))VKREC_DIRECT(W_BIG,vkCmdResolveImage(c,src,sl,dst,dl,n,r));}
inline void CmdClearColorImage(VkCommandBuffer c,VkImage img,VkImageLayout l,const VkClearColorValue *v,uint32_t n,const VkImageSubresourceRange *r){
    if(!on){DRVC_CALL(K_CLEAR_COLOR,vkCmdClearColorImage(c,img,l,v,n,r));return;}uint8_t *p=alloc(K_CLEAR_COLOR,c,sizeof(PClearColor)+n*sizeof(*r));if(!p)VKREC_DIRECT(W_BIG,vkCmdClearColorImage(c,img,l,v,n,r));
    PClearColor &q=*reinterpret_cast<PClearColor*>(p);q.img=img;q.layout=uint32_t(l);q.n=n;q.v=*v;if(n)memcpy(p+sizeof q,r,n*sizeof(*r));commit();}
inline void CmdClearDepthStencilImage(VkCommandBuffer c,VkImage img,VkImageLayout l,const VkClearDepthStencilValue *v,uint32_t n,const VkImageSubresourceRange *r){
    if(!on){DRVC_CALL(K_CLEAR_DS,vkCmdClearDepthStencilImage(c,img,l,v,n,r));return;}uint8_t *p=alloc(K_CLEAR_DS,c,sizeof(PClearDS)+n*sizeof(*r));if(!p)VKREC_DIRECT(W_BIG,vkCmdClearDepthStencilImage(c,img,l,v,n,r));
    PClearDS &q=*reinterpret_cast<PClearDS*>(p);q.img=img;q.layout=uint32_t(l);q.n=n;q.v=*v;if(n)memcpy(p+sizeof q,r,n*sizeof(*r));commit();}
inline void CmdClearAttachments(VkCommandBuffer c,uint32_t na,const VkClearAttachment *a,uint32_t nr,const VkClearRect *r){
    if(!on){DRVC_CALL(K_CLEAR_ATT,vkCmdClearAttachments(c,na,a,nr,r));return;}uint8_t *p=alloc(K_CLEAR_ATT,c,sizeof(PCount2)+na*sizeof(*a)+nr*sizeof(*r));if(!p)VKREC_DIRECT(W_BIG,vkCmdClearAttachments(c,na,a,nr,r));
    PCount2 &q=*reinterpret_cast<PCount2*>(p);q.a=na;q.b=nr;if(na)memcpy(p+sizeof q,a,na*sizeof(*a));if(nr)memcpy(p+sizeof q+na*sizeof(*a),r,nr*sizeof(*r));commit();}
inline void CmdBindPipeline(VkCommandBuffer c,VkPipelineBindPoint bp,VkPipeline pipe){if(!on){vkCmdBindPipeline(c,bp,pipe);return;}PBindPipe &q=*reinterpret_cast<PBindPipe*>(alloc(K_BIND_PIPE,c,sizeof(PBindPipe)));q.p=pipe;q.bp=uint32_t(bp);q.pad=0;commit();}
inline void CmdBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint bp,VkPipelineLayout l,uint32_t first,uint32_t ns,const VkDescriptorSet *s,uint32_t nd,const uint32_t *d){
    if(!on){vkCmdBindDescriptorSets(c,bp,l,first,ns,s,nd,d);return;}uint8_t *p=alloc(K_BIND_SETS,c,sizeof(PBindSets)+ns*sizeof(*s)+nd*sizeof(uint32_t));if(!p)VKREC_DIRECT(W_BIG,vkCmdBindDescriptorSets(c,bp,l,first,ns,s,nd,d));
    PBindSets &q=*reinterpret_cast<PBindSets*>(p);q.l=l;q.bp=uint32_t(bp);q.first=first;q.ns=ns;q.nd=nd;if(ns)memcpy(p+sizeof q,s,ns*sizeof(*s));if(nd)memcpy(p+sizeof q+ns*sizeof(*s),d,nd*sizeof(uint32_t));commit();}
inline void CmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t n,const VkViewport *v){
    if(!on){vkCmdSetViewport(c,first,n,v);return;}uint8_t *p=alloc(K_VIEWPORT,c,sizeof(PCount2)+n*sizeof(*v));if(!p)VKREC_DIRECT(W_BIG,vkCmdSetViewport(c,first,n,v));
    PCount2 &q=*reinterpret_cast<PCount2*>(p);q.a=first;q.b=n;if(n)memcpy(p+sizeof q,v,n*sizeof(*v));commit();}
inline void CmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t n,const VkRect2D *r){
    if(!on){vkCmdSetScissor(c,first,n,r);return;}uint8_t *p=alloc(K_SCISSOR,c,sizeof(PCount2)+n*sizeof(*r));if(!p)VKREC_DIRECT(W_BIG,vkCmdSetScissor(c,first,n,r));
    PCount2 &q=*reinterpret_cast<PCount2*>(p);q.a=first;q.b=n;if(n)memcpy(p+sizeof q,r,n*sizeof(*r));commit();}
inline void CmdBindVertexBuffers(VkCommandBuffer c,uint32_t first,uint32_t n,const VkBuffer *b,const VkDeviceSize *o){
    if(!on){vkCmdBindVertexBuffers(c,first,n,b,o);return;}uint8_t *p=alloc(K_BIND_VB,c,sizeof(PCount2)+n*(sizeof(*b)+sizeof(*o)));if(!p)VKREC_DIRECT(W_BIG,vkCmdBindVertexBuffers(c,first,n,b,o));
    PCount2 &q=*reinterpret_cast<PCount2*>(p);q.a=first;q.b=n;if(n){memcpy(p+sizeof q,b,n*sizeof(*b));memcpy(p+sizeof q+n*sizeof(*b),o,n*sizeof(*o));}commit();}
inline void CmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize off,VkIndexType t){if(!on){vkCmdBindIndexBuffer(c,b,off,t);return;}PBindIB &q=*reinterpret_cast<PBindIB*>(alloc(K_BIND_IB,c,sizeof(PBindIB)));q.b=b;q.off=off;q.type=uint32_t(t);q.pad=0;commit();}
inline void CmdDrawIndexedIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize off,uint32_t n,uint32_t stride){if(!on){vkCmdDrawIndexedIndirect(c,b,off,n,stride);return;}PDrawInd &q=*reinterpret_cast<PDrawInd*>(alloc(K_DRAW_IDX_IND,c,sizeof(PDrawInd)));q.b=b;q.off=off;q.count=n;q.stride=stride;commit();}
inline void CmdDrawIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize off,uint32_t n,uint32_t stride){if(!on){vkCmdDrawIndirect(c,b,off,n,stride);return;}PDrawInd &q=*reinterpret_cast<PDrawInd*>(alloc(K_DRAW_IND,c,sizeof(PDrawInd)));q.b=b;q.off=off;q.count=n;q.stride=stride;commit();}
inline void CmdDrawIndexed(VkCommandBuffer c,uint32_t ic,uint32_t inst,uint32_t fi,int32_t vo,uint32_t finst){if(!on){vkCmdDrawIndexed(c,ic,inst,fi,vo,finst);return;}PDrawIdx &q=*reinterpret_cast<PDrawIdx*>(alloc(K_DRAW_IDX,c,sizeof(PDrawIdx)));q.ic=ic;q.inst=inst;q.fi=fi;q.vo=vo;q.finst=finst;q.pad=0;commit();}
inline void CmdDraw(VkCommandBuffer c,uint32_t vc,uint32_t inst,uint32_t fv,uint32_t fi){if(!on){vkCmdDraw(c,vc,inst,fv,fi);return;}PDraw &q=*reinterpret_cast<PDraw*>(alloc(K_DRAW,c,sizeof(PDraw)));q.vc=vc;q.inst=inst;q.fv=fv;q.fi=fi;commit();}
inline void CmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){if(!on){vkCmdDispatch(c,x,y,z);return;}PDispatch &q=*reinterpret_cast<PDispatch*>(alloc(K_DISPATCH,c,sizeof(PDispatch)));q.x=x;q.y=y;q.z=z;q.pad=0;commit();}
inline void CmdDispatchIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize off){if(!on){vkCmdDispatchIndirect(c,b,off);return;}PDispatchInd &q=*reinterpret_cast<PDispatchInd*>(alloc(K_DISPATCH_IND,c,sizeof(PDispatchInd)));q.b=b;q.off=off;commit();}
inline void CmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags st,uint32_t off,uint32_t size,const void *v){
    if(!on){vkCmdPushConstants(c,l,st,off,size,v);return;}uint8_t *p=alloc(K_PUSH,c,sizeof(PPush)+size);if(!p)VKREC_DIRECT(W_BIG,vkCmdPushConstants(c,l,st,off,size,v));
    PPush &q=*reinterpret_cast<PPush*>(p);q.l=l;q.stages=st;q.off=off;q.size=size;q.pad=0;if(size)memcpy(p+sizeof q,v,size);commit();}
// vkCmdDraw*IndirectCountKHR through the device function pointer the engine looked up (the packet carries it).
inline void CmdDrawIndirectCount(Op op,void *pfn,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkBuffer cb,VkDeviceSize co,uint32_t n,uint32_t stride){
    PDrawCnt &q=*reinterpret_cast<PDrawCnt*>(alloc(op,c,sizeof(PDrawCnt)));q.pfn=pfn;q.b=b;q.off=o;q.cb=cb;q.coff=co;q.max=n;q.stride=stride;commit();}
// [DOOM] vkCmdFillBuffer (DM_GFX_CRUMBS GPU progress markers)
inline void CmdFillBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize off,VkDeviceSize size,uint32_t data){if(!on){DRVC_CALL(K_FILL_BUF,vkCmdFillBuffer(c,b,off,size,data));return;}PFill &q=*reinterpret_cast<PFill*>(alloc(K_FILL_BUF,c,sizeof(PFill)));q.b=b;q.off=off;q.size=size;q.data=data;q.pad=0;commit();}
#undef VKREC_DIRECT
// NVN PERF rec, once per PERF window (f = frames); also reset at an A/B phase flip (mt_window_reset).
uint64_t b_busy=0,b_exec=0,b_wakes=0,b_submit=0,b_ptwait=0,b_submits=0,b_subwaits=0,b_subwait_ns=0;
void window_reset(){packets=bytes=kicks=sync_ns=full_waits=full_ns=fallbacks=slots_on=slots_all=max_lag=0;for(auto &s:syncs)s=0;
    b_busy=busy_ns.load();b_exec=executed.load();b_wakes=wakes.load();b_submit=submit_ns.load();b_ptwait=ptwait_ns.load();b_submits=submits.load();b_subwaits=sub_waits.load();b_subwait_ns=sub_wait_ns.load();}
void report(int ab,double f){
    const double busy=(busy_ns.load()-b_busy)/1e6/f,sync_ms=sync_ns/1e6/f,full_ms=full_ns/1e6/f,subwait_ms=(sub_wait_ns.load()-b_subwait_ns)/1e6/f;
    uint64_t sync_n=0;for(auto s:syncs)sync_n+=s;
    xbg_logf("NVN PERF rec ab=%d mode=%d on=%.2f packets=%.0f kb=%.1f executed=%.0f kicks=%.1f wakes=%.1f syncs=%.2f sync_ms=%.3f sync_by=slot%llu/alloc%llu/big%llu/pnext%llu/present%llu/finish%llu/shutdown%llu"
        " full=%.2f full_ms=%.3f subwaits=%.2f subwait_ms=%.3f fallbacks=%llu busy_ms=%.3f submit_ms=%.3f ptwait_ms=%.3f submits=%.2f saved_ms=%.3f max_lag_kb=%.1f err=%d",
        ab,mode,slots_all?double(slots_on)/double(slots_all):0.0,packets/f,bytes/1024.0/f,(executed.load()-b_exec)/f,kicks/f,(wakes.load()-b_wakes)/f,sync_n/f,sync_ms,
        (unsigned long long)syncs[W_SLOT],(unsigned long long)syncs[W_ALLOC],(unsigned long long)syncs[W_BIG],(unsigned long long)syncs[W_PNEXT],(unsigned long long)syncs[W_PRESENT],(unsigned long long)syncs[W_FINISH],(unsigned long long)syncs[W_SHUTDOWN],
        full_waits/f,full_ms,(sub_waits.load()-b_subwaits)/f,subwait_ms,(unsigned long long)fallbacks,busy,(submit_ns.load()-b_submit)/1e6/f,(ptwait_ns.load()-b_ptwait)/1e6/f,(submits.load()-b_submits)/f,
        mode==1?busy-sync_ms-full_ms-subwait_ms:0.0,max_lag/1024.0,err.load());
    window_reset();
}
}
// [nonadreno] One GPU BC decode batch (issue #6; BcUpload / bcgpu): input = region table + raw blocks in a stream-arena range, copied
// into bcgpu's input buffer, decoded by xbg_bc_decode.comp into bcgpu's output buffer, then copied into the image (copies: offsets in it).
// verify: [nonadreno] DM_GFX_BC_VERIFY (bcv): the readbacks of this batch, null when not checked.
struct BcVerify;
struct BcJob{VkBuffer buffer;VkDeviceSize offset,size;VkPipeline pipeline;uint32_t regions,blocks,bpp,block_words;VkImage image;std::vector<VkBufferImageCopy> copies;std::shared_ptr<BcVerify> verify{};};
namespace bcgpu{void record(VkCommandBuffer cmd,const BcJob &job);}
struct Shader;
