// [DOOM][nonadreno] GPU family detection, per-family defaults and the DM_GFX_SIM_GPU capability simulation
// (docs/research/mali_powervr_impl.md). Pure functions over vendor IDs, names and Vulkan structs, no device calls, so
// tests/vendor_policy_test.cpp calls the production code directly.
//
// Rules:
// - Every non-Adreno path in the engine is chosen from a real capability (feature bit, format feature, limit) where Vulkan
//   offers one; the family only picks defaults for driver behaviour no query reveals (compiler thread safety, dma-buf
//   coherency, colour writes of output-less fragment shaders). On vendor 0x5143 every default below equals the code
//   before this file, so Adreno runs bit-identical unless a DM_GFX_* switch says otherwise.
// - DM_GFX_SIM_GPU narrows what the engine believes the device can do (features, subgroup, format features, a few limits)
//   to what a Mali / PowerVR / Xclipse stock driver reports (vulkan.gpuinfo.org reports 44439 Mali-G720 = Poco X7 Pro,
//   52993 / 52802 PowerVR BXM-8-256, 52657 DXT-48-1536). The device itself keeps every feature it was created with, so
//   using less of it is always valid: this is how the Mali / PowerVR paths are exercised on our Adreno phones.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

enum XbgGpuFamily { XBG_GPU_OTHER=0, XBG_GPU_ADRENO=1, XBG_GPU_MALI=2, XBG_GPU_POWERVR=3, XBG_GPU_XCLIPSE=4, XBG_GPU_MALEOON=5 };

inline const char *xbg_gpu_family_name(XbgGpuFamily f){
    switch(f){case XBG_GPU_ADRENO:return "adreno";case XBG_GPU_MALI:return "mali";case XBG_GPU_POWERVR:return "powervr";
    case XBG_GPU_XCLIPSE:return "xclipse";case XBG_GPU_MALEOON:return "maleoon";default:return "other";}
}

// PCI-style vendor IDs: 0x5143 Qualcomm (also Turnip), 0x13B5 ARM, 0x1010 Imagination, 0x144D Samsung (stock Xclipse),
// 0x19E5 Huawei (Maleoon). RADV on Xclipse reports AMD 0x1002, so the name decides there.
inline XbgGpuFamily xbg_gpu_family(uint32_t vendor_id,const char *device_name){
    switch(vendor_id){
    case 0x5143:return XBG_GPU_ADRENO;
    case 0x13B5:return XBG_GPU_MALI;
    case 0x1010:return XBG_GPU_POWERVR;
    case 0x144D:return XBG_GPU_XCLIPSE;
    case 0x19E5:return XBG_GPU_MALEOON;
    default:break;
    }
    if(device_name){
        if(strstr(device_name,"Xclipse"))return XBG_GPU_XCLIPSE;
        if(strstr(device_name,"Mali"))return XBG_GPU_MALI;
        if(strstr(device_name,"PowerVR") || strstr(device_name,"IMG "))return XBG_GPU_POWERVR;
        if(strstr(device_name,"Maleoon"))return XBG_GPU_MALEOON;
        if(strstr(device_name,"Adreno"))return XBG_GPU_ADRENO;
    }
    return XBG_GPU_OTHER;
}

