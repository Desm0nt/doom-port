#pragma once
// [ETR perf, KB PERF-E02] DM_GFX_REPORT_FAST, ported from the DOOM Eternal port (src-cmp16 526920b, d74cfb0: resolve_known_reports).
// resolve_known_reports() folds the known occlusion queries of every epoch into its running sum up to the earliest count a pending
// report still needs, then drops the epochs no pending report names. The former DOOM fold paired every epoch with every pending
// report, twice (epochs x reports; Eternal: ~3000 epochs and ~4300 reports a frame). pending_reports is only appended in record
// order and erased from the front (resolve_pending clears it), a report names the epoch current when it was recorded and
// counter_epoch only grows, so the reports are sorted by epoch and, inside one epoch, by count (the epoch's size when recorded,
// which never shrinks): the earliest needed count of an epoch is its first pending report's. One merge walk of the two ordered
// sequences replaces the pairing, without allocations. Verify (DM_GFX_REPORT_FAST=2) recomputes the minimum with a map over every
// pending report and counts mismatches (NVN PERF reports ... bad=).
// Templates so the host test (tests/report_fast_test.cpp) runs the production code on its own epoch / report types:
//   Reports: random access, elements with .epoch and .count; Epochs: ordered map epoch -> state with .known_upto, .prefix(n), .trim().
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>

// The merge walk (DM_GFX_REPORT_FAST=1).
template<class Reports,class Epochs> void xbg_repfast_fold(const Reports &pending,Epochs &epochs,uint64_t current){
    size_t j=0;
    for(auto it=epochs.begin();it!=epochs.end();){
        while(j<pending.size() && pending[j].epoch<it->first)++j;
        const bool needed=j<pending.size() && pending[j].epoch==it->first;
        uint64_t limit=it->second.known_upto;if(needed)limit=std::min<uint64_t>(limit,pending[j].count);
        it->second.prefix(limit);it->second.trim();
        if(needed || it->first==current)++it;else it=epochs.erase(it);
    }
}
// The former DOOM fold (DM_GFX_REPORT_FAST=0): every epoch against every pending report.
template<class Reports,class Epochs> void xbg_repfast_fold_ref(const Reports &pending,Epochs &epochs,uint64_t current){
    for(auto &e:epochs){uint64_t limit=e.second.known_upto;for(auto &r:pending)if(r.epoch==e.first)limit=std::min<uint64_t>(limit,r.count);e.second.prefix(limit);e.second.trim();}
    for(auto it=epochs.begin();it!=epochs.end();){bool used=it->first==current;for(auto &r:pending)if(r.epoch==it->first){used=true;break;}
        if(used)++it;else it=epochs.erase(it);}
}
// Verify (DM_GFX_REPORT_FAST=2, before the walk): per epoch, the merge walk's first pending report against the minimum count of a map
// over every pending report. bad(epoch, fast_has, fast_count, map_has, map_count) is called for each mismatch; returns the mismatches.
template<class Reports,class Epochs,class Bad> uint64_t xbg_repfast_verify(const Reports &pending,const Epochs &epochs,Bad &&bad){
    std::map<uint64_t,uint64_t> ref;
    for(auto &r:pending){auto f=ref.find(r.epoch);if(f==ref.end())ref.emplace(r.epoch,uint64_t(r.count));else if(uint64_t(r.count)<f->second)f->second=uint64_t(r.count);}
    uint64_t n=0;size_t j=0;
    for(auto &e:epochs){
        while(j<pending.size() && pending[j].epoch<e.first)++j;
        const bool has=j<pending.size() && pending[j].epoch==e.first;auto f=ref.find(e.first);const bool mhas=f!=ref.end();
        if(has!=mhas || (has && uint64_t(pending[j].count)!=f->second)){++n;bad(e.first,has,has?uint64_t(pending[j].count):0,mhas,mhas?f->second:0);}
    }
    return n;
}
