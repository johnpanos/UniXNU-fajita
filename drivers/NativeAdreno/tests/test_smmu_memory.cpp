/* SPDX-License-Identifier: MIT */
/* Real XNU memory adapter plus real table builder/auditor; IOKit peers only. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoSmmuMemory.h"
#include <cstdio>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{CHECK(out);*out={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));
    CHECK(op==NativeAdrenoClean);++fixture.cache_calls;
    return fixture.cache_calls==fixture.cache_fail_call?kIOReturnIOError:kIOReturnSuccess;
}
int main()
{
    unsigned profiles=0;
    for(unsigned bits=32;bits<=48;++bits)for(unsigned upper=0;upper<2;++upper) {
        NativeAdrenoMemory memory;ArmSmmu4KLiveArena arena;ArmSmmu4KLiveState state{};
        ArmSmmu4KLiveVisibility visibility;
        CHECK(memory.allocate(8*4096,40,true,8*4096)==kIOReturnSuccess);
        uint64_t va=(upper?UINT64_MAX-(UINT64_C(1)<<bits)+1:0)+0x1ff000;
        CHECK(NativeAdrenoPrepareSmmuMemory(&memory,bits,40,va,8192,upper,16,&arena,&state,&visibility)==kIOReturnSuccess);
        CHECK(arena.config.table_physical==memory.spans()[0].physical && arena.mapping==memory.mapping());
        CHECK(state.prepared && !state.poisoned && memory.deviceReferences()==1);
        CHECK(memory.release()==kIOReturnBusy);
        ArmSmmu4KLiveResult audit;
        CHECK(ArmSmmu4KLiveAudit(&arena,&state,nullptr,0,&audit)==ArmSmmu4KSuccess);
        CHECK(!visibility.publish_table_page(visibility.cookie,arena.mapping,4096));
        CHECK(visibility.publish_table_page(visibility.cookie,arena.mapping+memory.bytes()/8,4096));
        CHECK(visibility.publish_table_page(visibility.cookie,arena.mapping,8192));
        CHECK(visibility.publish_table_page(visibility.cookie,arena.mapping+1,4096));
        /* No context was ever enabled in this test; retire the software hold. */
        CHECK(memory.retireAfterQuiescence()==kIOReturnSuccess && memory.release()==kIOReturnSuccess);
        CHECK(fixture.live.empty() && fixture.metadata.empty());++profiles;
    }
    unsigned refusals=0;
    for(unsigned failure=0;failure<4;++failure) {
        fixture=MemoryFixture{};
        NativeAdrenoMemory memory;ArmSmmu4KLiveArena arena;ArmSmmu4KLiveState state{};
        ArmSmmu4KLiveVisibility visibility;
        if(failure==0)fixture.mode=Scattered;
        CHECK(memory.allocate((failure==1?1:8)*4096,40,false,8*4096)==kIOReturnSuccess);
        if(failure==2)fixture.cache_fail_call=2;
        if(failure==3)CHECK(memory.holdForDevice()==kIOReturnSuccess);
        IOReturn expected=failure==0?kIOReturnUnsupported:failure==1?kIOReturnNoResources:
            failure==2?kIOReturnIOError:kIOReturnBusy;
        CHECK(NativeAdrenoPrepareSmmuMemory(&memory,48,40,0xffff0000001ff000,8192,1,16,&arena,&state,&visibility)==expected);
        CHECK(!visibility.publish_table_page && state.poisoned==(failure==2));
        if(failure==3)CHECK(memory.retireAfterQuiescence()==kIOReturnSuccess);
        CHECK(memory.release()==kIOReturnSuccess && fixture.live.empty() && fixture.metadata.empty());++refusals;
    }
    std::printf("PASS: %u native allocator/table-pool bindings and %u capacity/scatter/publication/ownership refusals; no hardware context enabled\n",profiles,refusals);
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