// DM_GFX_SIM_GPU bits: comma / space separated tokens, or one of the profiles mali / powervr / xclipse (combinable,
// e.g. "mali,nogs"). Unknown tokens are reported through *unknown (first one) and ignored.
enum : uint32_t {
    XBG_SIM_NOBC          =1u<<0,   // no BC1-7 format features, textureCompressionBC=0 (Mali, PowerVR BXM, Maleoon)
    XBG_SIM_NOBC47        =1u<<1,   // no BC4-7 format features (stock Xclipse)
    XBG_SIM_NOFILTER16    =1u<<2,   // R16G16_UNORM / R16G16B16A16_UNORM without linear filtering (Mali r32-r54)
    XBG_SIM_DEPTHNOLINEAR =1u<<3,   // only D16 filters linearly; X8_D24 / D24S8 / D32 / D32S8 / R32_SFLOAT do not (PowerVR 25.x / DXT)
    XBG_SIM_NOD24         =1u<<4,   // no X8_D24 / D24S8 at all (stock Xclipse, AMD-based)
    XBG_SIM_PVRFORMATS    =1u<<5,   // R8G8B8A8_SRGB without storage, E5B9G9R9 sampled only (PowerVR)
    XBG_SIM_SUBGROUP16    =1u<<6,   // subgroup 16 in fragment + compute only, no quad ops in all stages (Mali Valhall / 5th gen)
    XBG_SIM_SUBGROUP1     =1u<<7,   // subgroup size 1 (PowerVR BXM-4-64 DDK 24.2)
    XBG_SIM_NOGS          =1u<<8,   // geometryShader=0 (PanVK, Maleoon, old PowerVR; Arm advises against GS anyway)
    XBG_SIM_NODEPTHBOUNDS =1u<<9,   // depthBounds=0 (every Mali / PowerVR report)
    XBG_SIM_NOFILLMODE    =1u<<10,  // fillModeNonSolid=0 (Mali, PowerVR BXM)
    XBG_SIM_NOCLIP        =1u<<11,  // shaderClipDistance / shaderCullDistance=0 (Mali)
    XBG_SIM_NOGATHER      =1u<<12,  // shaderImageGatherExtended=0 (PowerVR BXM driver 1.15)
    XBG_SIM_NOANISO       =1u<<13,  // samplerAnisotropy=0 (PowerVR BXM driver 1.15)
    XBG_SIM_TEXEL64K      =1u<<14,  // maxTexelBufferElements=65536 (PowerVR)
    XBG_SIM_NOVERTEX3     =1u<<15,  // no VERTEX_BUFFER on 3-component 8/16-bit formats (AMD-style hardware: RADV / Xclipse)
    XBG_SIM_FAMILY_MALI   =1u<<24,  // the per-family defaults below act as on that family
    XBG_SIM_FAMILY_POWERVR=1u<<25,
    XBG_SIM_FAMILY_XCLIPSE=1u<<26,
};
constexpr uint32_t XBG_SIM_PROFILE_MALI=XBG_SIM_NOBC|XBG_SIM_NOFILTER16|XBG_SIM_SUBGROUP16|XBG_SIM_NODEPTHBOUNDS|XBG_SIM_NOFILLMODE|XBG_SIM_NOCLIP|XBG_SIM_FAMILY_MALI;
// [pvr] + NOANISO (BUGS_AND_FIXES 2.47): the only PowerVR a player ran (moto g64, BXM-8-256 1.15) has samplerAnisotropy=0 and the game
// asks aniso 4 for some samplers anyway (a fatal in v0.4.19), so the profile models it.
constexpr uint32_t XBG_SIM_PROFILE_POWERVR=XBG_SIM_NOBC|XBG_SIM_DEPTHNOLINEAR|XBG_SIM_PVRFORMATS|XBG_SIM_SUBGROUP1|XBG_SIM_NODEPTHBOUNDS|XBG_SIM_NOFILLMODE|XBG_SIM_TEXEL64K|XBG_SIM_NOANISO|XBG_SIM_FAMILY_POWERVR;
constexpr uint32_t XBG_SIM_PROFILE_XCLIPSE=XBG_SIM_NOBC47|XBG_SIM_NOD24|XBG_SIM_NOVERTEX3|XBG_SIM_FAMILY_XCLIPSE;

inline uint32_t xbg_sim_parse(const char *text,char *unknown=nullptr,size_t unknown_bytes=0){
    if(unknown && unknown_bytes)*unknown=0;
    if(!text)return 0;
    static const struct {const char *name;uint32_t bits;} names[]={
        {"mali",XBG_SIM_PROFILE_MALI},{"powervr",XBG_SIM_PROFILE_POWERVR},{"pvr",XBG_SIM_PROFILE_POWERVR},{"xclipse",XBG_SIM_PROFILE_XCLIPSE},
        {"nobc",XBG_SIM_NOBC},{"nobc47",XBG_SIM_NOBC47},{"nofilter16",XBG_SIM_NOFILTER16},{"depthnolinear",XBG_SIM_DEPTHNOLINEAR},
        {"nod24",XBG_SIM_NOD24},{"pvrformats",XBG_SIM_PVRFORMATS},{"subgroup16",XBG_SIM_SUBGROUP16},{"subgroup1",XBG_SIM_SUBGROUP1},
        {"nogs",XBG_SIM_NOGS},{"nodepthbounds",XBG_SIM_NODEPTHBOUNDS},{"nofillmode",XBG_SIM_NOFILLMODE},{"noclip",XBG_SIM_NOCLIP},
        {"nogather",XBG_SIM_NOGATHER},{"noaniso",XBG_SIM_NOANISO},{"texel64k",XBG_SIM_TEXEL64K},{"novertex3",XBG_SIM_NOVERTEX3},
        {"family-mali",XBG_SIM_FAMILY_MALI},{"family-powervr",XBG_SIM_FAMILY_POWERVR},{"family-xclipse",XBG_SIM_FAMILY_XCLIPSE}};
    uint32_t bits=0;const char *p=text;
    while(*p){
        while(*p==',' || *p==' ' || *p==';' || *p=='+' || *p=='"' || *p=='\'')++p;
        if(!*p)break;
        const char *e=p;while(*e && *e!=',' && *e!=' ' && *e!=';' && *e!='+' && *e!='"' && *e!='\'')++e;
        size_t n=size_t(e-p);bool found=false;
        if(n==1 && *p=='0'){p=e;continue;}   // "0" = off
        for(const auto &k:names)if(strlen(k.name)==n && !strncmp(k.name,p,n)){bits|=k.bits;found=true;break;}
        if(!found && unknown && unknown_bytes && !*unknown){size_t c=n<unknown_bytes-1?n:unknown_bytes-1;memcpy(unknown,p,c);unknown[c]=0;}
        p=e;
    }
    return bits;
}

