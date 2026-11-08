// [ETR perf] DM_GFX_REPORT_FAST (xbg_report_fast.h): the merge walk must leave every epoch exactly as the former epochs x reports fold does,
// over random record / retire / resolve sequences shaped like the engine's (reports appended in record order with the current epoch and the
// epoch's size, erased from the front once known; epochs only grow; queries become known out of order). The verify helper must report 0
// on such sequences and must catch an unsorted list.
// Build: g++ -std=c++17 -O1 -g -fsanitize=address,undefined -UNDEBUG report_fast_test.cpp -o /tmp/report_fast_test && /tmp/report_fast_test
#ifdef NDEBUG
#error Build this regression with assertions enabled (-UNDEBUG).
#endif
#include "../xbg_report_fast.h"
#include <cassert>
#include <cstdio>
#include <deque>
#include <map>
#include <random>
#include <vector>

// The engine's EpochState (xbg_game.cpp), copied: queries known out of order, prefix sum folded once, consumed entries dropped.
struct EpochQuery{uint64_t value;bool known;};
struct EpochState{std::deque<EpochQuery> q;uint64_t base=0,known_upto=0,summed_upto=0,sum=0;uint64_t size()const{return base+q.size();}
    void advance_known(){while(known_upto<size() && q[known_upto-base].known)++known_upto;}
    uint64_t prefix(uint64_t count){while(summed_upto<count && summed_upto<size()){sum+=q[summed_upto-base].value;++summed_upto;}return sum;}
    void trim(){while(base<summed_upto && !q.empty()){q.pop_front();++base;}}
    bool operator==(const EpochState &o)const{if(base!=o.base||known_upto!=o.known_upto||summed_upto!=o.summed_upto||sum!=o.sum||q.size()!=o.q.size())return false;
        for(size_t i=0;i<q.size();i++)if(q[i].value!=o.q[i].value||q[i].known!=o.q[i].known)return false;return true;}};
struct Report{uint64_t epoch;size_t count;bool ts_known;};

struct World{
    uint64_t current=0;std::map<uint64_t,EpochState> epochs;std::vector<Report> pending;
    struct Ref{uint64_t epoch,index;};std::vector<Ref> unknown;   // queries not known yet
};
// resolve_known_reports' first half: write the leading reports whose timestamp and samples are known, then fold (fast or reference).
static size_t resolve(World &w,bool fast){
    size_t done=0;
    for(;done<w.pending.size();done++){const auto &r=w.pending[done];if(!r.ts_known)break;auto e=w.epochs.find(r.epoch);bool known=true;
        if(e!=w.epochs.end()){e->second.advance_known();known=e->second.known_upto>=std::min<uint64_t>(r.count,e->second.size());}if(!known)break;
        if(e!=w.epochs.end()){e->second.advance_known();(void)e->second.prefix(r.count);}}
    if(done)w.pending.erase(w.pending.begin(),w.pending.begin()+done);
    if(fast)xbg_repfast_fold(w.pending,w.epochs,w.current);else xbg_repfast_fold_ref(w.pending,w.epochs,w.current);
    return done;
}

int main(){
    std::mt19937_64 rng(12345);uint64_t total_written=0,resolves=0,checks=0;
    for(int run=0;run<400;run++){
        World a,b;   // a: merge walk, b: reference; driven by the same random choices
        const int steps=200+int(rng()%800);
        for(int s=0;s<steps;s++){
            const unsigned op=unsigned(rng()%100);
            if(op<10){++a.current;++b.current;}                                   // OP_RESET_COUNTER
            else if(op<55){for(World *w:{&a,&b}){auto &e=w->epochs[w->current];e.q.push_back({0,false});w->unknown.push_back({w->current,e.size()-1});}}   // a counted draw's query
            else if(op<70){const bool ts=(rng()&1)!=0;for(World *w:{&a,&b}){Report r{w->current,size_t(w->epochs[w->current].size()),ts};w->pending.push_back(r);}}       // OP_REPORT_COUNTER
            else if(op<85){   // a slot retires: some queries become known (values), timestamps of reports known
                const size_t n=a.unknown.size();assert(n==b.unknown.size());
                const uint64_t seed=rng();std::mt19937_64 pick(seed);
                std::vector<size_t> idx;for(size_t i=0;i<n;i++)if(pick()%3)idx.push_back(i);
                for(World *w:{&a,&b}){std::mt19937_64 val(seed^0x9e37);
                    for(size_t i:idx){auto &u=w->unknown[i];auto e=w->epochs.find(u.epoch);uint64_t v=val()%5;if(e!=w->epochs.end() && u.index>=e->second.base && u.index<e->second.size()){auto &q=e->second.q[u.index-e->second.base];q.value=v;q.known=true;}}
                    std::vector<World::Ref> keep;for(size_t i=0;i<n;i++)if(!std::binary_search(idx.begin(),idx.end(),i))keep.push_back(w->unknown[i]);w->unknown.swap(keep);
                    for(auto &e:w->epochs)e.second.advance_known();
                    for(auto &r:w->pending)r.ts_known=true;}}
            else{   // resolve_known_reports
                uint64_t bad=xbg_repfast_verify(a.pending,a.epochs,[](uint64_t,bool,uint64_t,bool,uint64_t){});assert(bad==0);++checks;
                {bool same=a.epochs.size()==b.epochs.size() && a.pending.size()==b.pending.size();if(same)for(auto ia=a.epochs.begin(),ib=b.epochs.begin();ia!=a.epochs.end();++ia,++ib)if(ia->first!=ib->first || !(ia->second==ib->second))same=false;
                 if(!same){std::printf("diverged before resolve run=%d step=%d\n",run,s);return 1;}}
                const size_t da=resolve(a,true),db=resolve(b,false);if(da!=db){std::printf("run=%d step=%d da=%zu db=%zu\n",run,s,da,db);return 1;}
                total_written+=da;++resolves;
                assert(a.epochs.size()==b.epochs.size());
                for(auto ia=a.epochs.begin(),ib=b.epochs.begin();ia!=a.epochs.end();++ia,++ib){assert(ia->first==ib->first);assert(ia->second==ib->second);}
                assert(a.pending.size()==b.pending.size());}
            if(op>=95 && s%7==0){a.pending.clear();b.pending.clear();   // resolve_pending: everything written, every older epoch dropped
                for(World *w:{&a,&b})for(auto it=w->epochs.begin();it!=w->epochs.end();)if(it->first!=w->current)it=w->epochs.erase(it);else ++it;}
        }
    }
    // the verify catches a list that breaks the ordering assumption (a report of an older epoch after a newer one)
    {std::vector<Report> p{{1,3,true},{2,1,true},{1,1,true}};std::map<uint64_t,EpochState> e;e[1];e[2];
     const uint64_t bad=xbg_repfast_verify(p,e,[](uint64_t,bool,uint64_t,bool,uint64_t){});assert(bad==1);}
    std::printf("report_fast_test: OK (%llu resolves, %llu reports written, %llu verify checks)\n",(unsigned long long)resolves,(unsigned long long)total_written,(unsigned long long)checks);
    return 0;
}
