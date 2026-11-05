// [nonadreno] ASTC / ETC2 / EAC overlay (variant A): calls the production policy of xbg_astc_overlay.h directly (no device):
// format mapping, the real-BC shape rule (mirror of tools/astc_overlay_doom.py keep_bc, checked on the textures of the dumps),
// the manifest parser (the tool's flat engine_* keys, rejected / partial manifests) and the boot choice for Adreno / Mali / PowerVR.
// The tool side: tools/test_astc_overlay_doom.py. Build and run (WSL, from this directory):
//   g++ -std=c++17 -Wall -idirafter /mnt/g/Tools/Android/Sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include -o astc_overlay_test astc_overlay_test.cpp && ./astc_overlay_test
#include "../xbg_astc_overlay.h"
#include <stdio.h>
#include <initializer_list>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

int main(){
    // ---- format mapping: same block size and sRGB-ness; only the unsigned formats the tool writes
    CHECK(xbg_astc_substitute_format(0x42)==VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
    CHECK(xbg_astc_substitute_format(0x46)==VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK);
    CHECK(xbg_astc_substitute_format(0x44)==VK_FORMAT_ASTC_4x4_UNORM_BLOCK && xbg_astc_substitute_format(0x48)==VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
    CHECK(xbg_astc_substitute_format(0x4d)==VK_FORMAT_ASTC_4x4_UNORM_BLOCK && xbg_astc_substitute_format(0x4e)==VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
    CHECK(xbg_astc_substitute_format(0x49)==VK_FORMAT_EAC_R11_UNORM_BLOCK && xbg_astc_substitute_format(0x4b)==VK_FORMAT_EAC_R11G11_UNORM_BLOCK);
    CHECK(xbg_astc_substitute_format(0x50)==VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK);
    for(uint32_t f:{0x43u,0x47u,0x4au,0x4cu,0x4fu,0x37u,0x79u,0x00u})CHECK(xbg_astc_substitute_format(f)==VK_FORMAT_UNDEFINED && !xbg_astc_format_bit(f));
    for(uint32_t f=0x41;f<=0x50;f++)CHECK((xbg_astc_format_bit(f)!=0)==(xbg_astc_substitute_format(f)!=VK_FORMAT_UNDEFINED));

    // ---- masks
    CHECK(xbg_astc_formats_mask("bc1 bc3 bc4 bc5 bc6h bc7")==0x3fu);
    CHECK(xbg_astc_formats_mask("bc3,bc7  bogus bc6h")==(XBG_AO_BC3|XBG_AO_BC7|XBG_AO_BC6H));
    CHECK(xbg_astc_formats_mask(nullptr)==0 && xbg_astc_formats_mask("")==0 && xbg_astc_formats_mask("bc")==0 && xbg_astc_formats_mask("bc66h")==0);
    {char s[64];xbg_astc_formats_str(XBG_AO_BC3|XBG_AO_BC4|XBG_AO_BC7,s,sizeof s);CHECK(!strcmp(s,"bc3 bc4 bc7"));xbg_astc_formats_str(0,s,sizeof s);CHECK(!strcmp(s,"none"));}

    // ---- per texture: the live BC textures of the dumps (work/dump3-11 all.txt), with the full overlay mask
    const uint32_t all=0x3fu;const char *why=nullptr;
    struct Case{uint32_t fmt;int target;uint32_t w,h,levels,layers;VkFormat want;const char *why;};
    const Case cases[]={
        {0x4d,1,8192,8192,5,1,VK_FORMAT_ASTC_4x4_UNORM_BLOCK,nullptr},      // 27 decal atlas BC7 (blocks copied from the files)
        {0x4b,1,8192,4096,5,1,VK_FORMAT_EAC_R11G11_UNORM_BLOCK,nullptr},    // 25 transSort atlas BC5
        {0x49,1,2048,1024,5,1,VK_FORMAT_EAC_R11_UNORM_BLOCK,nullptr},       // 26 light atlas BC4
        {0x50,9,128,128,7,5760,VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK,nullptr},    // 94 light-probe cube array BC6H
        {0x48,4,4096,4096,1,3,VK_FORMAT_UNDEFINED,"vt-cache-shape"},       // 103 VT cache BC3 sRGB (game transcoder): real BC
        {0x50,1,8192,4096,1,1,VK_FORMAT_UNDEFINED,"vt-cache-shape"},       // 1880 VT cache BC6H: real BC
        {0x50,1,512,512,1,1,VK_FORMAT_UNDEFINED,"bc6h-not-cube"},          // any other 2D BC6H: real BC (every file BC6H is a cube)
        {0x48,4,1024,1024,1,4,VK_FORMAT_UNDEFINED,"bc3-array"},            // a smaller BC3 array: real BC
        {0x44,1,4096,1024,1,1,VK_FORMAT_ASTC_4x4_UNORM_BLOCK,nullptr},      // the 4096x1024 single-level BC3 file texture
        {0x4d,1,1280,3004,1,1,VK_FORMAT_ASTC_4x4_UNORM_BLOCK,nullptr},      // swf screen, single level
        {0x42,1,512,512,8,1,VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK,nullptr},     // BC1 (only when the overlay substitutes bc1)
        {0x4d,8,512,512,8,6,VK_FORMAT_ASTC_4x4_UNORM_BLOCK,nullptr},        // BC7 cube
        {0x4d,2,64,64,1,1,VK_FORMAT_UNDEFINED,"not-2d"},                   // 3D: never from the files
        {0x4a,1,64,64,1,1,VK_FORMAT_UNDEFINED,"format"},                   // signed BC4: not the tool's data
    };
    for(const Case &c:cases){
        const VkFormat f=xbg_astc_texture_format(all,c.fmt,c.target,c.w,c.h,c.levels,c.layers,&why);
        CHECK(f==c.want);
        CHECK((c.why==nullptr && why==nullptr) || (c.why && why && !strcmp(c.why,why)));
        if(f!=c.want || !((c.why==nullptr && why==nullptr) || (c.why && why && !strcmp(c.why,why))))printf("  case fmt=%#x target=%d %ux%u: got vk=%d why=%s\n",c.fmt,c.target,c.w,c.h,int(f),why?why:"-");
    }
    // a format outside the mask is never substituted (e.g. bc6h without ASTC HDR, bc1 by default)
    CHECK(xbg_astc_texture_format(all&~XBG_AO_BC6H,0x50,9,128,128,7,5760,&why)==VK_FORMAT_UNDEFINED && !strcmp(why,"format"));
    CHECK(xbg_astc_texture_format(all&~XBG_AO_BC1,0x42,1,512,512,8,1,&why)==VK_FORMAT_UNDEFINED);
    CHECK(xbg_astc_texture_format(0,0x4d,1,512,512,8,1,&why)==VK_FORMAT_UNDEFINED);

    // ---- manifest (the shape tools/astc_overlay_doom.py writes; nested objects before the flat keys must not confuse the parser)
    const char *good=
        "{\n \"format\": \"doom-astc-overlay\",\n \"version\": 1,\n \"settings\": {\"format\": \"x\", \"version\": 7},\n"
        " \"engine_formats_data\": \"bc3 bc4 bc5 bc6h bc7\",\n \"engine_formats_nohdr\": \"bc3 bc4 bc5 bc7\",\n"
        " \"engine_files\": \"data/base/gameresources_003.patch=809758644 data/base/gameresources_003.pindex=14717929 nohdr/base/gameresources_003.pindex=14717929\",\n"
        " \"engine_source\": \"base/gameresources_003.pindex=14717929 base/gameresources_003.patch=280404745\"\n}\n";
    XbgAstcManifest m;char why2[256];
    CHECK(xbg_astc_manifest_parse(good,&m,why2,sizeof why2)==0);
    CHECK(m.version==1 && m.data_mask==(XBG_AO_BC3|XBG_AO_BC4|XBG_AO_BC5|XBG_AO_BC6H|XBG_AO_BC7) && m.nohdr_mask==(XBG_AO_BC3|XBG_AO_BC4|XBG_AO_BC5|XBG_AO_BC7));
    CHECK(m.nfiles==3 && !strcmp(m.file[0],"data/base/gameresources_003.patch") && m.file_size[0]==809758644ull && !strcmp(m.file[2],"nohdr/base/gameresources_003.pindex"));
    CHECK(m.nsources==2 && !strcmp(m.source[1],"base/gameresources_003.patch") && m.source_size[1]==280404745ull);
    auto rejects=[&](const char *text){XbgAstcManifest x;char w[256];return xbg_astc_manifest_parse(text,&x,w,sizeof w)!=0;};
    CHECK(rejects("{\"format\": \"doom-astc-overlay-partial\", \"version\": 1}"));   // --limit runs
    CHECK(rejects("{\"format\": \"doom-astc-overlay\", \"version\": 2, \"engine_formats_data\": \"bc7\"}"));
    CHECK(rejects("{\"format\": \"doom-astc-overlay\", \"version\": 1, \"engine_formats_data\": \"bc7\", \"engine_formats_nohdr\": \"bc6h\", \"engine_files\": \"a=1\", \"engine_source\": \"b=2\"}"));
    CHECK(rejects("{\"format\": \"doom-astc-overlay\", \"version\": 1, \"engine_formats_data\": \"bc7\", \"engine_formats_nohdr\": \"bc7\", \"engine_files\": \"../x=1\", \"engine_source\": \"b=2\"}"));
    CHECK(rejects("{\"format\": \"doom-astc-overlay\", \"version\": 1, \"engine_formats_data\": \"bc7\", \"engine_formats_nohdr\": \"bc7\", \"engine_files\": \"a=1x\", \"engine_source\": \"b=2\"}"));
    CHECK(rejects("{\"format\": \"doom-astc-overlay\", \"version\": 1, \"engine_formats_data\": \"bc7\", \"engine_formats_nohdr\": \"bc7\", \"engine_files\": \"\", \"engine_source\": \"b=2\"}"));
    CHECK(rejects(""));CHECK(rejects("not json at all"));

    // ---- boot choice: mode -1 auto, 0 off, 1 forced
    CHECK(xbg_astc_mode(nullptr)==-1 && xbg_astc_mode("auto")==-1 && xbg_astc_mode("0")==0 && xbg_astc_mode("1")==1 && xbg_astc_mode("yes")==-1);
    XbgAstcChoice c;
    c=xbg_astc_choose(-1,1,1,1,1,&m);CHECK(!c.use);                                            // Adreno, auto: untouched
    c=xbg_astc_choose(1,1,1,1,0,&m);CHECK(c.use && !c.hdr && !strcmp(c.variant,"nohdr") && c.mask==m.nohdr_mask);   // Adreno forced, no HDR
    c=xbg_astc_choose(1,1,1,1,1,&m);CHECK(c.use && c.hdr && !strcmp(c.variant,"data") && c.mask==m.data_mask);
    c=xbg_astc_choose(-1,0,1,1,1,&m);CHECK(c.use && c.hdr);                                    // Mali with ASTC HDR
    c=xbg_astc_choose(-1,0,1,1,0,&m);CHECK(c.use && !c.hdr && !(c.mask&XBG_AO_BC6H));          // PowerVR-like: BC6H stays BC
    c=xbg_astc_choose(-1,0,0,1,0,&m);CHECK(!c.use);                                            // no ASTC LDR: nothing
    c=xbg_astc_choose(-1,0,1,0,0,&m);CHECK(!c.use);                                            // no ETC2: nothing
    c=xbg_astc_choose(0,0,1,1,1,&m);CHECK(!c.use);                                             // switched off
    c=xbg_astc_choose(-1,0,1,1,1,nullptr);CHECK(!c.use);                                       // no / bad manifest
    {XbgAstcManifest n=m;n.data_mask&=~XBG_AO_BC6H;n.nohdr_mask&=~XBG_AO_BC6H;c=xbg_astc_choose(-1,0,1,1,1,&n);CHECK(c.use && !c.hdr);}   // overlay without BC6H

    printf(failures?"astc_overlay_test: %d FAILURES\n":"astc_overlay_test: all passed\n",failures);
    return failures?1:0;
}