// The family the per-family defaults use: a simulated family wins over the real one.
inline XbgGpuFamily xbg_effective_family(XbgGpuFamily real,uint32_t sim){
    if(sim&XBG_SIM_FAMILY_MALI)return XBG_GPU_MALI;
    if(sim&XBG_SIM_FAMILY_POWERVR)return XBG_GPU_POWERVR;
    if(sim&XBG_SIM_FAMILY_XCLIPSE)return XBG_GPU_XCLIPSE;
    return real;
}

// Switch precedence of these defaults: an environment value "0" / "1" wins, else the family default.
inline bool xbg_env_or(const char *environment,bool fallback){
    if(environment && *environment)return atoi(environment)!=0;
    return fallback;
}

// A level switch ("0" .. "hi" from the environment, clamped to lo..hi) or the family default.
inline int xbg_env_level(const char *environment,int fallback,int lo,int hi){
    int v=environment && *environment?atoi(environment):fallback;
    return v<lo?lo:v>hi?hi:v;
}

// [pvr] DM_GFX_FENCE_GUARD (BUGS_AND_FIXES 2.38). The PowerVR B-Series 1.15 driver (moto g64, BXM-8-256, 07.11) returned VK_TIMEOUT
// from the pool-reset worker's vkWaitForFences 1 ms after its own sync_wait failed with EINVAL ("PVRSRVFenceWaitI ... 22 Invalid
// argument": the fence's sync file descriptor was not valid any more), while the translation thread's wait on the same fence then
// succeeded: a fence shared by threads, not a GPU hang. Our side touched slot fences from three threads, one of them in a Vulkan race
// (vkGetFenceStatus of a fence the record thread was passing to vkQueueSubmit, whose fence is externally synchronized). Level 1: no
// slot fence access before its SUBMIT ran; 2: also one thread at a time, waits poll vkGetFenceStatus. 2 on PowerVR, 0 elsewhere.
inline int xbg_fence_guard_default(XbgGpuFamily f){return f==XBG_GPU_POWERVR?2:0;}
// DM_GFX_CRUMBS family default: GPU progress markers on PowerVR (1), where no run has worked yet, so the next hang log names the pass /
// dispatch the GPU was in. Costs a barrier + fill per pass end; DM_GFX_CRUMBS=0 turns it off. Off elsewhere.
inline int xbg_crumbs_default(XbgGpuFamily f){return f==XBG_GPU_POWERVR?1:0;}
// A fence wait that came back VK_TIMEOUT well before its timeout: the driver gave up (a fence error), the GPU did not run the whole time.
// Only for timeouts of 100 ms and more (a short or zero timeout is a poll).
inline bool xbg_fence_wait_early(VkResult r,uint64_t waited_ns,uint64_t timeout_ns){
    return r==VK_TIMEOUT && timeout_ns>=100000000ull && waited_ns<timeout_ns/2;
}
// Back-off of a polled fence wait: n-th status check -> microseconds to sleep before the next (0 = just yield). 16 yields, then 10 us
// more per check up to 200 us: a polled wait ends at most ~0.2 ms after the fence signals.
inline unsigned xbg_fence_pause_us(uint64_t n){
    if(n<=16)return 0;
    const uint64_t us=(n-16)*10;
    return us>200?200u:unsigned(us);
}
// A fence wait made of status checks: status() -> VK_SUCCESS / VK_NOT_READY / an error, now() -> ns, pause(n) between checks.
// Returns VK_SUCCESS, the error, or VK_TIMEOUT once timeout_ns has passed (timeout 0: one check, as vkWaitForFences with 0).
template<class Status,class Now,class Pause>
inline VkResult xbg_fence_poll(Status status,Now now,Pause pause,uint64_t timeout_ns,uint64_t *checks){
    const uint64_t t0=now();uint64_t n=0;
    for(;;){
        const VkResult r=status();++n;
        if(r!=VK_NOT_READY){if(checks)*checks=n;return r;}
        if(now()-t0>=timeout_ns){if(checks)*checks=n;return VK_TIMEOUT;}
        pause(n);
    }
}

// [pvr] DM_GFX_ANISO_FALLBACK (BUGS_AND_FIXES 2.47): the anisotropy a sampler may use on this device. NVN takes any value up to 16
// (Maxwell filters anisotropically); Vulkan needs samplerAnisotropy enabled and maxAnisotropy <= maxSamplerAnisotropy. Without the
// feature the sampler is trilinear / bilinear (1 = off), above the limit it is clamped. A capability rule (no family): it only acts
// where the old code stopped the game ("NVN sampler anisotropy unsupported/disabled/out of range result=-5").
inline float xbg_aniso_allowed(float wanted,bool feature,float max_aniso){
    if(!(wanted>1))return wanted;   // 1 (off), or garbage the converter rejects as before
    if(!feature)return 1;
    if(max_aniso>=1 && wanted>max_aniso)return max_aniso;
    return wanted;
}

