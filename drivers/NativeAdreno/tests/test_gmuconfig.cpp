/* SPDX-License-Identifier: MIT */
/* Real assembler over host doubles; only structure fields are checked. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoGmuConfig.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *state)
{CHECK(state);*state={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{CHECK(mapping && bytes && op<=NativeAdrenoInvalidate);return kIOReturnSuccess;}
int main()
{
    NativeAdrenoGmuAllocator allocator;
    CHECK(allocator.allocate(NativeAdrenoGmuDefaultSizes(0x10000))==kIOReturnSuccess);
    uint8_t image[16]={1,2,3};
    NativeAdrenoFirmwareView firmware{image,sizeof(image),{}};
    ShengGmuConfig config{};
    CHECK(NativeAdrenoAssembleGmuConfig(&firmware,&allocator,0x43050a01u,100,&config)==kIOReturnSuccess);
    const NativeAdrenoGmuMemoryLayout &layout=allocator.layout();
    CHECK(config.chip_id==0x43050a01u && config.poll_budget==100);
    CHECK(config.firmware==image && config.firmware_bytes==sizeof(image));
    CHECK(config.icache.cpu==static_cast<volatile uint32_t *>(allocator.icache().mapping()));
    CHECK(config.icache.bytes==0xffc000 && config.icache.iova==layout.icache_iova);
    CHECK(config.dummy.bytes==0x4000 && config.dummy.iova==layout.dummy_iova);
    CHECK(config.debug.bytes==0x7000 && config.debug.iova==layout.debug_iova);
    CHECK(config.log.bytes==0x4000 && config.log.iova==layout.log_iova);
    CHECK(config.hfi.bytes==0x4000 && config.hfi.iova==layout.hfi_iova);
    /* Refusals leave no partially assembled config. */
    ShengGmuConfig empty{};
    CHECK(NativeAdrenoAssembleGmuConfig(nullptr,&allocator,1,100,&empty)==kIOReturnBadArgument);
    CHECK(NativeAdrenoAssembleGmuConfig(&firmware,nullptr,1,100,&empty)==kIOReturnBadArgument);
    CHECK(NativeAdrenoAssembleGmuConfig(&firmware,&allocator,0,100,&empty)==kIOReturnBadArgument);
    CHECK(NativeAdrenoAssembleGmuConfig(&firmware,&allocator,1,0,&empty)==kIOReturnBadArgument);
    CHECK(NativeAdrenoAssembleGmuConfig(&firmware,&allocator,1,1025,&empty)==kIOReturnBadArgument);
    CHECK(NativeAdrenoAssembleGmuConfig(&firmware,&allocator,1,100,nullptr)==kIOReturnBadArgument);
    NativeAdrenoFirmwareView missing{};
    CHECK(NativeAdrenoAssembleGmuConfig(&missing,&allocator,1,100,&empty)==kIOReturnBadArgument);
    CHECK(empty.chip_id==0 && empty.firmware==nullptr);
    /* A profile with non-startup payload sizes is refused, not assembled. */
    NativeAdrenoGmuAllocator small;
    CHECK(small.allocate({0x4000,0x1000,0x2000,0x1000,0x1000,0x10000})==kIOReturnSuccess);
    CHECK(NativeAdrenoAssembleGmuConfig(&firmware,&small,1,100,&empty)==kIOReturnBadArgument);
    CHECK(empty.firmware==nullptr);
    CHECK(small.release()==kIOReturnSuccess && allocator.release()==kIOReturnSuccess);
    CHECK(fixture.live.empty());
    std::printf("PASS: 1 assembled GMU config, 7 argument refusals and 1 non-startup-profile refusal over host doubles\n");
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change of user BOs has no host equivalent (no host page is touched). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;return kIOReturnSuccess;
}
