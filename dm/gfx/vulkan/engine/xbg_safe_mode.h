// [DOOM][safe-mode] BUGS_AND_FIXES 2.57: safe mode for Mali / PowerVR (real families and the DM_GFX_SIM_GPU models). Pure functions over
// strings and the family, no device calls and no allocation, so tests/safe_mode_test.cpp calls the production code directly and the crash
// handler may use the classifiers (they only read memory).
//
// A run that ends with a graphics failure on such a GPU (a native crash inside a dm-record packet, on a graphics thread, in the Vulkan
// driver's code or in zero-copy memory; a GPU-hang fence fatal; a VK_ERROR_DEVICE_LOST fatal) adds one to DM_FILES_DIR/gfx-safe-mode
// (xbg_game.cpp xbg_safe_mode_record, signal-safe). The next start (xbg_safe_mode_boot, before the Vulkan device exists) turns on the step
// that count asks for, a switch the player set (dm/env) always winning:
//   step 1  the experimental 0.5.x speed-ups off: zero-copy (mode 2 and every view, the mode-3 self-test) DM_GFX_ZEROCOPY=0 /
//           DM_GFX_ZC3_SELFTEST=0, fence trylock DM_GFX_FENCE_TRYLOCK=0, dynamic UBOs at their 0.4.22 value (PowerVR DM_GFX_UBO_DYNAMIC=0,
//           Mali keeps 1 = its 0.4.22 value and today's default);
//   step 2  + the tile policies off (the old PowerVR ladder step 1): NOLOAD, STORE_DEAD, STORE_NONE, LEAN_USAGE, TILE_BARRIER, LAZY_CLEAR,
//           TILE_AREA = 0;
//   step 3  + COMPILE_LOCK 2, NOOUT_MASK 0, GS_PASSTHRU 0 (the old ladder steps 2-3).
// DM_SAFE_MODE: unset / auto = the counted step (min(count, 3)); 0 / off = none; on = at least step 1 (the gear's "On"); 1..3 = that step.
// DM_PVR_LADDER (v0.5.0 alias, PowerVR only, used when DM_SAFE_MODE is unset): 0 -> 0, 1 -> 2, 2 / 3 -> 3. An old gfx-pvr-ladder count c
// becomes a safe-mode count c + 1 (the workarounds it had stay on).
#pragma once
#include "xbg_vendor_policy.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>

enum { XBG_SAFE_MAX_STEP = 3 };
// The families safe mode acts on (the policy family: a simulated one included).
inline bool xbg_safe_family(XbgGpuFamily f){return f==XBG_GPU_MALI || f==XBG_GPU_POWERVR;}

// How this boot's step was decided (the status string for the app says it in words: xbg_safe_how_name).
enum XbgSafeHow { XBG_SAFE_COUNTED=0, XBG_SAFE_FORCED=1, XBG_SAFE_ON=2, XBG_SAFE_OFF=3, XBG_SAFE_ALIAS=4, XBG_SAFE_NOT_ELIGIBLE=5 };
inline const char *xbg_safe_how_name(int how){
    switch(how){case XBG_SAFE_COUNTED:return "counted";case XBG_SAFE_FORCED:return "forced";case XBG_SAFE_ON:return "on";case XBG_SAFE_OFF:return "off";
    case XBG_SAFE_ALIAS:return "pvr-ladder";default:return "not-eligible";}
}
// This boot's step from DM_SAFE_MODE, the DM_PVR_LADDER alias (PowerVR only) and the counted graphics crashes.
inline int xbg_safe_step(const char *safe_mode,const char *pvr_ladder,bool powervr,int count,int *how){
    int h=XBG_SAFE_COUNTED,step=count<0?0:count>XBG_SAFE_MAX_STEP?XBG_SAFE_MAX_STEP:count;
    if(safe_mode && *safe_mode && strcmp(safe_mode,"auto")){
        if(!strcmp(safe_mode,"off") || !strcmp(safe_mode,"0")){h=XBG_SAFE_OFF;step=0;}
        else if(!strcmp(safe_mode,"on")){h=XBG_SAFE_ON;if(step<1)step=1;}
        else if(*safe_mode>='1' && *safe_mode<='9'){h=XBG_SAFE_FORCED;step=atoi(safe_mode);if(step>XBG_SAFE_MAX_STEP)step=XBG_SAFE_MAX_STEP;}
        // anything else: counted (an unknown word does not turn safe mode off)
    }
    else if((!safe_mode || !*safe_mode) && powervr && pvr_ladder && *pvr_ladder){
        const int l=atoi(pvr_ladder);h=XBG_SAFE_ALIAS;step=l<=0?0:l==1?2:3;
    }
    if(how)*how=h;
    return step;
}
// An old v0.5.0 gfx-pvr-ladder count -> the safe-mode count that keeps its workarounds (ladder 1 = tile policies off = safe step 2).
inline int xbg_safe_from_ladder(int ladder){return ladder<=0?0:ladder+1>9?9:ladder+1;}