// Zero-copy guest memory (AHardwareBuffer BLOB pages imported as VkBuffer memory, CPU-cached pages read by the GPU through IO
// coherency) was proven only on Adreno (work/zc_test). Elsewhere the copy path is the default: a GPU that does not snoop the CPU
// caches would read stale vertices / uniforms (Arm documents ACE-Lite only for some SoCs). DM_GFX_ZEROCOPY=1 forces it on.
inline bool xbg_zero_copy_default(XbgGpuFamily f){return f==XBG_GPU_ADRENO;}
// The PowerVR shader compiler is not thread-safe (forums.imgtec.com t/4334, BXM-8-256 1.15) and builds slowly (t/4280):
// one vkCreate*Pipelines at a time (warm-up threads included). DM_GFX_PIPE_SERIAL.
inline bool xbg_pipe_serial_default(XbgGpuFamily f){return f==XBG_GPU_POWERVR;}
// [pvr] DM_GFX_COMPILE_LOCK (BUGS_AND_FIXES 2.47). The same compiler also runs outside vkCreate*Pipelines on this driver: the moto g64
// (BXM-8-256, 1.15@6133110) spends 2-56 ms in vkCreateShaderModule (the SPIR-V front end), and v0.4.19 died with SIGBUS inside
// PVRUniFlexCompileToHw called from a vkCmd* on the dm-record thread (driver-made transfer / render-pass programs) while the
// translation thread was creating modules and pipelines. Level 1: the pipeline lock also covers vkCreateShaderModule, the record
// thread's render-pass / copy / blit / clear / fill packets (and their direct calls) and the present thread's blit and clear;
// 2: every record-thread packet but submit / present (diagnostic). 1 on PowerVR, 0 elsewhere.
inline int xbg_compile_lock_default(XbgGpuFamily f){return f==XBG_GPU_POWERVR?1:0;}
// Graphics-pipeline crash guard (in-flight markers, skip after two crashed boots). Off on Adreno, whose compiler crashes are
// known and handled by the compute guard; on for the rest, where we have never run. DM_GFX_PIPE_GUARD.
inline bool xbg_pipe_guard_default(XbgGpuFamily f){return f!=XBG_GPU_ADRENO;}
// PowerVR writes the address of every dynamic uniform buffer of a set into a table sized for the ones the stage uses: a page
// fault in vkCmdDrawIndexed on BXM-8-256 (PPSSPP PVR_UNUSED_DYNAMIC_UBO_OVERRUN). No dynamic uniform buffers there. DM_GFX_UBO_DYNAMIC.
inline bool xbg_ubo_dynamic_default(XbgGpuFamily f){return f!=XBG_GPU_POWERVR;}
// A fragment shader that declares no colour output (NVN's depth-only draw) leaves the colour attachments undefined in Vulkan;
// Adreno keeps them, Mali writes 0 (d2r-mobile issue #179, PR #229). Colour writes are masked off for such draws everywhere
// but Adreno (whose pipelines stay as they were). DM_GFX_NOOUT_MASK.
inline bool xbg_noout_mask_default(XbgGpuFamily f){return f!=XBG_GPU_ADRENO;}

// [nonadreno][tile] Tile-based GPUs (Mali, PowerVR) read every LOAD attachment into the tile memory and write every STORE one back
// at each render pass instance, over the render area (docs/research/tile_gpu_impl.md; Arm GPU Best Practices 101897 §7.3, PowerVR
// "Using Render Passes and Sub-passes"). On Adreno with TU_DEBUG=sysmem none of this costs anything, so Adreno keeps the old
// behaviour; every default below is "off" on vendor 0x5143.
inline bool xbg_tile_family(XbgGpuFamily f){return f==XBG_GPU_MALI || f==XBG_GPU_POWERVR;}
// DM_GFX_NOLOAD: a pass whose first draw provably overwrites every colour pixel (first_draw_covers) begins with colour loadOp
// DONT_CARE instead of LOAD (DOOM's ~40 full-screen post-process passes a frame).
// Known issue (06.11, DM_GFX_TILE_POISON=1 under DM_GFX_SIM_GPU=mali): a rare magenta flash, i.e. first_draw_covers now and then accepted
// a pass whose old contents are still read. Kept on for tilers (the owner: rare, and it saves ~50 ms a frame on a Mali-G57 MC2). Later on
// 06.11 first_draw_covers also refuses rasterizer discard, a viewport draw() skips after the begin, zero instances and a first draw that
// samples its own colour attachment (the likely causes; to be confirmed with the poison run).
inline bool xbg_noload_default(XbgGpuFamily f){return xbg_tile_family(f);}
// DM_GFX_TILE_AREA: renderArea = the bounding box of the draws the pass will hold (look-ahead over the guest command stream,
// enforced per draw), so a tiler loads / stores only those tiles (4096x4096 shadow atlas updates). Mali only: Mesa's PowerVR
// driver turns a renderArea that is not the whole image into an extra depth/stencil load (pvr_is_render_area_tile_aligned).
inline bool xbg_tile_area_default(XbgGpuFamily f){return f==XBG_GPU_MALI;}
// DM_GFX_TILE_BARRIER: dependencies between passes wait in the fragment stages only (Arm 101897 §3.9: ALL_COMMANDS ->
// ALL_COMMANDS drains the pipeline; between render passes use ... -> FRAGMENT_SHADER), so the vertex / tiling work of the next
// pass overlaps the fragment work of the previous one. Image reads from pre-fragment stages keep the full barrier.
inline bool xbg_tile_barrier_default(XbgGpuFamily f){return xbg_tile_family(f);}
// DM_GFX_CHUNK_MID_SPLIT: a mid-submit chunk cut every N draws may end an open render pass (a store + reload of every attachment
// on a tiler); off, the every-N-draws cut waits until no pass is open (cuts at pass ends are unchanged).
inline bool xbg_chunk_mid_split_default(XbgGpuFamily f){return !xbg_tile_family(f);}

