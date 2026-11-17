// [DOOM] DM_GFX_TU=sgsr2 (default off; BUGS_AND_FIXES 2.74, docs/research/temporal_upscale.md route B1): a temporal upscaler in
// place of "the game's TAA + FSR 1 in the presenter". Prototype. Included by xbg_game.cpp after namespace taafix; with the switch
// unset nothing here runs (tu::on() is one integer test in draw() and in the present).
//
// The upscaler is Qualcomm's Snapdragon Game Super Resolution 2, the 2-pass fragment variant (BSD-3-Clause, re-implemented in
// tu/sgsr2_convert.frag and tu/sgsr2_upscale.frag; docs/THIRD_PARTY.md). Everything it needs the game already makes:
//   TAA resolve draw (taa_vsps, ps 615e5352a9db7427; Shader::tu_role 1), once a frame:
//     - its uniforms (previous view-projection rows, frustum rays, view origin, resolution scale, depth linearisation) and its
//       inputs slot 0 depth / slot 1 RG16F object velocity are taken here, and pass 1 ("convert": motion vector + depth-clip
//       factor per render pixel, the game's own computeVelocity) is recorded right before the draw, while both textures hold
//       this frame for sure;
//     - while the upscaler is live the draw runs with tu/tu_notaa.frag instead of the game's module: the game's
//       taadisabledtransfers logic on the TAA program's interface (colour passed through, the motion-blur velocity target
//       written as before). The game itself keeps TAA on (r_antialiasing 6..9), so it keeps jittering the projection
//       (r_jitter follows r_antialiasing) and keeps the -1 texture LOD bias of TSSAA. DM_GFX_TU_NOTAA=0 keeps the game's TAA
//       (the upscaler then runs without jitter over the resolved image: softer, for A/B of the bloom / motion-blur risk);
//   depth pre-pass draws with motion vectors (prezdrawmotionvector* vertex shaders; tu_role 3): the first one of a frame gives
//     the frame's jitter, (mvpmatrix - mvpmatrixnojitter) / mvpmatrixw of its vertex uniforms, in NDC. A frame without such a
//     draw runs with jitter 0 (counted: nojitter= in the status line);
//   the last scene pass (viewcolorupsample_vsps, ps 270b1b7a7fe4d8ab; tu_role 2): slot 0 = the post-processed LDR scene at the
//     render resolution (before the UI), slot 1 = the UI layer. The draw itself is left alone (its window texture is the
//     fallback frame);
//   present: when this frame had all three and the presented texture is that draw's target, pass 2 ("upscale") is recorded
//     into the present's command buffer: scene + history + motion -> the new history (scene only) and the frame to present
//     (scene + UI blended as the game blends it), both at the window's size (dm_present_window_size, vulkan_present.cpp; it
//     answers only with DM_GFX_UPSCALE=fsr, which the gear choice sets as well so that the swapchain is at the window size).
//     That frame is handed to the presenter upright instead of the window texture, so no flip blit and, being the window's
//     size already, no FSR pass. Any frame that lacks a piece (menus, loading, videos: no TAA draw) is presented as before
//     (FSR 1 with DM_GFX_UPSCALE=fsr) and the history starts again on the next upscaled frame.
// History reset: first frame, a frame gap, a change of the output or render-target size, a camera cut (view direction turned
// by more than 60 degrees or the origin moved more than DM_GFX_TU_CUT units in one frame).
// Needs the NVN lower-left origin (DM_GFX_TAA_FIX bit 2, DOOM's default): the game's rows are then the Switch's rows, which
// the game's reprojection maths assumes.
// Any failure (a Vulkan object, a format) is logged once and turns the upscaler off for the run; the old path goes on.
// Switches (docs/SWITCHES.md): DM_GFX_TU=sgsr2, DM_GFX_TU_NOTAA=0, DM_GFX_TU_DEBUG=mv|clip|nohist|input,
// DM_GFX_TU_ROUTE=replay|b1, DM_GFX_TU_JITTER=<sx>,<sy> (jitter multipliers, e.g. -1,1 or 0,0 to check the sign), DM_GFX_TU_MAXH=<rows> (cap the output
// height; the presenter's FSR pass does the rest), DM_GFX_TU_CUT=<units>, DM_GFX_TU_HISTORY=16f (RGBA16F history instead of
// A2B10G10R10). DM_GFX_TU_INPUT=reduced is read by host/hle_args.c (3D render fraction 75 %).
// Status: "NVN TU sgsr2 ..." lines: one at the start, one when it goes live, one every ~3 s with the GPU time of the passes.
//
// [2.75] Route A-replay (DM_GFX_TU_ROUTE=replay, the default; =b1 is everything described above). On the phone route B1 made the
// scene's brightness pulse in motion: the game's bloom, auto-exposure and motion blur were computed from the jittered frame that
// nothing had accumulated yet. Here the upscaler takes the TAA resolve's place instead, and the game's post chain gets its output:
//   TAA resolve draw: pass 1 as before, then pass 2 at once (tu/sgsr2_upscale.frag compiled with TU_HDR): the game's linear HDR
//     colour before TAA (slot 2, jittered) + our history -> the accumulated scene at the output size, linear HDR. The draw itself
//     runs with tu/tu_down.frag: RT0 = that image brought down to the render size, RT1 = the motion-blur velocity target. So the
//     game's whole chain after TAA (max-tile velocity, motion blur, bloom and luminance chains, auto-exposure, postprocess,
//     viewcolorupsample) runs untouched at the render size on a picture that is accumulated and does not jitter, and its frame
//     stays the fallback frame;
//   motion blur draws (motionblurlowq_vsps, ps 5b8eb6217af85abb; tu_role 4) and the postprocess draw (postprocess_vsps, ps
//     daa747b766d39197; tu_role 5): after the game's draw the same draw is issued a second time ("replayed") by the engine's own
//     draw path with four things changed for that draw: the colour input (slot 1) is the output-size image of the texture the
//     game bound there, the target is an output-size image of ours, viewport and scissor are scaled, and the first vector of the
//     fragment uniform block (renderpositiontoviewtexture: gl_FragCoord -> uv) has its scale divided by the ratio. Both shaders
//     work in uv only, so every other input (velocity targets, bloom, flares, lens dirt, overlays, exposure) is read from the
//     game's own render-size textures by the same uv. The bloom and exposure chains are not replayed: they are tiny (<= 400x180)
//     and now fed by the stable render-size image;
//   present: the replayed postprocess image (LDR at the output size) + the UI layer -> the frame (tu/tu_final.frag), upright.
// "Our" images are engine images made from tex_t descriptions we fabricate (struct Fake: the game's target description with the
// size multiplied, own serial, no guest memory), so the engine's pass / barrier / descriptor machinery handles them as it handles
// the game's. A texture of ours is put in front of a game shader through a descriptor pool table of our own (the draw's other
// texture ids are mirrored into it), a uniform block through a host copy. The targets are the game's targets times the upscale
// ratio, so that a uv means the same in both (with the 3D view at 75 % that is 3200x1440 for a 2400x1080 output).
// A frame the replay cannot do is presented as before (FSR 1); five such frames in a row turn the route to b1 for the run.
extern "C" __attribute__((weak)) int dm_present_window_size(int *w,int *h);
namespace tu {
const uint32_t vs_code[]={
#include "tu/tu_fullscreen.vert.spv.inc"
};
const uint32_t notaa_code[]={
#include "tu/tu_notaa.frag.spv.inc"
};
const uint32_t convert_code[]={
#include "tu/sgsr2_convert.frag.spv.inc"
};
const uint32_t upscale_code[]={
#include "tu/sgsr2_upscale.frag.spv.inc"
};
const uint32_t hup_code[]={   // [2.75] the same pass with TU_HDR, for the TAA point
#include "tu/sgsr2_upscale_hdr.frag.spv.inc"
};
const uint32_t down_code[]={
#include "tu/tu_down.frag.spv.inc"
};
const uint32_t final_code[]={
#include "tu/tu_final.frag.spv.inc"
};
int diag_left=0;bool diag_auto=true;uint64_t diag_frame=UINT64_MAX;unsigned diag_lines=0;   // [2.75] DM_GFX_TU_DIAG (diag_line below)
int mode_v=-1,debug_v=0,max_h=0,route_v=1;bool mb_on=true;   // route_v: 1 A-replay (2.75), 0 B1; mb_on: the motion blur draws are replayed too (DM_GFX_TU_MB)
bool failed=false,live=false,notaa_on=false,hist16=false;   // [17.11] default: the game's TAA stays (owner: the only variant with steady light), SGSR2 upscales its output (B1)
bool game_taa=true,hdr_tm=true,hist11=false;   // [2.75] route replay: the game's own TAA resolve stays (DM_GFX_TU_GAMETAA), tone map around the HDR accumulation (DM_GFX_TU_HDRTM), history in the game's R11G11B10F instead of RGBA16F (DM_GFX_TU_HISTORY=11f)
float jit_mul[2]={1,1},cut_units=256;
int mode(){if(mode_v<0){const char *e=getenv("DM_GFX_TU");mode_v=e && (!strcmp(e,"sgsr2") || !strcmp(e,"1"))?1:0;
    if(e && *e && !mode_v && strcmp(e,"0") && strcmp(e,"off"))xbg_logf("NVN TU: DM_GFX_TU=%s is not known (sgsr2 | 0): off",e);
    if(mode_v){
        if(const char *d=getenv("DM_GFX_TU_DEBUG"))debug_v=!strcmp(d,"mv")?1:!strcmp(d,"clip")?2:!strcmp(d,"nohist")?3:!strcmp(d,"input")?4:std::clamp(atoi(d),0,4);
        if(const char *j=getenv("DM_GFX_TU_JITTER")){float a=1,b=1;if(sscanf(j,"%f,%f",&a,&b)==2){jit_mul[0]=a;jit_mul[1]=b;}}
        // [17.11] DM_GFX_TU_NOTAA=1: the TAA resolve replaced (B1 pass-through, or the stand-in of route replay); DM_GFX_TU_ROUTE=replay alone
        // implies it. Default 0: B1 over the game's TAA (the phone tests: B1 pass-through and A-replay made surfaces / light flicker in motion).
        {const char *r=getenv("DM_GFX_TU_ROUTE");if(r && !strcmp(r,"replay"))notaa_on=true;}
        if(const char *n=getenv("DM_GFX_TU_NOTAA"))notaa_on=atoi(n)!=0;
        if(const char *m=getenv("DM_GFX_TU_MAXH"))max_h=std::max(0,atoi(m));
        if(const char *c=getenv("DM_GFX_TU_CUT"))cut_units=float(std::max(1.0,atof(c)));
        if(const char *h=getenv("DM_GFX_TU_HISTORY")){hist16=!strcmp(h,"16f");hist11=!strcmp(h,"11f");}
        if(const char *g=getenv("DM_GFX_TU_GAMETAA"))game_taa=atoi(g)!=0;
        if(const char *t=getenv("DM_GFX_TU_HDRTM"))hdr_tm=atoi(t)!=0;
        if(route_v || getenv("DM_GFX_TU_ROUTE"))xbg_logf("NVN TU sgsr2 replay options: game TAA for the game's own chain=%d (DM_GFX_TU_GAMETAA; 0 = the upscaled scene brought down instead), tone map around the HDR accumulation=%d (DM_GFX_TU_HDRTM), history %s (DM_GFX_TU_HISTORY=11f|16f)",int(game_taa),int(hdr_tm),hist11?"in the TAA target's format":"RGBA16F");
        if(const char *r=getenv("DM_GFX_TU_ROUTE")){if(!strcmp(r,"b1"))route_v=0;else if(strcmp(r,"replay"))xbg_logf("NVN TU: DM_GFX_TU_ROUTE=%s is not known (replay | b1): replay",r);}
        if(!notaa_on)route_v=0;   // keeping the game's TAA is a route B1 variant
        if(const char *m=getenv("DM_GFX_TU_MB"))mb_on=atoi(m)!=0;
        if(const char *g=getenv("DM_GFX_TU_DIAG")){diag_left=std::clamp(atoi(g),0,64);diag_auto=false;}   // given (also 0): no automatic one-frame listing
        const char *in=getenv("DM_GFX_TU_INPUT");
        xbg_logf("NVN TU sgsr2 on (DM_GFX_TU): SGSR2 2-pass fragment upscale, route=%s (DM_GFX_TU_ROUTE), input=%s (DM_GFX_TU_INPUT), game TAA %s (DM_GFX_TU_NOTAA=%d), motion blur at the output size=%d (DM_GFX_TU_MB, route replay), debug=%d jitter x%g,%g max_h=%d cut=%g%s%s",
            route_v?"replay: at the TAA point, the game's motion blur and postprocess replayed at the output size":"b1: at the viewcolorupsample point",in && *in?in:"full",notaa_on?(route_v?"replaced by the upscaler":"replaced by a pass-through while live"):"kept, no jitter used",int(notaa_on),int(mb_on),debug_v,double(jit_mul[0]),double(jit_mul[1]),max_h,double(cut_units),
            taafix::lower_left()?"":"; NOT ACTIVE: needs the NVN lower-left origin (DM_GFX_TAA_FIX bit 2)",dm_present_window_size?"":"; NOT ACTIVE: this presenter gives no window size");}}
    return mode_v;}
inline bool on(){return mode()>0 && !failed;}
void fail(const char *what,int r){if(!failed)xbg_logf("NVN TU sgsr2 off: %s failed (VkResult %d); the game's TAA and the old present path from now on",what,r);failed=true;live=false;}

// ---- what the frame's draws gave
struct Tex{tex_t t{};view_t v{};bool has_view=false;};
struct Taa{uint64_t frame=UINT64_MAX;float hi[56]{},lo[28]{};uint32_t rw=0,rh=0,tw=0,th=0;bool converted=false,still=false,cut=false,raw=false;unsigned slot=0;} taa;   // raw: this frame's TAA draw ran as the pass-through   // hi / lo: the TAA draw's two uniform blocks
struct Up{uint64_t frame=UINT64_MAX,target=0;Tex scene,ui;bool has_ui=false;float hi[12]{},lo[8]{};} up;
struct Jit{uint64_t frame=UINT64_MAX;double x=0,y=0;} jit;   // NDC
// history state
uint64_t last_frame=UINT64_MAX,retry_at=0;bool hist_valid=false;unsigned hist_cur=0,fin_next=0;float prev_cam[15]{},prev_fwd[3]{};bool have_prev_cam=false;unsigned static_frames=0;   // prev_cam: the last TAA draw's frustum rays + origin
// counters of the ~3 s status window
uint64_t n_frames=0,n_fallback=0,n_reset=0,n_nojit=0,n_static=0,n_post=0,n_foreign=0,win_t0=0;const char *last_why="";double conv_ns=0,up_ns=0,post_ns=0,fin_ns=0,max_ns=0;unsigned timed=0;float last_jit[2]={};

// The texture bound to fragment texture slot `slot` of the draw being translated (as taafix::probe resolves it)
bool bound(const Shader *ps,unsigned slot,Tex &out){
    for(const auto &b:ps->bindings){
        if(b.source!=XBG_SOURCE_TEXTURE || b.slot!=slot)continue;
        const uint64_t handle=state.tex[1][b.slot];const int id=int(handle&0xfffff);if(!id || !state.texture_pool.pool)return false;
        auto pool=textures.find(pool_key(&state.texture_pool));const auto &tab=pool==textures.end()?legacy_textures:pool->second;auto t=tab.find(id);if(t==tab.end())return false;
        out.t=t->second.texture;out.v=t->second.view;out.has_view=t->second.has_view;return true;}
    return false;}
// The image of a texture, only when it can be sampled as it is (no upload, no creation here)
Image *sampled(const Tex &x){Image *im=find_image(x.t.serial);return im && im->gpu_written && im->samples==VK_SAMPLE_COUNT_1_BIT && (im->usage&VK_IMAGE_USAGE_SAMPLED_BIT)?im:nullptr;}

// ---- Vulkan objects
struct Tgt{VkImage image{};VkDeviceMemory memory{};VkImageView view{};uint32_t w=0,h=0;VkFormat fmt=VK_FORMAT_UNDEFINED;};
constexpr unsigned RING=8;   // descriptor sets / timestamp queries per upscaler frame: a slot is reused 8 such frames later, long after its commands ran
constexpr unsigned QN=12;    // queries of a slot: 0/1 convert, 2/3 upscale, [2.75] 4..9 up to three replayed post draws, 10/11 the final pass
VkSampler s_linear{},s_nearest{};VkDescriptorSetLayout dsl[3]{};VkPipelineLayout pl[3]{};VkDescriptorPool dpool{};VkDescriptorSet sets[3][RING]{};   // 0 convert, 1 upscale, 2 [2.75] final
VkShaderModule vs_mod{};VkRenderPass rp_conv{},rp_up{};VkPipeline pipe_conv{},pipe_up{};VkFormat hist_fmt=VK_FORMAT_UNDEFINED,fin_fmt=VK_FORMAT_UNDEFINED;
Tgt motion,hist[2],fin[3];VkFramebuffer fb_motion{},fb_up[2][3]{};
VkQueryPool queries{};uint8_t stamped[RING]{};unsigned ring=0;bool base_ready=false,told_live=false;
Shader *notaa_shader=nullptr;const Shader *notaa_of=nullptr;bool notaa_bad=false;

void drop(Tgt &t){if(!t.image)return;const Tgt o=t;t=Tgt{};retire_handle([o]{if(o.view)vkDestroyImageView(cfg.device,o.view,nullptr);vkDestroyImage(cfg.device,o.image,nullptr);if(o.memory)vkFreeMemory(cfg.device,o.memory,nullptr);});}
void drop_fb(VkFramebuffer &f){if(!f)return;const VkFramebuffer o=f;f=VK_NULL_HANDLE;retire_handle([o]{vkDestroyFramebuffer(cfg.device,o,nullptr);});}
bool make(Tgt &t,VkFormat f,uint32_t w,uint32_t h,VkImageUsageFlags usage,const char *what){
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=f;ci.extent={w,h,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
    ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r=vkCreateImage(cfg.device,&ci,nullptr,&t.image);if(r!=VK_SUCCESS){t=Tgt{};fail(what,r);return false;}
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(cfg.device,t.image,&mr);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if((r=vkAllocateMemory(cfg.device,&ai,nullptr,&t.memory))!=VK_SUCCESS || (r=vkBindImageMemory(cfg.device,t.image,t.memory,0))!=VK_SUCCESS){vkDestroyImage(cfg.device,t.image,nullptr);if(t.memory)vkFreeMemory(cfg.device,t.memory,nullptr);t=Tgt{};fail(what,r);return false;}
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=f;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if((r=vkCreateImageView(cfg.device,&vi,nullptr,&t.view))!=VK_SUCCESS){vkDestroyImage(cfg.device,t.image,nullptr);vkFreeMemory(cfg.device,t.memory,nullptr);t=Tgt{};fail(what,r);return false;}
    t.w=w;t.h=h;t.fmt=f;return true;}
bool usable(VkFormat f,VkFormatFeatureFlags need_f){VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(cfg.physical,f,&fp);return (fp.optimalTilingFeatures&need_f)==need_f;}
// A one-attachment-per-output render pass: contents not loaded (every pixel of the viewport is written), stored, left in `last`
VkRenderPass make_pass(uint32_t n,const VkFormat *fmt,const VkImageLayout *last,const char *what){
    VkAttachmentDescription a[2]{};VkAttachmentReference ref[2]{};
    for(uint32_t i=0;i<n;i++){a[i].format=fmt[i];a[i].samples=VK_SAMPLE_COUNT_1_BIT;a[i].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;a[i].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a[i].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[i].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a[i].finalLayout=last[i];ref[i]={i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};}
    VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=n;sp.pColorAttachments=ref;
    // earlier readers of the attachments (the upscale pass sampled the history, the presenter blitted or sampled the frame) before the
    // layout change, later readers after the writes; the engine's full barriers around the op order the rest
    VkSubpassDependency d[2]{};d[0].srcSubpass=VK_SUBPASS_EXTERNAL;d[0].dstSubpass=0;d[0].srcStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
    d[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;d[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;d[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    d[1].srcSubpass=0;d[1].dstSubpass=VK_SUBPASS_EXTERNAL;d[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;d[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
    d[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;d[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=n;rp.pAttachments=a;rp.subpassCount=1;rp.pSubpasses=&sp;rp.dependencyCount=2;rp.pDependencies=d;
    VkRenderPass out{};VkResult r=vkCreateRenderPass(cfg.device,&rp,nullptr,&out);if(r!=VK_SUCCESS){fail(what,r);return VK_NULL_HANDLE;}crashinfo::note_rp(out,rp,what);return out;}
VkPipeline make_pipeline(VkRenderPass rp,VkPipelineLayout layout,uint32_t outputs,const uint32_t *code,size_t bytes,const char *what){
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=bytes;mi.pCode=code;VkShaderModule fs{};VkResult r;{drvc::Lock compile(true);r=vkCreateShaderModule(cfg.device,&mi,nullptr,&fs);}if(r!=VK_SUCCESS){fail(what,r);return VK_NULL_HANDLE;}
    VkPipelineShaderStageCreateInfo st[2]{};for(int k=0;k<2;k++){st[k].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st[k].stage=k?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;st[k].module=k?fs:vs_mod;st[k].pName="main";}
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba[2]{};for(auto &b:ba)b.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=outputs;cb.pAttachments=ba;
    VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};ds.dynamicStateCount=2;ds.pDynamicStates=dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=st;gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;
    gp.pMultisampleState=&ms;gp.pColorBlendState=&cb;gp.pDynamicState=&ds;gp.layout=layout;gp.renderPass=rp;gp.subpass=0;
    VkPipeline out{};{drvc::Lock compile(true);r=vkCreateGraphicsPipelines(cfg.device,VK_NULL_HANDLE,1,&gp,nullptr,&out);}vkDestroyShaderModule(cfg.device,fs,nullptr);
    if(r!=VK_SUCCESS || !out){fail(what,r);return VK_NULL_HANDLE;}crashinfo::note_pl(out,rp,what);return out;}
// Once: samplers, set layouts (0 convert: depth + object velocity; 1 upscale: history, motion, colour, UI), sets, the convert pass
bool base(){
    if(base_ready)return true;VkResult r;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if((r=vkCreateSampler(cfg.device,&si,nullptr,&s_linear))!=VK_SUCCESS){fail("sampler",r);return false;}
    si.magFilter=si.minFilter=VK_FILTER_NEAREST;if((r=vkCreateSampler(cfg.device,&si,nullptr,&s_nearest))!=VK_SUCCESS){fail("sampler",r);return false;}
    const uint32_t counts[3]={2,4,2},push[3]={128,96,32};
    for(int k=0;k<3;k++){VkDescriptorSetLayoutBinding b[4]{};for(uint32_t i=0;i<counts[k];i++){b[i].binding=i;b[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[i].descriptorCount=1;b[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=counts[k];dl.pBindings=b;if((r=vkCreateDescriptorSetLayout(cfg.device,&dl,nullptr,&dsl[k]))!=VK_SUCCESS){fail("descriptor set layout",r);return false;}
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT,0,push[k]};VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&dsl[k];pi.pushConstantRangeCount=1;pi.pPushConstantRanges=&pc;
        if((r=vkCreatePipelineLayout(cfg.device,&pi,nullptr,&pl[k]))!=VK_SUCCESS){fail("pipeline layout",r);return false;}}
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,RING*8};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=RING*3;dp.poolSizeCount=1;dp.pPoolSizes=&ps;
    if((r=vkCreateDescriptorPool(cfg.device,&dp,nullptr,&dpool))!=VK_SUCCESS){fail("descriptor pool",r);return false;}
    for(int k=0;k<3;k++){VkDescriptorSetLayout ls[RING];for(auto &l:ls)l=dsl[k];VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=dpool;ai.descriptorSetCount=RING;ai.pSetLayouts=ls;
        if((r=vkAllocateDescriptorSets(cfg.device,&ai,sets[k]))!=VK_SUCCESS){fail("descriptor sets",r);return false;}}
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=sizeof vs_code;mi.pCode=vs_code;{drvc::Lock compile(true);r=vkCreateShaderModule(cfg.device,&mi,nullptr,&vs_mod);}if(r!=VK_SUCCESS){fail("vertex shader",r);return false;}
    if(!usable(VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)){fail("RGBA16F motion target (format features)",0);return false;}
    const VkFormat mf=VK_FORMAT_R16G16B16A16_SFLOAT;const VkImageLayout ml=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(!(rp_conv=make_pass(1,&mf,&ml,"tu convert pass")) || !(pipe_conv=make_pipeline(rp_conv,pl[0],1,convert_code,sizeof convert_code,"tu sgsr2 convert")))return false;
    // GPU cost of the two passes: timestamps, when the queue has them
    if(timestamp_bits && properties.limits.timestampPeriod>0){VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=RING*QN;if(vkCreateQueryPool(cfg.device,&qi,nullptr,&queries)!=VK_SUCCESS)queries=VK_NULL_HANDLE;}
    if(!queries)xbg_logf("NVN TU sgsr2: no timestamps on this queue, gpu_us is not measured");
    base_ready=true;return true;}
// The motion image: the size of the game's depth target (the view is its lower-left part)
bool motion_target(uint32_t w,uint32_t h){
    if(motion.image && motion.w==w && motion.h==h)return true;
    drop_fb(fb_motion);drop(motion);hist_valid=false;
    if(!make(motion,VK_FORMAT_R16G16B16A16_SFLOAT,w,h,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,"motion image"))return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=rp_conv;fi.attachmentCount=1;fi.pAttachments=&motion.view;fi.width=w;fi.height=h;fi.layers=1;
    if(VkResult r=vkCreateFramebuffer(cfg.device,&fi,nullptr,&fb_motion)){fb_motion=VK_NULL_HANDLE;fail("motion framebuffer",r);return false;}
    return true;}
// The output-size targets: two history images (ping-pong), three frames to present (the presenter may still read the previous
// ones, as with taafix::flips), the pass and pipeline for their formats. New images start cleared, in the layout they are read in.
bool output_targets(VkCommandBuffer cmd,uint32_t w,uint32_t h,VkFormat present_fmt){
    if(fin[0].image && fin[0].w==w && fin[0].h==h && fin_fmt==present_fmt)return true;
    for(auto &row:fb_up)for(auto &f:row)drop_fb(f);for(auto &t:hist)drop(t);for(auto &t:fin)drop(t);hist_valid=false;
    if(fin_fmt!=present_fmt || !pipe_up){
        if(pipe_up){const VkPipeline p=pipe_up;const VkRenderPass rp=rp_up;pipe_up=VK_NULL_HANDLE;rp_up=VK_NULL_HANDLE;retire_handle([p,rp]{vkDestroyPipeline(cfg.device,p,nullptr);vkDestroyRenderPass(cfg.device,rp,nullptr);});}
        const VkFormatFeatureFlags hf=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        // history: the scene is sRGB-encoded LDR; 10 bits per channel keep its dithering through the accumulation (R11G11B10F has
        // only 6 / 5 mantissa bits in [0.5, 1)); RGBA16F where the driver cannot render to A2B10G10R10 or DM_GFX_TU_HISTORY=16f
        hist_fmt=!hist16 && usable(VK_FORMAT_A2B10G10R10_UNORM_PACK32,hf)?VK_FORMAT_A2B10G10R10_UNORM_PACK32:VK_FORMAT_R16G16B16A16_SFLOAT;
        if(!usable(hist_fmt,hf)){fail("history format (features)",0);return false;}
        if(!usable(present_fmt,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT)){fail("presented format as a colour attachment (features)",int(present_fmt));return false;}
        const VkFormat f[2]={hist_fmt,present_fmt};const VkImageLayout l[2]={VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};   // the presenter takes TRANSFER_SRC_OPTIMAL
        if(!(rp_up=make_pass(2,f,l,"tu upscale pass")) || !(pipe_up=make_pipeline(rp_up,pl[1],2,upscale_code,sizeof upscale_code,"tu sgsr2 upscale")))return false;
        fin_fmt=present_fmt;}
    VkFormatProperties pf;vkGetPhysicalDeviceFormatProperties(cfg.physical,present_fmt,&pf);
    for(auto &t:hist)if(!make(t,hist_fmt,w,h,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,"history image"))return false;
    for(auto &t:fin)if(!make(t,present_fmt,w,h,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|((pf.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)?VK_IMAGE_USAGE_SAMPLED_BIT:0),"output image"))return false;
    for(int a=0;a<2;a++)for(int b=0;b<3;b++){VkImageView v[2]={hist[a].view,fin[b].view};VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=rp_up;fi.attachmentCount=2;fi.pAttachments=v;fi.width=w;fi.height=h;fi.layers=1;
        if(VkResult r=vkCreateFramebuffer(cfg.device,&fi,nullptr,&fb_up[a][b])){fb_up[a][b]=VK_NULL_HANDLE;fail("output framebuffer",r);return false;}}
    for(auto &t:hist){   // cleared and readable: the first frame samples one of them (its weight is 0 with reset, but the read must be valid)
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=t.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkrec::CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkClearColorValue zero{};vkrec::CmdClearColorImage(cmd,t.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&b.subresourceRange);
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkrec::CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);}
    xbg_logf("NVN TU sgsr2 targets: output %ux%u history vk=%d x2 frame vk=%d x3 motion %ux%u",w,h,int(hist_fmt),int(present_fmt),motion.w,motion.h);
    return true;}
void write_set(VkDescriptorSet set,uint32_t n,const VkDescriptorImageInfo *ii){
    VkWriteDescriptorSet wr[4]{};for(uint32_t i=0;i<n;i++){wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=set;wr[i].dstBinding=i;wr[i].descriptorCount=1;wr[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr[i].pImageInfo=&ii[i];}
    vkUpdateDescriptorSets(cfg.device,n,wr,0,nullptr);}
void fullscreen(VkCommandBuffer cmd,VkRenderPass rp,VkFramebuffer fb,VkPipeline pipe,VkPipelineLayout layout,VkDescriptorSet set,uint32_t fbw,uint32_t fbh,uint32_t w,uint32_t h,const void *pc,uint32_t pc_bytes){
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderPass=rp;rb.framebuffer=fb;rb.renderArea={{0,0},{fbw,fbh}};vkrec::CmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
    vkrec::CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);vkrec::CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    VkViewport v{0,0,float(w),float(h),0,1};VkRect2D sc{{0,0},{w,h}};vkrec::CmdSetViewport(cmd,0,1,&v);vkrec::CmdSetScissor(cmd,0,1,&sc);
    vkrec::CmdPushConstants(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,pc_bytes,pc);vkrec::CmdDraw(cmd,3,1,0,0);vkrec::CmdEndRenderPass(cmd);}
// The frame slot's timestamps of 8 frames ago (pairs: 0/1 convert, 2/3 upscale, 4..9 replayed post draws, 10/11 final), then the slot is reset
void collect(VkCommandBuffer cmd,unsigned slot){
    if(!queries)return;
    if(stamped[slot]){uint64_t t[QN*2]{};   // value, availability per query
        // no WAIT: a pass that was not recorded for this slot leaves its queries unavailable (VK_NOT_READY, availability 0)
        const VkResult qr=vkGetQueryPoolResults(cfg.device,queries,slot*QN,QN,sizeof t,t,16,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(qr==VK_SUCCESS || qr==VK_NOT_READY){
            const uint64_t mask=timestamp_bits<64?(uint64_t(1)<<timestamp_bits)-1:~0ull;const double per=properties.limits.timestampPeriod;const unsigned have=stamped[slot];
            auto span=[&](unsigned q,unsigned bit){return (have&bit) && t[2*q+1] && t[2*q+3]?double((t[2*q+2]-t[2*q])&mask)*per:0.0;};
            const double c=span(0,1),u=span(2,2),p=span(4,4)+span(6,8)+span(8,16),f=span(10,32);
            if(u>0){conv_ns+=c;up_ns+=u;post_ns+=p;fin_ns+=f;max_ns=std::max(max_ns,c+u+p+f);++timed;}}
        stamped[slot]=0;}
    vkrec::CmdResetQueryPool(cmd,queries,slot*QN,QN);}
Tgt finr[3];   // [2.75] the frames route A-replay presents (its other objects are with the route, below)
void status(){
    const uint64_t now=timing_clock();if(!win_t0)win_t0=now;if(now-win_t0<3000000000ull)return;
    if(route_v)xbg_logf("NVN TU sgsr2 replay %ux%u -> %ux%u gpu_us=%.0f (convert %.0f + upscale %.0f + post replay %.0f + final %.0f) max_us=%.0f frames=%llu fallback=%llu%s%s resets=%llu nojitter=%llu static=%llu jitter_px=%.3f,%.3f post_draws=%.1f foreign=%llu",
        taa.rw,taa.rh,finr[0].w,finr[0].h,timed?(conv_ns+up_ns+post_ns+fin_ns)/timed/1e3:-1.0,timed?conv_ns/timed/1e3:-1.0,timed?up_ns/timed/1e3:-1.0,timed?post_ns/timed/1e3:-1.0,timed?fin_ns/timed/1e3:-1.0,max_ns/1e3,
        (unsigned long long)n_frames,(unsigned long long)n_fallback,n_fallback?" last=":"",n_fallback?last_why:"",(unsigned long long)n_reset,(unsigned long long)n_nojit,(unsigned long long)n_static,double(last_jit[0]),double(last_jit[1]),
        n_frames?double(n_post)/double(n_frames):0.0,(unsigned long long)n_foreign);
    else xbg_logf("NVN TU sgsr2 %ux%u -> %ux%u gpu_us=%.0f (convert %.0f + upscale %.0f) max_us=%.0f frames=%llu fallback=%llu%s%s resets=%llu nojitter=%llu static=%llu jitter_px=%.3f,%.3f notaa=%d",
        taa.rw,taa.rh,fin[0].w,fin[0].h,timed?(conv_ns+up_ns)/timed/1e3:-1.0,timed?conv_ns/timed/1e3:-1.0,timed?up_ns/timed/1e3:-1.0,max_ns/1e3,(unsigned long long)n_frames,(unsigned long long)n_fallback,
        n_fallback?" last=":"",n_fallback?last_why:"",(unsigned long long)n_reset,(unsigned long long)n_nojit,(unsigned long long)n_static,double(last_jit[0]),double(last_jit[1]),int(taa.raw));
    n_frames=n_fallback=n_reset=n_nojit=n_static=n_post=n_foreign=0;conv_ns=up_ns=post_ns=fin_ns=max_ns=0;timed=0;win_t0=now;}
// A frame with a TAA draw that could not be upscaled: the old path shows it; the convert pass is not recorded for the next half
// second (`delay` frames; a lasting cause would otherwise cost it every frame), and the game's TAA comes back meanwhile
bool fallback(const char *why,uint64_t frame,unsigned delay=30){
    static std::set<std::string> told;if(told.size()<32 && told.insert(why).second)xbg_logf("NVN TU sgsr2: frame %llu not upscaled (%s); the game's frame goes the old way",(unsigned long long)frame,why);
    ++n_fallback;last_why=why;live=false;hist_valid=false;retry_at=frame+delay;status();return false;}

// ---- draw side
// The first motion-vector pre-pass draw of the frame: jitter = (mvp - mvpnojitter) / mvp.w, least squares over the four columns
void jitter_capture(){
    const uint64_t f=stats.presents.load();if(jit.frame==f)return;
    const NVNbufferRange &u=state.ub[0][0];if(!u.address || u.size<208)return;   // freqHigh_vertexUniforms: hardwareskinning, mvpmatrix x y z w, mvpmatrixnojitter x y z w, mvpmatrixlast
    float m[36];memcpy(m,reinterpret_cast<const void*>(uintptr_t(u.address)+16),sizeof m);
    double sx=0,sy=0,ww=0;for(int i=0;i<4;i++){const double w=m[12+i];sx+=(double(m[i])-m[16+i])*w;sy+=(double(m[4+i])-m[20+i])*w;ww+=w*w;}
    if(!(ww>1e-12))return;
    jit.frame=f;jit.x=sx/ww;jit.y=sy/ww;}
// The module that stands in for the TAA resolve: the bundle's binding table with our SPIR-V (tu/tu_notaa.frag)
Shader *notaa(Shader *orig){
    if(notaa_bad)return nullptr;if(notaa_shader && notaa_of==orig)return notaa_shader;
    if(notaa_shader){notaa_bad=true;return nullptr;}   // a second TAA Shader object: not expected (one bundle per key)
    unsigned have=0;for(const auto &b:orig->bindings){if(b.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b.source==XBG_SOURCE_TEXTURE && ((b.binding==10 && b.slot==0) || (b.binding==22 && b.slot==1) || (b.binding==16 && b.slot==2)))++have;
        if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source==XBG_SOURCE_UNIFORM && !(b.flags&XBG_BIND_SSBO) && ((b.binding==40 && b.slot==0) || (b.binding==52 && b.slot==1)))++have;}
    if(have!=5){notaa_bad=true;xbg_logf("NVN TU sgsr2: the TAA bundle's bindings are not the expected ones (%u of 5): the game's TAA stays, the upscaler runs over its output without jitter",have);return nullptr;}
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=sizeof notaa_code;mi.pCode=notaa_code;VkShaderModule m{};VkResult r;{drvc::Lock compile(true);r=vkCreateShaderModule(cfg.device,&mi,nullptr,&m);}
    if(r!=VK_SUCCESS){notaa_bad=true;xbg_logf("NVN TU sgsr2: pass-through module failed (VkResult %d): the game's TAA stays, the upscaler runs over its output without jitter",int(r));return nullptr;}
    Shader *p=new Shader();p->key=orig->key+"-tu";p->stage=orig->stage;p->module=m;p->bindings=orig->bindings;p->inputs=orig->inputs;p->input_classes=orig->input_classes;p->c1=orig->c1;
    p->discards=false;p->effects=false;p->raster=orig->raster;   // writes both targets, no discard: the same outputs as the game's module
    notaa_shader=p;notaa_of=orig;xbg_logf("NVN TU sgsr2: TAA resolve %s -> pass-through module (colour as it is + the motion-blur velocity target)",orig->key.c_str());return p;}
inline bool notaa_usable(){return notaa_on && !notaa_bad;}
// At the TAA resolve draw: take its uniforms and record pass 1 from its depth and object-velocity inputs. true = this frame can be upscaled.
bool taa_draw(const Shader *ps){
    const uint64_t frame=stats.presents.load();taa.frame=frame;taa.converted=false;taa.raw=false;
    if(frame<retry_at)return false;
    if(!taafix::lower_left())return fallback("needs the NVN lower-left origin (DM_GFX_TAA_FIX bit 2)",frame);
    if(!dm_present_window_size)return fallback("this presenter gives no window size",frame);
    const float *h=taafix::ubo(0,224),*l=taafix::ubo(1,112);Tex dt,ot;
    if(!h || !l || !bound(ps,0,dt) || !bound(ps,1,ot))return fallback("TAA uniforms or inputs not resolved",frame);
    Image *di=sampled(dt),*oi=sampled(ot);if(!di || !oi || !(di->aspects&VK_IMAGE_ASPECT_DEPTH_BIT) || oi->aspects!=VK_IMAGE_ASPECT_COLOR_BIT)return fallback("depth / velocity image not usable",frame);
    memcpy(taa.hi,h,sizeof taa.hi);memcpy(taa.lo,l,sizeof taa.lo);
    const float *rs=taa.lo+12;const long rw=lround(double(h[0])*rs[0]),rh=lround(double(h[1])*rs[1]);
    if(!(h[0]>=16 && h[1]>=16 && rs[0]>0.05f && rs[0]<=1.0001f && rs[1]>0.05f && rs[1]<=1.0001f) || rw<16 || rh<16 || uint32_t(rw)>di->width || uint32_t(rh)>di->height || lround(h[0])!=long(di->width) || lround(h[1])!=long(di->height) || oi->width<uint32_t(rw) || oi->height<uint32_t(rh))
        return fallback("TAA target / view size not as expected",frame);
    if(!base() || !motion_target(di->width,di->height))return false;
    taa.rw=uint32_t(rw);taa.rh=uint32_t(rh);taa.tw=di->width;taa.th=di->height;
    // camera of this frame = the four frustum rays + the view origin. Unchanged since the last TAA draw: still (the camera part of the
    // motion is then exactly 0, and the published minLerpContribution applies after 5 such frames); turned by more than 60 degrees or
    // moved by more than DM_GFX_TU_CUT units in one frame: a cut (history reset).
    {float cam[15];memcpy(cam,taa.lo+4,12);memcpy(cam+3,taa.lo+8,12);memcpy(cam+6,taa.lo+16,12);memcpy(cam+9,taa.lo+20,12);memcpy(cam+12,taa.lo+24,12);
     float fwd[3];double fl=0;for(int i=0;i<3;i++){fwd[i]=cam[i]+cam[3+i]+cam[6+i]+cam[9+i];fl+=double(fwd[i])*fwd[i];}fl=fl>0?sqrt(fl):1;for(float &f:fwd)f=float(f/fl);
     bool same=have_prev_cam;taa.cut=false;
     if(have_prev_cam){for(int i=0;i<15 && same;i++)same=fabsf(cam[i]-prev_cam[i])<=1e-6f*std::max(1.0f,fabsf(cam[i]));
        double dot=0,dist=0;for(int i=0;i<3;i++){dot+=double(fwd[i])*prev_fwd[i];const double d=double(cam[12+i])-prev_cam[12+i];dist+=d*d;}
        taa.cut=dot<0.5 || dist>double(cut_units)*cut_units;}
     memcpy(prev_cam,cam,sizeof cam);memcpy(prev_fwd,fwd,sizeof fwd);have_prev_cam=true;taa.still=same;static_frames=same?static_frames+1:0;}
    // previous clip x / y / w of world = origin + ray(s,t) * z: per row P, P.xyz . ray is bilinear in (s,t) (the game mixes the four corner
    // rays), P . origin + P.w is constant. In double: the origin's coordinates are large and cancel against the row's translation.
    struct{float ax[4],ay[4],aw[4],k[4],texsize[4],r2t[4],rs[4],pz[4];}pc{};
    const float *tl=taa.lo+4,*tr=taa.lo+8,*bl=taa.lo+16,*br=taa.lo+20,*org=taa.lo+24;const float *rows[3]={taa.hi+8,taa.hi+12,taa.hi+4};float *out[3]={pc.ax,pc.ay,pc.aw};
    for(int r=0;r<3;r++){const float *P=rows[r];double c[4]={0,0,0,0},k=P[3];
        for(int i=0;i<3;i++){const double p=P[i];c[0]+=p*bl[i];c[1]+=p*(double(br[i])-bl[i]);c[2]+=p*(double(tl[i])-bl[i]);c[3]+=p*(double(tr[i])-tl[i]-br[i]+bl[i]);k+=p*org[i];}
        for(int i=0;i<4;i++)out[r][i]=float(c[i]);pc.k[r]=float(k);}
    // Kfov of the published pass = tan(horizontal fov / 2): half the distance between the left and right rays over the centre ray's length
    double cx[3],dx[3],cl=0,dl=0;for(int i=0;i<3;i++){cx[i]=(double(tl[i])+tr[i]+bl[i]+br[i])*0.25;dx[i]=(double(tr[i])+br[i]-tl[i]-bl[i])*0.25;cl+=cx[i]*cx[i];dl+=dx[i]*dx[i];}
    const double kfov=cl>0?sqrt(dl/cl):1.0;pc.k[3]=float(1.37e-05*kfov*sqrt(double(rw)*rw+double(rh)*rh));
    memcpy(pc.texsize,taa.hi,16);memcpy(pc.r2t,taa.hi+16,16);pc.rs[0]=rs[0];pc.rs[1]=rs[1];pc.rs[2]=rs[2];pc.rs[3]=rs[3];
    pc.pz[0]=taa.lo[2];pc.pz[1]=taa.lo[3];pc.pz[2]=taa.lo[3]<0?1.0f:-1.0f;   // depth = w / z - projz.z: grows with distance when w < 0
    pc.pz[3]=taa.still?1.0f:0.0f;
    const unsigned slot=ring++%RING;taa.slot=slot;   // a new slot for every recorded convert pass: its set is never rewritten while an earlier frame's commands may still use it
    Commands c;   // ends the open pass; full barriers before and after: the depth pre-pass and velocity writes are complete
    if(di->layout!=VK_IMAGE_LAYOUT_GENERAL)di->transition(c.command,VK_IMAGE_LAYOUT_GENERAL);if(oi->layout!=VK_IMAGE_LAYOUT_GENERAL)oi->transition(c.command,VK_IMAGE_LAYOUT_GENERAL);
    const VkDescriptorImageInfo ii[2]={{s_nearest,di->view(dt.has_view?&dt.v:nullptr),VK_IMAGE_LAYOUT_GENERAL},{s_nearest,oi->view(ot.has_view?&ot.v:nullptr),VK_IMAGE_LAYOUT_GENERAL}};
    write_set(sets[0][slot],2,ii);collect(c.command,slot);
    if(queries)vkrec::CmdWriteTimestamp(c.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,slot*QN);
    fullscreen(c.command,rp_conv,fb_motion,pipe_conv,pl[0],sets[0][slot],motion.w,motion.h,taa.rw,taa.rh,&pc,sizeof pc);
    if(queries){vkrec::CmdWriteTimestamp(c.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,slot*QN+1);stamped[slot]|=1;}
    c.finish();taa.converted=true;return true;}
void up_draw(const Shader *ps){
    up.frame=stats.presents.load();up.target=state.rt.n>0 && state.rt.color[0]?state.rt.color[0]->serial:0;up.has_ui=false;
    const float *h=taafix::ubo(0,48),*l=taafix::ubo(1,32);if(!h || !l || !bound(ps,0,up.scene)){up.target=0;return;}
    memcpy(up.hi,h,sizeof up.hi);memcpy(up.lo,l,sizeof up.lo);up.has_ui=bound(ps,1,up.ui);}
// ---- [2.75] route A-replay (see the head of the file)
// An image of ours that the engine handles as one of the game's: get_image() makes it from `t` on first use (a copy of a game
// target's description with our size and serial, no guest memory: it is always rendered before it is read, so nothing is ever
// uploaded from t.mem). `id` is its place in our descriptor pool table. Fakes are kept until shutdown (state.rt points at t while
// a draw of ours is recorded); a new size gets new Fakes with new serials and the old ones' images are released.
constexpr int FAKE_ID0=0xFFF00;constexpr unsigned OUTS=6;
struct Fake{tex_t t{};int id=0;VkFramebuffer fb{};};
std::vector<std::unique_ptr<Fake>> fakes;pool_t fake_mem{};dpool_t fake_pool{};uint64_t fake_serial=0xD00D000000000000ull;
Fake *hist_f[2]{};struct Out{uint64_t serial=0,used=0;Fake *f=nullptr;} outs[OUTS];   // the accumulated scene (ping-pong); the replayed draws' targets, by the game target they stand for
// what the replay did in this frame: track = game texture serial -> its output-size counterpart
struct Rp{uint64_t frame=UINT64_MAX;bool hdr=false,pp=false;unsigned ntrack=0,draws=0,mb=0,stamps=0;struct{uint64_t serial;Fake *f;} track[12]{};unsigned copies=0;int pslot=1;const Shader *pps=nullptr;double kx=1,ky=1;uint32_t ow=0,oh=0,bw=0,bh=0;uint64_t pp_serial=0;Fake *ldr=nullptr,*scene=nullptr;const char *why=nullptr;uint8_t pending=0;} rp;   // scene: the accumulated scene of this frame
uint32_t cfg_now[7]{},out_w=0,out_h=0;uint64_t cfg_frame=0;   // cfg_now: target / view / output size and format the Fakes were made for; out_*: the output size the last present worked out
unsigned rp_strikes=0,finr_next=0,ubo_next=0;bool in_replay=false;const char *rp_why=nullptr;uint64_t rp_why_frame=0;
VkRenderPass rp_hup{},rp_fin{};VkPipeline pipe_hup{},pipe_fin{};VkFormat hup_fmt=VK_FORMAT_UNDEFINED,finr_fmt=VK_FORMAT_UNDEFINED;VkFramebuffer fb_fin[3]{};
Shader *down_shader=nullptr;const Shader *down_of=nullptr;bool down_bad=false;
alignas(16) uint8_t ubo_copy[4][4096];   // host copies of a replayed draw's fragment uniform block (one per draw of a frame)
struct Swap{bool tex=false,rt=false;uint64_t handle=0;unsigned slot=0;dpool_t pool{};p_set_rt rtv{};VkViewport vp{};VkRect2D sc{};NVNbufferRange ub{};} swp;

void rnote(const char *why){static std::set<std::string> told;if(told.size()<32 && told.insert(why).second)xbg_logf("NVN TU sgsr2 %s (frame %llu)",why,(unsigned long long)stats.presents.load());}
Fake *fake_make(const tex_t &like,uint32_t w,uint32_t h,int id){
    auto f=std::make_unique<Fake>();f->t=like;f->t.d.width=w;f->t.d.height=h;f->t.d.levels=1;f->t.d.pkg_block_h=0;f->t.L.size=f->t.L.layer_size=uint64_t(w)*h*std::max(1u,like.L.bpb);
    f->t.mem=nullptr;f->t.pool=nullptr;f->t.off=0;f->t.has_layout=0;f->t.snap=nullptr;f->t.serial=++fake_serial;f->id=id;fakes.push_back(std::move(f));return fakes.back().get();}
// As xbg_legacy_tex_release_now: the image goes when the recording slot retires, every cache that could still name it is aged
void fake_release(Fake *&f){
    if(!f)return;drop_fb(f->fb);
    auto i=images.find(f->t.serial);if(i!=images.end()){retire_object(i->second);images.erase(i);image_index_dirty=true;++images_epoch;++memo_check_gen;}
    f=nullptr;}
void release_fakes(){end_open_pass();last_pass_reset();for(auto &h:hist_f)fake_release(h);for(auto &o:outs){fake_release(o.f);o=Out{};}rp=Rp{};hist_valid=false;}
// [2.75 OOM] The upscaled targets of the replay are made by the engine's image path, which ends the game on a failed allocation. So before
// they are made: (1) MemAvailable from /proc/meminfo (phones share RAM with the GPU: an allocation that "fits" can still get the game
// killed) must cover twice their size + 256 MB, (2) one probe allocation per target, of its size + 25 %, all held at once, then freed.
// Either fails -> replay off for this run (route b1, whose output-size images fail softly), never a crash. DM_GFX_TU_MEMCHECK=0: skip it.
bool replay_memory_ok(uint64_t per_target,unsigned targets,char *why,size_t wn){
    static int on=-1;if(on<0){const char *e=getenv("DM_GFX_TU_MEMCHECK");on=!(e && *e=='0');}if(!on)return true;
    const uint64_t total=per_target*targets;
    if(FILE *m=fopen("/proc/meminfo","r")){char line[128];unsigned long long kb=0;bool got=false;
        while(fgets(line,sizeof line,m))if(sscanf(line,"MemAvailable: %llu kB",&kb)==1){got=true;break;}
        fclose(m);
        if(got && kb*1024ull<total*2+(256ull<<20)){snprintf(why,wn,"not enough free memory for the upscaled targets: %llu MB needed, %llu MB available",(unsigned long long)(total>>20),kb>>10);return false;}}
    uint32_t ti=UINT32_MAX;for(uint32_t i=0;i<memories.memoryTypeCount;i++)if(memories.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT){ti=i;break;}
    if(ti==UINT32_MAX)return true;
    std::vector<VkDeviceMemory> held;bool ok=true;VkResult r=VK_SUCCESS;
    for(unsigned k=0;k<targets && ok;k++){VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=per_target+per_target/4;ai.memoryTypeIndex=ti;
        VkDeviceMemory mem{};if((r=vkAllocateMemory(cfg.device,&ai,nullptr,&mem))==VK_SUCCESS)held.push_back(mem);else ok=false;}
    for(VkDeviceMemory mem:held)vkFreeMemory(cfg.device,mem,nullptr);
    if(!ok)snprintf(why,wn,"the GPU could not allocate the upscaled targets: %u x %llu MB (VkResult %d)",targets,(unsigned long long)(per_target>>20),int(r));
    return ok;}
void replay_off(const char *why){
    if(!route_v)return;route_v=0;release_fakes();live=false;told_live=false;memset(cfg_now,0,sizeof cfg_now);
    xbg_logf("NVN TU sgsr2 replay off for this run (%s): route b1 (upscale at the viewcolorupsample point) from now on",why);}
bool rfail(const char *why,uint64_t frame){if(diag_auto){diag_auto=false;diag_left=std::max(diag_left,1);}   // the next frame's passes are listed once (NVN TU diag)
    if(++rp_strikes>=5)replay_off(why);return fallback(why,frame);}
Fake *tracked(uint64_t serial){for(unsigned i=0;i<rp.ntrack;i++)if(rp.track[i].serial==serial)return rp.track[i].f;return nullptr;}
void track(uint64_t serial,Fake *f){for(unsigned i=0;i<rp.ntrack;i++)if(rp.track[i].serial==serial){rp.track[i].f=f;return;}if(rp.ntrack<12){rp.track[rp.ntrack].serial=serial;rp.track[rp.ntrack].f=f;++rp.ntrack;}}
void untrack(uint64_t serial){for(unsigned i=0;i<rp.ntrack;i++)if(rp.track[i].serial==serial){rp.track[i]=rp.track[--rp.ntrack];return;}}
bool plain_target(const tex_t *t){return t && t->d.width==taa.tw && t->d.height==taa.th && t->d.target==1 && t->d.samples<=1 && t->L.layers==1 && aspect(t->d.format)==VK_IMAGE_ASPECT_COLOR_BIT;}
// The output-size target that stands for a game target in the replayed draws
Fake *out_for(const tex_t *game){
    const uint64_t frame=stats.presents.load();
    for(auto &o:outs)if(o.f && o.serial==game->serial && o.f->t.d.format==game->d.format){o.used=frame;return o.f;}
    Out *k=nullptr;for(auto &o:outs)if(!o.f){k=&o;break;}
    if(!k){for(auto &o:outs)if(o.used!=frame && (!k || o.used<k->used))k=&o;   // the game went through more targets than we keep: the one unused the longest goes (never one of this frame: track may name it)
        if(!k)return nullptr;end_open_pass();last_pass_reset();fake_release(k->f);}
    k->serial=game->serial;k->used=frame;k->f=fake_make(*game,rp.bw,rp.bh,FAKE_ID0+2+int(k-outs));return k->f;}
void gens(){for(auto &g:tex_gen)++g;++buf_gen[1];sg::invalidate();}   // what OP_BIND_TEX / OP_BIND_UBO / the per-command stategen bump do
// Put our texture `f` on fragment texture slot `slot` of the draw being translated. The handle keeps its sampler; the pool becomes
// ours, whose table holds the records of every texture the draw's shaders name (mirrored under the same ids) and ours.
bool swap_tex(unsigned slot,Fake *f,const char *&why){
    if(!state.texture_pool.pool || slot>=128){why="replay: no texture pool bound";return false;}
    if(!fake_pool.pool){fake_mem.mem=reinterpret_cast<uint8_t*>(&fake_mem);fake_pool.pool=&fake_mem;fake_pool.off=0;fake_pool.n=0x100000;}
    auto &to=textures[pool_key(&fake_pool)];auto src=textures.find(pool_key(&state.texture_pool));const auto &from=src==textures.end()?legacy_textures:src->second;
    for(unsigned st=0;st<5;st++)if(const Shader *sh=state.shader[st]){const unsigned ns=nvn_stage[st];
        for(const auto &b:sh->bindings){if(b.source!=XBG_SOURCE_TEXTURE && b.source!=XBG_SOURCE_IMAGE)continue;const bool img=b.source==XBG_SOURCE_IMAGE;
            for(unsigned e=0;e<b.count;e++){const unsigned at=b.slot+e;if(at>=(img?16u:128u)){why="replay: a texture slot out of range";return false;}
                const int id=int((img?state.img[ns][at]:state.tex[ns][at])&0xfffff);if(!id)continue;
                if(id>=FAKE_ID0 || (state.texture_pool.n>0 && id>=state.texture_pool.n)){why="replay: a texture id outside the game's pool";return false;}
                auto r=from.find(id);if(r==from.end()){why="replay: an unregistered texture id";return false;}
                register_texture_record(to[id],r->second.texture,r->second.has_view?&r->second.view:nullptr);}}}
    register_texture_record(to[f->id],f->t,nullptr);
    swp.tex=true;swp.slot=slot;swp.handle=state.tex[1][slot];swp.pool=state.texture_pool;
    state.texture_pool=fake_pool;state.tex[1][slot]=(swp.handle&~0xfffffull)|uint64_t(f->id);gens();return true;}
void swap_out(){
    if(swp.rt){state.rt=swp.rtv;state.viewport=swp.vp;state.scissor=swp.sc;state.ub[1][0]=swp.ub;}
    if(swp.tex){state.texture_pool=swp.pool;state.tex[1][swp.slot]=swp.handle;}
    if(swp.rt || swp.tex)gens();swp.rt=swp.tex=false;}
// The module that stands in for the TAA resolve in this route: the bundle's binding table with tu/tu_down.frag
Shader *down(Shader *orig){
    if(down_bad)return nullptr;if(down_shader && down_of==orig)return down_shader;
    if(down_shader){down_bad=true;return nullptr;}
    unsigned have=0;for(const auto &b:orig->bindings){if(b.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b.source==XBG_SOURCE_TEXTURE && ((b.binding==10 && b.slot==0) || (b.binding==22 && b.slot==1) || (b.binding==34 && b.slot==4)))++have;
        if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source==XBG_SOURCE_UNIFORM && !(b.flags&XBG_BIND_SSBO) && ((b.binding==40 && b.slot==0) || (b.binding==52 && b.slot==1)))++have;}
    if(have!=5){down_bad=true;xbg_logf("NVN TU sgsr2 replay: the TAA bundle's bindings are not the expected ones (%u of 5)",have);return nullptr;}
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=sizeof down_code;mi.pCode=down_code;VkShaderModule m{};VkResult r;{drvc::Lock compile(true);r=vkCreateShaderModule(cfg.device,&mi,nullptr,&m);}
    if(r!=VK_SUCCESS){down_bad=true;xbg_logf("NVN TU sgsr2 replay: the TAA stand-in module failed (VkResult %d)",int(r));return nullptr;}
    Shader *p=new Shader();p->key=orig->key+"-tud";p->stage=orig->stage;p->module=m;p->bindings=orig->bindings;p->inputs=orig->inputs;p->input_classes=orig->input_classes;p->c1=orig->c1;
    p->discards=false;p->effects=false;p->raster=orig->raster;   // writes both targets, no discard: the same outputs as the game's module
    down_shader=p;down_of=orig;xbg_logf("NVN TU sgsr2 replay: TAA resolve %s -> stand-in module (the upscaled scene brought to the render size + the motion-blur velocity target)",orig->key.c_str());return p;}
bool hup_objects(VkFormat f){
    if(pipe_hup && hup_fmt==f)return true;
    if(pipe_hup){const VkPipeline p=pipe_hup;const VkRenderPass q=rp_hup;pipe_hup=VK_NULL_HANDLE;rp_hup=VK_NULL_HANDLE;retire_handle([p,q]{vkDestroyPipeline(cfg.device,p,nullptr);vkDestroyRenderPass(cfg.device,q,nullptr);});}
    if(!usable(f,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)){replay_off("the TAA target's format cannot be rendered and filtered");return false;}
    const VkImageLayout l=VK_IMAGE_LAYOUT_GENERAL;   // the layout the engine keeps its images in
    if(!(rp_hup=make_pass(1,&f,&l,"tu replay upscale pass")) || !(pipe_hup=make_pipeline(rp_hup,pl[1],1,hup_code,sizeof hup_code,"tu sgsr2 hdr upscale")))return false;
    hup_fmt=f;return true;}
// The frames this route presents: three images (the presenter may still read the previous ones), one pass
bool final_targets(uint32_t w,uint32_t h,VkFormat present_fmt){
    if(finr[0].image && finr[0].w==w && finr[0].h==h && finr_fmt==present_fmt)return true;
    for(auto &f:fb_fin)drop_fb(f);for(auto &t:finr)drop(t);
    if(finr_fmt!=present_fmt || !pipe_fin){
        if(pipe_fin){const VkPipeline p=pipe_fin;const VkRenderPass q=rp_fin;pipe_fin=VK_NULL_HANDLE;rp_fin=VK_NULL_HANDLE;retire_handle([p,q]{vkDestroyPipeline(cfg.device,p,nullptr);vkDestroyRenderPass(cfg.device,q,nullptr);});}
        if(!usable(present_fmt,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT)){fail("presented format as a colour attachment (features)",int(present_fmt));return false;}
        const VkImageLayout l=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;   // the presenter takes TRANSFER_SRC_OPTIMAL
        if(!(rp_fin=make_pass(1,&present_fmt,&l,"tu final pass")) || !(pipe_fin=make_pipeline(rp_fin,pl[2],1,final_code,sizeof final_code,"tu final")))return false;
        finr_fmt=present_fmt;}
    VkFormatProperties pf;vkGetPhysicalDeviceFormatProperties(cfg.physical,present_fmt,&pf);
    for(auto &t:finr)if(!make(t,present_fmt,w,h,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|((pf.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)?VK_IMAGE_USAGE_SAMPLED_BIT:0),"output image"))return false;
    for(int b=0;b<3;b++){VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=rp_fin;fi.attachmentCount=1;fi.pAttachments=&finr[b].view;fi.width=w;fi.height=h;fi.layers=1;
        if(VkResult r=vkCreateFramebuffer(cfg.device,&fi,nullptr,&fb_fin[b])){fb_fin[b]=VK_NULL_HANDLE;fail("output framebuffer",r);return false;}}
    return true;}
// At the TAA resolve draw, after pass 1: pass 2 at the output size from the draw's HDR colour input, then the draw is set up to run
// with the stand-in module over that image. Returns the module (the texture swap is already in place: swap_out() after the draw).
bool replay_taa(Shader *ps,const char *&why,Shader *&stand_in){
    const uint64_t frame=stats.presents.load();rp.frame=UINT64_MAX;rp.hdr=false;rp.pending=0;
    stand_in=nullptr;if(!out_w || !out_h)return false;   // the first present works the output size out
    const tex_t *rt0=state.rt.n>0?state.rt.color[0]:nullptr;Tex cur;
    if(!plain_target(rt0)){why="replay: the TAA target is not a plain colour texture of the depth target's size";return false;}
    if(!bound(ps,2,cur)){why="replay: the TAA colour input is not resolved";return false;}
    Image *ci=sampled(cur);if(!ci || ci->aspects!=VK_IMAGE_ASPECT_COLOR_BIT || ci->width<taa.rw || ci->height<taa.rh){why="replay: the TAA colour input is not usable";return false;}
    const uint32_t ow=out_w,oh=out_h;const double kx=double(ow)/taa.rw,ky=double(oh)/taa.rh,bwf=taa.tw*kx,bhf=taa.th*ky;const uint32_t bw=uint32_t(lround(bwf)),bh=uint32_t(lround(bhf));
    if(kx<1.0 || ky<1.0 || fabs(bwf-bw)>1e-3 || fabs(bhf-bh)>1e-3){why="replay: the upscale ratio gives no whole target size (dynamic resolution)";return false;}
    if(bw>properties.limits.maxImageDimension2D || bh>properties.limits.maxImageDimension2D){why="replay: the upscaled targets would exceed the device's image limit";return false;}
    const uint32_t want[7]={taa.tw,taa.th,taa.rw,taa.rh,ow,oh,uint32_t(rt0->d.format)};
    if(memcmp(want,cfg_now,sizeof cfg_now) || !hist_f[0]){
        if(cfg_now[0] && frame<cfg_frame+60){why="replay: the render or output size is changing";return false;}   // new images at most once a second
        // two history images + the replayed post targets (motion blur x2 + postprocess; room for one more)
        {char mw[160];if(!replay_memory_ok(uint64_t(bw)*bh*std::max(hist11?1u:8u,rt0->L.bpb),2+4,mw,sizeof mw)){replay_off(mw);why="replay: off (memory)";return false;}}
        release_fakes();memcpy(cfg_now,want,sizeof cfg_now);cfg_frame=frame;
        // The history: RGBA16F (NVN format 0x29) unless asked otherwise or not renderable / filterable. The game's R11G11B10F has 6 (blue: 5)
        // mantissa bits: every frame's re-quantisation of the accumulated value is a 1.5 % (3 %) step, which shows as shimmer on smooth light.
        tex_t like=*rt0;
        if(!hist11 && usable(VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)){like.d.format=0x29;like.L.bpb=8;}
        hist_f[0]=fake_make(like,bw,bh,FAKE_ID0);hist_f[1]=fake_make(like,bw,bh,FAKE_ID0+1);hist_cur=0;
        xbg_logf("NVN TU sgsr2 replay targets: view %ux%u in %ux%u -> output %ux%u in %ux%u (ratio %.4f,%.4f), NVN fmt=%#x, history x2 + one per replayed post target",taa.rw,taa.rh,taa.tw,taa.th,ow,oh,bw,bh,kx,ky,rt0->d.format);}
    Shader *dn=game_taa?nullptr:down(ps);if(!game_taa && !dn){replay_off("the TAA bundle's bindings are not the expected ones");return false;}
    Image &hr=get_image(&hist_f[hist_cur]->t,false),&hw=get_image(&hist_f[hist_cur^1u]->t,false);
    const VkImageUsageFlags need_u=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if((hr.usage&need_u)!=need_u || (hw.usage&need_u)!=need_u || hw.samples!=VK_SAMPLE_COUNT_1_BIT){replay_off("the TAA target's format cannot be rendered and sampled");return false;}
    if(!base() || !hup_objects(hw.fmt))return false;
    Fake *wf=hist_f[hist_cur^1u];
    if(!wf->fb){VkImageView v=hw.view(nullptr,true);VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=rp_hup;fi.attachmentCount=1;fi.pAttachments=&v;fi.width=hw.width;fi.height=hw.height;fi.layers=1;
        if(VkResult r=vkCreateFramebuffer(cfg.device,&fi,nullptr,&wf->fb)){wf->fb=VK_NULL_HANDLE;fail("replay history framebuffer",r);return false;}}
    const bool reset=!hist_valid || last_frame+1!=frame || taa.cut;
    // jitter of this frame in render pixels, the game's rows; the colour here is always the jittered one
    float jx=0,jy=0;
    if(jit.frame==frame){jx=float(jit.x*0.5*taa.rw)*jit_mul[0];jy=float(jit.y*0.5*taa.rh)*jit_mul[1];if(!(fabsf(jx)<=1.5f && fabsf(jy)<=1.5f)){jx=jy=0;++n_nojit;}}
    else ++n_nojit;
    last_jit[0]=jx;last_jit[1]=jy;
    struct{float rsz[4],osz[4],jit[4],par[4],tsz[4],ui[4];}pc{};
    pc.rsz[0]=float(taa.rw);pc.rsz[1]=float(taa.rh);pc.rsz[2]=1.0f/taa.rw;pc.rsz[3]=1.0f/taa.rh;pc.osz[0]=float(ow);pc.osz[1]=float(oh);pc.osz[2]=1.0f/ow;pc.osz[3]=1.0f/oh;
    pc.jit[0]=jx;pc.jit[1]=jy;pc.jit[2]=float(kx);pc.jit[3]=float(std::min(20.0,pow(kx*ky,3.0)));
    pc.par[0]=static_frames>=5?0.3f:0.0f;pc.par[1]=reset?1.0f:0.0f;pc.par[2]=0.0f;pc.par[3]=float(debug_v);   // par.z 0: the output keeps the game's rows
    pc.tsz[0]=1.0f/ci->width;pc.tsz[1]=1.0f/ci->height;pc.tsz[2]=1.0f/motion.w;pc.tsz[3]=1.0f/motion.h;
    pc.ui[0]=float(double(ow)/hw.width);pc.ui[1]=float(double(oh)/hw.height);pc.ui[2]=hdr_tm?0.0f:1.0f;   // the part of the history image the output covers
    const unsigned slot=taa.slot;
    Commands c;   // ends the open pass; full barriers before and after
    for(Image *im:{&hr,&hw,ci})if(im->layout!=VK_IMAGE_LAYOUT_GENERAL)im->transition(c.command,VK_IMAGE_LAYOUT_GENERAL);
    if(!hr.gpu_written){   // a new history image: cleared so that the read is defined (its weight is 0: reset)
        VkClearColorValue zero{};VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkrec::CmdClearColorImage(c.command,hr.image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&all);
        hr.gpu_written=true;hr.prior_write=hr.last_write;hr.last_write=++write_seq;full_barrier(c.command);}
    const VkImageView cv=ci->view(cur.has_view?&cur.v:nullptr);
    const VkDescriptorImageInfo ii[4]={{s_linear,hr.view(nullptr),VK_IMAGE_LAYOUT_GENERAL},{s_linear,motion.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{s_nearest,cv,VK_IMAGE_LAYOUT_GENERAL},{s_nearest,cv,VK_IMAGE_LAYOUT_GENERAL}};
    write_set(sets[1][slot],4,ii);
    if(queries)vkrec::CmdWriteTimestamp(c.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,slot*QN+2);
    fullscreen(c.command,rp_hup,wf->fb,pipe_hup,pl[1],sets[1][slot],ow,oh,ow,oh,&pc,sizeof pc);
    if(queries){vkrec::CmdWriteTimestamp(c.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,slot*QN+3);stamped[slot]|=2;}
    hw.prior_write=hw.last_write;hw.last_write=++write_seq;hw.gpu_written=true;   // as create_pass marks an attachment
    c.finish();
    hist_cur^=1u;hist_valid=true;last_frame=frame;if(reset)++n_reset;
    // DM_GFX_TU_GAMETAA=1 (default): the game's TAA draw is left exactly as it is (its module, its history), so everything the game computes
    // from its TAA output at the render size -- the bloom chain, the luminance chain of the auto-exposure, its own motion blur and
    // postprocess -- gets the very picture it gets without the upscaler. Only the replayed draws read our image in place of that output.
    // =0: the draw runs with the stand-in module over our image (cheaper by the game's TAA pass, but the game's chain then follows SGSR 2's
    // accumulation, which keeps much less history than the game's TAA: on the phone the light flickered more than with route b1).
    if(dn && !swap_tex(4,wf,why))return false;   // the stand-in reads it where the game's module reads its history
    rp=Rp{};rp.frame=frame;rp.hdr=true;rp.kx=kx;rp.ky=ky;rp.ow=ow;rp.oh=oh;rp.bw=bw;rp.bh=bh;rp.scene=wf;track(rt0->serial,wf);
    stand_in=dn;return true;}
// A draw that writes a texture the replay tracks and is not one of the replayed kinds: its effect is missing at the output size
void foreign(const Shader *ps){++n_foreign;static std::set<std::string> told;
    if(told.size()<16 && told.insert(ps->key).second)xbg_logf("NVN TU sgsr2 replay: draw ps=%s writes the scene after the TAA point and is not replayed (missing in the upscaled frame)",ps->key.c_str());}
// ---- [2.75] DM_GFX_TU_DIAG=<frames> (and once by itself after the first frame the replay could not do): what the game does between
// its TAA draw and the present of one frame, one "NVN TU diag" line per draw / dispatch / texture copy: shader key and role, colour
// targets, viewport, blend, the first vector of the fragment uniform block, the textures bound to the shader's slots. A texture is
// serial:WxH:NVN format, with '*' when the replay has an output-size image for it. At most 160 lines a frame.
const tex_t *tex_of(unsigned ns,unsigned slot,bool image){
    if(!state.texture_pool.pool)return nullptr;const int id=int((image?state.img[ns][slot]:state.tex[ns][slot])&0xfffff);if(!id)return nullptr;
    auto pool=textures.find(pool_key(&state.texture_pool));const auto &tab=pool==textures.end()?legacy_textures:pool->second;auto t=tab.find(id);return t==tab.end()?nullptr:&t->second.texture;}
inline bool diag_now(){return diag_frame==stats.presents.load();}
void diag_line(const char *kind,const Shader *sh,unsigned st){
    if(diag_lines>=160)return;++diag_lines;
    char line[1400];line[0]=0;int n=0;auto put=[&](const char *f,auto... a){if(n<int(sizeof line))n+=snprintf(line+n,sizeof line-n,f,a...);};
    auto tex=[&](const char *tag,unsigned slot,const tex_t *t){if(t)put(" %s%u=%llu:%ux%u:%#x%s",tag,slot,(unsigned long long)t->serial,t->d.width,t->d.height,t->d.format,rp.hdr && rp.frame==diag_frame && tracked(t->serial)?"*":"");else put(" %s%u=-",tag,slot);};
    put("%s%s %s=%s role=%u",in_replay?"replay of ":"",kind,st==5?"cs":"ps",sh?sh->key.c_str():"-",sh?unsigned(sh->tu_role):0u);
    if(st!=5){put(" vs=%s targets:",state.shader[0]?state.shader[0]->key.c_str():"-");
        for(int i=0;i<state.rt.n && i<8;i++)tex("rt",unsigned(i),state.rt.color[i]);
        if(state.rt.depth)tex("depth",0,state.rt.depth);
        put(" vp=%g,%g %gx%g blend=%#x mask=%#x",double(state.viewport.x),double(state.viewport.y),double(state.viewport.width),double(state.viewport.height),unsigned(state.color.blend_en),unsigned(state.mask.mask));
        if(const float *u=taafix::ubo(0,16))put(" c3[0]=%g,%g,%g,%g",double(u[0]),double(u[1]),double(u[2]),double(u[3]));}
    put("%s"," inputs:");
    if(sh){const unsigned ns=nvn_stage[st];for(const auto &b:sh->bindings){const bool img=b.source==XBG_SOURCE_IMAGE;if(b.source!=XBG_SOURCE_TEXTURE && !img)continue;
        for(unsigned e=0;e<b.count && e<4;e++){const unsigned slot=b.slot+e;if(slot<(img?16u:128u))tex(img?"img":"tex",slot,tex_of(ns,slot,img));}}}
    xbg_logf("NVN TU diag f=%llu #%u %s",(unsigned long long)diag_frame,diag_lines,line);}
// A texture copy of the game (copy_texture). A whole copy of a texture the replay has an output-size image for makes that image stand
// for the destination too (DOOM: postprocess draws into the window texture, the game copies it into the texture viewcolorupsample
// reads); any other copy into a tracked texture ends its tracking.
void copied(const p_copy_t2t &p){
    if(!p.src || !p.dst)return;const uint64_t frame=stats.presents.load();const bool mine=rp.hdr && rp.frame==frame;
    Fake *f=mine?tracked(p.src->serial):nullptr;
    const bool whole=p.src->d.width==p.dst->d.width && p.src->d.height==p.dst->d.height && p.sr.xoffset==0 && p.sr.yoffset==0 && p.dr.xoffset==0 && p.dr.yoffset==0 && uint32_t(p.sr.width)==p.src->d.width && uint32_t(p.sr.height)==p.src->d.height && !p.has_sv && !p.has_dv;
    if(diag_now() && diag_lines<160){++diag_lines;xbg_logf("NVN TU diag f=%llu #%u copy src=%llu:%ux%u:%#x%s -> dst=%llu:%ux%u:%#x region %d,%d %dx%d -> %d,%d views=%d/%d%s",(unsigned long long)frame,diag_lines,
        (unsigned long long)p.src->serial,p.src->d.width,p.src->d.height,p.src->d.format,f?"*":"",(unsigned long long)p.dst->serial,p.dst->d.width,p.dst->d.height,p.dst->d.format,int(p.sr.xoffset),int(p.sr.yoffset),int(p.sr.width),int(p.sr.height),int(p.dr.xoffset),int(p.dr.yoffset),int(p.has_sv),int(p.has_dv),f?(whole?" (followed)":" (partial: not followed)"):"");}
    if(!mine)return;
    if(f && whole){track(p.dst->serial,f);++rp.copies;}
    else{if(f)rnote("replay: a partial copy of a replayed target is not followed");untrack(p.dst->serial);}}
// A draw of a shader we have no role for that looks like one more fullscreen post pass over the scene: it overwrites a plain target of
// the scene's size (no blending, all channels, no depth test), its fragment uniform block starts with renderpositiontoviewtexture
// (zw = 1 / target size) and one of its textures has an output-size image. Returns that texture's slot: replayed like the known ones.
int generic_slot(const Shader *ps){
    const tex_t *rt0=state.rt.color[0];if(!plain_target(rt0) || (state.color.blend_en&1) || (state.mask.mask&15)!=15 || state.ds.dt || state.ds.st)return -1;
    const float *u=taafix::ubo(0,16);if(!u || !(fabs(double(u[2])*taa.tw-1.0)<1e-3 && fabs(double(u[3])*taa.th-1.0)<1e-3))return -1;
    for(const auto &b:ps->bindings){if(b.source!=XBG_SOURCE_TEXTURE || b.count!=1 || b.slot>=128)continue;
        const tex_t *t=tex_of(1,b.slot,false);if(t && t->serial!=rt0->serial && tracked(t->serial))return int(b.slot);}
    return -1;}
// After a motion blur / postprocess draw of the game, or a draw generic_slot() took for one more post pass (rp.pending 4 / 5 / 6): the
// same draw once more at the output size. `again` repeats the draw call.
template<class F> void replay(F &&again){
    const uint8_t role=rp.pending;rp.pending=0;Shader *ps=state.shader[4];const uint64_t frame=stats.presents.load();
    if(!ps || ps!=rp.pps || ps->tu_role!=(role==6?0:role) || rp.frame!=frame || !rp.hdr || in_replay || capture.active)return;
    const unsigned in_slot=role==6?unsigned(rp.pslot):1u;
    if(role==4 && !mb_on)return;   // DM_GFX_TU_MB=0: the upscaled frame gets no motion blur (two output-size passes less)
    const char *why=nullptr;Tex in;const tex_t *rt0=state.rt.n>0?state.rt.color[0]:nullptr;const NVNbufferRange ub=state.ub[1][0];
    if(!bound(ps,in_slot,in))why="replay: a post draw's colour input is not resolved";
    Fake *src=why?nullptr:tracked(in.t.serial);
    if(!why && !src){if(role!=5)return;   // a motion blur draw over something else is not the scene's
        // postprocess reads a texture no replayed draw stands for (the motion blur draws were not replayed, or a pass we do not know sits
        // in between): it gets the accumulated scene itself, so the upscaled frame lacks that pass (the motion blur) and nothing else
        src=rp.scene;if(mb_on)rnote("replay: the postprocess input is not a replayed target; the upscaled frame is made from the accumulated scene directly (no motion blur in it)");}
    if(!why && (!plain_target(rt0) || (state.rt.has_cview[0] && (state.rt.cview[0].set_levels || state.rt.cview[0].set_layers || state.rt.cview[0].set_target))))why="replay: a post draw's target is not a plain texture of the scene's size";
    if(!why && (!ub.address || ub.size<16 || ub.size>sizeof ubo_copy[0]))why="replay: a post draw's uniform block is not as expected";
    Fake *out=why?nullptr:out_for(rt0);
    if(!why && !out)why="replay: more post targets in one frame than expected";
    if(!why)swap_tex(in_slot,src,why);
    if(why){rnote(why);if(role==5)rp.why=why;return;}
    swp.rt=true;swp.rtv=state.rt;swp.vp=state.viewport;swp.sc=state.scissor;swp.ub=ub;
    p_set_rt r=state.rt;r.n=1;r.color[0]=&out->t;for(int i=1;i<8;i++){r.color[i]=nullptr;r.has_cview[i]=0;}r.depth=nullptr;r.has_dview=0;state.rt=r;   // one target (postprocess writes the same colour twice), no depth
    state.viewport.x=float(state.viewport.x*rp.kx);state.viewport.width=float(state.viewport.width*rp.kx);state.viewport.y=float(state.viewport.y*rp.ky);state.viewport.height=float(state.viewport.height*rp.ky);
    auto grow=[](uint32_t v,double k){return uint32_t(std::min(double(v)*k+0.5,4294967295.0));};
    state.scissor.offset.x=int32_t(lround(state.scissor.offset.x*rp.kx));state.scissor.offset.y=int32_t(lround(state.scissor.offset.y*rp.ky));state.scissor.extent.width=grow(state.scissor.extent.width,rp.kx);state.scissor.extent.height=grow(state.scissor.extent.height,rp.ky);
    // the fragment uniform block with renderpositiontoviewtexture.zw (gl_FragCoord -> uv) divided by the ratio: a host copy, announced
    // as a CPU write (as OP_UPDATE_UB does) so that no snapshot of an earlier use of the copy is taken for it
    uint8_t *copy=ubo_copy[ubo_next++%4];invalidate_cpu_range(uint64_t(uintptr_t(copy)),ub.size);memcpy(copy,reinterpret_cast<const void*>(uintptr_t(ub.address)),ub.size);
    float v[4];memcpy(v,copy,16);v[2]=float(v[2]/rp.kx);v[3]=float(v[3]/rp.ky);memcpy(copy,v,16);
    state.ub[1][0].address=uint64_t(uintptr_t(copy));state.ub[1][0].size=ub.size;gens();
    const unsigned slot=taa.slot,nth=rp.stamps;const bool stamp=queries && nth<3;if(stamp)++rp.stamps;   // a query pair is written once per reset
    if(stamp){Commands t(true);end_open_pass();vkrec::CmdWriteTimestamp(t.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,slot*QN+4+2*nth);}
    in_replay=true;again();in_replay=false;
    if(stamp){Commands t(true);end_open_pass();vkrec::CmdWriteTimestamp(t.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,slot*QN+5+2*nth);stamped[slot]|=uint8_t(4u<<nth);}
    swap_out();
    const Image *oi=find_image(out->t.serial);
    if(!oi || !oi->gpu_written){rnote("replay: a replayed post draw recorded nothing");if(role==5)rp.why="replay: the replayed postprocess draw recorded nothing";return;}   // never hand an unwritten image on
    // every colour target of the game's draw now has this image (postprocess writes the same colour to two targets)
    for(int i=0;i<state.rt.n && i<8;i++)if(state.rt.color[i])track(state.rt.color[i]->serial,out);
    ++rp.draws;if(role==5){rp.pp=true;rp.pp_serial=rt0->serial;rp.ldr=out;}else if(role==4)++rp.mb;
    else{static std::set<std::string> told;if(told.size()<16 && told.insert(ps->key).second)xbg_logf("NVN TU sgsr2 replay: draw ps=%s taken for a fullscreen post pass over the scene (input slot %u) and replayed at the output size",ps->key.c_str(),in_slot);}}
// draw(): constructed after the shaders are known; swaps the TAA module (and, route A-replay, its history texture) for this draw only
// (as GsPassthru drops a stage)
struct Draw{Shader *saved=nullptr;bool swapped=false;
    Draw(){if(!on())return;
        const Shader *vs=state.shader[0];Shader *ps=state.shader[4];
        if(diag_left>0 && !in_replay && ps && ps->tu_role==1 && !capture.active && stats.presents.load()>=retry_at){--diag_left;diag_frame=stats.presents.load();diag_lines=0;}   // a listed frame starts at its TAA draw
        if(diag_now())diag_line("draw",ps,4);
        if(in_replay)return;
        if(vs && vs->tu_role==3)jitter_capture();
        if(!ps || capture.active)return;
        if(!ps->tu_role){if(rp.hdr && rp.frame==stats.presents.load() && state.rt.n>0 && state.rt.color[0]){
                const int slot=generic_slot(ps);if(slot>=0){rp.pending=6;rp.pslot=slot;rp.pps=ps;}else if(tracked(state.rt.color[0]->serial))foreign(ps);}
            return;}
        if(ps->tu_role==1){const bool ok=taa_draw(ps);
            if(ok && route_v){const char *why=nullptr;
                Shader *r=nullptr;if(replay_taa(ps,why,r)){taa.raw=true;if(r){saved=ps;state.shader[4]=r;swapped=true;}}
                else{rp_why=why;rp_why_frame=stats.presents.load();}}
            else if(ok && live && notaa_usable())if(Shader *r=notaa(ps)){saved=ps;state.shader[4]=r;taa.raw=true;}}
        else if(ps->tu_role==2)up_draw(ps);
        else if(rp.hdr && rp.frame==stats.presents.load()){rp.pending=ps->tu_role;rp.pps=ps;}}   // 4 motion blur, 5 postprocess: replayed after this draw (tu::replay at the draw's call site)
    ~Draw(){if(swapped)swap_out();if(saved)state.shader[4]=saved;}};

// ---- present side
struct Shown{VkImage image{};VkFormat fmt=VK_FORMAT_UNDEFINED;uint32_t w=0,h=0;int32_t crop[4]{};};
// Pass 2 into the present's command buffer. true: `out` is the frame to present (upright, the window's size, TRANSFER_SRC_OPTIMAL).
bool resolve(VkCommandBuffer cmd,const win_t *window,const tex_t *texture,const Image &win,Shown &out){
    const uint64_t frame=stats.presents.load();
    if(diag_frame==frame){xbg_logf("NVN TU diag f=%llu end (%u lines): presented texture %llu, viewcolorupsample target %llu scene %llu%s, replay this frame: upscale=%d post draws=%u copies followed=%u",(unsigned long long)frame,diag_lines,(unsigned long long)texture->serial,
        (unsigned long long)(up.frame==frame?up.target:0),(unsigned long long)(up.frame==frame?up.scene.t.serial:0),rp.hdr && rp.frame==frame && up.frame==frame && tracked(up.scene.t.serial)?"*":"",int(rp.hdr && rp.frame==frame),rp.frame==frame?rp.draws:0u,rp.frame==frame?rp.copies:0u);diag_frame=UINT64_MAX;}
    if(taa.frame!=frame){hist_valid=false;return false;}   // no TAA draw in this frame (menu, loading, video): not an upscaler frame
    if(!taa.converted){if(frame>=retry_at)fallback("convert pass not recorded",frame);return false;}
    if(up.frame!=frame || !up.target)return fallback("no viewcolorupsample draw in the frame",frame);
    if(up.target!=texture->serial)return fallback("the presented texture is not the viewcolorupsample target",frame);
    int ww=0,wh=0;if(!dm_present_window_size(&ww,&wh) || ww<16 || wh<16)return fallback("window size not known (the gear choice sets DM_GFX_UPSCALE=fsr too)",frame);
    // the presenter's crop of the window texture, and the scale that brings that crop to the window (as present_body fits it)
    int cx=0,cy=0,cw=int(win.width),ch=int(win.height);const int32_t *wc=window->crop;const bool cropped=wc[2]>0 && wc[3]>0;
    if(cropped){cx=std::clamp(int(wc[0]),0,cw);cy=std::clamp(int(wc[1]),0,ch);cw=std::min(int(wc[2]),int(win.width)-cx);ch=std::min(int(wc[3]),int(win.height)-cy);}
    if(cw<=0 || ch<=0)return fallback("empty crop",frame);
    double s=std::min(double(ww)/cw,double(wh)/ch);if(max_h>0)s=std::min(s,double(max_h)/ch);
    if(s<1.0)return fallback("the window is smaller than the game's output",frame);
    uint32_t ow=uint32_t(lround(win.width*s)),oh=uint32_t(lround(win.height*s));
    if(route_v){   // [2.75] the replay targets are the game's targets times output / view: whole sizes need the output to be a multiple of view / gcd(view, target) (3 with the view at 75 %)
        auto fit=[](uint32_t o,uint32_t view,uint32_t target){if(!view || !target)return o;uint32_t a=view,b=target;while(b){const uint32_t r=a%b;a=b;b=r;}const uint32_t g=view/a;return g>1 && g<=16 && o>g?o-o%g:o;};
        ow=fit(ow,taa.rw,taa.tw);oh=fit(oh,taa.rh,taa.th);}
    if(ow>properties.limits.maxImageDimension2D || oh>properties.limits.maxImageDimension2D)return fallback("output larger than the device's image limit",frame);
    out_w=ow;out_h=oh;
    if(route_v){   // [2.75] route A-replay: the scene was upscaled at the TAA draw and the post draws were replayed; here only the UI goes over it
        if(rp.frame!=frame || !rp.hdr){if(rp_why && rp_why_frame==frame)return rfail(rp_why,frame);return fallback("replay: the output size is known now, starts with the next frame",frame,0);}
        // the scene viewcolorupsample reads: a replayed draw's target, or a copy of one (DOOM: postprocess -> window texture -> copy)
        Fake *sf=tracked(up.scene.t.serial);
        if(!sf || sf==rp.scene)return rfail(rp.why?rp.why:!rp.pp?"replay: no postprocess draw after the TAA draw":"replay: viewcolorupsample reads a texture that no replayed draw or copy produced",frame);
        if(rp.ow!=ow || rp.oh!=oh)return fallback("replay: the output size changed",frame,0);
        Image *si=find_image(sf->t.serial),*ui=up.has_ui?sampled(up.ui):nullptr;
        if(!si || !si->gpu_written || !(si->usage&VK_IMAGE_USAGE_SAMPLED_BIT) || si->samples!=VK_SAMPLE_COUNT_1_BIT || si->width<ow || si->height<oh)return rfail("replay: the replayed scene image is not usable",frame);
        if(ui && ui->aspects!=VK_IMAGE_ASPECT_COLOR_BIT)ui=nullptr;
        if(!base() || !final_targets(ow,oh,win.fmt))return false;
        struct{float osz[4],ui[4];}pc{};pc.osz[0]=float(ow);pc.osz[1]=float(oh);pc.osz[2]=1.0f/ow;pc.osz[3]=1.0f/oh;
        const float gamma=up.hi[4];pc.ui[0]=gamma>0.2f && gamma<5.0f?gamma:1.0f;pc.ui[1]=up.lo[4]==0.0f?1.0f:0.9f;pc.ui[2]=ui?1.0f:0.0f;   // viewcolorupsample: gamma.x, rendermode.x
        pc.ui[3]=(win.fmt==VK_FORMAT_R8G8B8A8_SRGB || win.fmt==VK_FORMAT_B8G8R8A8_SRGB)?1.0f:0.0f;
        if(si->layout!=VK_IMAGE_LAYOUT_GENERAL)si->transition(cmd,VK_IMAGE_LAYOUT_GENERAL);if(ui && ui->layout!=VK_IMAGE_LAYOUT_GENERAL)ui->transition(cmd,VK_IMAGE_LAYOUT_GENERAL);
        const unsigned slot=taa.slot,k=finr_next%3;const VkImageView sv=si->view(nullptr);
        const VkDescriptorImageInfo ii[2]={{s_nearest,sv,VK_IMAGE_LAYOUT_GENERAL},{s_linear,ui?ui->view(up.ui.has_view?&up.ui.v:nullptr):sv,VK_IMAGE_LAYOUT_GENERAL}};
        write_set(sets[2][slot],2,ii);
        if(queries)vkrec::CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,slot*QN+10);
        fullscreen(cmd,rp_fin,fb_fin[k],pipe_fin,pl[2],sets[2][slot],ow,oh,ow,oh,&pc,sizeof pc);
        if(queries){vkrec::CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,slot*QN+11);stamped[slot]|=32;}
        ++finr_next;out.image=finr[k].image;out.fmt=finr_fmt;out.w=ow;out.h=oh;
        if(cropped){out.crop[0]=int32_t(lround(cx*s));out.crop[1]=int32_t(lround(cy*s));out.crop[2]=std::min(int32_t(lround(cw*s)),int32_t(ow)-out.crop[0]);out.crop[3]=std::min(int32_t(lround(ch*s)),int32_t(oh)-out.crop[1]);}
        ++n_frames;n_post+=rp.draws;if(static_frames>=5)++n_static;rp_strikes=0;
        if(!told_live){told_live=true;xbg_logf("NVN TU sgsr2 replay live: render %ux%u (target %ux%u) -> output %ux%u (window %dx%d, window texture %ux%u crop %d,%d %dx%d), upscaled targets %ux%u (ratio %.4f,%.4f) hdr vk=%d ldr vk=%d, post draws replayed: motion blur %u + postprocess %d + other %u, scene taken through %u cop%s, ui=%d gamma=%g jitter_ndc=%g,%g%s",
            taa.rw,taa.rh,taa.tw,taa.th,ow,oh,ww,wh,win.width,win.height,cx,cy,cw,ch,rp.bw,rp.bh,rp.kx,rp.ky,int(hup_fmt),int(si->fmt),rp.mb,int(rp.pp),rp.draws-rp.mb-(rp.pp?1u:0u),rp.copies,rp.copies==1?"y":"ies",int(ui!=nullptr),double(pc.ui[0]),jit.frame==frame?jit.x:0.0,jit.frame==frame?jit.y:0.0,jit.frame==frame?"":" (no motion-vector pre-pass draw in this frame)");}
        live=true;status();return true;}
    Image *si=sampled(up.scene),*ui=up.has_ui?sampled(up.ui):nullptr;
    if(!si || si->aspects!=VK_IMAGE_ASPECT_COLOR_BIT || si->width<taa.rw || si->height<taa.rh)return fallback("scene colour image not usable",frame);
    if(ui && ui->aspects!=VK_IMAGE_ASPECT_COLOR_BIT)ui=nullptr;
    // the scene the last pass reads must be the view the TAA pass saw (same pixel grid): its size * its resolutionscale
    if(labs(lround(double(si->width)*up.lo[0])-long(taa.rw))>1 || labs(lround(double(si->height)*up.lo[1])-long(taa.rh))>1){
        static bool told=false;if(!told){told=true;xbg_logf("NVN TU sgsr2: scene %ux%u x scale %g,%g != TAA view %ux%u",si->width,si->height,double(up.lo[0]),double(up.lo[1]),taa.rw,taa.rh);}
        return fallback("the scene's size is not the TAA view's",frame);}
    if(!base() || !output_targets(cmd,ow,oh,win.fmt))return false;
    const bool reset=!hist_valid || last_frame+1!=frame || taa.cut;
    // jitter of this frame in render pixels, the game's rows (lower-left: texture v = NDC y * 0.5 + 0.5)
    float jx=0,jy=0;
    if(taa.raw){   // the colour is jittered only when the pass-through ran; the game's TAA output is resolved onto the un-jittered grid
        if(jit.frame==frame){jx=float(jit.x*0.5*taa.rw)*jit_mul[0];jy=float(jit.y*0.5*taa.rh)*jit_mul[1];if(!(fabsf(jx)<=1.5f && fabsf(jy)<=1.5f)){jx=jy=0;++n_nojit;}}
        else ++n_nojit;}
    last_jit[0]=jx;last_jit[1]=jy;
    struct{float rsz[4],osz[4],jit[4],par[4],tsz[4],ui[4];}pc{};
    pc.rsz[0]=float(taa.rw);pc.rsz[1]=float(taa.rh);pc.rsz[2]=1.0f/taa.rw;pc.rsz[3]=1.0f/taa.rh;pc.osz[0]=float(ow);pc.osz[1]=float(oh);pc.osz[2]=1.0f/ow;pc.osz[3]=1.0f/oh;
    const double rx=double(ow)/taa.rw,ry=double(oh)/taa.rh;
    pc.jit[0]=jx;pc.jit[1]=jy;pc.jit[2]=float(rx);pc.jit[3]=float(std::min(20.0,pow(rx*ry,3.0)));   // scaleRatio of the published sample: (output / render width, min(20, area ratio ^ 3))
    pc.par[0]=static_frames>=5?0.3f:0.0f;pc.par[1]=reset?1.0f:0.0f;pc.par[2]=1.0f;pc.par[3]=float(debug_v);   // minLerpContribution 0.3 on a still camera, as the published sample
    pc.tsz[0]=1.0f/si->width;pc.tsz[1]=1.0f/si->height;pc.tsz[2]=1.0f/motion.w;pc.tsz[3]=1.0f/motion.h;
    const float gamma=up.hi[4];pc.ui[0]=gamma>0.2f && gamma<5.0f?gamma:1.0f;pc.ui[1]=up.lo[4]==0.0f?1.0f:0.9f;pc.ui[2]=ui?1.0f:0.0f;   // viewcolorupsample: gamma.x, rendermode.x
    pc.ui[3]=(win.fmt==VK_FORMAT_R8G8B8A8_SRGB || win.fmt==VK_FORMAT_B8G8R8A8_SRGB)?1.0f:0.0f;
    if(si->layout!=VK_IMAGE_LAYOUT_GENERAL)si->transition(cmd,VK_IMAGE_LAYOUT_GENERAL);if(ui && ui->layout!=VK_IMAGE_LAYOUT_GENERAL)ui->transition(cmd,VK_IMAGE_LAYOUT_GENERAL);
    const unsigned slot=taa.slot,wr=hist_cur^1u,k=fin_next%3;
    const VkImageView sv=si->view(up.scene.has_view?&up.scene.v:nullptr);
    const VkDescriptorImageInfo ii[4]={{s_linear,hist[hist_cur].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{s_linear,motion.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{s_nearest,sv,VK_IMAGE_LAYOUT_GENERAL},
        {s_linear,ui?ui->view(up.ui.has_view?&up.ui.v:nullptr):sv,VK_IMAGE_LAYOUT_GENERAL}};
    write_set(sets[1][slot],4,ii);
    if(queries)vkrec::CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,slot*QN+2);
    fullscreen(cmd,rp_up,fb_up[wr][k],pipe_up,pl[1],sets[1][slot],ow,oh,ow,oh,&pc,sizeof pc);
    if(queries){vkrec::CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,slot*QN+3);stamped[slot]|=2;}
    hist_cur=wr;++fin_next;hist_valid=true;last_frame=frame;
    out.image=fin[k].image;out.fmt=fin_fmt;out.w=ow;out.h=oh;
    if(cropped){out.crop[0]=int32_t(lround(cx*s));out.crop[1]=int32_t(lround(cy*s));out.crop[2]=std::min(int32_t(lround(cw*s)),int32_t(ow)-out.crop[0]);out.crop[3]=std::min(int32_t(lround(ch*s)),int32_t(oh)-out.crop[1]);}
    ++n_frames;if(reset)++n_reset;if(static_frames>=5)++n_static;
    if(!told_live){told_live=true;xbg_logf("NVN TU sgsr2 live: render %ux%u (target %ux%u, scene %ux%u vk=%d) -> output %ux%u (window %dx%d, window texture %ux%u crop %d,%d %dx%d) ui=%d gamma=%g jitter_ndc=%g,%g%s",
        taa.rw,taa.rh,taa.tw,taa.th,si->width,si->height,int(si->fmt),ow,oh,ww,wh,win.width,win.height,cx,cy,cw,ch,int(ui!=nullptr),double(pc.ui[0]),jit.frame==frame?jit.x:0.0,jit.frame==frame?jit.y:0.0,jit.frame==frame?"":" (no motion-vector pre-pass draw in this frame)");}
    live=true;status();return true;}
// Engine shutdown (the device goes away; a later initialize starts clean)
void shutdown(){
    if(notaa_shader){delete notaa_shader;notaa_shader=nullptr;}notaa_of=nullptr;notaa_bad=false;
    if(down_shader){delete down_shader;down_shader=nullptr;}down_of=nullptr;down_bad=false;
    // [2.75] route A-replay: its framebuffers, passes and pipelines; the images behind the Fakes are the engine's (images.clear() right after)
    for(auto &f:fakes)if(f->fb)vkDestroyFramebuffer(cfg.device,f->fb,nullptr);fakes.clear();hist_f[0]=hist_f[1]=nullptr;for(auto &o:outs)o=Out{};
    for(auto &f:fb_fin){if(f)vkDestroyFramebuffer(cfg.device,f,nullptr);f=VK_NULL_HANDLE;}
    if(pipe_hup)vkDestroyPipeline(cfg.device,pipe_hup,nullptr);if(pipe_fin)vkDestroyPipeline(cfg.device,pipe_fin,nullptr);if(rp_hup)vkDestroyRenderPass(cfg.device,rp_hup,nullptr);if(rp_fin)vkDestroyRenderPass(cfg.device,rp_fin,nullptr);
    pipe_hup=pipe_fin=VK_NULL_HANDLE;rp_hup=rp_fin=VK_NULL_HANDLE;hup_fmt=finr_fmt=VK_FORMAT_UNDEFINED;rp=Rp{};swp=Swap{};memset(cfg_now,0,sizeof cfg_now);out_w=out_h=0;rp_strikes=0;rp_why=nullptr;in_replay=false;
    for(auto &row:fb_up)for(auto &f:row){if(f)vkDestroyFramebuffer(cfg.device,f,nullptr);f=VK_NULL_HANDLE;}if(fb_motion)vkDestroyFramebuffer(cfg.device,fb_motion,nullptr);fb_motion=VK_NULL_HANDLE;
    auto kill=[](Tgt &t){if(t.view)vkDestroyImageView(cfg.device,t.view,nullptr);if(t.image)vkDestroyImage(cfg.device,t.image,nullptr);if(t.memory)vkFreeMemory(cfg.device,t.memory,nullptr);t=Tgt{};};
    kill(motion);for(auto &t:hist)kill(t);for(auto &t:fin)kill(t);for(auto &t:finr)kill(t);
    if(pipe_conv)vkDestroyPipeline(cfg.device,pipe_conv,nullptr);if(pipe_up)vkDestroyPipeline(cfg.device,pipe_up,nullptr);if(rp_conv)vkDestroyRenderPass(cfg.device,rp_conv,nullptr);if(rp_up)vkDestroyRenderPass(cfg.device,rp_up,nullptr);
    if(vs_mod)vkDestroyShaderModule(cfg.device,vs_mod,nullptr);if(queries)vkDestroyQueryPool(cfg.device,queries,nullptr);if(dpool)vkDestroyDescriptorPool(cfg.device,dpool,nullptr);
    for(int k=0;k<3;k++){if(pl[k])vkDestroyPipelineLayout(cfg.device,pl[k],nullptr);if(dsl[k])vkDestroyDescriptorSetLayout(cfg.device,dsl[k],nullptr);pl[k]=VK_NULL_HANDLE;dsl[k]=VK_NULL_HANDLE;}
    if(s_linear)vkDestroySampler(cfg.device,s_linear,nullptr);if(s_nearest)vkDestroySampler(cfg.device,s_nearest,nullptr);
    pipe_conv=pipe_up=VK_NULL_HANDLE;rp_conv=rp_up=VK_NULL_HANDLE;vs_mod=VK_NULL_HANDLE;queries=VK_NULL_HANDLE;dpool=VK_NULL_HANDLE;s_linear=s_nearest=VK_NULL_HANDLE;
    memset(sets,0,sizeof sets);memset(stamped,0,sizeof stamped);fin_fmt=hist_fmt=VK_FORMAT_UNDEFINED;base_ready=false;told_live=false;live=false;hist_valid=false;have_prev_cam=false;
    taa=Taa{};up=Up{};jit=Jit{};last_frame=UINT64_MAX;retry_at=0;failed=false;}
}