// The switches of a step on a family, in the order they are set. Returns how many were written to out (at most max).
struct XbgSafeSwitch { const char *key, *value; };
inline int xbg_safe_switches(int step,XbgGpuFamily f,XbgSafeSwitch *out,int max){
    int n=0;auto add=[&](const char *k,const char *v){if(n<max)out[n++]={k,v};};
    if(!xbg_safe_family(f) || step<=0)return 0;
    add("DM_GFX_ZEROCOPY","0");add("DM_GFX_ZC3_SELFTEST","0");add("DM_GFX_FENCE_TRYLOCK","0");
    if(f==XBG_GPU_POWERVR)add("DM_GFX_UBO_DYNAMIC","0");   // 0.4.22: no dynamic UBOs on PowerVR (Mali: 1 then and now)
    if(step>=2){add("DM_GFX_NOLOAD","0");add("DM_GFX_STORE_DEAD","0");add("DM_GFX_STORE_NONE","0");add("DM_GFX_LEAN_USAGE","0");
        add("DM_GFX_TILE_BARRIER","0");add("DM_GFX_LAZY_CLEAR","0");add("DM_GFX_TILE_AREA","0");}
    if(step>=3){add("DM_GFX_COMPILE_LOCK","2");add("DM_GFX_NOOUT_MASK","0");add("DM_GFX_GS_PASSTHRU","0");}
    return n;
}
inline const char *xbg_safe_step_text(int step){
    switch(step){case 0:return "nothing changed";
    case 1:return "the experimental 0.5.x speed-ups off (zero-copy, fence trylock, PowerVR dynamic UBOs)";
    case 2:return "step 1 + the tile policies off (noload / store / lean usage / tile barrier / lazy clear / tile area)";
    default:return "step 2 + compile lock 2, colour mask of output-less shaders off, culling GS kept";}
}

// The marker file DM_FILES_DIR/gfx-safe-mode: "key=value" lines (count, step, pid, time = wall clock ms, family, why). Unknown keys are
// skipped; a file without "count=" is count 0.
struct XbgSafeMarker { int count=0, step=-1, pid=0; long long time_ms=0; char family[16]={}; char why[200]={}; };
inline void xbg_safe_parse(const char *text,XbgSafeMarker *m){
    *m=XbgSafeMarker{};if(!text)return;
    for(const char *p=text;*p;){
        const char *e=p;while(*e && *e!='\n')++e;
        auto val=[&](const char *k)->const char*{size_t n=strlen(k);return size_t(e-p)>n && !strncmp(p,k,n) && p[n]=='='?p+n+1:nullptr;};
        auto copy=[&](const char *v,char *dst,size_t cap){size_t n=size_t(e-v);if(n && v[n-1]=='\r')--n;if(n>=cap)n=cap-1;memcpy(dst,v,n);dst[n]=0;};
        if(const char *v=val("count"))m->count=atoi(v);
        else if(const char *v=val("step"))m->step=atoi(v);
        else if(const char *v=val("pid"))m->pid=atoi(v);
        else if(const char *v=val("time"))m->time_ms=atoll(v);
        else if(const char *v=val("family"))copy(v,m->family,sizeof m->family);
        else if(const char *v=val("why"))copy(v,m->why,sizeof m->why);
        p=*e?e+1:e;
    }
    if(m->count<0)m->count=0;
}