// [nonadreno][afbc] AFBC-friendly images and store ops (docs/research/tile_gpu_impl.md section 7). Every default is "off" on 0x5143.
// DM_GFX_LEAN_USAGE: an NVN texture with NVN_TEXTURE_FLAGS_IMAGE is created without VK_IMAGE_USAGE_STORAGE_BIT when nothing else
// already rules AFBC out; the first storage bind recreates it with the bit and copies the contents (upgrade_mutable). Arm's AFBC
// sample (Vulkan-Samples performance/afbc): no AFBC with STORAGE / TRANSIENT usage, MUTABLE_FORMAT / ALIAS, samples > 1, not 2D,
// not OPTIMAL tiling, or a format outside the list (Valhall: any format up to 32 bits per texel).
inline bool xbg_lean_usage_default(XbgGpuFamily f){return xbg_tile_family(f);}
// DM_GFX_STORE_NONE: an attachment aspect that no draw or clear of the render pass instance writes ends it with STORE_OP_NONE
// (VK_EXT/KHR_load_store_op_none): no write-back of a read-only depth / stencil (depth-tested transparents, fog, decals).
inline bool xbg_store_none_default(XbgGpuFamily f){return xbg_tile_family(f);}
// DM_GFX_STORE_DEAD: STORE_OP_DONT_CARE for an aspect whose contents the guest command stream provably overwrites (a full clear)
// before anything can read them (store_scan).
inline bool xbg_store_dead_default(XbgGpuFamily f){return xbg_tile_family(f);}

// Why the driver would not compress an image (Arm's documented rules; one bit per reason). The CERTAIN ones hold on every Mali
// generation; STORAGE / MUTABLE / WIDE were relaxed on some newer ones (VK_EXT_image_compression_control tells, NVN afbc summary).
enum : uint32_t {
    XBG_AFBC_STORAGE  =1u<<0,   // VK_IMAGE_USAGE_STORAGE_BIT
    XBG_AFBC_MUTABLE  =1u<<1,   // MUTABLE_FORMAT without a VkImageFormatListCreateInfo
    XBG_AFBC_LISTED   =1u<<2,   // MUTABLE_FORMAT with a format list (allowed by drivers that check the list, else as MUTABLE)
    XBG_AFBC_MSAA     =1u<<3,   // samples > 1
    XBG_AFBC_NOT2D    =1u<<4,   // 1D / 3D image
    XBG_AFBC_BLOCK    =1u<<5,   // block-compressed format (BC / ASTC / ETC2): already compressed
    XBG_AFBC_WIDE     =1u<<6,   // more than 32 bits per texel (Valhall <= 3rd gen documents 32 at most)
    XBG_AFBC_LINEAR   =1u<<7,   // VK_IMAGE_TILING_LINEAR
    XBG_AFBC_TRANSIENT=1u<<8,   // VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
    XBG_AFBC_ALIAS    =1u<<9,   // VK_IMAGE_CREATE_ALIAS_BIT
    XBG_AFBC_REASONS  =10
};
constexpr uint32_t XBG_AFBC_CERTAIN=XBG_AFBC_MSAA|XBG_AFBC_NOT2D|XBG_AFBC_BLOCK|XBG_AFBC_LINEAR|XBG_AFBC_TRANSIENT|XBG_AFBC_ALIAS;
inline const char *xbg_afbc_reason_name(unsigned bit){
    static const char *const n[XBG_AFBC_REASONS]={"storage","mutable","listed","msaa","not2d","block","wide","linear","transient","alias"};
    return bit<XBG_AFBC_REASONS?n[bit]:"?";}
