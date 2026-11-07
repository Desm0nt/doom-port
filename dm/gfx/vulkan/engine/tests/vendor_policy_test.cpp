// [nonadreno] Calls the production GPU-family detection, per-family defaults and DM_GFX_SIM_GPU masks (xbg_vendor_policy.h)
// directly; integer / struct work only, no device. Build (WSL or any host with the Vulkan headers, e.g. the NDK sysroot):
//   g++ -std=c++17 -Wall -idirafter <ndk>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include -o vendor_policy_test vendor_policy_test.cpp
#include "../xbg_vendor_policy.h"
#include <stdio.h>
#include <initializer_list>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

int main(){
    // Families by vendor ID, and by name where the ID says nothing (RADV on Xclipse = AMD).
    CHECK(xbg_gpu_family(0x5143,"Adreno (TM) 750")==XBG_GPU_ADRENO);
    CHECK(xbg_gpu_family(0x5143,"Turnip Adreno (TM) 750")==XBG_GPU_ADRENO);
    CHECK(xbg_gpu_family(0x13B5,"Mali-G720 MC7")==XBG_GPU_MALI);
    CHECK(xbg_gpu_family(0x1010,"PowerVR B-Series BXM-8-256")==XBG_GPU_POWERVR);
    CHECK(xbg_gpu_family(0x144D,"Samsung Xclipse 940")==XBG_GPU_XCLIPSE);
    CHECK(xbg_gpu_family(0x1002,"AMD Xclipse 940 (RADV)")==XBG_GPU_XCLIPSE);
    CHECK(xbg_gpu_family(0x19E5,"Maleoon 910")==XBG_GPU_MALEOON);
    CHECK(xbg_gpu_family(0x10005,"llvmpipe")==XBG_GPU_OTHER);
    CHECK(xbg_gpu_family(0,nullptr)==XBG_GPU_OTHER);

    // Adreno keeps every old default: zero-copy on, no serial builds, no graphics guard, dynamic UBOs, no colour mask.
    CHECK(xbg_zero_copy_default(XBG_GPU_ADRENO));
    CHECK(!xbg_pipe_serial_default(XBG_GPU_ADRENO));
    CHECK(!xbg_pipe_guard_default(XBG_GPU_ADRENO));
    CHECK(xbg_ubo_dynamic_default(XBG_GPU_ADRENO));
    CHECK(!xbg_noout_mask_default(XBG_GPU_ADRENO));
    // Mali: copy path, guard, colour mask; PowerVR also serial builds and no dynamic UBOs.
    CHECK(!xbg_zero_copy_default(XBG_GPU_MALI));
    CHECK(xbg_pipe_guard_default(XBG_GPU_MALI));
    CHECK(xbg_noout_mask_default(XBG_GPU_MALI));
    CHECK(!xbg_pipe_serial_default(XBG_GPU_MALI));
    CHECK(xbg_ubo_dynamic_default(XBG_GPU_MALI));
    CHECK(xbg_pipe_serial_default(XBG_GPU_POWERVR));
    CHECK(!xbg_ubo_dynamic_default(XBG_GPU_POWERVR));
    CHECK(!xbg_zero_copy_default(XBG_GPU_OTHER));

    // Switch precedence: "0" / "1" win, empty / unset falls back.
    CHECK(xbg_env_or("1",false));
    CHECK(!xbg_env_or("0",true));
    CHECK(xbg_env_or(nullptr,true));
    CHECK(!xbg_env_or("",false));

    // DM_GFX_SIM_GPU parsing.
    char unknown[32];
    CHECK(xbg_sim_parse(nullptr)==0);
    CHECK(xbg_sim_parse("")==0);
    CHECK(xbg_sim_parse("0")==0);
    CHECK(xbg_sim_parse("mali")==XBG_SIM_PROFILE_MALI);
    CHECK(xbg_sim_parse("powervr")==XBG_SIM_PROFILE_POWERVR);
    CHECK(xbg_sim_parse("pvr")==XBG_SIM_PROFILE_POWERVR);
    CHECK(xbg_sim_parse("\"mali,nogs\"")==(XBG_SIM_PROFILE_MALI|XBG_SIM_NOGS));
    CHECK(xbg_sim_parse("nobc nodepthbounds")==(XBG_SIM_NOBC|XBG_SIM_NODEPTHBOUNDS));
    CHECK(xbg_sim_parse("nobc,bogus",unknown,sizeof unknown)==XBG_SIM_NOBC && !strcmp(unknown,"bogus"));
    CHECK(xbg_sim_parse("nobc",unknown,sizeof unknown)==XBG_SIM_NOBC && !*unknown);
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,0)==XBG_GPU_ADRENO);
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,XBG_SIM_NOBC)==XBG_GPU_ADRENO);
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,XBG_SIM_PROFILE_MALI)==XBG_GPU_MALI);
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,XBG_SIM_PROFILE_POWERVR)==XBG_GPU_POWERVR);
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,XBG_SIM_PROFILE_XCLIPSE)==XBG_GPU_XCLIPSE);

    // Format masks: no profile = untouched; BC gone; 16-bit unorm loses linear filtering only; depth stand-ins.
    const VkFormatFeatureFlags all=0x1ffffu;VkFormatProperties p;
    p={all,all,all};xbg_sim_format_features(0,VK_FORMAT_BC1_RGB_UNORM_BLOCK,&p);CHECK(p.optimalTilingFeatures==all);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_MALI,VK_FORMAT_BC1_RGB_UNORM_BLOCK,&p);CHECK(!p.optimalTilingFeatures && !p.bufferFeatures);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_MALI,VK_FORMAT_BC7_SRGB_BLOCK,&p);CHECK(!p.optimalTilingFeatures);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_MALI,VK_FORMAT_R8G8B8A8_UNORM,&p);CHECK(p.optimalTilingFeatures==all);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_MALI,VK_FORMAT_R16G16_UNORM,&p);
    CHECK(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && (p.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (p.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT));
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_MALI,VK_FORMAT_R16G16B16A16_UNORM,&p);CHECK(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT));
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_MALI,VK_FORMAT_D24_UNORM_S8_UINT,&p);CHECK(p.optimalTilingFeatures==all);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_XCLIPSE,VK_FORMAT_BC1_RGB_UNORM_BLOCK,&p);CHECK(p.optimalTilingFeatures==all);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_XCLIPSE,VK_FORMAT_BC4_UNORM_BLOCK,&p);CHECK(!p.optimalTilingFeatures);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_XCLIPSE,VK_FORMAT_D24_UNORM_S8_UINT,&p);CHECK(!p.optimalTilingFeatures);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_POWERVR,VK_FORMAT_D16_UNORM,&p);CHECK(p.optimalTilingFeatures==all);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_POWERVR,VK_FORMAT_D32_SFLOAT,&p);
    CHECK(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && (p.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_POWERVR,VK_FORMAT_R8G8B8A8_SRGB,&p);CHECK(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT));
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_POWERVR,VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,&p);CHECK(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));

    // Features / subgroup / limits.
    VkPhysicalDeviceFeatures f;memset(&f,0xff,sizeof f);for(VkBool32 *b=&f.robustBufferAccess;b<=&f.inheritedQueries;++b)*b=VK_TRUE;
    VkPhysicalDeviceFeatures g=f;xbg_sim_features(0,&g);CHECK(!memcmp(&f,&g,sizeof f));
    g=f;xbg_sim_features(XBG_SIM_PROFILE_MALI,&g);CHECK(!g.textureCompressionBC && !g.depthBounds && !g.fillModeNonSolid && !g.shaderClipDistance && g.geometryShader && g.shaderImageGatherExtended);
    g=f;xbg_sim_features(XBG_SIM_PROFILE_MALI|XBG_SIM_NOGS,&g);CHECK(!g.geometryShader);
    g=f;xbg_sim_features(XBG_SIM_NOGATHER|XBG_SIM_NOANISO,&g);CHECK(!g.shaderImageGatherExtended && !g.samplerAnisotropy && g.textureCompressionBC);
    VkPhysicalDeviceSubgroupProperties s{};s.subgroupSize=64;s.supportedStages=VK_SHADER_STAGE_ALL;s.supportedOperations=0xff;s.quadOperationsInAllStages=VK_TRUE;
    VkPhysicalDeviceSubgroupProperties t=s;xbg_sim_subgroup(XBG_SIM_PROFILE_MALI,&t);
    CHECK(t.subgroupSize==16 && t.supportedStages==(VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT) && !t.quadOperationsInAllStages && t.supportedOperations==0xff);
    t=s;xbg_sim_subgroup(XBG_SIM_PROFILE_POWERVR,&t);CHECK(t.subgroupSize==1);
    t=s;xbg_sim_subgroup(0,&t);CHECK(t.subgroupSize==64 && t.quadOperationsInAllStages);
    VkPhysicalDeviceLimits l{};l.maxTexelBufferElements=1u<<27;xbg_sim_limits(XBG_SIM_PROFILE_MALI,&l);CHECK(l.maxTexelBufferElements==1u<<27);
    xbg_sim_limits(XBG_SIM_PROFILE_POWERVR,&l);CHECK(l.maxTexelBufferElements==65536);

    // Texel buffer ranges: unchanged when they fit (Adreno's 2^27 covers any DOOM buffer), cut to whole texels otherwise.
    CHECK(xbg_texel_range_clamp(64ull<<20,1,1u<<27)==64ull<<20);
    CHECK(xbg_texel_range_clamp(1u<<20,16,65536)==1u<<20);
    CHECK(xbg_texel_range_clamp(2u<<20,16,65536)==1u<<20);
    CHECK(xbg_texel_range_clamp(1000000,0,65536)==1000000);
    CHECK(xbg_vk_texel_bytes(VK_FORMAT_R32G32B32A32_SFLOAT)==16 && xbg_vk_texel_bytes(VK_FORMAT_R8_UNORM)==1 && xbg_vk_texel_bytes(VK_FORMAT_BC1_RGB_UNORM_BLOCK)==0);

    // Vertex widening (only reached without VERTEX_BUFFER support) and its simulation.
    CHECK(xbg_vertex_widen(VK_FORMAT_R16G16B16_SNORM)==VK_FORMAT_R16G16B16A16_SNORM);
    CHECK(xbg_vertex_widen(VK_FORMAT_R8G8B8_UNORM)==VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(xbg_vertex_widen(VK_FORMAT_R32G32B32_SFLOAT)==VK_FORMAT_R32G32B32A32_SFLOAT);
    CHECK(xbg_vertex_widen(VK_FORMAT_R8G8B8A8_UNORM)==VK_FORMAT_UNDEFINED);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_XCLIPSE,VK_FORMAT_R16G16B16_SNORM,&p);CHECK(!(p.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) && p.optimalTilingFeatures==all);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_XCLIPSE,VK_FORMAT_R32G32B32_SFLOAT,&p);CHECK(p.bufferFeatures==all);
    p={all,all,all};xbg_sim_format_features(XBG_SIM_PROFILE_MALI,VK_FORMAT_R16G16B16_SNORM,&p);CHECK(p.bufferFeatures==all);
    CHECK(xbg_sim_parse("novertex3")==XBG_SIM_NOVERTEX3);

    // [tile] Tiler defaults: Adreno (and Xclipse / other) keep the old behaviour, Mali / PowerVR get the tile-friendly paths;
    // the render-area cut is Mali only (PowerVR's Mesa driver reloads depth for a partial area).
    for(XbgGpuFamily fam:{XBG_GPU_ADRENO,XBG_GPU_XCLIPSE,XBG_GPU_OTHER,XBG_GPU_MALEOON}){
        CHECK(!xbg_noload_default(fam));CHECK(!xbg_tile_area_default(fam));CHECK(!xbg_tile_barrier_default(fam));CHECK(xbg_chunk_mid_split_default(fam));}
    CHECK(xbg_noload_default(XBG_GPU_MALI) && xbg_tile_area_default(XBG_GPU_MALI) && xbg_tile_barrier_default(XBG_GPU_MALI) && !xbg_chunk_mid_split_default(XBG_GPU_MALI));
    CHECK(xbg_noload_default(XBG_GPU_POWERVR) && !xbg_tile_area_default(XBG_GPU_POWERVR) && xbg_tile_barrier_default(XBG_GPU_POWERVR) && !xbg_chunk_mid_split_default(XBG_GPU_POWERVR));
    CHECK(xbg_tile_area_default(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("mali"))));
    CHECK(!xbg_tile_area_default(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("nobc,subgroup16"))));   // capability tokens alone keep Adreno's defaults

    // [tile] Draw rectangles: scissor exact; triangles narrowed to the (possibly flipped) viewport with a pixel of margin; points /
    // lines scissor only; clamped to the framebuffer; empty when nothing is left.
    {XbgRect r=xbg_draw_rect(0,0,4096,4096,1024,2048,512,512,true,4096,4096);CHECK(r.x0==1023 && r.y0==2047 && r.x1==1538 && r.y1==2562);
     r=xbg_draw_rect(0,0,4096,4096,1024,2560,512,-512,true,4096,4096);CHECK(r.x0==1023 && r.y0==2047 && r.x1==1538 && r.y1==2562);   // flipped viewport, same rows
     r=xbg_draw_rect(0,0,4096,4096,1024,2048,512,512,false,4096,4096);CHECK(r.x0==0 && r.y0==0 && r.x1==4096 && r.y1==4096);
     r=xbg_draw_rect(100,200,50,60,0,0,4096,4096,true,4096,4096);CHECK(r.x0==100 && r.y0==200 && r.x1==150 && r.y1==260);
     r=xbg_draw_rect(0,0,1600,720,-0.5f,-3.25f,1601.5f,724.f,true,1600,720);CHECK(r.x0==0 && r.y0==0 && r.x1==1600 && r.y1==720);
     r=xbg_draw_rect(0,0,100,100,500,500,10,10,true,100,100);CHECK(xbg_rect_empty(r));
     r=xbg_draw_rect(0,0,100,100,10,10,0,10,true,100,100);CHECK(r.x1==100 && r.y1==100);   // degenerate viewport: scissor only
     CHECK(xbg_rect_inside(XbgRect{10,10,20,20},XbgRect{0,0,32,32}) && !xbg_rect_inside(XbgRect{10,10,40,20},XbgRect{0,0,32,32}) && xbg_rect_inside(XbgRect{0,0,0,0},XbgRect{5,5,6,6}));
     XbgRect u=xbg_rect_union(XbgRect{0,0,0,0},XbgRect{5,6,7,8});CHECK(u.x0==5 && u.y0==6 && u.x1==7 && u.y1==8);
     u=xbg_rect_union(XbgRect{5,6,7,8},XbgRect{1,9,3,12});CHECK(u.x0==1 && u.y0==6 && u.x1==7 && u.y1==12);
     XbgRect a=xbg_area_align(XbgRect{1023,2047,1538,2562},32,32,4096,4096,0.9f);CHECK(a.x0==992 && a.y0==2016 && a.x1==1568 && a.y1==2592);
     a=xbg_area_align(XbgRect{0,0,1590,700},32,32,1600,720,0.9f);CHECK(a.x0==0 && a.y0==0 && a.x1==1600 && a.y1==720);   // >= 90 %: whole framebuffer
     a=xbg_area_align(XbgRect{0,0,0,0},16,16,64,64,0.9f);CHECK(a.x1==64 && a.y1==64);
     a=xbg_area_align(XbgRect{3,3,5,5},1,1,64,64,0.9f);CHECK(a.x0==3 && a.y0==3 && a.x1==5 && a.y1==5);}

    // [afbc] Lean usage / store ops: off on Adreno, on for Mali and PowerVR.
    CHECK(!xbg_lean_usage_default(XBG_GPU_ADRENO) && !xbg_store_none_default(XBG_GPU_ADRENO) && !xbg_store_dead_default(XBG_GPU_ADRENO));
    CHECK(xbg_lean_usage_default(XBG_GPU_MALI) && xbg_store_none_default(XBG_GPU_MALI) && xbg_store_dead_default(XBG_GPU_MALI));
    CHECK(xbg_lean_usage_default(XBG_GPU_POWERVR) && xbg_store_none_default(XBG_GPU_POWERVR) && !xbg_lean_usage_default(XBG_GPU_XCLIPSE));
    {const VkImageUsageFlags rt=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
     // A plain 2D render target: nothing blocks; with STORAGE only storage; mutable with and without a list; 3D, MSAA, BC, 64-bit.
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_2D,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,rt,0,false,32,false)==0);
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_2D,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,rt|VK_IMAGE_USAGE_STORAGE_BIT,0,false,32,false)==XBG_AFBC_STORAGE);
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_2D,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,rt,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,false,32,false)==XBG_AFBC_MUTABLE);
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_2D,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,rt,VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,true,32,false)==XBG_AFBC_LISTED);
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_3D,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,rt,0,false,32,false)==XBG_AFBC_NOT2D);
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_2D,VK_SAMPLE_COUNT_4_BIT,VK_IMAGE_TILING_OPTIMAL,rt,0,false,32,false)==XBG_AFBC_MSAA);
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_2D,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,rt,0,false,128,true)==XBG_AFBC_BLOCK);
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_2D,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,rt,0,false,64,false)==XBG_AFBC_WIDE);
     CHECK(xbg_afbc_blockers(VK_IMAGE_TYPE_2D,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_LINEAR,rt,0,false,32,false)==XBG_AFBC_LINEAR);
     // STORAGE is dropped only when nothing certain blocks AFBC anyway; wide / mutable do not keep it; no STORAGE -> unchanged.
     const VkImageUsageFlags st=rt|VK_IMAGE_USAGE_STORAGE_BIT;
     CHECK(xbg_lean_usage(st,XBG_AFBC_STORAGE)==rt);
     CHECK(xbg_lean_usage(st,XBG_AFBC_STORAGE|XBG_AFBC_WIDE|XBG_AFBC_LISTED)==rt);
     CHECK(xbg_lean_usage(st,XBG_AFBC_STORAGE|XBG_AFBC_NOT2D)==st);
     CHECK(xbg_lean_usage(st,XBG_AFBC_STORAGE|XBG_AFBC_MSAA)==st);
     CHECK(xbg_lean_usage(rt,0)==rt);
     CHECK(!strcmp(xbg_afbc_reason_name(0),"storage") && !strcmp(xbg_afbc_reason_name(XBG_AFBC_REASONS-1),"alias"));}
    {VkAttachmentDescription d[3]{};for(auto &x:d){x.storeOp=x.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;}
     // colour 0, colour 1, depth-stencil 2: none on colour 1 and the depth + stencil of 2, dead on the depth of 2 (wins)
     CHECK(xbg_store_twin(d,3,(1u<<2)|(1u<<4)|(2u<<4),1u<<4,VK_ATTACHMENT_STORE_OP_NONE,true));
     CHECK(d[0].storeOp==VK_ATTACHMENT_STORE_OP_STORE && d[0].stencilStoreOp==VK_ATTACHMENT_STORE_OP_STORE);
     CHECK(d[1].storeOp==VK_ATTACHMENT_STORE_OP_NONE && d[1].stencilStoreOp==VK_ATTACHMENT_STORE_OP_STORE);
     CHECK(d[2].storeOp==VK_ATTACHMENT_STORE_OP_DONT_CARE && d[2].stencilStoreOp==VK_ATTACHMENT_STORE_OP_NONE);
     VkAttachmentDescription e[1]{};e[0].storeOp=e[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
     CHECK(!xbg_store_twin(e,1,0,0,VK_ATTACHMENT_STORE_OP_NONE,false) && e[0].storeOp==VK_ATTACHMENT_STORE_OP_STORE);
     CHECK(xbg_store_twin(e,1,2u,0,VK_ATTACHMENT_STORE_OP_NONE,false)==false);}   // a stencil bit on a colour attachment changes nothing

    // [pvr] DM_GFX_FENCE_GUARD / DM_GFX_CRUMBS family defaults (BUGS_AND_FIXES 2.38): PowerVR only; Adreno and Mali keep 0.
    CHECK(xbg_fence_guard_default(XBG_GPU_POWERVR)==2);
    for(XbgGpuFamily f:{XBG_GPU_ADRENO,XBG_GPU_MALI,XBG_GPU_XCLIPSE,XBG_GPU_MALEOON,XBG_GPU_OTHER}){CHECK(xbg_fence_guard_default(f)==0);CHECK(xbg_crumbs_default(f)==0);}
    CHECK(xbg_crumbs_default(XBG_GPU_POWERVR)==1);
    // the simulated PowerVR family (DM_GFX_SIM_GPU=powervr on Adreno) takes the PowerVR defaults
    CHECK(xbg_fence_guard_default(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("powervr")))==2);
    CHECK(xbg_fence_guard_default(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("mali")))==0);
    // level switches: the environment wins, clamped; unset / empty = the family default
    CHECK(xbg_env_level(nullptr,2,0,2)==2 && xbg_env_level("",1,0,2)==1);
    CHECK(xbg_env_level("0",2,0,2)==0 && xbg_env_level("1",2,0,2)==1 && xbg_env_level("7",0,0,2)==2 && xbg_env_level("-3",2,0,2)==0);
    // Early VK_TIMEOUT: the 07.11 log (1 ms of a 10 s wait) is early; a whole timeout, a success, short / zero timeouts (polls) are not.
    CHECK(xbg_fence_wait_early(VK_TIMEOUT,1000000ull,10000000000ull));
    CHECK(!xbg_fence_wait_early(VK_TIMEOUT,10000000000ull,10000000000ull));
    CHECK(!xbg_fence_wait_early(VK_TIMEOUT,6000000000ull,10000000000ull));
    CHECK(!xbg_fence_wait_early(VK_SUCCESS,1000000ull,10000000000ull));
    CHECK(!xbg_fence_wait_early(VK_ERROR_DEVICE_LOST,1000000ull,10000000000ull));
    CHECK(!xbg_fence_wait_early(VK_TIMEOUT,0,0) && !xbg_fence_wait_early(VK_TIMEOUT,1000ull,50000000ull));
    CHECK(xbg_fence_wait_early(VK_TIMEOUT,1000000ull,UINT64_MAX));   // an endless wait that returns at all returned early
    // Back-off: 16 yields, then 10 us steps up to 200 us.
    CHECK(xbg_fence_pause_us(1)==0 && xbg_fence_pause_us(16)==0 && xbg_fence_pause_us(17)==10 && xbg_fence_pause_us(26)==100 && xbg_fence_pause_us(36)==200 && xbg_fence_pause_us(1000000)==200);
    // Polled waits on a simulated clock (1 us per clock read, pauses advance it by their length).
    {uint64_t clock=0;auto now=[&]{return clock++*1000ull;};auto pause=[&](uint64_t n){clock+=xbg_fence_pause_us(n);};uint64_t checks=0;
     int calls=0;auto after5=[&]{return ++calls>=5?VK_SUCCESS:VK_NOT_READY;};
     CHECK(xbg_fence_poll(after5,now,pause,10000000000ull,&checks)==VK_SUCCESS && checks==5 && calls==5);
     auto never=[]{return VK_NOT_READY;};clock=0;
     CHECK(xbg_fence_poll(never,now,pause,5000000ull,&checks)==VK_TIMEOUT && clock*1000ull>=5000000ull);   // gives up once 5 ms passed
     calls=0;auto once=[&]{++calls;return VK_NOT_READY;};
     CHECK(xbg_fence_poll(once,now,pause,0,&checks)==VK_TIMEOUT && calls==1 && checks==1);                 // timeout 0: one check
     calls=0;auto lost=[&]{++calls;return VK_ERROR_DEVICE_LOST;};
     CHECK(xbg_fence_poll(lost,now,pause,10000000000ull,&checks)==VK_ERROR_DEVICE_LOST && calls==1);      // an error at once
     // The 07.11 sequence: the driver's wait gave up early, the fence signalled 23 ms later; the recovery poll finds it.
     clock=0;const uint64_t signal_at=23000;auto later=[&]{return clock>=signal_at?VK_SUCCESS:VK_NOT_READY;};
     CHECK(xbg_fence_wait_early(VK_TIMEOUT,1000000ull,10000000000ull) && xbg_fence_poll(later,now,pause,10000000000ull,&checks)==VK_SUCCESS && clock<signal_at+400);}

    if(failures){printf("%d failure(s)\n",failures);return 1;}
    printf("vendor_policy_test: all checks passed\n");return 0;
}