// Crash classification (signal handler): a thread of the graphics engine / presenter or one a GPU driver names (Mali "mali-*", IMG
// "pvr*" / "IMG*" / "Vk*", Mesa "tu_*" / "pan*" / "vk*" queue / compiler threads), and the code of a GPU driver library.
inline bool xbg_safe_gfx_thread(const char *t){
    if(!t || !*t)return false;
    static const char *const ours[]={"dm-gfx","dm-record","dm-present","dm-crumbs","dm-acquire"};
    for(const char *o:ours)if(!strncmp(t,o,strlen(o)))return true;
    static const char *const drv[]={"mali-","pvr","IMG","Vk","vk","tu_","pan","Mali"};
    for(const char *d:drv)if(!strncmp(t,d,strlen(d)))return true;
    return false;
}
inline bool xbg_safe_gfx_lib(const char *path){
    if(!path || !*path)return false;
    const char *b=strrchr(path,'/');b=b?b+1:path;
    static const char *const libs[]={"vulkan","mali","GLES_","pvr","IMG","srv_um","freedreno","panfrost","panvk","adreno","libgsl","llvm-glnext","libVkLayer"};
    for(const char *l:libs)if(strstr(b,l))return true;
    return false;
}
// An engine FATAL text that names VK_ERROR_DEVICE_LOST: the word, or a result -4 ("failed: -4", "VkResult -4", "result=-4").
inline bool xbg_safe_text_device_lost(const char *t){
    if(!t)return false;
    if(strstr(t,"VK_ERROR_DEVICE_LOST") || strstr(t,"DEVICE_LOST"))return true;
    for(const char *p=strstr(t,"-4");p;p=strstr(p+1,"-4")){
        const char before=p>t?p[-1]:' ',after=p[2];
        if((before==' ' || before=='=' || before=='(' || before==':') && !(after>='0' && after<='9'))return true;
    }
    return false;
}

// DM_GFX_FAKE_GPU_CRASH=<s>[:rec|lost|hang] (test; Mali / PowerVR incl. DM_GFX_SIM_GPU only), DM_PVR_LADDER_FAKE=<s> (v0.5.0 alias = rec):
// rec = SIGBUS in the first vkCmdEndRenderPass packet on dm-record after s seconds, lost = a VK_ERROR_DEVICE_LOST fatal on the translation
// thread, hang = a fence-wait fatal (fence_fatal, the whole timeout). Returns the kind (0 = none) and *seconds.
enum { XBG_FAKE_NONE=0, XBG_FAKE_REC=1, XBG_FAKE_LOST=2, XBG_FAKE_HANG=3 };
inline int xbg_safe_fake_parse(const char *e,int *seconds){
    *seconds=0;if(!e || !*e)return XBG_FAKE_NONE;
    const int s=atoi(e);if(s<=0)return XBG_FAKE_NONE;
    *seconds=s;const char *k=strchr(e,':');
    if(!k || !strcmp(k+1,"rec"))return XBG_FAKE_REC;
    if(!strcmp(k+1,"lost"))return XBG_FAKE_LOST;
    if(!strcmp(k+1,"hang"))return XBG_FAKE_HANG;
    return XBG_FAKE_REC;
}
// DM_GFX_FAKE_FALLBACK=<kind>[:<s>][,<kind>[:<s>]...] (test, the in-process fallbacks): zc = the zero-copy self-test counts as failed,
// fence = a fence error recovered after s seconds, pipe = a pipeline build failure after s seconds. Returns s (0 without ":s") or -1.
inline int xbg_safe_fallback_fake(const char *e,const char *kind){
    if(!e || !kind)return -1;
    const size_t n=strlen(kind);
    for(const char *p=e;*p;){
        while(*p==',' || *p==' ')++p;
        if(!strncmp(p,kind,n) && (p[n]==0 || p[n]==',' || p[n]==':' || p[n]==' '))return p[n]==':'?atoi(p+n+1):0;
        while(*p && *p!=',')++p;
    }
    return -1;
}
