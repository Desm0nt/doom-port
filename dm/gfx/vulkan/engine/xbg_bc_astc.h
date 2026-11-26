// [nonadreno] bc-astc-gpu (DM_GFX_BC_ASTC, port-common/modules/bc-astc-gpu): host-side rules and the self-test of the GPU ASTC encoder
// that stores GPU-decoded BC1 / BC2 / BC3 / BC7 textures as ASTC 4x4 instead of RGBA8 (8 bits a texel instead of 32: 4x less memory and
// sampling bandwidth than the decoded texture; BC1's own 4 bits become 8). No Vulkan calls: the engine
// (xbg_bc_astc.inc, namespace bcastc) runs the GPU side, engine/tests/bc_astc_encode_test.cpp runs all of this on the host with the
// encoder compiled as C++ and writes xbg_bc_astc_kat.h (the expected error of every self-test block).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "xbg_texture.h"
#include "xbg_bc_gpu.h"
#include "xbg_astc_ref_decode.h"
#include "xbg_bc_astc_kat.h"

// The NVN BC formats that may be stored as ASTC 4x4 and their family (a linear format and its sRGB partner: copies and views stay
// inside a family, xbg_bc_view_compatible), -1 for the others (BC4 / BC5 / BC6H keep the decode path's formats).
static inline int xbg_bc_astc_family(uint32_t nvn) {
    switch(nvn){case 0x41:case 0x45:return 0;case 0x42:case 0x46:return 1;case 0x43:case 0x47:return 2;case 0x44:case 0x48:return 3;case 0x4d:case 0x4e:return 4;default:return -1;}
}
static inline const char *xbg_bc_astc_family_name(int f) {static const char *const n[5]={"bc1","bc1a","bc2","bc3","bc7"};return f>=0 && f<5?n[f]:"-";}
static inline uint32_t xbg_bc_astc_linear(int family) {static const uint32_t l[5]={0x41,0x42,0x43,0x44,0x4d};return family>=0 && family<5?l[family]:0;}
static inline uint32_t xbg_bc_astc_srgb(int family) {static const uint32_t s[5]={0x45,0x46,0x47,0x48,0x4e};return family>=0 && family<5?s[family]:0;}
static inline bool xbg_bc_astc_is_srgb(uint32_t nvn) {return (nvn>=0x45 && nvn<=0x48) || nvn==0x4e;}
// The ASTC 4x4 format of an NVN BC format of these families (sRGB stays sRGB: the decoder's bytes are sRGB-encoded either way).
static inline VkFormat xbg_bc_astc_vk_format(uint32_t nvn) {
    return xbg_bc_astc_family(nvn)<0?VK_FORMAT_UNDEFINED:xbg_bc_astc_is_srgb(nvn)?VK_FORMAT_ASTC_4x4_SRGB_BLOCK:VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
}
// The encoder's direct mode for a format (xbg_bc_astc.glsl xbg_astc_direct): 1 BC1 colour block in words 0-1, 2 BC2 / BC3 colour block
// in words 2-3, 0 none (BC7).
static inline uint32_t xbg_bc_astc_direct(uint32_t nvn) {const int f=xbg_bc_astc_family(nvn);return f==0 || f==1?1u:f==2 || f==3?2u:0u;}
// Push-constant flags of one encode dispatch: direct | words per BC block << 8.
static inline uint32_t xbg_bc_astc_flags(uint32_t nvn) {return xbg_bc_astc_direct(nvn)|((xbg_bc_astc_family(nvn)<=1?2u:4u)<<8);}

// Per image: why a texture of an encoded family stays RGBA8 (NULL: it may be ASTC). NVN flags 0x4 = image (storage) use, 0x10 = linear
// (uploaded through the single-buffer path, CPU-decoded). The overlay's keep rules (xbg_astc_overlay.h xbg_astc_keep_bc, same text) are
// kept: not 2D (1D, 3D, 1D array), the virtual-texture cache shape (one level, 4096 x 4096 or more), BC3 arrays.
// vt: DM_GFX_BC_ASTC_VT=1 lifts the VT-cache-shape and BC3-array rules (those textures are written by the game at run time; with the
// GPU encoder that only costs encode time per partial upload).
static inline const char *xbg_bc_astc_shape(uint32_t nvn, int target, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, int samples,
                                            uint32_t nvn_flags, bool vt) {
    if(samples>1)return "multisample";
    if(nvn_flags&0x4)return "storage";
    if(nvn_flags&0x10)return "linear";
    if(target==0 || target==2 || target==3)return "not-2d";
    if(!vt && levels==1 && w>=4096 && h>=4096)return "vt-cache-shape";
    if(!vt && xbg_bc_astc_family(nvn)==3 && (target==4 || layers>1))return "bc3-array";
    return nullptr;
}

