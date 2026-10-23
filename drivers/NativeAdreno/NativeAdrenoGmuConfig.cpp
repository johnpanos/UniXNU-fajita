/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGmuConfig.h"
#include <IOKit/IOLib.h>
enum : uint64_t {
    IcacheBytes=0xffc000, DummyBytes=0x4000, DebugBytes=0x7000,
    LogBytes=0x4000, HfiBytes=0x4000
};
static bool matches(const NativeAdrenoMemory &memory,uint64_t bytes,uint64_t iova,
    ShengGmuMemory *out)
{
    if(!memory.mapping() || memory.bytes()!=size_t(bytes) || memory.spanCount()!=1)
        return false;
    out->cpu=static_cast<volatile uint32_t *>(memory.mapping());
    out->bytes=size_t(bytes);
    out->iova=iova;
    return true;
}
IOReturn NativeAdrenoAssembleGmuConfig(const NativeAdrenoFirmwareView *firmware,
    const NativeAdrenoGmuAllocator *allocator,uint32_t chip_id,unsigned poll_budget,
    ShengGmuConfig *config)
{
    if(!config)return kIOReturnBadArgument;
    *config={};
    if(!firmware || !firmware->bytes || !firmware->length || !allocator ||
        !allocator->ready() || !chip_id || !poll_budget || poll_budget>1024)
        return kIOReturnBadArgument;
    const NativeAdrenoGmuMemoryLayout &layout=allocator->layout();
    ShengGmuConfig built{};
    if(!matches(allocator->icache(),IcacheBytes,layout.icache_iova,&built.icache) ||
        !matches(allocator->dummy(),DummyBytes,layout.dummy_iova,&built.dummy) ||
        !matches(allocator->debug(),DebugBytes,layout.debug_iova,&built.debug) ||
        !matches(allocator->log(),LogBytes,layout.log_iova,&built.log) ||
        !matches(allocator->hfi(),HfiBytes,layout.hfi_iova,&built.hfi))
        return kIOReturnBadArgument;
    built.chip_id=chip_id;
    built.firmware=firmware->bytes;
    built.firmware_bytes=firmware->length;
    built.poll_budget=poll_budget;
    *config=built;
    return kIOReturnSuccess;
}
