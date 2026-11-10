// [safe-mode] BUGS_AND_FIXES 2.57: xbg_safe_mode.h (the step decision, the switches per step and family, the marker file, the crash
// classifiers, the test-trigger parsers). Host build, e.g. WSL:
//   g++ -std=c++17 -Wall -fsanitize=address,undefined -idirafter <ndk>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include -o safe_mode_test safe_mode_test.cpp
#include <cstdio>
#include <cstring>
#include <string>
#include <vulkan/vulkan.h>
#include "../xbg_safe_mode.h"

static int fails=0;
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);++fails;}}while(0)

static std::string sw(int step,XbgGpuFamily f){XbgSafeSwitch s[32];const int n=xbg_safe_switches(step,f,s,32);std::string o;for(int i=0;i<n;i++){o+=s[i].key;o+="=";o+=s[i].value;o+=" ";}return o;}

int main(){
    // families
    CHECK(xbg_safe_family(XBG_GPU_MALI) && xbg_safe_family(XBG_GPU_POWERVR));
    CHECK(!xbg_safe_family(XBG_GPU_ADRENO) && !xbg_safe_family(XBG_GPU_XCLIPSE) && !xbg_safe_family(XBG_GPU_OTHER));
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("mali"))==XBG_GPU_MALI);
    CHECK(xbg_effective_family(XBG_GPU_ADRENO,xbg_sim_parse("powervr,nogs"))==XBG_GPU_POWERVR);
    // the step
    int how=-1;
    CHECK(xbg_safe_step(nullptr,nullptr,false,0,&how)==0 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step(nullptr,nullptr,false,1,&how)==1 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step("",nullptr,false,2,&how)==2 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step(nullptr,nullptr,false,7,&how)==3);
    CHECK(xbg_safe_step("auto",nullptr,false,2,&how)==2 && how==XBG_SAFE_COUNTED);
    CHECK(xbg_safe_step("0",nullptr,false,3,&how)==0 && how==XBG_SAFE_OFF);
    CHECK(xbg_safe_step("off",nullptr,false,3,&how)==0 && how==XBG_SAFE_OFF);
    CHECK(xbg_safe_step("on",nullptr,false,0,&how)==1 && how==XBG_SAFE_ON);
    CHECK(xbg_safe_step("on",nullptr,false,2,&how)==2 && how==XBG_SAFE_ON);
    CHECK(xbg_safe_step("2",nullptr,false,0,&how)==2 && how==XBG_SAFE_FORCED);
    CHECK(xbg_safe_step("9",nullptr,false,0,&how)==3 && how==XBG_SAFE_FORCED);
    CHECK(xbg_safe_step("bogus",nullptr,false,1,&how)==1 && how==XBG_SAFE_COUNTED);
    // the v0.5.0 alias: PowerVR only, only without DM_SAFE_MODE
    CHECK(xbg_safe_step(nullptr,"1",true,0,&how)==2 && how==XBG_SAFE_ALIAS);
    CHECK(xbg_safe_step(nullptr,"2",true,0,&how)==3 && how==XBG_SAFE_ALIAS);
    CHECK(xbg_safe_step(nullptr,"3",true,0,&how)==3);
    CHECK(xbg_safe_step(nullptr,"0",true,3,&how)==0 && how==XBG_SAFE_ALIAS);
    CHECK(xbg_safe_step(nullptr,"1",false,0,&how)==0 && how==XBG_SAFE_COUNTED);   // Mali: no alias
    CHECK(xbg_safe_step("1","3",true,0,&how)==1 && how==XBG_SAFE_FORCED);         // DM_SAFE_MODE wins
    CHECK(xbg_safe_from_ladder(0)==0 && xbg_safe_from_ladder(1)==2 && xbg_safe_from_ladder(3)==4 && xbg_safe_from_ladder(50)==9);
    // switches
    CHECK(sw(0,XBG_GPU_MALI).empty() && sw(3,XBG_GPU_ADRENO).empty());
    CHECK(sw(1,XBG_GPU_MALI)=="DM_GFX_ZEROCOPY=0 DM_GFX_ZC3_SELFTEST=0 DM_GFX_FENCE_TRYLOCK=0 ");
    CHECK(sw(1,XBG_GPU_POWERVR)=="DM_GFX_ZEROCOPY=0 DM_GFX_ZC3_SELFTEST=0 DM_GFX_FENCE_TRYLOCK=0 DM_GFX_UBO_DYNAMIC=0 ");
    {const std::string s2=sw(2,XBG_GPU_POWERVR),s3=sw(3,XBG_GPU_MALI);
     CHECK(s2.find("DM_GFX_NOLOAD=0")!=std::string::npos && s2.find("DM_GFX_LAZY_CLEAR=0")!=std::string::npos && s2.find("DM_GFX_TILE_AREA=0")!=std::string::npos);
     CHECK(s2.find("COMPILE_LOCK")==std::string::npos);
     CHECK(s3.find("DM_GFX_COMPILE_LOCK=2")!=std::string::npos && s3.find("DM_GFX_NOOUT_MASK=0")!=std::string::npos && s3.find("DM_GFX_GS_PASSTHRU=0")!=std::string::npos);
     CHECK(s3.find("UBO_DYNAMIC")==std::string::npos);}
    {XbgSafeSwitch s[4];CHECK(xbg_safe_switches(3,XBG_GPU_POWERVR,s,4)==4);}   // capped by max
    // the marker file
    {XbgSafeMarker m;xbg_safe_parse("count=2\nstep=1\npid=4242\ntime=1731230000123\nfamily=mali\nwhy=driver crash inside the dm-record packet vkCmdEndRenderPass, signal 7\n",&m);
     CHECK(m.count==2 && m.step==1 && m.pid==4242 && m.time_ms==1731230000123ll && !strcmp(m.family,"mali") && !strcmp(m.why,"driver crash inside the dm-record packet vkCmdEndRenderPass, signal 7"));
     xbg_safe_parse("why=x\r\ncount=3\r\n",&m);CHECK(m.count==3 && !strcmp(m.why,"x"));
     xbg_safe_parse("",&m);CHECK(m.count==0 && m.pid==0 && !m.why[0]);
     xbg_safe_parse(nullptr,&m);CHECK(m.count==0);
     xbg_safe_parse("count=-5\ncounter=9\n",&m);CHECK(m.count==0);
     std::string longwhy="why="+std::string(400,'a');xbg_safe_parse(longwhy.c_str(),&m);CHECK(strlen(m.why)==sizeof m.why-1);}
    // classifiers
    CHECK(xbg_safe_gfx_thread("dm-record") && xbg_safe_gfx_thread("dm-gfx") && xbg_safe_gfx_thread("dm-present") && xbg_safe_gfx_thread("mali-cmar-backe"));
    CHECK(!xbg_safe_gfx_thread("dm-android") && !xbg_safe_gfx_thread("dm-lifecycle") && !xbg_safe_gfx_thread("dm-log") && !xbg_safe_gfx_thread(nullptr) && !xbg_safe_gfx_thread(""));
    CHECK(xbg_safe_gfx_lib("/vendor/lib64/hw/vulkan.mtk.so") && xbg_safe_gfx_lib("/vendor/lib64/egl/libGLES_mali.so") && xbg_safe_gfx_lib("/vendor/lib64/libsrv_um.so"));
    CHECK(xbg_safe_gfx_lib("/data/user/0/org.doom.port/files/drivers/panvk/libvulkan_panfrost.so"));
    CHECK(!xbg_safe_gfx_lib("/apex/com.android.runtime/lib64/bionic/libc.so") && !xbg_safe_gfx_lib("/data/app/x/lib/arm64/libdm.so") && !xbg_safe_gfx_lib(nullptr));
    CHECK(xbg_safe_text_device_lost("vkQueueSubmit(...) failed: -4") && xbg_safe_text_device_lost("present failed: VkResult -4") && xbg_safe_text_device_lost("result=-4)"));
    CHECK(xbg_safe_text_device_lost("VK_ERROR_DEVICE_LOST in vkWaitForFences"));
    CHECK(!xbg_safe_text_device_lost("failed: -42") && !xbg_safe_text_device_lost("failed: -1000072003") && !xbg_safe_text_device_lost("x-4") && !xbg_safe_text_device_lost(nullptr));
    // test triggers
    int s=0;
    CHECK(xbg_safe_fake_parse("60",&s)==XBG_FAKE_REC && s==60);
    CHECK(xbg_safe_fake_parse("45:lost",&s)==XBG_FAKE_LOST && s==45);
    CHECK(xbg_safe_fake_parse("30:hang",&s)==XBG_FAKE_HANG && s==30);
    CHECK(xbg_safe_fake_parse("30:rec",&s)==XBG_FAKE_REC);
    CHECK(xbg_safe_fake_parse("0",&s)==XBG_FAKE_NONE && xbg_safe_fake_parse("",&s)==XBG_FAKE_NONE && xbg_safe_fake_parse(nullptr,&s)==XBG_FAKE_NONE);
    CHECK(xbg_safe_fallback_fake("zc,fence:60,pipe:90","zc")==0);
    CHECK(xbg_safe_fallback_fake("zc,fence:60,pipe:90","fence")==60);
    CHECK(xbg_safe_fallback_fake("zc,fence:60,pipe:90","pipe")==90);
    CHECK(xbg_safe_fallback_fake("fence:60","zc")==-1 && xbg_safe_fallback_fake("zcx","zc")==-1 && xbg_safe_fallback_fake(nullptr,"zc")==-1);
    CHECK(xbg_safe_fallback_fake(" pipe , fence:5","pipe")==0 && xbg_safe_fallback_fake(" pipe , fence:5","fence")==5);
    for(int st=0;st<=3;st++)CHECK(xbg_safe_step_text(st) && *xbg_safe_step_text(st));
    printf(fails?"safe_mode_test: %d FAILED\n":"safe_mode_test: all passed\n",fails);
    return fails?1:0;
}
