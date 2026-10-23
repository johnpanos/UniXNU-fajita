/* SPDX-License-Identifier: MIT */
/* Real bounds-checked adapter over fake windows; no GMU is touched. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoGmuPlatform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static uint32_t core[0x1000],rscc[0x400],pdc[0x400],gpu[0x1000],cx[0x400];
static NativeAdrenoGmuWindow windows[ShengGmuWindows]={
    {reinterpret_cast<volatile uint8_t *>(core),sizeof(core)},
    {reinterpret_cast<volatile uint8_t *>(rscc),sizeof(rscc)},
    {reinterpret_cast<volatile uint8_t *>(pdc),sizeof(pdc)},
    {reinterpret_cast<volatile uint8_t *>(gpu),sizeof(gpu)},
    {reinterpret_cast<volatile uint8_t *>(cx),sizeof(cx)}};
static uint64_t fake_now=1000;
static unsigned waits=0;
static int now_us(void *cookie,uint64_t *value){CHECK(cookie==&fake_now && value);*value=fake_now;return 0;}
static int wait_us(void *cookie,unsigned us){CHECK(cookie==&fake_now);waits++;fake_now+=us;return 0;}
int main()
{
    NativeAdrenoGmuPlatform platform;
    ShengGmuIo io{};
    CHECK(platform.bind(windows,&io)==kIOReturnSuccess);
    CHECK(io.cookie==&platform && io.read32 && io.write32 && io.barrier);
    CHECK(io.write32(io.cookie,ShengGmuCore,0x10,0xabcdef01)==0);
    CHECK(core[4]==0xabcdef01);
    uint32_t value=0;
    CHECK(io.read32(io.cookie,ShengGmuCore,0x10,&value)==0 && value==0xabcdef01);
    CHECK(io.write32(io.cookie,ShengGmuRscc,0x4,7)==0 && rscc[1]==7);
    CHECK(io.write32(io.cookie,ShengGmuPdc,0x0,8)==0 && pdc[0]==8);
    CHECK(io.write32(io.cookie,ShengGmuGpu,0x8,9)==0 && gpu[2]==9);
    CHECK(io.write32(io.cookie,ShengGmuCxMisc,0xc,10)==0 && cx[3]==10);
    CHECK(io.barrier(io.cookie)==0);
    /* Out-of-window, misaligned and unknown windows fail without touching. */
    CHECK(io.read32(io.cookie,ShengGmuCore,sizeof(core),&value)==1);
    CHECK(io.read32(io.cookie,ShengGmuCore,sizeof(core)-4,&value)==0);
    CHECK(io.write32(io.cookie,ShengGmuCore,sizeof(core),1)==1);
    CHECK(io.write32(io.cookie,ShengGmuCore,0x2,1)==1);
    CHECK(io.write32(io.cookie,ShengGmuWindows,0,1)==1);
    CHECK(io.read32(io.cookie,ShengGmuWindows,0,&value)==1);
    CHECK(io.read32(nullptr,ShengGmuCore,0,&value)==1);
    CHECK(io.read32(io.cookie,ShengGmuCore,0,nullptr)==1);
    CHECK(io.write32(nullptr,ShengGmuCore,0,1)==1);
    CHECK(io.barrier(nullptr)==1);
    /* Binding refuses missing, empty or misaligned windows. */
    NativeAdrenoGmuWindow bad[ShengGmuWindows];
    std::memcpy(bad,windows,sizeof(bad));
    bad[2].base=nullptr;
    CHECK(platform.bind(bad,&io)==kIOReturnBadArgument);
    std::memcpy(bad,windows,sizeof(bad));
    bad[3].bytes=0;
    CHECK(platform.bind(bad,&io)==kIOReturnBadArgument);
    std::memcpy(bad,windows,sizeof(bad));
    bad[4].bytes=sizeof(cx)-2;
    CHECK(platform.bind(bad,&io)==kIOReturnBadArgument);
    CHECK(platform.bind(nullptr,&io)==kIOReturnBadArgument);
    CHECK(platform.bind(windows,nullptr)==kIOReturnBadArgument);
    /* Services: real SHA-256 plus injected clock callbacks. */
    NativeAdrenoGmuTime time{&fake_now,now_us,wait_us};
    ShengGmuServices services{};
    CHECK(NativeAdrenoGmuPlatform::services(&time,&services)==kIOReturnSuccess);
    CHECK(services.cookie==&fake_now && services.now_us && services.wait_us);
    uint64_t observed=0;
    CHECK(services.now_us(services.cookie,&observed)==0 && observed==1000);
    CHECK(services.wait_us(services.cookie,5)==0);
    CHECK(waits==1 && fake_now==1005);
    uint8_t digest[32];
    CHECK(services.sha256(services.cookie,"abc",3,digest)==0);
    const uint8_t expected[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,
        0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    CHECK(!std::memcmp(digest,expected,32));
    CHECK(services.sha256(services.cookie,nullptr,3,digest)==1);
    CHECK(services.sha256(services.cookie,"",0,digest)==1);
    CHECK(services.sha256(services.cookie,"abc",3,nullptr)==1);
    NativeAdrenoGmuTime bad_time{&fake_now,nullptr,wait_us};
    CHECK(NativeAdrenoGmuPlatform::services(&bad_time,&services)==kIOReturnBadArgument);
    CHECK(NativeAdrenoGmuPlatform::services(&time,nullptr)==kIOReturnBadArgument);
    std::printf("PASS: 1 five-window adapter with real accesses, 11 bounds/argument refusals, 1 service binding and 1 SHA-256 vector over fake windows\n");
}
