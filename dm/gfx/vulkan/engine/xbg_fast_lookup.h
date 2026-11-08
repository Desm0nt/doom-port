#pragma once
// [ETR perf, KB PERF-E09] DM_GFX_FAST_LOOKUPS (zero-copy part), ported from the DOOM Eternal port (4a7043d: zc::find / mark / tail).
// zc::find walked the zero-copy chunk table (~26 chunks of 64 MB for DOOM's 1.6 GB of pools) on every snapshot, uniform and index
// view: ~11.6k lookups a frame (NVN PERF zerocopy hits=). This index answers them from a copy of the table sorted by start plus a
// most-recently-used chunk (consecutive draws read the same pool chunk). DOOM only ever appends chunks (map_chunk), so the copy is
// rebuilt when the chunk count moves. The table walk returns the FIRST chunk in table order that holds the address; the sorted copy
// is exact only while no two chunks overlap, so an overlap found at a rebuild turns the index off (ok=false) and every lookup takes
// the table walk again. Not thread-safe: the translation thread only (engine mutex), like every other zc user.
// Template on the chunk type so tests/fast_lookup_test.cpp runs the production code: Chunk needs .lo (uintptr_t) and .n (size_t).
#include <algorithm>
#include <cstddef>
#include <cstdint>

struct XbgSortedChunks{
    struct R{uintptr_t lo,end;int idx;};
    R s[512];int n=0,built_for=-1;bool ok=false;int mru=-1;   // built_for: table count of the copy (-1 none); mru: index into s
    uint64_t rebuilds=0,mru_hits=0,searches=0,overlaps=0;     // counters (NVN PERF fastlook)
    template<class Chunk> void build(const Chunk *table,int count){
        n=0;mru=-1;built_for=count;ok=true;
        for(int i=0;i<count && i<512;i++)if(table[i].n)s[n++]={uintptr_t(table[i].lo),uintptr_t(table[i].lo)+uintptr_t(table[i].n),i};
        std::sort(s,s+n,[](const R &a,const R &b){return a.lo<b.lo;});
        for(int i=1;i<n;i++)if(s[i].lo<s[i-1].end){ok=false;++overlaps;break;}
        ++rebuilds;
    }
    template<class Chunk> bool fresh(const Chunk *table,int count){if(count!=built_for)build(table,count);return ok;}
    // the first sorted entry whose end is above address (the only one that can hold it, or the first one overlapping a range from it)
    int first(uint64_t address)const{int lo=0,hi=n;while(lo<hi){const int mid=(lo+hi)>>1;if(s[mid].end>address)hi=mid;else lo=mid+1;}return lo;}
    // table index of the chunk holding address, or -1 (call fresh() first; only meaningful while ok)
    int holder(uint64_t address){
        if(mru>=0 && address>=s[mru].lo && address<s[mru].end){++mru_hits;return s[mru].idx;}
        ++searches;const int k=first(address);if(k<n && s[k].lo<=address){mru=k;return s[k].idx;}return -1;
    }
};
// The table walk zc::find did (and does while the index is off): first chunk in table order holding address.
template<class Chunk> int xbg_chunk_walk(const Chunk *table,int count,uint64_t address){
    for(int i=0;i<count;i++){const Chunk &c=table[i];if(address>=c.lo && address-c.lo<c.n)return i;}return -1;
}
