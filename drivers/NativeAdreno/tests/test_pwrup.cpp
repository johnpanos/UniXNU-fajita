/* SPDX-License-Identifier: MIT */
/* Exact power-up list layout over synthetic register pairs. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoPwrup.h"
#include "NativeAdrenoPwrupRegs.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main()
{
    const uint32_t ir[]={0x10,0x20},iv[]={0xaa,0xbb};
    const uint32_t pr[]={0x30},pv[]={0xcc};
    const uint32_t dt[]={0x1,0x40,0xdd,0x2,0x50,0xee};
    NativeAdrenoPwrupCounts counts{2,1,2};
    CHECK(NativeAdrenoPwrupList::requiredBytes(counts)==16+3*8+2*12);
    uint8_t image[128];
    std::memset(image,0x5a,sizeof(image));
    size_t written=0;
    CHECK(NativeAdrenoPwrupList::pack(counts,ir,iv,pr,pv,dt,image,sizeof(image),&written)==kIOReturnSuccess);
    CHECK(written==16+24+24);
    const uint32_t expected[]={0,0,0,0x00020102,0x10,0xaa,0x20,0xbb,0x30,0xcc,1,0x40,0xdd,2,0x50,0xee};
    CHECK(!std::memcmp(image,expected,sizeof(expected)));
    CHECK(image[written]==0x5a); /* no write past the image */
    /* Capacity and count refusals leave everything untouched. */
    size_t before=0;
    std::memset(image,0x5a,sizeof(image));
    CHECK(NativeAdrenoPwrupList::pack(counts,ir,iv,pr,pv,dt,image,written-1,&before)==kIOReturnNoResources);
    CHECK(!before && image[0]==0x5a && image[sizeof(image)-1]==0x5a);
    CHECK(NativeAdrenoPwrupList::pack(counts,nullptr,iv,pr,pv,dt,image,sizeof(image),&before)==kIOReturnBadArgument);
    CHECK(NativeAdrenoPwrupList::pack(counts,ir,iv,pr,pv,nullptr,image,sizeof(image),&before)==kIOReturnBadArgument);
    CHECK(NativeAdrenoPwrupList::pack(counts,ir,iv,pr,pv,dt,nullptr,sizeof(image),&before)==kIOReturnBadArgument);
    CHECK(NativeAdrenoPwrupList::pack(counts,ir,iv,pr,pv,dt,image,sizeof(image),nullptr)==kIOReturnBadArgument);
    NativeAdrenoPwrupCounts big{0x100,0,0};
    CHECK(NativeAdrenoPwrupList::pack(big,ir,iv,pr,pv,dt,image,sizeof(image),&before)==kIOReturnBadArgument);
    NativeAdrenoPwrupCounts zero{0,0,0};
    CHECK(NativeAdrenoPwrupList::requiredBytes(zero)==16);
    CHECK(NativeAdrenoPwrupList::pack(zero,nullptr,nullptr,nullptr,nullptr,nullptr,image,16,&before)==kIOReturnSuccess);
    CHECK(before==16 && !image[12] && !image[13] && !image[14] && !image[15]);
    /* A740 builder: sixteen live reads packed as the preemption list. */
    {
        struct Reader {unsigned calls;unsigned fail_at;} reader{0,0};
        auto read=[](void *cookie,uint32_t reg,uint32_t *value)->int {
            Reader *state=static_cast<Reader *>(cookie);
            ++state->calls;
            if(state->calls==state->fail_at)return 1;
            *value=reg^0xf0f0;return 0;
        };
        uint8_t built[256];
        size_t built_bytes=0;
        CHECK(NativeAdrenoPwrupList::buildA740(read,&reader,built,sizeof(built),&built_bytes)==kIOReturnSuccess);
        CHECK(reader.calls==16 && built_bytes==16+16*8);
        CHECK(built[12]==0 && built[13]==16 && built[14]==0 && built[15]==0);
        const uint32_t *slots=reinterpret_cast<const uint32_t *>(built+16);
        for(unsigned i=0;i<16;++i)CHECK(slots[i*2]==NativeAdrenoA740PwrupRegs[i] &&
            slots[i*2+1]==(NativeAdrenoA740PwrupRegs[i]^0xf0f0));
        Reader failing{0,9};size_t untouched=77;
        CHECK(NativeAdrenoPwrupList::buildA740(read,&failing,built,sizeof(built),&untouched)==kIOReturnIOError);
        CHECK(untouched==77);
        CHECK(NativeAdrenoPwrupList::buildA740(nullptr,nullptr,built,sizeof(built),&untouched)==kIOReturnBadArgument);
    }
    std::printf("PASS: 1 exact power-up list image, 1 A740 live-read list, 1 capacity refusal and 5 argument/count refusals\n");
}
