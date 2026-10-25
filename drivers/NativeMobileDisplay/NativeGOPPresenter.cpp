// SPDX-License-Identifier: BSD-2-Clause-Patent
// Transfer completed GPU pixels to actual firmware scanout. No CPU rendering,
// fabricated refresh event, Metal discovery or original display publication.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include "NativeGOPWire.h"
extern "C" {
volatile uint32_t native_gop_ready,native_gop_failures;
volatile uint64_t native_gop_transfers,native_gop_bytes,native_gop_physical,native_gop_virtual;
// Physical layout of the offered frames (zero-copy scan-out study): [0] frames measured, [1] physically contiguous runs of the last,
// [2] the most runs of any, [3] the fewest, [4] the last frame's bytes, [5] its first physical address. Measured once per new mapping.
volatile uint64_t native_gop_frame_runs[6];
volatile uint64_t native_gop_gpu_offers,native_gop_gpu_completions,native_gop_gpu_failures,native_gop_gpu_abandoned;
/* waitSwap: [0] calls, [1] timeouts, [2] the requested timeout of the last call (ms), [3] time the last timed-out call really waited (ns), [4] the shortest
 * and [5] the longest such wait (ns), [6] total ns waited in timed-out calls. A timed wait that returns long before its deadline shows here. */
volatile uint64_t native_gop_wait_stats[7];
/* Frame mappings kept across swaps (fajita-gop-mapcache, default 4 entries; 0 maps every frame): [0] hits, [1] misses (new mapping), [2] evictions,
 * [3] flushes (client task changed or gone). Timing per delivered swap: [4] swaps, [5] total ns mapping, [6] total ns from delivery to completion
 * (the client's import, GPU copy and wait), [7] the longest such ns. */
volatile uint64_t native_gop_map_stats[8];
IOService *native_gop_presenter_create(void);
bool native_gop_gpu_offer(IOService *,void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
void native_gop_presenter_set_source(IOService *presenter,IOService *nub);
// Provided by the display driver: finishes (status 0) or fails the swap that was offered.
void native_legacy_gpu_complete(void *display,int status);
// Double-buffered scan-out (fajita-fb-flip=1; the board's DPU, package/FajitaDpu.cpp): the back buffer's physical address, the buffer on
// screen, the tear counter, and the flip itself (latched: interrupt context, once the old buffer is no longer read). Weak: other boards.
uint64_t fajita_dpu_flip_back(uint64_t front,uint64_t bytes) __attribute__((weak));
uint64_t fajita_dpu_flip_third(uint64_t front,uint64_t bytes) __attribute__((weak));
uint64_t fajita_dpu_scanned(void) __attribute__((weak));
void fajita_dpu_count_scanned_write(void) __attribute__((weak));
int fajita_dpu_flip(uint64_t phys,void (*latched)(void *),void *context) __attribute__((weak));
// [0] flips handed to the DPU, [1] retired by the latch, [2] retired by the 100 ms watchdog (no latch), [3] refused by the DPU (retired at
// once), [4] deliveries held for a free buffer, [5] frames queued behind a pending flip, [6] buffers (2 or 3).
volatile uint32_t native_gop_flip_stats[8];
}
class NativeGOPClient;
class NativeGOPPresenter;
static NativeGOPPresenter *gHandoff;   // the presenter in GPU mode: the hand-off entry points below reach it without the registry
class NativeGOPPresenter : public IOService {
    OSDeclareDefaultStructors(NativeGOPPresenter)
    friend class NativeGOPClient;
    IOMemoryMap *mapping=nullptr;
    IOLock *gate=nullptr;
    IOService *owner=nullptr;
    NGPMode mode={};
    bool online=false;
    IOService *memorySource=nullptr;
    // GPU hand-off (sheng-fb-gpu=1): one offered frame at a time, from the display driver to the privileged client.
    bool gpuMode=false,gpuClient=false;
    struct Swap {bool offered,delivered;uint64_t sequence;void *display;IOMemoryDescriptor *memory;IOMemoryMap *map;
            uint64_t stride;uint32_t width,height;unsigned target;};
    Swap swap={};
    // Double-buffered scan-out (fajita-fb-flip=1, set up at the client's first wait, once the display attached the DPU interrupt): buffer 0
    // is the firmware's (mapping), 1 the back buffer; the GPU writes the one not on screen; a completed frame is flipped to and its swap
    // retires when the DPU latched it. No frame is delivered while a flip is pending.
    // Triple buffering where the third buffer fits (buffer 2): a frame completed while a flip is pending waits as `queued` and is
    // flipped to at that flip's latch, so the GPU always has a buffer that is neither on screen nor about to be.
    bool flipWanted=false,flipMode=false,flipTried=false,flipPending=false,queuedValid=false,retireEarly=false;
    IOMemoryMap *backMapping=nullptr,*thirdMapping=nullptr;
    uint64_t phys[3]={};
    unsigned front=0,buffers=0;
    Swap flipDone={},queued={};
    uint64_t flipSerial=0;
    int freeTarget() const {
        for(unsigned i=0;i<buffers;++i)
            if(i!=front && !(flipPending && i==flipDone.target) && !(queuedValid && i==queued.target))return (int)i;
        return -1;
    }
    void startFlip();
    thread_call_t latchCall=nullptr,watchdogCall=nullptr;
    void setupFlip();
    static void flipLatched(void *context);
    static void latchWork(thread_call_param_t p0,thread_call_param_t p1);
    void retireFlip(uint64_t serial,bool latched);
    uint64_t swapSequence=0;
    // The frame mappings kept for the client task (descriptor retained, so its pointer is its identity); id goes to NGPSwap.reserved.
    struct MapEntry {IOMemoryDescriptor *memory;IOMemoryMap *map;uint32_t id;uint64_t used;};
    MapEntry maps[8]={};
    unsigned mapLimit=4;
    uint32_t mapIds=0;
    uint64_t mapClock=0,deliveredAt=0;
    task_t mapTask=TASK_NULL;
    void flushMaps();
    IOReturn waitSwap(task_t,uint64_t timeoutMs,NGPSwap *);
    IOReturn completeSwap(uint64_t sequence,int status);
    void abandonSwap();
public:
    void setMemorySource(IOService *source) {memorySource=source;}
    static IOReturn handoffCall(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize);
    static IOMemoryDescriptor *handoffMemory();
    static IOMemoryDescriptor *handoffBackMemory(unsigned index);
    bool offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
private:
    IOReturn transfer(task_t,const NGPPresent &,NGPTransfer *);
public:
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    bool handleOpen(IOService *client,IOOptionBits,void *) override {if(owner)return false;owner=client;return true;}
    void handleClose(IOService *client,IOOptionBits) override {if(owner==client)owner=nullptr;}
    bool handleIsOpen(const IOService *client) const override {return client?owner==client:owner!=nullptr;}
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
};
class NativeGOPClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeGOPClient)
    NativeGOPPresenter *controller=nullptr;
    task_t task=TASK_NULL;
    bool opened=false;
    static IOReturn modeAction(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn presentAction(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn waitAction(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn completeAction(OSObject *,void *,IOExternalMethodArguments *);
public:
    bool initWithTask(task_t,void *,UInt32,OSDictionary *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn clientClose() override {terminate();return kIOReturnSuccess;}
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) override;
    IOReturn clientMemoryForType(UInt32,IOOptionBits *,IOMemoryDescriptor **) override;
};
OSDefineMetaClassAndStructors(NativeGOPPresenter,IOService)
OSDefineMetaClassAndStructors(NativeGOPClient,IOUserClient)
IOService *native_gop_presenter_create(void) {return new NativeGOPPresenter;}
bool NativeGOPPresenter::start(IOService *provider) {
    if(!IOService::start(provider))return false;
    PE_Video video={};
    if(PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
       video.v_width>16384 || video.v_height>16384 || video.v_depth!=32 || video.v_rotate ||
       video.v_rowBytes<video.v_width*4 || video.v_rowBytes>UINT64_MAX/video.v_height)return false;
    // The hand-off presenter is attached away from the display subsystem's node (the original display code enumerates that node's children),
    // so the framebuffer memory comes from the node it names instead of its provider.
    mapping=(memorySource?memorySource:provider)->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
    if(!mapping || !mapping->getVirtualAddress() || mapping->getPhysicalAddress()!=video.v_baseAddr ||
       mapping->getLength()!=video.v_rowBytes*video.v_height)return false;
    gate=IOLockAlloc();if(!gate)return false;
    {unsigned v=0;PE_parse_boot_argn("sheng-fb-gpu",&v,sizeof v);gpuMode=(v==1);}
    // fajita-fb-flip=1: a swap retires when the DPU shows its frame (the compositor's frame-done means on the panel); =2: when its copy
    // completes (as without flips: the display offers one swap at a time, so retiring at the latch holds the next frame back a frame),
    // the flip still taking the frame at the next frame boundary.
    {unsigned v=0;PE_parse_boot_argn("fajita-fb-flip",&v,sizeof v);flipWanted=gpuMode && (v==1 || v==2);retireEarly=v==2;}
    {unsigned v=4;PE_parse_boot_argn("fajita-gop-mapcache",&v,sizeof v);mapLimit=v>8?8:v;}
    mode={NGP_VERSION,NGP_BGRX8888,video.v_width,video.v_height,video.v_rowBytes,mapping->getLength()};
    native_gop_physical=mapping->getPhysicalAddress();native_gop_virtual=mapping->getVirtualAddress();
    online=true;native_gop_ready=1;if(gpuMode)gHandoff=this;registerService();return true;
}
void NativeGOPPresenter::stop(IOService *provider) {
    if(gHandoff==this)gHandoff=nullptr;
    if(gate)IOLockLock(gate);online=false;native_gop_ready=0;if(gate){IOLockWakeup(gate,&swap,false);IOLockUnlock(gate);}
    abandonSwap();
    IOService::stop(provider);
}
void NativeGOPPresenter::free() {
    if(latchCall){thread_call_cancel_wait(latchCall);thread_call_free(latchCall);latchCall=nullptr;}
    if(watchdogCall){thread_call_cancel_wait(watchdogCall);thread_call_free(watchdogCall);watchdogCall=nullptr;}
    OSSafeReleaseNULL(backMapping);OSSafeReleaseNULL(thirdMapping);
    OSSafeReleaseNULL(mapping);if(gate){IOLockFree(gate);gate=nullptr;}IOService::free();
}
IOReturn NativeGOPPresenter::transfer(task_t task,const NGPPresent &q,NGPTransfer *result) {
    if(gpuMode)return kIOReturnUnsupported;   // in GPU mode no CPU copy exists, not even on request
    if(!q.address || q.bytes!=mode.bytes || q.width!=mode.width || q.height!=mode.height ||
       q.stride!=mode.stride || q.format!=mode.format || q.address>UINT64_MAX-q.bytes)return kIOReturnBadArgument;
    IOMemoryDescriptor *input=IOMemoryDescriptor::withAddressRange(q.address,q.bytes,kIODirectionIn,task);
    if(!input)return kIOReturnNoMemory;
    IOReturn status=input->prepare(kIODirectionIn);
    if(status==kIOReturnSuccess) {
        IOLockLock(gate);
        if(!online)status=kIOReturnNotReady;
        else if(input->readBytes(0,reinterpret_cast<void *>(mapping->getVirtualAddress()),q.bytes)!=q.bytes)status=kIOReturnUnderrun;
        else {
            __asm__ volatile("dsb oshst" ::: "memory");
            native_gop_transfers=native_gop_transfers+1;result->sequence=native_gop_transfers;
            result->bytes=q.bytes;native_gop_bytes=native_gop_bytes+q.bytes;
        }
        IOLockUnlock(gate);
        IOReturn completion=input->complete(kIODirectionIn);if(status==kIOReturnSuccess)status=completion;
    }
    input->release();if(status!=kIOReturnSuccess)__atomic_fetch_add(&native_gop_failures,1,__ATOMIC_RELAXED);return status;
}
// The display driver offers a finished frame. Accepted only while the privileged GPU client is alive and waiting for work; otherwise the
// display driver keeps its own path (counted by it as a fallback).
bool NativeGOPPresenter::offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    if(!gpuMode || !memory || !display || !gate)return false;
    IOLockLock(gate);
    bool accepted=online && gpuClient && !swap.offered;
    if(accepted) {
        memory->retain();
        swap={true,false,++swapSequence,display,memory,nullptr,stride,width,height,0};
        native_gop_gpu_offers=native_gop_gpu_offers+1;
        IOLockWakeup(gate,&swap,false);
    }
    IOLockUnlock(gate);return accepted;
}
IOReturn NativeGOPPresenter::waitSwap(task_t task,uint64_t timeoutMs,NGPSwap *out) {
    if(!gpuMode)return kIOReturnUnsupported;
    if(timeoutMs>1000)timeoutMs=1000;
    uint64_t deadline;clock_interval_to_deadline((uint32_t)timeoutMs,kMillisecondScale,&deadline);
    const uint64_t began=mach_absolute_time();
    native_gop_wait_stats[0]=native_gop_wait_stats[0]+1;native_gop_wait_stats[2]=timeoutMs;
    if(flipWanted && !flipTried)setupFlip();
    IOLockLock(gate);
    gpuClient=true;
    if(flipMode && freeTarget()<0)native_gop_flip_stats[4]=native_gop_flip_stats[4]+1;
    while(online && (!swap.offered || swap.delivered || (flipMode && freeTarget()<0))) {
        if(IOLockSleepDeadline(gate,&swap,deadline,THREAD_ABORTSAFE)!=THREAD_AWAKENED) {
            IOLockUnlock(gate);
            uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-began,&ns);
            native_gop_wait_stats[1]=native_gop_wait_stats[1]+1;native_gop_wait_stats[3]=ns;
            if(!native_gop_wait_stats[4] || ns<native_gop_wait_stats[4])native_gop_wait_stats[4]=ns;
            if(ns>native_gop_wait_stats[5])native_gop_wait_stats[5]=ns;
            native_gop_wait_stats[6]=native_gop_wait_stats[6]+ns;
            return kIOReturnTimeout;
        }
    }
    if(!online) {IOLockUnlock(gate);return kIOReturnNotReady;}
    swap.delivered=true;
    IOMemoryDescriptor *memory=swap.memory;memory->retain();
    const uint64_t sequence=swap.sequence,stride=swap.stride;const uint32_t width=swap.width,height=swap.height;
    IOLockUnlock(gate);
    // The frame's pages go into the client's task here, writable like any import (the GPU maps them for reading; the kernel pins and maintains them).
    // Only the waiting client thread reaches the cache (waitSwap, and abandonSwap once it is gone), and only one swap is outstanding, so the entry
    // in use is always the newest and never the one evicted.
    const uint64_t mapBegan=mach_absolute_time();
    IOMemoryMap *map=nullptr;uint32_t id=0;
    if(mapLimit) {
        if(mapTask!=task)flushMaps();
        for(unsigned i=0;i<mapLimit;++i)if(maps[i].memory==memory && maps[i].map) {map=maps[i].map;id=maps[i].id;maps[i].used=++mapClock;break;}
        if(map)native_gop_map_stats[0]=native_gop_map_stats[0]+1;
    }
    bool cached=map!=nullptr;
    if(!map) {
        {
            uint64_t runs=0,offset=0,last=0,first=0;const uint64_t length=memory->getLength();
            while(offset<length) {
                IOByteCount seg=0;const addr64_t pa=memory->getPhysicalSegment(offset,&seg,kIOMemoryMapperNone);
                if(!pa || !seg)break;
                if(!runs || pa!=last)++runs;
                if(!first)first=pa;
                last=pa+seg;offset+=seg;
            }
            native_gop_frame_runs[0]=native_gop_frame_runs[0]+1;native_gop_frame_runs[1]=runs;native_gop_frame_runs[4]=length;native_gop_frame_runs[5]=first;
            if(runs>native_gop_frame_runs[2])native_gop_frame_runs[2]=runs;
            if(!native_gop_frame_runs[3] || runs<native_gop_frame_runs[3])native_gop_frame_runs[3]=runs;
        }
        map=memory->createMappingInTask(task,0,kIOMapAnywhere);
        if(mapLimit && map && map->getVirtualAddress()) {
            native_gop_map_stats[1]=native_gop_map_stats[1]+1;
            unsigned victim=0;
            for(unsigned i=0;i<mapLimit;++i) {
                if(!maps[i].map){victim=i;break;}
                if(maps[i].used<maps[victim].used)victim=i;
            }
            if(maps[victim].map) {
                OSSafeReleaseNULL(maps[victim].map);OSSafeReleaseNULL(maps[victim].memory);
                native_gop_map_stats[2]=native_gop_map_stats[2]+1;
            }
            if(mapTask==TASK_NULL){mapTask=task;task_reference(task);}
            memory->retain();
            if(!++mapIds)mapIds=1;
            maps[victim]={memory,map,mapIds,++mapClock};id=mapIds;cached=true;
        }
    }
    memory->release();
    {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-mapBegan,&ns);native_gop_map_stats[5]=native_gop_map_stats[5]+ns;}
    IOLockLock(gate);
    if(!map || !map->getVirtualAddress() || !swap.offered || swap.sequence!=sequence) {
        IOLockUnlock(gate);if(!cached)OSSafeReleaseNULL(map);__atomic_fetch_add(&native_gop_gpu_failures,1,__ATOMIC_RELAXED);return kIOReturnNoMemory;
    }
    swap.map=cached?nullptr:map;   // a cached mapping stays with its entry; an uncached one is released when the swap completes
    deliveredAt=mach_absolute_time();
    out->sequence=sequence;out->address=map->getVirtualAddress();out->bytes=map->getLength();out->stride=stride;
    out->width=width;out->height=height;out->format=NGP_BGRX8888;out->reserved=id;
    const unsigned target=flipMode?(unsigned)freeTarget():0;
    swap.target=target;out->target=target;out->flags=flipMode?1:0;
    const uint64_t targetPhys=phys[target];
    IOLockUnlock(gate);
    // The tear counter: a frame the GPU is told to write into the buffer the DPU reads (every frame without flips).
    if(fajita_dpu_scanned && fajita_dpu_count_scanned_write && targetPhys && fajita_dpu_scanned()==targetPhys)fajita_dpu_count_scanned_write();
    return kIOReturnSuccess;
}
IOReturn NativeGOPPresenter::completeSwap(uint64_t sequence,int status) {
    IOLockLock(gate);
    if(!swap.offered || !swap.delivered || swap.sequence!=sequence) {IOLockUnlock(gate);return kIOReturnBadArgument;}
    auto done=swap;swap={};
    const uint64_t delivered=deliveredAt;
    IOLockUnlock(gate);
    if(delivered) {
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-delivered,&ns);
        native_gop_map_stats[4]=native_gop_map_stats[4]+1;native_gop_map_stats[6]=native_gop_map_stats[6]+ns;
        if(ns>native_gop_map_stats[7])native_gop_map_stats[7]=ns;
    }
    if(status)__atomic_fetch_add(&native_gop_gpu_failures,1,__ATOMIC_RELAXED);
    else native_gop_gpu_completions=native_gop_gpu_completions+1;
    // Flipped: the swap retires when the DPU shows the frame (retireFlip), the frame's pages stay held until then.
    if(flipMode && !status) {
        void *display=nullptr;
        if(retireEarly) {   // retired now; the flip keeps only the buffer
            OSSafeReleaseNULL(done.map);done.memory->release();display=done.display;
            done.memory=nullptr;done.display=nullptr;
        }
        IOLockLock(gate);
        if(flipPending) {   // behind the pending flip: flipped to at its latch
            queued=done;queuedValid=true;native_gop_flip_stats[5]=native_gop_flip_stats[5]+1;
            IOLockUnlock(gate);
        } else {
            flipDone=done;flipPending=true;
            IOLockUnlock(gate);
            startFlip();
        }
        if(display)native_legacy_gpu_complete(display,0);
        return kIOReturnSuccess;
    }
    OSSafeReleaseNULL(done.map);done.memory->release();
    native_legacy_gpu_complete(done.display,status);return kIOReturnSuccess;
}
// Hands flipDone to the DPU. A refused flip retires at once (counted), and the queued frame, if any, is tried next.
void NativeGOPPresenter::startFlip() {
    for(;;) {
        IOLockLock(gate);
        if(!flipPending){IOLockUnlock(gate);return;}
        const uint64_t serial=++flipSerial,target=phys[flipDone.target];
        IOLockUnlock(gate);
        if(fajita_dpu_flip(target,&NativeGOPPresenter::flipLatched,this)==0) {
            native_gop_flip_stats[0]=native_gop_flip_stats[0]+1;
            uint64_t deadline;clock_interval_to_deadline(100,kMillisecondScale,&deadline);
            thread_call_enter1_delayed(watchdogCall,(thread_call_param_t)(uintptr_t)serial,deadline);
            return;
        }
        native_gop_flip_stats[3]=native_gop_flip_stats[3]+1;
        IOLockLock(gate);
        Swap done=flipDone;flipDone={};flipPending=false;
        if(queuedValid){flipDone=queued;queued={};queuedValid=false;flipPending=true;}
        IOLockWakeup(gate,&swap,false);
        IOLockUnlock(gate);
        OSSafeReleaseNULL(done.map);if(done.memory)done.memory->release();
        if(done.display)native_legacy_gpu_complete(done.display,0);
    }
}
void NativeGOPPresenter::setupFlip() {
    flipTried=true;
    if(!fajita_dpu_flip_back || !fajita_dpu_flip || !mapping)return;
    const uint64_t front0=mapping->getPhysicalAddress(),bytes=mapping->getLength();
    const uint64_t back=fajita_dpu_flip_back(front0,bytes);
    if(!back)return;
    IODeviceMemory *m=IODeviceMemory::withRange(back,bytes);
    IOMemoryMap *map=m?m->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    if(m)m->release();
    if(!map)return;
    IOMemoryMap *third=nullptr;uint64_t thirdPhys=0;
    if(fajita_dpu_flip_third && (thirdPhys=fajita_dpu_flip_third(front0,bytes))) {
        IODeviceMemory *t=IODeviceMemory::withRange(thirdPhys,bytes);
        third=t?t->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(t)t->release();
    }
    latchCall=thread_call_allocate_with_options(&NativeGOPPresenter::latchWork,this,THREAD_CALL_PRIORITY_KERNEL_HIGH,0);
    watchdogCall=thread_call_allocate_with_options(&NativeGOPPresenter::latchWork,this,THREAD_CALL_PRIORITY_KERNEL_HIGH,0);
    if(!latchCall || !watchdogCall){map->release();OSSafeReleaseNULL(third);return;}
    IOLockLock(gate);
    backMapping=map;thirdMapping=third;phys[0]=front0;phys[1]=back;phys[2]=third?thirdPhys:0;buffers=third?3:2;front=0;flipMode=true;
    native_gop_flip_stats[6]=buffers;
    IOLockUnlock(gate);
}
// The DPU's rd_ptr interrupt found the flip latched: retire it from a thread (the display's completion takes its work loop).
void NativeGOPPresenter::flipLatched(void *context) {
    NativeGOPPresenter *p=static_cast<NativeGOPPresenter *>(context);
    thread_call_enter1(p->latchCall,(thread_call_param_t)0);
}
// latchCall (param 0: the latch) and watchdogCall (param: the flip's serial, 100 ms after it was handed over).
void NativeGOPPresenter::latchWork(thread_call_param_t p0,thread_call_param_t p1) {
    NativeGOPPresenter *p=static_cast<NativeGOPPresenter *>(p0);
    const uint64_t serial=(uint64_t)(uintptr_t)p1;
    p->retireFlip(serial,serial==0);
}
void NativeGOPPresenter::retireFlip(uint64_t serial,bool latched) {
    IOLockLock(gate);
    if(!flipPending || (!latched && serial!=flipSerial)) {IOLockUnlock(gate);return;}
    Swap done=flipDone;flipDone={};flipPending=false;front=done.target;
    // The scanout's readers (screenshots, the diagnostic export) read the buffer on the panel.
    if(IOMemoryMap *shown=front==0?mapping:front==1?backMapping:thirdMapping){native_gop_physical=shown->getPhysicalAddress();native_gop_virtual=shown->getVirtualAddress();}
    const bool next=queuedValid;
    if(next){flipDone=queued;queued={};queuedValid=false;flipPending=true;}
    IOLockWakeup(gate,&swap,false);
    IOLockUnlock(gate);
    if(latched){thread_call_cancel(watchdogCall);native_gop_flip_stats[1]=native_gop_flip_stats[1]+1;}
    else native_gop_flip_stats[2]=native_gop_flip_stats[2]+1;
    OSSafeReleaseNULL(done.map);if(done.memory)done.memory->release();
    if(done.display)native_legacy_gpu_complete(done.display,0);
    if(next)startFlip();
}
// The client went away with a frame outstanding: the display driver is told it failed, so it can use its own path instead of waiting forever.
void NativeGOPPresenter::abandonSwap() {
    if(!gate)return;
    IOLockLock(gate);
    gpuClient=false;
    auto done=swap;swap={};
    IOLockUnlock(gate);
    flushMaps();
    if(!done.offered)return;
    OSSafeReleaseNULL(done.map);if(done.memory)done.memory->release();
    __atomic_fetch_add(&native_gop_gpu_abandoned,1,__ATOMIC_RELAXED);
    native_legacy_gpu_complete(done.display,kIOReturnAborted);
}
void NativeGOPPresenter::flushMaps() {
    bool any=false;
    for(auto &e:maps) {if(e.map||e.memory)any=true;OSSafeReleaseNULL(e.map);OSSafeReleaseNULL(e.memory);e={};}
    if(mapTask!=TASK_NULL){task_deallocate(mapTask);mapTask=TASK_NULL;}
    if(any)native_gop_map_stats[3]=native_gop_map_stats[3]+1;
}
extern "C" void native_gop_presenter_set_source(IOService *presenter,IOService *nub) {
    if(NativeGOPPresenter *p=OSDynamicCast(NativeGOPPresenter,presenter))p->setMemorySource(nub);
}
extern "C" bool native_gop_gpu_offer(IOService *presenter,void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    NativeGOPPresenter *p=OSDynamicCast(NativeGOPPresenter,presenter);
    return p && p->offer(display,memory,stride,width,height);
}
// Entry points for the GPU client that reaches the hand-off through an already admitted connection (the Adreno accelerator's user client, which
// the original sandbox profiles allow where they do not allow this service): op 0 mode, 2 wait for a frame, 3 complete it (the NGP_METHOD_*
// numbers). Same checks as the service's own methods; sizes are the wire records'.
IOReturn NativeGOPPresenter::handoffCall(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize) {
    NativeGOPPresenter *p=gHandoff;
    if(!p || !p->gpuMode || !p->online)return kIOReturnNotReady;
    switch(op) {
    case NGP_METHOD_MODE:
        if(inSize || !out || outSize!=sizeof(NGPMode))return kIOReturnBadArgument;
        // The client's open reads the mode first and maps the buffers next: the back buffer must exist by then.
        if(p->flipWanted && !p->flipTried)p->setupFlip();
        memcpy(out,&p->mode,sizeof(NGPMode));return kIOReturnSuccess;
    case NGP_METHOD_WAIT_SWAP: {
        if(!in || inSize!=sizeof(NGPWait) || !out || outSize!=sizeof(NGPSwap))return kIOReturnBadArgument;
        NGPWait q={};NGPSwap swap={};memcpy(&q,in,sizeof(q));
        IOReturn status=p->waitSwap(task,q.timeout_ms,&swap);
        if(status==kIOReturnSuccess)memcpy(out,&swap,sizeof(swap));return status;
    }
    case NGP_METHOD_COMPLETE_SWAP: {
        if(!in || inSize!=sizeof(NGPComplete) || outSize)return kIOReturnBadArgument;
        NGPComplete q={};memcpy(&q,in,sizeof(q));
        return p->completeSwap(q.sequence,(int)q.status);
    }
    default:return kIOReturnUnsupported;
    }
}
// The scanout as a writable, retained descriptor (GPU mode only); the caller maps it into its task.
IOMemoryDescriptor *NativeGOPPresenter::handoffMemory() {
    NativeGOPPresenter *p=gHandoff;
    if(!p || !p->gpuMode || !p->online || !p->mapping)return nullptr;
    IOMemoryDescriptor *memory=p->mapping->getMemoryDescriptor();
    if(memory)memory->retain();
    return memory;
}
extern "C" IOReturn native_gop_handoff_call(uint32_t op,task_t task,const void *in,size_t inSize,void *out,size_t outSize) {
    return NativeGOPPresenter::handoffCall(op,task,in,inSize,out,outSize);
}
extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void) {return NativeGOPPresenter::handoffMemory();}
// The back buffer (fajita-fb-flip=1, once set up), writable and retained for the caller; null otherwise.
extern "C" IOMemoryDescriptor *native_gop_handoff_back_memory(void) {return NativeGOPPresenter::handoffBackMemory(1);}
extern "C" IOMemoryDescriptor *native_gop_handoff_third_memory(void) {return NativeGOPPresenter::handoffBackMemory(2);}
IOMemoryDescriptor *NativeGOPPresenter::handoffBackMemory(unsigned index) {
    NativeGOPPresenter *p=gHandoff;
    IOMemoryMap *m=p?(index==1?p->backMapping:index==2?p->thirdMapping:nullptr):nullptr;
    if(!p || !p->flipMode || !p->online || !m)return nullptr;
    IOMemoryDescriptor *memory=m->getMemoryDescriptor();
    if(memory)memory->retain();
    return memory;
}
IOReturn NativeGOPPresenter::newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out) {
    if(!out || type)return kIOReturnBadArgument;
    *out=nullptr;NativeGOPClient *client=new NativeGOPClient;if(!client)return kIOReturnNoMemory;
    bool attached=false;
    if(!client->initWithTask(task,security,type,properties) || !(attached=client->attach(this)) || !client->start(this)) {
        if(attached)client->detach(this);client->release();return kIOReturnNotOpen;
    }
    *out=client;return kIOReturnSuccess;
}
bool NativeGOPClient::initWithTask(task_t ownerTask,void *security,UInt32 type,OSDictionary *properties) {
    if(!ownerTask || type || clientHasPrivilege(security,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess ||
       !IOUserClient::initWithTask(ownerTask,security,type,properties))return false;
    task=ownerTask;task_reference(task);return true;
}
bool NativeGOPClient::start(IOService *provider) {
    controller=OSDynamicCast(NativeGOPPresenter,provider);
    if(!controller || !IOUserClient::start(provider))return false;
    opened=controller->open(this);return opened;
}
void NativeGOPClient::stop(IOService *provider) {
    if(opened){if(controller)controller->abandonSwap();controller->close(this);opened=false;}controller=nullptr;IOUserClient::stop(provider);
}
void NativeGOPClient::free() {if(task){task_deallocate(task);task=TASK_NULL;}IOUserClient::free();}
IOReturn NativeGOPClient::modeAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureOutputDescriptor || !a->structureOutput)return kIOReturnUnsupported;
    memcpy(a->structureOutput,&client->controller->mode,sizeof(NGPMode));return kIOReturnSuccess;
}
IOReturn NativeGOPClient::presentAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureInputDescriptor || a->structureOutputDescriptor || !a->structureInput || !a->structureOutput)return kIOReturnUnsupported;
    NGPPresent q={};NGPTransfer result={};memcpy(&q,a->structureInput,sizeof(q));
    IOReturn status=client->controller->transfer(client->task,q,&result);
    if(status==kIOReturnSuccess)memcpy(a->structureOutput,&result,sizeof(result));return status;
}
IOReturn NativeGOPClient::waitAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureInputDescriptor || a->structureOutputDescriptor || !a->structureInput || !a->structureOutput)return kIOReturnUnsupported;
    NGPWait q={};NGPSwap swap={};memcpy(&q,a->structureInput,sizeof(q));
    IOReturn status=client->controller->waitSwap(client->task,q.timeout_ms,&swap);
    if(status==kIOReturnSuccess)memcpy(a->structureOutput,&swap,sizeof(swap));return status;
}
IOReturn NativeGOPClient::completeAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureInputDescriptor || a->structureOutputDescriptor || !a->structureInput)return kIOReturnUnsupported;
    NGPComplete q={};memcpy(&q,a->structureInput,sizeof(q));
    return client->controller->completeSwap(q.sequence,(int)q.status);
}
IOReturn NativeGOPClient::externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) {
    static const IOExternalMethodDispatch methods[]={{modeAction,0,0,0,sizeof(NGPMode)},
                                                    {presentAction,0,sizeof(NGPPresent),0,sizeof(NGPTransfer)},
                                                    {waitAction,0,sizeof(NGPWait),0,sizeof(NGPSwap)},
                                                    {completeAction,0,sizeof(NGPComplete),0,0}};
    if(selector>=4)return kIOReturnUnsupported;
    return IOUserClient::externalMethod(selector,a,const_cast<IOExternalMethodDispatch *>(methods+selector),this,nullptr);
}
IOReturn NativeGOPClient::clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) {
    if(type>NGP_MEMORY_SCANOUT_BACK_RW || !options || !memory)return kIOReturnBadArgument;
    *memory=nullptr;if(!opened || !controller || !controller->online)return kIOReturnNotReady;
    if(type==NGP_MEMORY_SCANOUT_BACK_RW) {
        if(!controller->flipMode || !controller->backMapping)return kIOReturnUnsupported;
        *memory=controller->backMapping->getMemoryDescriptor();if(!*memory)return kIOReturnNoMemory;
        (*memory)->retain();return kIOReturnSuccess;
    }
    // Type 1: the scanout itself, writable, for the GPU to write; only in GPU mode, where no CPU copy exists.
    if(type==NGP_MEMORY_SCANOUT_RW && !controller->gpuMode)return kIOReturnUnsupported;
    *memory=controller->mapping->getMemoryDescriptor();if(!*memory)return kIOReturnNoMemory;
    (*memory)->retain();if(type!=NGP_MEMORY_SCANOUT_RW)*options|=kIOMapReadOnly;return kIOReturnSuccess;
}
