// [ETR perf] DM_GFX_FAST_LOOKUPS (xbg_fast_lookup.h): the sorted zero-copy chunk index must answer every address exactly as the table walk
// (first chunk in table order holding it) while chunks do not overlap, follow appended chunks (rebuild on a count change), and turn itself
// off when two chunks overlap (the walk then keeps answering).
// Build: g++ -std=c++17 -O1 -g -fsanitize=address,undefined -UNDEBUG fast_lookup_test.cpp -o /tmp/fast_lookup_test && /tmp/fast_lookup_test
#ifdef NDEBUG
#error Build this regression with assertions enabled (-UNDEBUG).
#endif
#include "../xbg_fast_lookup.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

struct Chunk{uintptr_t lo=0;size_t n=0;};

int main(){
    std::mt19937_64 rng(7);uint64_t probes=0;
    for(int run=0;run<200;run++){
        static Chunk table[512];int count=0;XbgSortedChunks idx;
        // pools of 1..4 chunks of up to 64 MB at random, non-overlapping guest addresses, appended in random address order
        std::vector<uintptr_t> starts;uintptr_t at=0x10000000ull;for(int i=0;i<40;i++){at+=uintptr_t(1+rng()%8)<<24;starts.push_back(at);at+=uintptr_t(4)<<26;}
        for(size_t i=starts.size();i>1;i--)std::swap(starts[i-1],starts[rng()%i]);
        for(uintptr_t s:starts){
            const int k=1+int(rng()%4);for(int c=0;c<k && count<512;c++){table[count].lo=s+uintptr_t(c)*(64u<<20);table[count].n=c+1<k?(64u<<20):size_t(1+rng()%(64u<<20));++count;}
            for(int p=0;p<300;p++){   // probes inside, at the edges and between chunks
                const Chunk &c=table[rng()%count];uint64_t a;
                switch(rng()%4){case 0:a=c.lo;break;case 1:a=c.lo+c.n-1;break;case 2:a=c.lo+c.n;break;default:a=c.lo+rng()%(c.n*2+1)-c.n/2;}
                const bool ok=idx.fresh(table,count);assert(ok);
                const int w=xbg_chunk_walk(table,count,a),h=idx.holder(a);assert(w==h);++probes;}
        }
        assert(idx.rebuilds>=1);
    }
    // overlap: index off, the caller walks
    {Chunk t[3];t[0]={0x1000000,0x200000};t[1]={0x5000000,0x100000};t[2]={0x1100000,0x100000};XbgSortedChunks idx;assert(!idx.fresh(t,3));assert(idx.overlaps==1);
     assert(xbg_chunk_walk(t,3,0x1100000)==0);}
    // MRU: repeated lookups in one chunk are hits
    {Chunk t[2];t[0]={0x1000000,0x100000};t[1]={0x3000000,0x100000};XbgSortedChunks idx;assert(idx.fresh(t,2));
     for(int i=0;i<10;i++)assert(idx.holder(0x3000000+i*64)==1);assert(idx.mru_hits==9 && idx.searches==1);assert(idx.holder(0x2000000)==-1);}
    std::printf("fast_lookup_test: OK (%llu probes)\n",(unsigned long long)probes);
    return 0;
}
