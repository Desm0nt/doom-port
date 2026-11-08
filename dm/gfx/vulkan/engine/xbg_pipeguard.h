// [DOOM] Pipeguard v2 (KB SH-06, after ETR 745950b / da700ae / 190059b): the file side of the "this shader killed the driver's
// compiler" lists, as pure functions over paths and strings so tests/pipeguard_test.cpp runs the production code on a host.
// xbg_game.cpp (namespaces pipeguard / gpguard) owns the engine side: where the files live, the SPIR-V hashes, the logging.
//
// - Each list (gfx-pipe-skip, gfx-lone-lane, gfx-gpipe-skip; one per driver, see xbg_pg_suffix) starts with
//     # pipeguard v2 driver=<package | system> driverVersion=<hex> cache=<pipelineCacheUUID> translator=<sha16 of shaders/_chain.json | ->
//   and holds `key \t reason \t unix time \t spv` lines (spv = first 16 hex of the SHA-256 of the bundle's SPIR-V). A list under
//   another header (driver update, other translator pack) is renamed <name>.stale and not applied; a list without a header is a
//   v1 list (DOOM before 08.11) and goes to .stale the same way, so its bans get a fresh chance.
// - Inflight markers / suspects are `key \t spv` lines. A boot that finds a marker counts a death only for the SPIR-V that was being
//   compiled, and only the second dead boot in a row on the same key + spv records it (one may be MIUI killing the app, a
//   force-stop or low memory during the few milliseconds of a build).
// - The driver name is the driver that really runs (vk_driver.c status, "active" field), never the one DM_VK_DRIVER asked for (the
//   XC2 bug: a package that did not take over left its lists under the package's name). DM_PIPEGUARD_DRIVER overrides; a trailing
//   "-occ" is cut (ETR: the occlusion-patched Turnip shares its compiler with the plain one).
// - mode 1 (DM_PIPEGUARD=1): the v1 behaviour: no header, no spv, no driver suffix.
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace xbg_pg {

static const char kHeaderPrefix[]="# pipeguard v2 ";

typedef std::function<void(const std::string &)> Log;

inline std::string header(const char *driver,uint32_t driver_version,const uint8_t *cache_uuid,size_t uuid_bytes,const char *translator){
    std::string uuid;char b[8];for(size_t i=0;i<uuid_bytes;i++){snprintf(b,sizeof b,"%02x",cache_uuid[i]);uuid+=b;}
    char line[320];snprintf(line,sizeof line,"%s" "driver=%s driverVersion=%#x cache=%s translator=%s",kHeaderPrefix,driver && *driver?driver:"system",driver_version,uuid.c_str(),translator && *translator?translator:"-");
    return line;
}

// Field `index` (0-based) of a tab-separated line ("" when missing)
inline std::string tab_field(const char *line,unsigned index){
    if(!line)return {};const char *s=line;for(unsigned i=0;i<index;i++){s=strchr(s,'\t');if(!s)return {};s++;}
    const char *e=strchr(s,'\t');return e?std::string(s,size_t(e-s)):std::string(s);
}

// The driver the lists are kept under ("" = the system driver). status = dm_vk_driver_status():
// "<requested>\t<active>\t<reason>\t<details>", active = "system" or the custom library that really runs.
inline std::string guard_driver(const char *override_env,const char *status){
    std::string s;
    if(override_env && *override_env)s=override_env;
    else if(status && *status){const std::string active=tab_field(status,1);if(!active.empty() && active!="system")s=tab_field(status,0);}
    if(s.size()>4 && s.compare(s.size()-4,4,"-occ")==0)s.resize(s.size()-4);
    if(s=="system")s.clear();
    return s;
}
// driverID from the status details ("... id=<n>"), 0 when unknown
inline uint32_t status_driver_id(const char *status){
    if(!status)return 0;const char *p=strstr(status,"\tverified=");if(!p)return 0;p=strstr(p," id=");return p?uint32_t(strtoul(p+4,nullptr,10)):0;
}
// File-name suffix of a driver name: "" for the system driver, else "-<name>" with other characters than [A-Za-z0-9_.] as '_'
inline std::string suffix(const std::string &driver){
    if(driver.empty())return {};std::string s="-";for(char c:driver)s+=(isalnum((unsigned char)c) || c=='_' || c=='.')?c:'_';return s;
}

inline void chomp(char *line){line[strcspn(line,"\r\n")]=0;}

enum ReadResult { READ_MISSING=0, READ_OK=1, READ_STALE=2, READ_LEGACY=3, READ_EMPTY=4 };

// Moves `path` to `path`.stale (an older .stale is replaced); returns rename()'s result
inline int set_aside(const std::string &path){const std::string stale=path+".stale";remove(stale.c_str());return rename(path.c_str(),stale.c_str());}

// Entries of a list into out (key -> spv). mode 2: the first line must be `hdr`; another v2 header -> READ_STALE, no v2 header ->
// READ_LEGACY (a v1 list); both are renamed .stale and logged, nothing is applied. mode 1: every non-comment line's first field is
// a key, spv "" (v1 reads v2 files too: the header is a comment).
inline ReadResult read_list(const std::string &path,const std::string &hdr,int mode,std::unordered_map<std::string,std::string> &out,const Log &log){
    FILE *f=fopen(path.c_str(),"rb");if(!f)return READ_MISSING;char line[1024];
    if(mode>=2){
        if(!fgets(line,sizeof line,f)){fclose(f);return READ_EMPTY;}chomp(line);
        if(hdr!=line){
            const bool v2=!strncmp(line,kHeaderPrefix,sizeof kHeaderPrefix-1);std::string keys;unsigned n=0;
            if(!v2){if(*line && *line!='#'){keys=tab_field(line,0);n=1;}while(fgets(line,sizeof line,f)){chomp(line);if(!*line || *line=='#')continue;if(++n<=8){keys+=keys.empty()?"":" ";keys+=tab_field(line,0);}}}
            fclose(f);const int r=set_aside(path);
            if(v2)log("[pipeguard] stale: "+path+" was written under '"+std::string(line).substr(0,200)+"', now '"+hdr+"': moved to .stale (rc "+std::to_string(r)+"), not applied");
            else log("[pipeguard] legacy: "+path+" is a v1 list without a header ("+std::to_string(n)+" entr"+(n==1?"y":"ies")+(keys.empty()?"":": "+keys)+(n>8?" ...":"")+"): moved to .stale (rc "+std::to_string(r)+"), not applied; these shaders get a fresh chance and are listed again only after two deaths in a row");
            return v2?READ_STALE:READ_LEGACY;}
    }else rewind(f);
    while(fgets(line,sizeof line,f)){chomp(line);if(!*line || *line=='#')continue;const std::string key=tab_field(line,0);if(key.empty())continue;
        out[key]=mode>=2?tab_field(line,3):std::string();}   // a line without the spv field never applies in mode 2
    fclose(f);return READ_OK;
}
// mode 2 on a list another driver's session left without its suffix: only a v1 list (no v2 header) is set aside, a v2 one is the
// system driver's and stays. Returns whether it was moved.
inline bool set_aside_legacy(const std::string &path,const Log &log){
    FILE *f=fopen(path.c_str(),"rb");if(!f)return false;char line[1024];const bool got=fgets(line,sizeof line,f)!=nullptr;fclose(f);
    if(got && !strncmp(line,kHeaderPrefix,sizeof kHeaderPrefix-1))return false;
    std::unordered_map<std::string,std::string> none;return read_list(path,"\x01",2,none,log)==READ_LEGACY;
}
// One entry; mode 2 writes the header first into a new / empty file, mode 1 only the key (the v1 format)
inline bool append_list(const std::string &path,const std::string &hdr,int mode,const std::string &key,const char *why,const std::string &spv,long long when){
    struct stat st{};const bool fresh=stat(path.c_str(),&st)!=0 || st.st_size==0;
    FILE *f=fopen(path.c_str(),"ab");if(!f)return false;
    if(mode>=2){if(fresh)fprintf(f,"%s\n",hdr.c_str());std::string r=why?why:"";for(auto &c:r)if(c=='\t' || c=='\n' || c=='\r')c=' ';
        fprintf(f,"%s\t%s\t%lld\t%s\n",key.c_str(),r.c_str(),when,spv.c_str());}
    else fprintf(f,"%s\n",key.c_str());
    fclose(f);return true;
}
// Marker / suspect line: "key \t spv" (mode 2) or "key" (mode 1)
inline std::string marker_line(const std::string &key,const std::string &spv,int mode){return mode>=2?key+"\t"+spv:key;}
inline std::pair<std::string,std::string> marker(const char *line){const std::string l=line?line:"";const size_t t=l.find('\t');
    return t==std::string::npos?std::make_pair(l,std::string()):std::make_pair(l.substr(0,t),l.substr(t+1));}

// What a marker left by the last boot means. now_spv = the shader's SPIR-V as it is now; prev_suspect = the line the boot before
// left in the suspect file ("" none).
enum Death { DEATH_NONE=0, DEATH_SPV_CHANGED=1, DEATH_FIRST=2, DEATH_SECOND=3 };
inline Death judge(const std::string &marker_text,const std::string &now_spv,const std::string &prev_suspect,int mode){
    const auto m=marker(marker_text.c_str());if(m.first.empty())return DEATH_NONE;
    if(mode>=2 && m.second!=now_spv)return DEATH_SPV_CHANGED;
    return marker_text==prev_suspect?DEATH_SECOND:DEATH_FIRST;
}
// Whether an entry recorded for spv `listed` applies to a shader whose SPIR-V is `now` (mode 1: always)
inline bool applies(const std::string &listed,const std::string &now,int mode){return mode<2 || listed==now;}

}