// texel_bits: bits per texel of an uncompressed format (0 unknown); block: the format is block-compressed.
inline uint32_t xbg_afbc_blockers(VkImageType type,VkSampleCountFlagBits samples,VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags,
                                  bool format_list,uint32_t texel_bits,bool block){
    uint32_t r=0;
    if(usage&VK_IMAGE_USAGE_STORAGE_BIT)r|=XBG_AFBC_STORAGE;
    if(flags&VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)r|=format_list?XBG_AFBC_LISTED:XBG_AFBC_MUTABLE;
    if(samples!=VK_SAMPLE_COUNT_1_BIT)r|=XBG_AFBC_MSAA;
    if(type!=VK_IMAGE_TYPE_2D)r|=XBG_AFBC_NOT2D;
    if(block)r|=XBG_AFBC_BLOCK;
    else if(texel_bits>32)r|=XBG_AFBC_WIDE;
    if(tiling!=VK_IMAGE_TILING_OPTIMAL)r|=XBG_AFBC_LINEAR;
    if(usage&VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)r|=XBG_AFBC_TRANSIENT;
    if(flags&VK_IMAGE_CREATE_ALIAS_BIT)r|=XBG_AFBC_ALIAS;
    return r;
}
// DM_GFX_LEAN_USAGE decision: the usage to create with. STORAGE goes only when it is the one thing between the image and AFBC on
// some generation (no CERTAIN blocker; WIDE / MUTABLE are not certain on newer GPUs, so they do not keep it). Anything else as wanted.
inline VkImageUsageFlags xbg_lean_usage(VkImageUsageFlags wanted,uint32_t blockers){
    if(!(wanted&VK_IMAGE_USAGE_STORAGE_BIT) || (blockers&XBG_AFBC_CERTAIN))return wanted;
    return wanted&~VkImageUsageFlags(VK_IMAGE_USAGE_STORAGE_BIT);
}
// Store-op twins (DM_GFX_STORE_NONE / DM_GFX_STORE_DEAD): attachment k's bit 2k is its colour or depth aspect, bit 2k+1 its stencil.
// none: aspects no draw / clear of the instance writes -> none_op (STORE_OP_NONE; DONT_CARE only with DM_GFX_STORE_NONE=2);
// dead: aspects whose contents are provably overwritten before any read -> DONT_CARE (wins over none). Only store ops change, so
// the twin is compatible with the base pass, its framebuffers and pipelines. Returns whether anything changed.
inline bool xbg_store_twin(VkAttachmentDescription *d,uint32_t n,uint32_t none,uint32_t dead,VkAttachmentStoreOp none_op,bool depth_last){
    bool any=false;
    for(uint32_t k=0;k<n && k<16;k++){
        const bool ds=depth_last && k+1==n;
        const uint32_t a=1u<<(2*k),s=2u<<(2*k);
        if(dead&a){d[k].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;any=true;}
        else if(none&a){d[k].storeOp=none_op;any=true;}
        if(ds){
            if(dead&s){d[k].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;any=true;}
            else if(none&s){d[k].stencilStoreOp=none_op;any=true;}
        }
    }
    return any;
}

// [tile] Render-area geometry (DM_GFX_TILE_AREA), pure so tests/vendor_policy_test.cpp checks it. A rectangle as x0,y0 (inclusive)
// .. x1,y1 (exclusive) in framebuffer pixels.
struct XbgRect{int32_t x0,y0,x1,y1;};
inline bool xbg_rect_empty(const XbgRect &r){return r.x1<=r.x0 || r.y1<=r.y0;}
inline XbgRect xbg_rect_union(const XbgRect &a,const XbgRect &b){
    if(xbg_rect_empty(a))return b;
    if(xbg_rect_empty(b))return a;
    return {a.x0<b.x0?a.x0:b.x0,a.y0<b.y0?a.y0:b.y0,a.x1>b.x1?a.x1:b.x1,a.y1>b.y1?a.y1:b.y1};}
inline XbgRect xbg_rect_clamp(XbgRect r,uint32_t fw,uint32_t fh){
    r.x0=r.x0<0?0:r.x0;r.y0=r.y0<0?0:r.y0;r.x1=r.x1>int32_t(fw)?int32_t(fw):r.x1;r.y1=r.y1>int32_t(fh)?int32_t(fh):r.y1;return r;}
