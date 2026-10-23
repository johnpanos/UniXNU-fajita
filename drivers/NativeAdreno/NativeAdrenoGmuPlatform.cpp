/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGmuPlatform.h"
#include <IOKit/IOLib.h>

int NativeAdrenoGmuPlatform::read32(void *cookie,unsigned window,size_t at,uint32_t *out)
{
    if(!cookie || !out || window>=ShengGmuWindows)return 1;
    NativeAdrenoGmuPlatform *self=static_cast<NativeAdrenoGmuPlatform *>(cookie);
    const NativeAdrenoGmuWindow &bounds=self->windows_[window];
    if(!bounds.base || (at&3) || at+4>bounds.bytes)return 1;
    *out=*reinterpret_cast<volatile uint32_t *>(bounds.base+at);
    return 0;
}
int NativeAdrenoGmuPlatform::write32(void *cookie,unsigned window,size_t at,uint32_t value)
{
    if(!cookie || window>=ShengGmuWindows)return 1;
    NativeAdrenoGmuPlatform *self=static_cast<NativeAdrenoGmuPlatform *>(cookie);
    const NativeAdrenoGmuWindow &bounds=self->windows_[window];
    if(!bounds.base || (at&3) || at+4>bounds.bytes)return 1;
    *reinterpret_cast<volatile uint32_t *>(bounds.base+at)=value;
    return 0;
}
int NativeAdrenoGmuPlatform::barrier(void *cookie)
{
    if(!cookie)return 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}
IOReturn NativeAdrenoGmuPlatform::bind(const NativeAdrenoGmuWindow windows[ShengGmuWindows],
    ShengGmuIo *io)
{
    if(!windows || !io)return kIOReturnBadArgument;
    for(unsigned index=0;index<ShengGmuWindows;++index) {
        if(!windows[index].base || !windows[index].bytes ||
            (windows[index].bytes&3) || windows[index].bytes>UINT32_MAX)
            return kIOReturnBadArgument;
    }
    for(unsigned index=0;index<ShengGmuWindows;++index)windows_[index]=windows[index];
    ShengGmuIo built{};
    built.cookie=this;
    for(unsigned index=0;index<ShengGmuWindows;++index)built.bytes[index]=windows_[index].bytes;
    built.read32=read32;built.write32=write32;built.barrier=barrier;
    *io=built;
    return kIOReturnSuccess;
}
static int sha256(void *,const void *bytes,size_t length,uint8_t digest[32])
{
    if(!bytes || !length || !digest)return 1;
    NativeAdrenoSha256::hash(static_cast<const uint8_t *>(bytes),length,digest);
    return 0;
}
IOReturn NativeAdrenoGmuPlatform::services(const NativeAdrenoGmuTime *time,
    ShengGmuServices *services)
{
    if(!services)return kIOReturnBadArgument;
    *services={};
    if(!time || !time->now_us || !time->wait_us)return kIOReturnBadArgument;
    services->cookie=time->cookie;
    services->sha256=sha256;
    services->now_us=time->now_us;
    services->wait_us=time->wait_us;
    return kIOReturnSuccess;
}
