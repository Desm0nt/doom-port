// [ETR perf, KB PERF-E04] GPU-side synchronisation switches and their counters, ported from the DOOM Eternal port's xbg_gsync.h
// (src-cmp16: gpu a2ca20f, dispatch rules e24b901 / 96cf24c). Only DM_GFX_RP_DEPS and DM_GFX_DISPATCH_RUNS: DM_GFX_CLEAR_OUTSIDE gave
// Eternal no win and flickered, DM_GFX_PASS_GMEM was not asked for. Textually included by xbg_game.cpp twice: once early
// (XBG_GSYNC_EARLY: switches, counters, declarations; before full_barrier / end_open_pass / Commands) and once late (XBG_GSYNC_LATE:
// the parts that need Pass and Descriptors; after the lazy-clear state, before begin_pass). Not a standalone header.
// Both switches default to 0 = the former recording. One line per perf window: NVN PERF gpu-sync (the counters run in every mode, so a
// mode-0 log already shows what each switch could remove: dispatches adjacent= without guest_between= are what DISPATCH_RUNS skips).
//
//  DM_GFX_RP_DEPS (boot only; render passes are cached; bits): the external subpass dependencies of every render pass the engine creates
//    (create_pass, its noload twin, the lazy-clear and store-op twins).
//    0: start ALL_COMMANDS / MEMORY_WRITE -> attachment access, end attachment writes -> ALL_COMMANDS / MEMORY_READ|WRITE. On Turnip
//       (tu_barrier, tu_flush_for_access / _for_stage) the end one is a CCU clean + invalidate of every cache (UCHE, CCU, CCHE) + WFI +
//       WAIT_FOR_BR at every pass end, the start one a clean of every cache + WFI. (DM_GFX_TILE_BARRIER narrows the stages first.)
//    bit 0x1: lean start, graphics attachment writes -> attachment access (CCU clean + invalidate + WFI, no UCHE work). Writes of
//       transfers and dispatches are ordered by the full barriers of Commands; storage images written by draws get a full barrier before
//       the pass (lean_pre).
//    bit 0x2: no end dependency. Every later reader is ordered by the engine itself: prepare_read (written / layout / attached), the full
//       barriers of every non-draw Commands, the slot-start barrier, present (a Commands). A pass with a depth attachment then makes earlier
//       attachment writes visible to shader reads at its start (UCHE invalidate) for the depth-feedback shortcut of prepare_read.
//    [DOOM] with either bit, a draw whose pre-fragment stages write storage (the culling GS) after a pass ended gets the full barrier the
//       old dependencies implied, once per run of passes: the same write-after-read guard as DM_GFX_TILE_BARRIER (draw(), passes_since_full).
//    The pipeline / render-pass key keeps the mode-0 dependencies (pipelines and the warm list stay valid; dependencies do not affect
//    render-pass compatibility).
//  DM_GFX_DISPATCH_RUNS (boot; A/B target disprun): 1 = two dispatches with nothing recorded between them and no guest
//    nvnCommandBufferBarrier between them run without our two full barriers (NVN orders compute -> compute only by the guest's barriers;
//    a 3D <-> compute switch keeps the full barrier, as Maxwell's class switch waits for idle). The dispatch's closing barrier becomes
//    pending (post_pending) and lands before the next recorded operation of any other kind (Commands, also a draw's); its buffer results
//    join the lazy storage tracking (storage_unbarriered / storage_dirty / cs_unbarriered: a reader through view_writeback or an in-place
//    chain still gets its barrier); a sampled read of a storage image it wrote still barriers in prepare_read (storage_write > barrier_seq).
//    Excluded (keep both barriers): indirect dispatches, the colour-table dispatch of DM_GFX_BRIGHT (bright::after_dispatch records its
//    own copy / dispatch in the same span).
#ifdef XBG_GSYNC_EARLY
namespace gsync {
int rp_deps=0,disp_runs=0;bool inited=false;   // set by init() (boot)
bool post_pending=false,guest_seen=false;   // DM_GFX_DISPATCH_RUNS: a dispatch's closing full barrier not recorded yet; a guest OP_BARRIER since the last dispatch / full barrier
uint64_t work=0,work_at_barrier=~0ull,tail_work=~0ull;   // work: recorded GPU operations (draws, non-draw Commands), never reset; tail_work: work right after the last dispatch
// per window (NVN PERF gpu-sync)
uint64_t fb_all=0,fb_merged=0,begins=0,restarts=0,restart_by[4]{},disp_n=0,disp_adjacent=0,disp_guest=0,disp_skipped=0,disp_flushed=0,lean_pre_n=0,war_n=0;
uint64_t rp_made=0,rp_made_lean=0;   // run totals: render passes created, of them with the DM_GFX_RP_DEPS dependencies
VkFramebuffer last_fb{};unsigned last_op=0;
inline void init(){
    if(inited)return;inited=true;
    const char *e=getenv("DM_GFX_RP_DEPS");rp_deps=e && *e?int(strtol(e,nullptr,0))&3:0;
    e=getenv("DM_GFX_DISPATCH_RUNS");disp_runs=e && *e?atoi(e):0;
    xbg_logf("NVN gpu-sync: DM_GFX_RP_DEPS=%d (0x1 lean pass start, 0x2 no pass-end dependency) DM_GFX_DISPATCH_RUNS=%d (adjacent dispatches without a guest barrier: no full barriers between them)",rp_deps,disp_runs);
}
// full_barrier: one with no recorded GPU work since the previous one costs nothing more on Turnip (flush bits are emitted at the next draw / blit / dispatch)
inline void barrier_note(){++fb_all;if(work==work_at_barrier)++fb_merged;work_at_barrier=work;guest_seen=false;post_pending=false;cs_unbarriered=false;}   // a full barrier also is the pending one ([DOOM] and orders the run's results: no stale cs_unbarriered splitting a later pass)
// vkCmdBeginRenderPass (begin_pass, lazy_clear_flush, lazy_clear_begin): a pass on the framebuffer the last ended pass used = a split of one pass
inline void on_begin(VkFramebuffer fb){
    ++begins;
    if(fb && fb==last_fb){++restarts;const unsigned o=last_op;const bool draw=o==OP_DRAW || o==OP_DRAW_IDX || o==OP_X_MULTI_DRAW_INDIRECT || o==OP_X_DRAW_CALL || o==OP_X_DRAW_INDIRECT || o==62;
        ++restart_by[o==60?0:draw?1:(o==OP_CLEAR_COLOR || o==OP_CLEAR_DS || o==OP_X_TYPED_CLEAR)?2:3];}   // 60 = slot / chunk cut (submit_slots), 62 = a writeback draw
}
inline void on_end(VkFramebuffer fb,unsigned op){last_fb=fb;last_op=op;}
inline void note_made(){++rp_made;if(rp_deps)++rp_made_lean;}   // create_pass made a render pass (cached or not; twins not counted)
// Defined in the XBG_GSYNC_LATE part.
uint32_t rp_deps_make(const VkSubpassDependency in[2],bool has_depth,VkSubpassDependency out[2]);
void report(double f);
}
#endif