inline bool xbg_rect_inside(const XbgRect &in,const XbgRect &out){return xbg_rect_empty(in) || (in.x0>=out.x0 && in.y0>=out.y0 && in.x1<=out.x1 && in.y1<=out.y1);}
// The pixels a draw can touch: its scissor (exact in Vulkan), narrowed to its viewport for triangles (clipped to the view volume;
// one pixel of margin for rounding). Points and lines may leave the viewport, so they keep the scissor only. Clamped to the
// framebuffer. Viewport height may be negative (flipped).
inline XbgRect xbg_draw_rect(int32_t sx,int32_t sy,uint32_t sw,uint32_t sh,float vx,float vy,float vw,float vh,bool triangles,uint32_t fw,uint32_t fh){
    XbgRect r{sx,sy,int32_t(int64_t(sx)+sw),int32_t(int64_t(sy)+sh)};
    if(triangles && vw>0 && vw==vw && vh==vh){
        float y0=vh<0?vy+vh:vy,y1=vh<0?vy:vy+vh,x1=vx+vw;
        const float lim=float(1<<30);
        if(vx>-lim && x1<lim && y0>-lim && y1<lim){
            const int32_t px0=int32_t(vx)-1,px1=int32_t(x1)+2,py0=int32_t(y0)-1,py1=int32_t(y1)+2;   // truncation toward zero, then a pixel of margin
            r.x0=px0>r.x0?px0:r.x0;r.y0=py0>r.y0?py0:r.y0;r.x1=px1<r.x1?px1:r.x1;r.y1=py1<r.y1?py1:r.y1;}}
    r=xbg_rect_clamp(r,fw,fh);
    if(xbg_rect_empty(r))r={0,0,0,0};
    return r;
}
// Grow a render area to the driver's granularity (vkGetRenderAreaGranularity; PanVK 32x32, PowerVR the tile size) and fall back to
// the whole framebuffer when the saving is small (fraction of the framebuffer's pixels the area still covers >= full_fraction).
inline XbgRect xbg_area_align(XbgRect r,uint32_t gw,uint32_t gh,uint32_t fw,uint32_t fh,float full_fraction){
    const XbgRect full{0,0,int32_t(fw),int32_t(fh)};
    if(xbg_rect_empty(r))return full;
    if(gw>1){r.x0-=r.x0%int32_t(gw);r.x1=int32_t((int64_t(r.x1)+gw-1)/gw*gw);}
    if(gh>1){r.y0-=r.y0%int32_t(gh);r.y1=int32_t((int64_t(r.y1)+gh-1)/gh*gh);}
    r=xbg_rect_clamp(r,fw,fh);
    const double a=double(r.x1-r.x0)*double(r.y1-r.y0),f=double(fw)*double(fh);
    if(f<=0 || a>=f*double(full_fraction))return full;
    return r;
}

