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
constexpr uint32_t XBG_SIM_PROFILE_POWERVR=XBG_SIM_NOBC|XBG_SIM_DEPTHNOLINEAR|XBG_SIM_PVRFORMATS|XBG_SIM_SUBGROUP1|XBG_SIM_NODEPTHBOUNDS|XBG_SIM_NOFILLMODE|XBG_SIM_TEXEL64K|XBG_SIM_FAMILY_POWERVR;
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

// Zero-copy guest memory (AHardwareBuffer BLOB pages imported as VkBuffer memory, CPU-cached pages read by the GPU through IO
// coherency) was proven only on Adreno (work/zc_test). Elsewhere the copy path is the default: a GPU that does not snoop the CPU
// caches would read stale vertices / uniforms (Arm documents ACE-Lite only for some SoCs). DM_GFX_ZEROCOPY=1 forces it on.
inline bool xbg_zero_copy_default(XbgGpuFamily f){return f==XBG_GPU_ADRENO;}
// The PowerVR shader compiler is not thread-safe (forums.imgtec.com t/4334, BXM-8-256 1.15) and builds slowly (t/4280):
// one vkCreate*Pipelines at a time (warm-up threads included). DM_GFX_PIPE_SERIAL.
inline bool xbg_pipe_serial_default(XbgGpuFamily f){return f==XBG_GPU_POWERVR;}
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