// ---------------------------------------------------------------- self-test blocks (identical on the device and the host)
// Categories: RGBA blocks given as texels (gradients, flat, alpha edges, GameNative's self-test block; no BC source, direct 0), then for
// each family random BC blocks (xbg_bc_kat_blocks; BC2 / BC3 half with one alpha value, so the direct path runs) decoded by the CPU
// decoder (the bytes the GPU decoder writes, bc_gpu_decode_test). Texels: 16 x RGBA8 per block, row-major.
struct XbgAstcKatSet {
    struct Cat { const char *name; uint32_t nvn, first, count; };
    std::vector<Cat> cats; std::vector<uint8_t> rgba; std::vector<uint32_t> bc;   // bc: 4 words per block (BC1: words 2-3 zero)
    uint32_t blocks() const {return uint32_t(rgba.size()/64);}
};
static inline void xbg_bc_astc_kat_rgba(std::vector<uint8_t> &o) {
    auto blk=[&](auto f){for(int i=0;i<16;i++){uint8_t p[4];f(i&3,i>>2,i,p);o.insert(o.end(),p,p+4);}};
    // GameNative's ASTC self-test block (varying alpha; wrapper_bcn_gpu.c, golden 0xffc10253 0xb57f7460 0xd9d04b38 0x01256a2b)
    blk([](int,int,int i,uint8_t *p){p[0]=uint8_t(i*17);p[1]=uint8_t(255-i*15);p[2]=uint8_t((i&3)*80);p[3]=uint8_t(40+i*13);});
    // gradients: horizontal, vertical, diagonal, per channel, opaque and with alpha
    for(int k=0;k<12;k++)blk([k](int x,int y,int,uint8_t *p){const int s=k%3==0?x:k%3==1?y:x+y,step=4+k*5;
        p[0]=uint8_t(std::min(255,20+s*step));p[1]=uint8_t(std::max(0,200-s*step/2));p[2]=uint8_t((k*37+s*step/3)&255);p[3]=uint8_t(k<6?255:std::min(255,64+s*24));});
    // flat (void extent): black, white, transparent, mid colours
    static const uint8_t flat[6][4]={{0,0,0,255},{255,255,255,255},{0,0,0,0},{128,64,32,255},{17,200,99,128},{255,0,255,1}};
    for(const auto &f:flat)blk([&f](int,int,int,uint8_t *p){memcpy(p,f,4);});
    // alpha edges: hard 0 / 255 splits (foliage cut-outs) at every column / row, colour constant and varying
    for(int k=0;k<8;k++)blk([k](int x,int y,int,uint8_t *p){const bool in=k<4?x>=k:y>=(k-4);p[0]=uint8_t(90+x*20);p[1]=uint8_t(160-y*10);p[2]=uint8_t(k*30);p[3]=in?255:0;});
    // two-colour checker and a hard colour edge (single partition worst cases)
    blk([](int x,int y,int,uint8_t *p){const bool c=(x^y)&1;p[0]=c?250:10;p[1]=c?240:20;p[2]=c?5:220;p[3]=255;});
    blk([](int x,int,int,uint8_t *p){p[0]=x<2?255:0;p[1]=x<2?0:255;p[2]=40;p[3]=255;});
}
static inline void xbg_bc_astc_kat(XbgAstcKatSet &s) {
    s.cats.clear();s.rgba.clear();s.bc.clear();
    xbg_bc_astc_kat_rgba(s.rgba);s.bc.assign(size_t(s.blocks())*4,0u);s.cats.push_back({"rgba",0,0,s.blocks()});
    static const uint32_t fmts[5]={0x41,0x42,0x43,0x44,0x4d};
    for(uint32_t f:fmts){
        const uint32_t bb=f<=0x42?8:16,n=f==0x4d?64:48;std::vector<uint8_t> raw;xbg_bc_kat_blocks(f,bb,n,raw);
        for(uint32_t i=0;i<n;i++){uint8_t *b=raw.data()+size_t(i)*bb;
            if((f==0x43 || f==0x44) && (i&1)){if(f==0x43){const uint8_t a=uint8_t(b[0]&15);for(int k=0;k<8;k++)b[k]=uint8_t(a|a<<4);}else{b[1]=b[0];memset(b+2,0,6);}}   // one alpha value
            if(f<=0x42 && (i&3)==3){uint16_t c0,c1;memcpy(&c0,b,2);memcpy(&c1,b+2,2);if(c0>c1){memcpy(b,&c1,2);memcpy(b+2,&c0,2);}}   // some three-colour blocks
        }
        const uint32_t first=s.blocks();s.cats.push_back({xbg_bc_astc_family_name(xbg_bc_astc_family(f)),f,first,n});
        for(uint32_t i=0;i<n;i++){uint8_t px[128];xbg_decode_block(f,raw.data()+size_t(i)*bb,bb,px,sizeof px);s.rgba.insert(s.rgba.end(),px,px+64);
            uint32_t w[4]={0,0,0,0};memcpy(w,raw.data()+size_t(i)*bb,bb);s.bc.insert(s.bc.end(),w,w+4);}
    }
}

