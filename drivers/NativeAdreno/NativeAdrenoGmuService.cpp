/* SPDX-License-Identifier: MIT */
/* GMU platform-node driver: discovers the three GMU register windows, maps
 * them read/write but performs no access, and publishes the bases so the GPU
 * service can find them without resolving the DT phandle itself. It does not
 * power CX/GX, start the CM3, load firmware or claim a working GMU; those steps
 * belong to the GPU bring-up once board power exists. Publishing this node is
 * safe because nothing is written and no interrupt is claimed. */
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include "NativeAdrenoGmuService.h"

OSDefineMetaClassAndStructors(NativeAdrenoGmuService,IOService)



static bool advertisesGmu(IOService *provider)
{
    OSObject *property=provider?provider->getProperty("compatible"):nullptr;
    OSData *data=OSDynamicCast(OSData,property);
    if(!data || !data->getBytesNoCopy() || !data->getLength())return false;
    const char *text=static_cast<const char *>(data->getBytesNoCopy());
    const size_t length=data->getLength();
    static const char needle[]="qcom,adreno-gmu";
    for(size_t at=0;at+sizeof(needle)-1<=length;++at) {
        size_t index=0;
        while(index<sizeof(needle)-1 && text[at+index]==needle[index])++index;
        if(index==sizeof(needle)-1)return true;
    }
    return false;
}
IOService *NativeAdrenoGmuService::probe(IOService *provider,SInt32 *score)
{
    if(!advertisesGmu(provider))return nullptr;
    if(score)*score=1000;
    return this;
}
bool NativeAdrenoGmuService::start(IOService *provider)
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
        IOLog("NativeAdrenoGmu: reg windows are not usable\n");
        count_=0;IOService::stop(provider);
        return false;
    }
    for(size_t index=0;index<count_;++index) {
        IODeviceMemory *memory=getDeviceMemoryWithIndex(static_cast<unsigned>(index));
        if(!memory) {
            IOLog("NativeAdrenoGmu: window %lu has no device memory\n",
                static_cast<unsigned long>(index));
            stop(provider);
            return false;
        }
        maps_[index]=OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere),
            OSNoRetain);
        if(!maps_[index]) {
            IOLog("NativeAdrenoGmu: window %lu did not map\n",
                static_cast<unsigned long>(index));
            stop(provider);
            return false;
        }
    }
    /* Bases are properties for the GPU service; no phandle resolution needed. */
    OSData *bases=OSData::withCapacity(static_cast<UInt32>(count_*sizeof(uint64_t)));
    if(!bases) {stop(provider);return false;}
    for(size_t index=0;index<count_;++index) {
        const uint64_t base=windows_[index].base;
        bases->appendBytes(&base,sizeof(base));
    }
    setProperty("gmu-window-bases",bases);
    bases->release();
    IOLog("NativeAdrenoGmu: %lu windows mapped, no register access\n",
        static_cast<unsigned long>(count_));
    registerService();
    return true;
}
void NativeAdrenoGmuService::stop(IOService *provider)
{
    for(auto &map:maps_)map.reset();
    count_=0;
    IOService::stop(provider);
}
