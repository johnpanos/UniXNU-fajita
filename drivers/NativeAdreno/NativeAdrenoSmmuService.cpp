/* SPDX-License-Identifier: MIT */
/* GPU SMMU platform node: maps the SMMU register window and publishes its base
 * so the GPU service does not resolve the DT phandle itself. No register is
 * read or written, no context is allocated and no stream is attached; those
 * steps remain with the GPU bring-up and its stream table. */
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include "NativeAdrenoSmmuService.h"

OSDefineMetaClassAndStructors(NativeAdrenoSmmuService,IOService)



static bool advertisesSmmu(IOService *provider)
{
    OSObject *property=provider?provider->getProperty("compatible"):nullptr;
    OSData *data=OSDynamicCast(OSData,property);
    if(!data || !data->getBytesNoCopy() || !data->getLength())return false;
    const char *text=static_cast<const char *>(data->getBytesNoCopy());
    const size_t length=data->getLength();
    static const char needle[]="qcom,adreno-smmu";
    for(size_t at=0;at+sizeof(needle)-1<=length;++at) {
        size_t index=0;
        while(index<sizeof(needle)-1 && text[at+index]==needle[index])++index;
        if(index==sizeof(needle)-1)return true;
    }
    return false;
}
IOService *NativeAdrenoSmmuService::probe(IOService *provider,SInt32 *score)
{
    if(!advertisesSmmu(provider))return nullptr;
    if(score)*score=1000;
    return this;
}
bool NativeAdrenoSmmuService::start(IOService *provider)
{
    if(!IOService::start(provider))return false;
    OSData *reg=OSDynamicCast(OSData,provider->getProperty("reg"));
    OSData *address=OSDynamicCast(OSData,provider->getProperty("#address-cells"));
    OSData *size=OSDynamicCast(OSData,provider->getProperty("#size-cells"));
    uint32_t address_cells=2,size_cells=2;
    if(address && address->getLength()>=4)
        address_cells=*static_cast<const uint32_t *>(address->getBytesNoCopy());
    if(size && size->getLength()>=4)
        size_cells=*static_cast<const uint32_t *>(size->getBytesNoCopy());
    if(!reg || NativeAdrenoBoard::parseReg(
        static_cast<const uint8_t *>(reg->getBytesNoCopy()),reg->getLength(),
        address_cells,size_cells,windows_,NativeAdrenoMaxWindows,&count_)!=kIOReturnSuccess) {
        IOLog("NativeAdrenoSmmu: reg windows are not usable\n");
        count_=0;IOService::stop(provider);
        return false;
    }
    for(size_t index=0;index<count_;++index) {
        IODeviceMemory *memory=getDeviceMemoryWithIndex(static_cast<unsigned>(index));
        if(!memory) {
            IOLog("NativeAdrenoSmmu: window %lu has no device memory\n",
                static_cast<unsigned long>(index));
            stop(provider);
            return false;
        }
        maps_[index]=OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere|kIOMapInhibitCache),
            OSNoRetain);
        if(!maps_[index]) {
            IOLog("NativeAdrenoSmmu: window %lu did not map\n",
                static_cast<unsigned long>(index));
            stop(provider);
            return false;
        }
    }
    OSData *bases=OSData::withCapacity(static_cast<UInt32>(count_*sizeof(uint64_t)));
    if(!bases) {stop(provider);return false;}
    for(size_t index=0;index<count_;++index) {
        const uint64_t base=windows_[index].base;
        bases->appendBytes(&base,sizeof(base));
    }
    setProperty("smmu-window-bases",bases);
    bases->release();
    OSData *cells=OSDynamicCast(OSData,provider->getProperty("#iommu-cells"));
    if(cells && cells->getLength()>=4) {
        const uint32_t value=*static_cast<const uint32_t *>(cells->getBytesNoCopy());
        setProperty("iommu-cells",value,32);
    }
    IOLog("NativeAdrenoSmmu: %lu window mapped, no register access\n",
        static_cast<unsigned long>(count_));
    registerService();
    return true;
}
void NativeAdrenoSmmuService::stop(IOService *provider)
{
    for(auto &map:maps_)map.reset();
    count_=0;
    IOService::stop(provider);
}