inline bool xbg_sim_format_in(VkFormat f,VkFormat lo,VkFormat hi){return int(f)>=int(lo) && int(f)<=int(hi);}
// Narrow the format features the driver reported to what the simulated device reports.
inline void xbg_sim_format_features(uint32_t sim,VkFormat f,VkFormatProperties *p){
    if(!sim || !p)return;
    const VkFormatFeatureFlags linear=VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT;
    if((sim&XBG_SIM_NOBC) && xbg_sim_format_in(f,VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK))*p=VkFormatProperties{};
    if((sim&XBG_SIM_NOBC47) && xbg_sim_format_in(f,VK_FORMAT_BC4_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK))*p=VkFormatProperties{};
    if((sim&XBG_SIM_NOFILTER16) && (f==VK_FORMAT_R16G16_UNORM || f==VK_FORMAT_R16G16B16A16_UNORM)){p->optimalTilingFeatures&=~linear;p->linearTilingFeatures&=~linear;}
    if((sim&XBG_SIM_DEPTHNOLINEAR) && (f==VK_FORMAT_X8_D24_UNORM_PACK32 || f==VK_FORMAT_D24_UNORM_S8_UINT || f==VK_FORMAT_D32_SFLOAT ||
        f==VK_FORMAT_D32_SFLOAT_S8_UINT || f==VK_FORMAT_R32_SFLOAT)){p->optimalTilingFeatures&=~linear;p->linearTilingFeatures&=~linear;}
    if((sim&XBG_SIM_NOD24) && (f==VK_FORMAT_X8_D24_UNORM_PACK32 || f==VK_FORMAT_D24_UNORM_S8_UINT))*p=VkFormatProperties{};
    if((sim&XBG_SIM_NOVERTEX3) && (xbg_sim_format_in(f,VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R8G8B8_SRGB) || xbg_sim_format_in(f,VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16_SFLOAT)))
        p->bufferFeatures&=~VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if(sim&XBG_SIM_PVRFORMATS){
        if(f==VK_FORMAT_R8G8B8A8_SRGB)p->optimalTilingFeatures&=~(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT);
        if(f==VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)p->optimalTilingFeatures&=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
}
inline void xbg_sim_features(uint32_t sim,VkPhysicalDeviceFeatures *f){
    if(!sim || !f)return;
    if(sim&(XBG_SIM_NOBC))f->textureCompressionBC=VK_FALSE;
    if(sim&XBG_SIM_NOGS)f->geometryShader=VK_FALSE;
    if(sim&XBG_SIM_NODEPTHBOUNDS)f->depthBounds=VK_FALSE;
    if(sim&XBG_SIM_NOFILLMODE)f->fillModeNonSolid=VK_FALSE;
    if(sim&XBG_SIM_NOCLIP){f->shaderClipDistance=VK_FALSE;f->shaderCullDistance=VK_FALSE;}
    if(sim&XBG_SIM_NOGATHER)f->shaderImageGatherExtended=VK_FALSE;
    if(sim&XBG_SIM_NOANISO)f->samplerAnisotropy=VK_FALSE;
}
inline void xbg_sim_subgroup(uint32_t sim,VkPhysicalDeviceSubgroupProperties *s){
    if(!sim || !s)return;
    if(sim&XBG_SIM_SUBGROUP16){s->subgroupSize=16;s->supportedStages&=VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT;s->quadOperationsInAllStages=VK_FALSE;}
    if(sim&XBG_SIM_SUBGROUP1)s->subgroupSize=1;
}
inline void xbg_sim_limits(uint32_t sim,VkPhysicalDeviceLimits *l){
    if(!sim || !l)return;
    if((sim&XBG_SIM_TEXEL64K) && l->maxTexelBufferElements>65536)l->maxTexelBufferElements=65536;
}

// The 4-component format a 3-component vertex format widens to (same channel type), or VK_FORMAT_UNDEFINED. Only used when the
// driver lacks VERTEX_BUFFER on the 3-component one (none of the Mali / PowerVR reports we checked; AMD-style hardware lacks the
// 8/16-bit ones, cf. yuzu's GetFormatAlternatives). The fourth channel then reads the next bytes of the vertex instead of 1.
inline VkFormat xbg_vertex_widen(VkFormat f){
    switch(f){
    case VK_FORMAT_R8G8B8_UNORM:return VK_FORMAT_R8G8B8A8_UNORM;case VK_FORMAT_R8G8B8_SNORM:return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT:return VK_FORMAT_R8G8B8A8_UINT;case VK_FORMAT_R8G8B8_SINT:return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_UNORM:return VK_FORMAT_R16G16B16A16_UNORM;case VK_FORMAT_R16G16B16_SNORM:return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT:return VK_FORMAT_R16G16B16A16_UINT;case VK_FORMAT_R16G16B16_SINT:return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R32G32B32_UINT:return VK_FORMAT_R32G32B32A32_UINT;case VK_FORMAT_R32G32B32_SINT:return VK_FORMAT_R32G32B32A32_SINT;
    case VK_FORMAT_R32G32B32_SFLOAT:return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:return VK_FORMAT_UNDEFINED;
    }
}

// Bytes per texel of the uncompressed formats texel buffers use (0 = unknown: no clamp).
inline uint32_t xbg_vk_texel_bytes(VkFormat f){
    switch(f){
    case VK_FORMAT_R8_UNORM:case VK_FORMAT_R8_SNORM:case VK_FORMAT_R8_UINT:case VK_FORMAT_R8_SINT:return 1;
    case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R8G8_SNORM:case VK_FORMAT_R8G8_UINT:case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_UNORM:case VK_FORMAT_R16_SNORM:case VK_FORMAT_R16_UINT:case VK_FORMAT_R16_SINT:case VK_FORMAT_R16_SFLOAT:return 2;
    case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SNORM:case VK_FORMAT_R8G8B8A8_UINT:case VK_FORMAT_R8G8B8A8_SINT:case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:case VK_FORMAT_A2B10G10R10_UINT_PACK32:case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_R16G16_UNORM:case VK_FORMAT_R16G16_SNORM:case VK_FORMAT_R16G16_UINT:case VK_FORMAT_R16G16_SINT:case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_UINT:case VK_FORMAT_R32_SINT:case VK_FORMAT_R32_SFLOAT:return 4;
    case VK_FORMAT_R16G16B16A16_UNORM:case VK_FORMAT_R16G16B16A16_SNORM:case VK_FORMAT_R16G16B16A16_UINT:case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_R32G32_UINT:case VK_FORMAT_R32G32_SINT:case VK_FORMAT_R32G32_SFLOAT:return 8;
    case VK_FORMAT_R32G32B32_UINT:case VK_FORMAT_R32G32B32_SINT:case VK_FORMAT_R32G32B32_SFLOAT:return 12;
    case VK_FORMAT_R32G32B32A32_UINT:case VK_FORMAT_R32G32B32A32_SINT:case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;
    default:return 0;
    }
}

// Bytes of a texel buffer view the device can address: at most maxTexelBufferElements texels (PowerVR: 65536). Returns the
// clamped range, rounded down to whole texels; bytes itself when it fits or the texel size is unknown.
inline uint64_t xbg_texel_range_clamp(uint64_t bytes,uint32_t texel_bytes,uint32_t max_elements){
    if(!texel_bytes || !max_elements)return bytes;
    const uint64_t cap=uint64_t(max_elements)*texel_bytes;
    return bytes>cap?cap:bytes;
}
