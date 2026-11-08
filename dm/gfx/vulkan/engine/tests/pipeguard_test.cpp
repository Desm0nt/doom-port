// [DOOM] Pipeguard v2 (xbg_pipeguard.h, KB SH-06): list header / .stale / v1 migration, entries keyed by SPIR-V, the two-deaths
// rule, the driver the lists are kept under, and the SHA-256 helper (xbg_shader_key.c). Files only, no device. Build (WSL):
//   gcc -c -g -fsanitize=address,undefined -o /tmp/pg_sha.o ../xbg_shader_key.c
//   g++ -std=c++17 -Wall -Wextra -Wno-misleading-indentation -g -fsanitize=address,undefined -o /tmp/pipeguard_test pipeguard_test.cpp /tmp/pg_sha.o && /tmp/pipeguard_test
#include "../xbg_pipeguard.h"
#include <stdio.h>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" int xbg_sha256_hex16(const void *data,size_t n,char out_hex17[17]);

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static std::string dir;
static std::vector<std::string> logs;
static const xbg_pg::Log log_fn=[](const std::string &s){logs.push_back(s);};
static bool exists(const std::string &p){return access(p.c_str(),F_OK)==0;}
static void put(const std::string &p,const char *text){FILE *f=fopen(p.c_str(),"wb");fputs(text,f);fclose(f);}
static std::string get(const std::string &p){std::string s;FILE *f=fopen(p.c_str(),"rb");if(!f)return s;char b[512];size_t n;while((n=fread(b,1,sizeof b,f))>0)s.append(b,n);fclose(f);return s;}
static bool logged(const char *what){for(auto &l:logs)if(l.find(what)!=std::string::npos)return true;return false;}