// ---------------------------------------------------------------- checks
// Block decoded with the reference decoder (LDR profile, top byte of the UNORM16 result = the UNORM8 texel); error: an ASTC error block.
static inline bool xbg_bc_astc_decode(const uint8_t blk[16], uint8_t out[64]) {
    uint16_t px[64];AstcRefInfo info{};astcref::decode_block(blk,4,4,0,px,&info);
    for(int i=0;i<64;i++)out[i]=uint8_t(px[i]>>8);
    return !info.error;
}
static inline double xbg_bc_astc_mse(const uint8_t *a, const uint8_t *b) {double s=0;for(int i=0;i<64;i++){const double d=double(a[i])-double(b[i]);s+=d*d;}return s/64.0;}
static inline double xbg_bc_astc_psnr(double mse) {return mse<=1e-9?99.0:10.0*log10(255.0*255.0/mse);}
// The self-test verdict of one set of encoded blocks (16 bytes each, in the set's order). A block fails when it is an ASTC error block,
// all zero, or its error exceeds the host's (xbg_bc_astc_kat_mse3 / _mse1 for 3 / 1 candidates; expected = null: not compared) * 1.25 + 10
// (GameNative's tolerance for its golden block: room for an endpoint rounded one level the other way by the GPU's float arithmetic).
static inline const float *xbg_bc_astc_kat_expected(uint32_t cands) {return XBG_BC_ASTC_KAT_N?(cands>=3?xbg_bc_astc_kat_mse3:xbg_bc_astc_kat_mse1):nullptr;}
struct XbgAstcKatResult {
    uint32_t blocks=0,bad=0,error=0,zero=0,worse=0,first_bad=UINT32_MAX;double mse_sum=0,min_psnr=99;
    struct Cat { double psnr_sum=0,min_psnr=99;uint32_t n=0,bad=0; } cat[8];
};
static inline XbgAstcKatResult xbg_bc_astc_check(const XbgAstcKatSet &s, const uint8_t *enc, const float *expected) {
    XbgAstcKatResult r;r.blocks=s.blocks();
    for(uint32_t i=0;i<s.blocks();i++){
        const uint8_t *b=enc+size_t(i)*16;static const uint8_t z[16]={};uint8_t out[64];
        const bool zero=!memcmp(b,z,16),ok=xbg_bc_astc_decode(b,out);const double mse=xbg_bc_astc_mse(out,s.rgba.data()+size_t(i)*64);
        const bool worse=expected && i<XBG_BC_ASTC_KAT_N && !(mse<=double(expected[i])*1.25+10.0);
        const bool bad=zero || !ok || worse;
        r.zero+=zero;r.error+=!ok;r.worse+=worse;
        if(bad){++r.bad;if(r.first_bad==UINT32_MAX)r.first_bad=i;}
        const double p=xbg_bc_astc_psnr(mse);r.mse_sum+=mse;if(p<r.min_psnr)r.min_psnr=p;
        for(size_t c=0;c<s.cats.size() && c<8;c++)if(i>=s.cats[c].first && i<s.cats[c].first+s.cats[c].count){
            XbgAstcKatResult::Cat &k=r.cat[c];k.psnr_sum+=p;++k.n;if(p<k.min_psnr)k.min_psnr=p;k.bad+=bad;}
    }
    return r;
}
