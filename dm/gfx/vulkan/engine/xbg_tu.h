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
// DM_GFX_TU_JITTER=<sx>,<sy> (jitter multipliers, e.g. -1,1 or 0,0 to check the sign), DM_GFX_TU_MAXH=<rows> (cap the output
// height; the presenter's FSR pass does the rest), DM_GFX_TU_CUT=<units>, DM_GFX_TU_HISTORY=16f (RGBA16F history instead of
// A2B10G10R10). DM_GFX_TU_INPUT=reduced is read by host/hle_args.c (3D render fraction 75 %).
// Status: "NVN TU sgsr2 ..." lines: one at the start, one when it goes live, one every ~3 s with the GPU time of the passes.
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
int mode_v=-1,debug_v=0,max_h=0;bool failed=false,live=false,notaa_on=true,hist16=false;float jit_mul[2]={1,1},cut_units=256;
int mode(){if(mode_v<0){const char *e=getenv("DM_GFX_TU");mode_v=e && (!strcmp(e,"sgsr2") || !strcmp(e,"1"))?1:0;
    if(e && *e && !mode_v && strcmp(e,"0") && strcmp(e,"off"))xbg_logf("NVN TU: DM_GFX_TU=%s is not known (sgsr2 | 0): off",e);
    if(mode_v){
        if(const char *d=getenv("DM_GFX_TU_DEBUG"))debug_v=!strcmp(d,"mv")?1:!strcmp(d,"clip")?2:!strcmp(d,"nohist")?3:!strcmp(d,"input")?4:std::clamp(atoi(d),0,4);
        if(const char *j=getenv("DM_GFX_TU_JITTER")){float a=1,b=1;if(sscanf(j,"%f,%f",&a,&b)==2){jit_mul[0]=a;jit_mul[1]=b;}}
        if(const char *n=getenv("DM_GFX_TU_NOTAA"))notaa_on=atoi(n)!=0;
        if(const char *m=getenv("DM_GFX_TU_MAXH"))max_h=std::max(0,atoi(m));
        if(const char *c=getenv("DM_GFX_TU_CUT"))cut_units=float(std::max(1.0,atof(c)));
        if(const char *h=getenv("DM_GFX_TU_HISTORY"))hist16=!strcmp(h,"16f");
        const char *in=getenv("DM_GFX_TU_INPUT");
        xbg_logf("NVN TU sgsr2 on (DM_GFX_TU): SGSR2 2-pass fragment upscale at the viewcolorupsample point, input=%s (DM_GFX_TU_INPUT), game TAA %s (DM_GFX_TU_NOTAA=%d), debug=%d jitter x%g,%g max_h=%d cut=%g%s%s",
            in && *in?in:"full",notaa_on?"replaced by a pass-through while live":"kept, no jitter used",int(notaa_on),debug_v,double(jit_mul[0]),double(jit_mul[1]),max_h,double(cut_units),
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
uint64_t n_frames=0,n_fallback=0,n_reset=0,n_nojit=0,n_static=0,win_t0=0;const char *last_why="";double conv_ns=0,up_ns=0,max_ns=0;unsigned timed=0;float last_jit[2]={};

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
VkSampler s_linear{},s_nearest{};VkDescriptorSetLayout dsl[2]{};VkPipelineLayout pl[2]{};VkDescriptorPool dpool{};VkDescriptorSet sets[2][RING]{};
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
    const uint32_t counts[2]={2,4},push[2]={128,96};
    for(int k=0;k<2;k++){VkDescriptorSetLayoutBinding b[4]{};for(uint32_t i=0;i<counts[k];i++){b[i].binding=i;b[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[i].descriptorCount=1;b[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=counts[k];dl.pBindings=b;if((r=vkCreateDescriptorSetLayout(cfg.device,&dl,nullptr,&dsl[k]))!=VK_SUCCESS){fail("descriptor set layout",r);return false;}
        VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT,0,push[k]};VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&dsl[k];pi.pushConstantRangeCount=1;pi.pPushConstantRanges=&pc;
        if((r=vkCreatePipelineLayout(cfg.device,&pi,nullptr,&pl[k]))!=VK_SUCCESS){fail("pipeline layout",r);return false;}}
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,RING*6};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=RING*2;dp.poolSizeCount=1;dp.pPoolSizes=&ps;
    if((r=vkCreateDescriptorPool(cfg.device,&dp,nullptr,&dpool))!=VK_SUCCESS){fail("descriptor pool",r);return false;}
    for(int k=0;k<2;k++){VkDescriptorSetLayout ls[RING];for(auto &l:ls)l=dsl[k];VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=dpool;ai.descriptorSetCount=RING;ai.pSetLayouts=ls;
        if((r=vkAllocateDescriptorSets(cfg.device,&ai,sets[k]))!=VK_SUCCESS){fail("descriptor sets",r);return false;}}
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=sizeof vs_code;mi.pCode=vs_code;{drvc::Lock compile(true);r=vkCreateShaderModule(cfg.device,&mi,nullptr,&vs_mod);}if(r!=VK_SUCCESS){fail("vertex shader",r);return false;}
    if(!usable(VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)){fail("RGBA16F motion target (format features)",0);return false;}
    const VkFormat mf=VK_FORMAT_R16G16B16A16_SFLOAT;const VkImageLayout ml=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(!(rp_conv=make_pass(1,&mf,&ml,"tu convert pass")) || !(pipe_conv=make_pipeline(rp_conv,pl[0],1,convert_code,sizeof convert_code,"tu sgsr2 convert")))return false;
    // GPU cost of the two passes: timestamps, when the queue has them
    if(timestamp_bits && properties.limits.timestampPeriod>0){VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=RING*4;if(vkCreateQueryPool(cfg.device,&qi,nullptr,&queries)!=VK_SUCCESS)queries=VK_NULL_HANDLE;}
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
// The frame slot's timestamps of 8 frames ago (0/1 around the convert pass, 2/3 around the upscale pass), then the slot is reset
void collect(VkCommandBuffer cmd,unsigned slot){
    if(!queries)return;
    if(stamped[slot]){uint64_t t[8]{};   // value, availability per query
        // no WAIT: a pass that was not recorded for this slot leaves its queries unavailable (VK_NOT_READY, availability 0)
        const VkResult qr=vkGetQueryPoolResults(cfg.device,queries,slot*4,4,sizeof t,t,16,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(qr==VK_SUCCESS || qr==VK_NOT_READY){
            const uint64_t mask=timestamp_bits<64?(uint64_t(1)<<timestamp_bits)-1:~0ull;const double per=properties.limits.timestampPeriod;double c=0,u=0;
            if((stamped[slot]&1) && t[1] && t[3])c=double((t[2]-t[0])&mask)*per;
            if((stamped[slot]&2) && t[5] && t[7])u=double((t[6]-t[4])&mask)*per;
            if(u>0){conv_ns+=c;up_ns+=u;max_ns=std::max(max_ns,c+u);++timed;}}
        stamped[slot]=0;}
    vkrec::CmdResetQueryPool(cmd,queries,slot*4,4);}
void status(){
    const uint64_t now=timing_clock();if(!win_t0)win_t0=now;if(now-win_t0<3000000000ull)return;
    xbg_logf("NVN TU sgsr2 %ux%u -> %ux%u gpu_us=%.0f (convert %.0f + upscale %.0f) max_us=%.0f frames=%llu fallback=%llu%s%s resets=%llu nojitter=%llu static=%llu jitter_px=%.3f,%.3f notaa=%d",
        taa.rw,taa.rh,fin[0].w,fin[0].h,timed?(conv_ns+up_ns)/timed/1e3:-1.0,timed?conv_ns/timed/1e3:-1.0,timed?up_ns/timed/1e3:-1.0,max_ns/1e3,(unsigned long long)n_frames,(unsigned long long)n_fallback,
        n_fallback?" last=":"",n_fallback?last_why:"",(unsigned long long)n_reset,(unsigned long long)n_nojit,(unsigned long long)n_static,double(last_jit[0]),double(last_jit[1]),int(taa.raw));
    n_frames=n_fallback=n_reset=n_nojit=n_static=0;conv_ns=up_ns=max_ns=0;timed=0;win_t0=now;}
// A frame with a TAA draw that could not be upscaled: the old path shows it; the convert pass is not recorded for the next half
// second (a lasting cause would otherwise cost it every frame), and the game's TAA comes back meanwhile
bool fallback(const char *why,uint64_t frame){
    static std::set<std::string> told;if(told.size()<24 && told.insert(why).second)xbg_logf("NVN TU sgsr2: frame %llu not upscaled (%s); the game's frame goes the old way",(unsigned long long)frame,why);
    ++n_fallback;last_why=why;live=false;hist_valid=false;retry_at=frame+30;status();return false;}

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
    if(queries)vkrec::CmdWriteTimestamp(c.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,slot*4);
    fullscreen(c.command,rp_conv,fb_motion,pipe_conv,pl[0],sets[0][slot],motion.w,motion.h,taa.rw,taa.rh,&pc,sizeof pc);
    if(queries){vkrec::CmdWriteTimestamp(c.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,slot*4+1);stamped[slot]|=1;}
    c.finish();taa.converted=true;return true;}
void up_draw(const Shader *ps){
    up.frame=stats.presents.load();up.target=state.rt.n>0 && state.rt.color[0]?state.rt.color[0]->serial:0;up.has_ui=false;
    const float *h=taafix::ubo(0,48),*l=taafix::ubo(1,32);if(!h || !l || !bound(ps,0,up.scene)){up.target=0;return;}
    memcpy(up.hi,h,sizeof up.hi);memcpy(up.lo,l,sizeof up.lo);up.has_ui=bound(ps,1,up.ui);}
// draw(): constructed after the shaders are known; swaps the TAA module for this draw only (as GsPassthru drops a stage)
struct Draw{Shader *saved=nullptr;
    Draw(){if(!on())return;
        const Shader *vs=state.shader[0];Shader *ps=state.shader[4];
        if(vs && vs->tu_role==3)jitter_capture();
        if(!ps || !ps->tu_role || capture.active)return;
        if(ps->tu_role==1){const bool ok=taa_draw(ps);if(ok && live && notaa_usable())if(Shader *r=notaa(ps)){saved=ps;state.shader[4]=r;taa.raw=true;}}
        else if(ps->tu_role==2)up_draw(ps);}
    ~Draw(){if(saved)state.shader[4]=saved;}};

// ---- present side
struct Shown{VkImage image{};VkFormat fmt=VK_FORMAT_UNDEFINED;uint32_t w=0,h=0;int32_t crop[4]{};};
// Pass 2 into the present's command buffer. true: `out` is the frame to present (upright, the window's size, TRANSFER_SRC_OPTIMAL).
bool resolve(VkCommandBuffer cmd,const win_t *window,const tex_t *texture,const Image &win,Shown &out){
    const uint64_t frame=stats.presents.load();
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
    const uint32_t ow=uint32_t(lround(win.width*s)),oh=uint32_t(lround(win.height*s));
    if(ow>properties.limits.maxImageDimension2D || oh>properties.limits.maxImageDimension2D)return fallback("output larger than the device's image limit",frame);
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
    if(queries)vkrec::CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,slot*4+2);
    fullscreen(cmd,rp_up,fb_up[wr][k],pipe_up,pl[1],sets[1][slot],ow,oh,ow,oh,&pc,sizeof pc);
    if(queries){vkrec::CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,slot*4+3);stamped[slot]|=2;}
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
    for(auto &row:fb_up)for(auto &f:row){if(f)vkDestroyFramebuffer(cfg.device,f,nullptr);f=VK_NULL_HANDLE;}if(fb_motion)vkDestroyFramebuffer(cfg.device,fb_motion,nullptr);fb_motion=VK_NULL_HANDLE;
    auto kill=[](Tgt &t){if(t.view)vkDestroyImageView(cfg.device,t.view,nullptr);if(t.image)vkDestroyImage(cfg.device,t.image,nullptr);if(t.memory)vkFreeMemory(cfg.device,t.memory,nullptr);t=Tgt{};};
    kill(motion);for(auto &t:hist)kill(t);for(auto &t:fin)kill(t);
    if(pipe_conv)vkDestroyPipeline(cfg.device,pipe_conv,nullptr);if(pipe_up)vkDestroyPipeline(cfg.device,pipe_up,nullptr);if(rp_conv)vkDestroyRenderPass(cfg.device,rp_conv,nullptr);if(rp_up)vkDestroyRenderPass(cfg.device,rp_up,nullptr);
    if(vs_mod)vkDestroyShaderModule(cfg.device,vs_mod,nullptr);if(queries)vkDestroyQueryPool(cfg.device,queries,nullptr);if(dpool)vkDestroyDescriptorPool(cfg.device,dpool,nullptr);
    for(int k=0;k<2;k++){if(pl[k])vkDestroyPipelineLayout(cfg.device,pl[k],nullptr);if(dsl[k])vkDestroyDescriptorSetLayout(cfg.device,dsl[k],nullptr);pl[k]=VK_NULL_HANDLE;dsl[k]=VK_NULL_HANDLE;}
    if(s_linear)vkDestroySampler(cfg.device,s_linear,nullptr);if(s_nearest)vkDestroySampler(cfg.device,s_nearest,nullptr);
    pipe_conv=pipe_up=VK_NULL_HANDLE;rp_conv=rp_up=VK_NULL_HANDLE;vs_mod=VK_NULL_HANDLE;queries=VK_NULL_HANDLE;dpool=VK_NULL_HANDLE;s_linear=s_nearest=VK_NULL_HANDLE;
    memset(sets,0,sizeof sets);memset(stamped,0,sizeof stamped);fin_fmt=hist_fmt=VK_FORMAT_UNDEFINED;base_ready=false;told_live=false;live=false;hist_valid=false;have_prev_cam=false;
    taa=Taa{};up=Up{};jit=Jit{};last_frame=UINT64_MAX;retry_at=0;failed=false;}
}