int main(){
    char tmpl[]="/tmp/pgtestXXXXXX";CHECK(mkdtemp(tmpl)!=nullptr);dir=tmpl;
    const uint8_t uuid[16]={0xde,0xad,0xbe,0xef,1,2,3,4,5,6,7,8,9,10,11,12};
    const std::string hdr=xbg_pg::header("turnip27",0x5c0e2a,uuid,16,"0123456789abcdef");
    CHECK(hdr=="# pipeguard v2 driver=turnip27 driverVersion=0x5c0e2a cache=deadbeef0102030405060708090a0b0c translator=0123456789abcdef");
    CHECK(xbg_pg::header("",1,uuid,16,nullptr).find("driver=system driverVersion=0x1 ")!=std::string::npos);
    CHECK(xbg_pg::header("",1,uuid,16,"").find("translator=-")!=std::string::npos);

    // The driver the lists belong to: the active one from vk_driver.c's status, never the request alone.
    CHECK(xbg_pg::guard_driver(nullptr,"turnip27\tlibvulkan_freedreno.so\tok\tverified=1 gpu=Turnip Adreno (TM) 750 driver=turnip id=18")=="turnip27");
    CHECK(xbg_pg::guard_driver(nullptr,"turnip27-occ\tlibvulkan_freedreno.so\tok\tverified=1 gpu=x driver=turnip id=18")=="turnip27");
    CHECK(xbg_pg::guard_driver(nullptr,"turnip27\tsystem\tnot-taken-over\tverified=0 gpu=Adreno (TM) 750 driver=Qualcomm id=8")=="");   // XC2 bug avoided
    CHECK(xbg_pg::guard_driver(nullptr,"\tsystem\tok\tverified=0 gpu=Adreno (TM) 750 driver=Qualcomm id=8")=="");
    CHECK(xbg_pg::guard_driver(nullptr,"")=="");
    CHECK(xbg_pg::guard_driver(nullptr,nullptr)=="");
    CHECK(xbg_pg::guard_driver("mesa-25",nullptr)=="mesa-25");
    CHECK(xbg_pg::guard_driver("turnip27-occ","x\tsystem\tok\t")=="turnip27");
    CHECK(xbg_pg::guard_driver("system","turnip27\tlib.so\tok\t")=="");
    CHECK(xbg_pg::status_driver_id("turnip27\tsystem\tnot-taken-over\tverified=0 gpu=Adreno (TM) 750 driver=Qualcomm id=8")==8);
    CHECK(xbg_pg::status_driver_id("a\tb\tok\tverified=1 gpu=Turnip Adreno (TM) 750 driver=turnip id=18")==18);
    CHECK(xbg_pg::status_driver_id("")==0);
    CHECK(xbg_pg::status_driver_id(nullptr)==0);
    CHECK(xbg_pg::suffix("")=="");
    CHECK(xbg_pg::suffix("turnip27")=="-turnip27");
    CHECK(xbg_pg::suffix("Turnip v25.1/x")=="-Turnip_v25.1_x");

    // A v1 list (DOOM before 08.11, the test phone's two bans): set aside as .stale, logged, nothing applied.
    const std::string skip=dir+"/gfx-pipe-skip",lone=dir+"/gfx-lone-lane";
    put(skip,"098a126f92bf533a\n");put(lone,"cec755c4e27678b5\n");
    {std::unordered_map<std::string,std::string> out;logs.clear();
     CHECK(xbg_pg::read_list(skip,hdr,2,out,log_fn)==xbg_pg::READ_LEGACY);CHECK(out.empty());
     CHECK(!exists(skip) && exists(skip+".stale"));CHECK(get(skip+".stale")=="098a126f92bf533a\n");
     CHECK(logs.size()==1 && logged("[pipeguard] legacy: ") && logged("098a126f92bf533a") && logged("fresh chance"));
     logs.clear();CHECK(xbg_pg::read_list(skip,hdr,2,out,log_fn)==xbg_pg::READ_MISSING);CHECK(logs.empty());}   // logged once: the file is gone
    // set_aside_legacy: the unsuffixed v1 file under a custom driver goes; a v2 one (the system driver's) stays.
    {logs.clear();CHECK(xbg_pg::set_aside_legacy(lone,log_fn));CHECK(!exists(lone) && exists(lone+".stale"));CHECK(logged("cec755c4e27678b5"));
     put(lone,(xbg_pg::header("",1,uuid,16,"-")+"\nabc\tr\t1\tffff\n").c_str());CHECK(!xbg_pg::set_aside_legacy(lone,log_fn));CHECK(exists(lone));
     CHECK(!xbg_pg::set_aside_legacy(dir+"/none",log_fn));
     put(lone,"");CHECK(!xbg_pg::set_aside_legacy(lone,log_fn));CHECK(exists(lone));remove(lone.c_str());}

    // v2 round trip; an entry carries its spv.
    const std::string list=dir+"/gfx-pipe-skip-turnip27";
    CHECK(xbg_pg::append_list(list,hdr,2,"cec755c4e27678b5","crashed the driver's shader compiler\ton two boots","1111222233334444",1730000000));
    CHECK(xbg_pg::append_list(list,hdr,2,"927db1fc00000000","failed to build (result=-13)","aaaabbbbccccdddd",1730000001));
    CHECK(get(list)==hdr+"\ncec755c4e27678b5\tcrashed the driver's shader compiler on two boots\t1730000000\t1111222233334444\n927db1fc00000000\tfailed to build (result=-13)\t1730000001\taaaabbbbccccdddd\n");
    {std::unordered_map<std::string,std::string> out;logs.clear();
     CHECK(xbg_pg::read_list(list,hdr,2,out,log_fn)==xbg_pg::READ_OK);CHECK(out.size()==2);CHECK(out["cec755c4e27678b5"]=="1111222233334444");CHECK(logs.empty());
     CHECK(xbg_pg::applies(out["cec755c4e27678b5"],"1111222233334444",2));CHECK(!xbg_pg::applies(out["cec755c4e27678b5"],"9999222233334444",2));   // fixed pack: retried
     CHECK(xbg_pg::applies("","9999",1));}
    // Another header (driver update / other pack): .stale, not applied.
    {const std::string hdr2=xbg_pg::header("turnip27",0x5c0e2b,uuid,16,"0123456789abcdef");std::unordered_map<std::string,std::string> out;logs.clear();
     CHECK(xbg_pg::read_list(list,hdr2,2,out,log_fn)==xbg_pg::READ_STALE);CHECK(out.empty());CHECK(exists(list+".stale") && !exists(list));
     CHECK(logs.size()==1 && logged("[pipeguard] stale: ") && logged("driverVersion=0x5c0e2a"));
     // the next record writes a fresh list under the new header
     CHECK(xbg_pg::append_list(list,hdr2,2,"k","r","s",1));CHECK(get(list).rfind(hdr2+"\n",0)==0);remove(list.c_str());}
    // A line without the spv field (a v1 append into a v2 file) never applies in mode 2.
    {put(list,(hdr+"\nabc\nxyz\tr\t1\t\n").c_str());std::unordered_map<std::string,std::string> out;
     CHECK(xbg_pg::read_list(list,hdr,2,out,log_fn)==xbg_pg::READ_OK);CHECK(out.count("abc") && out["abc"].empty() && !xbg_pg::applies(out["abc"],"1234",2));CHECK(out.count("xyz") && out["xyz"].empty());remove(list.c_str());}
    // mode 1 (DM_PIPEGUARD=1): reads v1 and v2 files alike, writes plain keys.
    {put(skip,"098a126f92bf533a\n");std::unordered_map<std::string,std::string> out;
     CHECK(xbg_pg::read_list(skip,hdr,1,out,log_fn)==xbg_pg::READ_OK);CHECK(out.size()==1 && out.count("098a126f92bf533a"));CHECK(exists(skip));
     CHECK(xbg_pg::append_list(skip,hdr,1,"cec755c4e27678b5","x","s",1));CHECK(get(skip)=="098a126f92bf533a\ncec755c4e27678b5\n");
     put(skip,(hdr+"\nabc\tr\t1\tffff\n").c_str());out.clear();CHECK(xbg_pg::read_list(skip,"other",1,out,log_fn)==xbg_pg::READ_OK);CHECK(out.size()==1 && out.count("abc"));remove(skip.c_str());}
    {put(skip,"");std::unordered_map<std::string,std::string> out;CHECK(xbg_pg::read_list(skip,hdr,2,out,log_fn)==xbg_pg::READ_EMPTY);remove(skip.c_str());}

    // Markers and the two-deaths rule.
    CHECK(xbg_pg::marker_line("k","s",2)=="k\ts");CHECK(xbg_pg::marker_line("k","s",1)=="k");
    {auto m=xbg_pg::marker("k\ts");CHECK(m.first=="k" && m.second=="s");m=xbg_pg::marker("k");CHECK(m.first=="k" && m.second.empty());}
    CHECK(xbg_pg::judge("","s","",2)==xbg_pg::DEATH_NONE);
    CHECK(xbg_pg::judge("k\ts","s","",2)==xbg_pg::DEATH_FIRST);           // one death (MIUI kill, force-stop): suspect only
    CHECK(xbg_pg::judge("k\ts","s","k\ts",2)==xbg_pg::DEATH_SECOND);      // the next boot dies on it too: listed
    CHECK(xbg_pg::judge("k\ts","s","j\ts",2)==xbg_pg::DEATH_FIRST);       // another shader before: not in a row
    CHECK(xbg_pg::judge("k\ts","s","k\tt",2)==xbg_pg::DEATH_FIRST);       // same key, other SPIR-V
    CHECK(xbg_pg::judge("k\ts","t","k\ts",2)==xbg_pg::DEATH_SPV_CHANGED); // the pack changed since: not counted
    CHECK(xbg_pg::judge("k","s","",2)==xbg_pg::DEATH_SPV_CHANGED);        // a v1 marker under v2: not counted
    CHECK(xbg_pg::judge("k","","k",1)==xbg_pg::DEATH_SECOND);             // v1 still has its two-boot rule
    CHECK(xbg_pg::judge("k","","",1)==xbg_pg::DEATH_FIRST);
    CHECK(xbg_pg::tab_field("a\tb\tc",0)=="a" && xbg_pg::tab_field("a\tb\tc",2)=="c" && xbg_pg::tab_field("a\tb",3).empty() && xbg_pg::tab_field(nullptr,0).empty());

    // SHA-256 helper (FIPS 180-2 vectors, first 16 hex digits).
    {char out[17];CHECK(xbg_sha256_hex16("abc",3,out)==0 && std::string(out)=="ba7816bf8f01cfea");
     CHECK(xbg_sha256_hex16("",0,out)==0 && std::string(out)=="e3b0c44298fc1c14");
     const char *two="abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";CHECK(xbg_sha256_hex16(two,strlen(two),out)==0 && std::string(out)=="248d6a61d20638b8");
     std::string mil(1000000,'a');CHECK(xbg_sha256_hex16(mil.data(),mil.size(),out)==0 && std::string(out)=="cdc76e5c9914fb92");
     CHECK(xbg_sha256_hex16(nullptr,1,out)==-1);}

    for(const char *n:{"gfx-pipe-skip.stale","gfx-lone-lane.stale","gfx-pipe-skip-turnip27.stale"})remove((dir+"/"+n).c_str());rmdir(dir.c_str());
    if(failures){printf("pipeguard_test: %d failure(s)\n",failures);return 1;}
    printf("pipeguard_test: all checks passed\n");return 0;
}
