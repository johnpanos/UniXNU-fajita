/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoFirmware.h"
#include "NativeAdrenoSha256.h"
#include <string.h>
IOReturn NativeAdrenoFirmware::validate(const uint8_t *image,uint64_t length,
    const NativeAdrenoFirmwareRules *rules,NativeAdrenoFirmwareView *view)
{
    if(!view)return kIOReturnBadArgument;
    *view={};
    if(!rules || !image || !length || length!=rules->bytes ||
        length>uint64_t(SIZE_MAX))return kIOReturnBadArgument;
    uint8_t digest[32];
    NativeAdrenoSha256::hash(image,size_t(length),digest);
    if(memcmp(digest,rules->sha256,32))return kIOReturnIOError;
    view->bytes=image;view->length=size_t(length);
    memcpy(view->sha256,digest,32);
    return kIOReturnSuccess;
}
