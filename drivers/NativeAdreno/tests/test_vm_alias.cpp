/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoVmAlias.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Range {uint64_t physical,bytes;};
static uint64_t randomState=0x28372934;
static uint64_t next() {randomState^=randomState<<13;randomState^=randomState>>7;randomState^=randomState<<17;return randomState;}
static bool oracle(const std::vector<Range> &ranges,uint64_t start,uint64_t bytes) {
    for(const auto &r:ranges)if(start<r.physical+r.bytes && r.physical<start+bytes)return true;
    return false;
}
int main() {
    unsigned cases=0,queries=0;
    for(unsigned profile=0;profile<2048;++profile) {
        std::vector<Range> ranges;
        const unsigned count=profile%65;
        for(unsigned i=0;i<count;++i)ranges.push_back({(next()%128)*4096,(1+next()%64)*4096});
        // Explicit nesting plus a gap catches a binary search using starts alone.
        if(profile==1)ranges={{0x1000,0x10000},{0x2000,0x1000},{0x3000,0x1000},{0x20000,0x1000}};
        if(profile==2)ranges={{0x1000,0x1000},{0x1000,0x1000},{0x2000,0x1000},{0x4000,0x1000}};
        const auto original=ranges;
        // Filtered index subsets exercise caller-provided nonconsecutive indices.
        std::vector<uint32_t> scratch(ranges.size()+2,0xdeadbeef);
        std::vector<Range> selected;
        size_t n=0;
        for(size_t i=0;i<ranges.size();++i)if(profile%3 || i%2){scratch[1+n++]=(uint32_t)i;selected.push_back(ranges[i]);}
        NativeAdrenoPhysicalIndex index(ranges.data(),scratch.data()+1,n);
        for(unsigned q=0;q<256;++q) {
            uint64_t start=q*4096,bytes=(1+(q%7))*4096;
            CHECK(index.overlaps(start,bytes)==oracle(selected,start,bytes));++queries;
        }
        CHECK(scratch.front()==0xdeadbeef && scratch.back()==0xdeadbeef);
        CHECK(ranges.size()==original.size() && (!ranges.size() || !std::memcmp(ranges.data(),original.data(),ranges.size()*sizeof(Range))));
        ++cases;
    }
    // Scale well beyond the bounded oracle profiles: distinct spans with gaps.
    std::vector<Range> ranges(65536);std::vector<uint32_t> order(ranges.size());
    for(size_t i=0;i<ranges.size();++i){ranges[i]={0x100000+(ranges.size()-1-i)*8192,4096};order[i]=(uint32_t)i;}
    NativeAdrenoPhysicalIndex index(ranges.data(),order.data(),order.size());
    for(size_t i=0;i<ranges.size();++i){CHECK(index.overlaps(0x100000+i*8192,4096));CHECK(!index.overlaps(0x101000+i*8192,4096));}
    std::printf("PASS: physical alias index %u oracle profiles / %u queries; 65536 reversed spans and exact gap boundaries\n",cases,queries);
}
