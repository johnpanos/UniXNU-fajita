/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoBoard.h"
enum { CellBytes = 4 };
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}
IOReturn NativeAdrenoBoard::parseIommus(const uint8_t *cells,size_t length,
    uint32_t cells_per_entry,uint16_t *stream_ids,size_t capacity,size_t *count)
{
    if(!count)return kIOReturnBadArgument;
    *count=0;
    if(!cells || !length || cells_per_entry<1 || cells_per_entry>4)
        return kIOReturnBadArgument;
    const size_t per_entry=(1+cells_per_entry)*(size_t)CellBytes;
    if(length%per_entry)return kIOReturnBadArgument;
    const size_t total=length/per_entry;
    if(!stream_ids || total>capacity)return kIOReturnNoResources;
    for(size_t index=0;index<total;++index) {
        const uint8_t *at=cells+index*per_entry;
        if(!be32(at))return kIOReturnBadArgument; /* phandle must be named */
        const uint32_t sid=be32(at+CellBytes);
        if(sid>0xffffu)return kIOReturnBadArgument;
        stream_ids[index]=static_cast<uint16_t>(sid);
    }
    *count=total;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoBoard::parseReg(const uint8_t *cells,size_t length,
    uint32_t address_cells,uint32_t size_cells,NativeAdrenoWindow *windows,
    size_t capacity,size_t *count)
{
    if(!count)return kIOReturnBadArgument;
    *count=0;
    if(!cells || !length || (address_cells<1 || address_cells>2) ||
        (size_cells<1 || size_cells>2))return kIOReturnBadArgument;
    const size_t per_window=(address_cells+size_cells)*(size_t)CellBytes;
    if(length%per_window)return kIOReturnBadArgument;
    const size_t total=length/per_window;
    if(total>capacity || !windows)return kIOReturnNoResources;
    for(size_t index=0;index<total;++index) {
        const uint8_t *at=cells+index*per_window;
        uint64_t base=0,bytes=0;
        for(uint32_t cell=0;cell<address_cells;++cell) {
            if(base>>32)return kIOReturnBadArgument; /* more than 64 bits */
            base=(base<<32)|be32(at+cell*CellBytes);
        }
        for(uint32_t cell=0;cell<size_cells;++cell) {
            if(bytes>>32)return kIOReturnBadArgument;
            bytes=(bytes<<32)|be32(at+(address_cells+cell)*CellBytes);
        }
        if(!bytes || base+bytes<base)return kIOReturnBadArgument;
        windows[index]={base,bytes};
    }
    *count=total;
    return kIOReturnSuccess;
}