#ifdef XBG_GSYNC_LATE
namespace gsync {
// The dependencies a render pass is created with; in[] are the former ones (the key keeps them). Returns the count (1 or 2).
uint32_t rp_deps_make(const VkSubpassDependency in[2],bool has_depth,VkSubpassDependency out[2]){
    init();uint32_t n=0;VkSubpassDependency d0=in[0];
    if(rp_deps&1){
        d0.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;d0.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        d0.dstStageMask=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    // without the end dependency nothing invalidates the texture cache between the pass that wrote a depth image and a later pass that samples it
    // through prepare_read's depth-feedback shortcut (no barrier there): such a pass makes them visible to shader reads at its start
    if((rp_deps&2) && has_depth){d0.dstStageMask|=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;d0.dstAccessMask|=VK_ACCESS_SHADER_READ_BIT;}
    out[n++]=d0;
    if(!(rp_deps&2))out[n++]=in[1];
    return n;
}
// DM_GFX_RP_DEPS bit 0x1: storage-image writes of draws are not in the lean start dependency (it orders attachment writes only)
inline void lean_pre(const Pass &p,VkCommandBuffer cmd){
    if(!(rp_deps&1))return;
    for(Image *im:p.images)if(im->storage_write>barrier_seq){full_barrier(cmd);++lean_pre_n;return;}
}
inline bool runs_on(){return disp_runs>0 && !ab_off("disprun") && !ab_off("etrperf");}
// dispatch(): before its Commands. True = record it without the opening full barrier (DM_GFX_DISPATCH_RUNS).
bool dispatch_pre(bool excluded){
    ++disp_n;const bool adjacent=work==tail_work;
    if(adjacent){++disp_adjacent;if(guest_seen)++disp_guest;}
    bool skip=false;
    if(post_pending){post_pending=false;
        if(runs_on() && adjacent && !guest_seen && !excluded){skip=true;++disp_skipped;}
        else{end_open_pass();full_barrier(recording_slot().cmd);++disp_flushed;}}
    guest_seen=false;return skip;
}
// dispatch(): in place of c.finish(). The closing barrier becomes pending under DM_GFX_DISPATCH_RUNS; skipped: dispatch_pre left out
// its opening barrier (the previous dispatch's pending barrier is carried on when this one records nothing).
void dispatch_post(Commands &c,Descriptors &d,bool excluded){
    if(runs_on() && !excluded){c.lazy=true;post_pending=true;
        if(d.needs_writeback()){storage_unbarriered=true;for(auto &v:d.written)storage_dirty.push_back({v.first.first,v.first.first+v.first.second});
            if(storage_dirty.size()>4096)storage_dirty.clear(),storage_dirty.push_back({0,UINT64_MAX});cs_unbarriered=true;}}
    c.finish();tail_work=work;
}
void report(double f){
    init();
    perf_vlogf("NVN PERF gpu-sync per_frame full_barriers=%.1f merged=%.1f drains=%.1f pass_begins=%.1f same_fb_restarts=%.1f (cut %.1f, draw %.1f, clear %.1f, other %.1f) lean_pre=%.1f war=%.1f "
             "dispatches=%.1f adjacent=%.1f guest_between=%.1f runs skipped=%.1f flushed=%.1f | rp_deps=%d passes_made=%llu lean=%llu runs=%d",
             fb_all/f,fb_merged/f,(fb_all-fb_merged)/f,begins/f,restarts/f,restart_by[0]/f,restart_by[1]/f,restart_by[2]/f,restart_by[3]/f,lean_pre_n/f,war_n/f,
             disp_n/f,disp_adjacent/f,disp_guest/f,disp_skipped/f,disp_flushed/f,rp_deps,(unsigned long long)rp_made,(unsigned long long)rp_made_lean,disp_runs);
    fb_all=fb_merged=begins=restarts=disp_n=disp_adjacent=disp_guest=disp_skipped=disp_flushed=lean_pre_n=war_n=0;
    for(auto &r:restart_by)r=0;
}
}
#endif
