#include "NativeLegacyRequest.h"
#include "NativeLegacyScale.h"
#include "FajitaDisplayLogical.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" void native_legacy_backend_miss(unsigned,void *,void *);   // LegacyDisplayProbe.cpp: logs and returns
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" void native_legacy_stop_observed(unsigned reason,unsigned detail,void *caller);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// A finished frame whose request was no longer pending (why: 1 another head or none, 2 a current swap in the slot, 3 the links did not match).
extern "C" void native_legacy_retire_lost_observed(unsigned why,const void *request);
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" void native_legacy_gpu_fallback_observed(int status);
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
// Hardware vsync (boot-arg fajita-hw-vsync=1, fajita only; replaces the sheng-vsync timer): the board's DPU interrupt (package/FajitaDpu.cpp,
// PINGPONG_0 rd_ptr: once per frame the DPU sends the panel) triggers an interrupt event source on this work loop, whose action sends the
// compositor's vsync with the interrupt's mach time and the measured interval. Weak: absent on other boards (the timer stays).
extern "C" int fajita_dpu_vsync_attach(void (*vsync)(void *,uint64_t),void *context) __attribute__((weak));
// [0] events delivered, [1] sent to a client, [2] no client, [3] coalesced (more than one interrupt per action), [4] attach result + 100,
// [5] the last interval in us.
extern "C" {volatile uint32_t native_legacy_hw_vsync[8];}
// The panel's present timeline (package/FajitaDpu.cpp, fajita-fb-flip): the mach time of every VIG0 flip latch. Weak: other boards.
extern "C" volatile uint64_t fajita_dpu_latch_ring[2048] __attribute__((weak));
extern "C" volatile uint32_t fajita_dpu_latch_n __attribute__((weak));
// Frame info (fajita-frame-info, default 1): the keys the display reports per presented frame, in the order supportedFrameInfo
// (selector 74) enumerates them, and what was sent: [0] notifications, [1] clients they went to, [2] send failures, [3] frames with no
// client registered, [4] mode-2 frames whose swap id differed from the completed generation (mode 1's id), [5] the last id sent.
extern "C" {volatile uint32_t native_legacy_frame_info[6];}
// The frame-info backend calls answered with success: [0] slot 292 (registration), [1] slot 211 (ChangeFrameInfo).
extern "C" {volatile uint32_t native_legacy_frame_info_leaf[2];}
static const char *const kFrameInfoKeys[]={"Presentation_time","Vbl_FrameTime"};
// IOUserClient's async send is protected; the original family's own senders call it from its user client. Never instantiated.
struct NativeFrameInfoSend : public IOUserClient {
    static IOReturn send(OSAsyncReference64 reference,io_user_reference_t *args,UInt32 count) {
        return sendAsyncResult64WithOptions(reference,kIOReturnSuccess,args,count,kIOUserNotifyOptionCanDrop);
    }
};
// fajita-frame-info: 0 off, 1 (default) the frame's id is the family's completed generation, 2 it is the swap id of the request that
// completed last, read from the request itself (NATIVE_LEGACY_REQUEST_SWAP_ID, the id CA got from SwapBegin: QuartzCore's
// IOMFBDisplay::collect_frame_info matches its record by that exact id, NOTES-gfx.md "CA's display timer"); 2 falls back to 1 on a
// build whose offset is not qualified.
static unsigned frameInfoMode() {
    static int mode=-1;
    if(mode<0){unsigned v=1;PE_parse_boot_argn("fajita-frame-info",&v,sizeof v);mode=(int)(v>2?1:v);if(mode==2 && NATIVE_LEGACY_REQUEST_SWAP_ID<0)mode=1;}
    return (unsigned)mode;
}
static bool frameInfoEnabled() {return frameInfoMode()!=0;}
extern "C" uint64_t mach_continuous_time(void);
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}
// Differential copy is opt-in (sheng-fb-diff=1), see the copy below.
static bool diffCopyEnabled() {
    static int enabled=-1;
    if(enabled<0){unsigned v=0;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);enabled=(v==1);}
    return enabled;
}
// fajita: the logical display (FajitaDisplayLogical.h). Written once in startHardware, read by touch.
volatile struct fajita_display_logical fajita_display_logical;
// fajita-display-logical=<W>x<H>: the size userland is told. 0 when absent or malformed.
static bool fajitaLogicalSize(uint32_t *width,uint32_t *height) {
    char text[32]={};
    if(!PE_parse_boot_argn("fajita-display-logical",text,sizeof(text)-1))return false;
    uint64_t value[2]={0,0};unsigned part=0;const char *c=text;
    for(;;++c) {
        if(*c>='0' && *c<='9'){value[part]=value[part]*10+(uint64_t)(*c-'0');if(value[part]>16384)return false;continue;}
        if((*c=='x' || *c=='X') && part==0 && c!=text){++part;continue;}
        break;
    }
    if(*c || part!=1 || !value[0] || !value[1])return false;
    *width=(uint32_t)value[0];*height=(uint32_t)value[1];return true;
}
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    // fajita: the real scanout size, and with fajita-display-logical the scaling of each logical frame onto it. Every size userland
    // sees (the original dimension fields, NATIVE_LEGACY_WIDTH/HEIGHT, hence the IOMFB properties, modes and swap validation) is the logical one.
    uint32_t panelWidth=0,panelHeight=0;
    bool logical=false,letterboxValid=false;
    native_legacy_scale_map scaleMap={};
    uint32_t *scaleTables=nullptr;size_t scaleTableBytes=0;
    uint32_t *scaleLine=nullptr;size_t scaleLineBytes=0;
    void releaseScale() {
        if(scaleTables)IOFree(scaleTables,scaleTableBytes);
        if(scaleLine)IOFreeAligned(scaleLine,scaleLineBytes);
        scaleTables=nullptr;scaleLine=nullptr;scaleTableBytes=scaleLineBytes=0;scaleMap={};logical=false;letterboxValid=false;
    }
    bool planScale(uint32_t lw,uint32_t lh,uint32_t pw,uint32_t ph) {
        releaseScale();
        native_legacy_scale_map m={};m.lw=lw;m.lh=lh;m.pw=pw;m.ph=ph;
        if(!native_legacy_scale_fit(lw,lh,pw,ph,&m.ox,&m.oy,&m.ow,&m.oh))return false;
        size_t entries=2*(size_t)m.ow+2*(size_t)m.oh;
        scaleTableBytes=entries*sizeof(uint32_t);scaleTables=static_cast<uint32_t *>(IOMalloc(scaleTableBytes));
        scaleLineBytes=(size_t)m.ow*4;scaleLine=static_cast<uint32_t *>(IOMallocAligned(scaleLineBytes,64));
        if(!scaleTables || !scaleLine){releaseScale();return false;}
        m.col0=scaleTables;m.col1=m.col0+m.ow;m.row0=m.col1+m.ow;m.row1=m.row0+m.oh;
        native_legacy_scale_taps(m.col0,m.col1,m.ow,lw,4);
        native_legacy_scale_taps(m.row0,m.row1,m.oh,lh,1);
        scaleMap=m;logical=true;letterboxValid=false;return true;
    }
    // One logical frame onto the panel: the letterbox once (until something else paints the whole panel), then the image rectangle,
    // with the differential store of sheng-fb-diff=1 against a shadow of the scaled image. Bytes stored, or UINT64_MAX.
    uint64_t scaledCopy(uint64_t rowBytes,const void *source,uint64_t sourceBytes,uint64_t sourceStride) {
        volatile void *panel=reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress());
        const uint64_t panelBytes=bootMapping->getLength();
        if(!letterboxValid) {
            if(!native_legacy_scale_letterbox(panel,panelBytes,rowBytes,&scaleMap,0xff000000U))return UINT64_MAX;
            letterboxValid=true;
        }
        void *useShadow=nullptr;
        const uint64_t need=(uint64_t)scaleMap.ow*scaleMap.oh*4;
        if(diffCopyEnabled()) {
            if(!shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(shadow && shadowBytes>=need)useShadow=shadow;
        }
        const uint64_t stored=native_legacy_scale_copy(panel,panelBytes,rowBytes,source,sourceBytes,sourceStride,&scaleMap,scaleLine,
            useShadow,useShadow && shadowValid);
        shadowValid=useShadow && stored!=UINT64_MAX;
        return stored;
    }
    void publishLogical() {
        volatile struct fajita_display_logical &out=fajita_display_logical;
        out.valid=0;__asm__ volatile("dmb ish" ::: "memory");
        if(logical) {
            out.logical_width=scaleMap.lw;out.logical_height=scaleMap.lh;
            out.image_x=scaleMap.ox;out.image_y=scaleMap.oy;out.image_width=scaleMap.ow;out.image_height=scaleMap.oh;
        } else {
            out.logical_width=panelWidth;out.logical_height=panelHeight;
            out.image_x=0;out.image_y=0;out.image_width=panelWidth;out.image_height=panelHeight;
        }
        out.panel_width=panelWidth;out.panel_height=panelHeight;
        __asm__ volatile("dmb ish" ::: "memory");out.valid=1;
    }
    uint32_t scanBudget=32;
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue (NATIVE_LEGACY_VSYNC_CLIENTS) never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    IOInterruptEventSource *hwVsync=nullptr;
    volatile uint64_t hwStamp=0;
    uint64_t hwLast=0;
    static void hwVsyncInterrupt(void *context,uint64_t stamp) {
        NativeLegacyDisplay *d=static_cast<NativeLegacyDisplay *>(context);
        d->hwStamp=stamp;
        if(IOInterruptEventSource *source=d->hwVsync)source->interruptOccurred(nullptr,nullptr,0);
    }
    static void hwVsyncFired(OSObject *owner,IOInterruptEventSource *,int count) {static_cast<NativeLegacyDisplay *>(owner)->hwVsyncTick(count);}
    // Frame info (selector 74's keys, below), as the panel's own displays report each presented frame: the render server's display
    // timer locks its phase and period to Presentation_time (QuartzCore IOMFBServer::frame_info_callback -> IOMFBDisplay::add_timing).
    // Without it the timer ran free against the panel, at the family's 1/60 s. Sent on this work loop (the family's command gate's,
    // which registers the type-3 notification clients) at the first rd_ptr after a new flip latched, to every client registered for
    // frame info (the family's per-type client queue 3; each node's OSAsyncReference64 at +0x18, its next at +0, as the original
    // vsync sender walks queue 5). The arguments are IOMobileFramebuffer's FrameInfoNotifyFunc's: the key mask with bit 31 (each
    // value as two 32-bit halves), the frame's number, then the values in key order (mach times).
    uint32_t frameInfoLatches=0;
    uint32_t completedSwapId=0;bool completedSwapIdValid=false;   // the last completed request's own swap id (fajita-frame-info=2)
    void sendFrameInfo(uint64_t vblInterval) {
        if(!frameInfoEnabled() || !&fajita_dpu_latch_n || !&fajita_dpu_latch_ring)return;
        const uint32_t n=fajita_dpu_latch_n;
        if(n==frameInfoLatches || !n)return;
        frameInfoLatches=n;
        const uint64_t presented=fajita_dpu_latch_ring[(n-1)%2048];
        uint32_t generation=0;memcpy(&generation,reinterpret_cast<const unsigned char *>(this)+NATIVE_LEGACY_COMPLETED_GENERATION,4);
        uint32_t id=generation;
        if(frameInfoMode()==2 && completedSwapIdValid) {
            id=completedSwapId;
            if(id!=generation)native_legacy_frame_info[4]=native_legacy_frame_info[4]+1;
        }
        native_legacy_frame_info[5]=id;
        const uint64_t values[2]={presented,vblInterval};
        io_user_reference_t args[2+2*2];
        args[0]=0x80000000ull|((1u<<2)-1);args[1]=id;
        for(unsigned i=0;i<2;++i){args[2+2*i]=values[i]&0xffffffffull;args[3+2*i]=values[i]>>32;}
        native_legacy_frame_info[0]=native_legacy_frame_info[0]+1;
        const void *node=nullptr;memcpy(&node,reinterpret_cast<const unsigned char *>(this)+NATIVE_LEGACY_FRAME_INFO_CLIENTS,sizeof node);
        if(!node){native_legacy_frame_info[3]=native_legacy_frame_info[3]+1;return;}
        for(unsigned guard=0;node && guard<16;++guard) {
            const unsigned char *bytes=reinterpret_cast<const unsigned char *>(node);
            if(bytes[0x74]) {
                OSAsyncReference64 reference;memcpy(reference,bytes+0x18,sizeof reference);
                const IOReturn sent=NativeFrameInfoSend::send(reference,args,2+2*2);
                if(sent)native_legacy_frame_info[2]=native_legacy_frame_info[2]+1;else native_legacy_frame_info[1]=native_legacy_frame_info[1]+1;
            }
            memcpy(&node,bytes,sizeof node);
        }
    }
    void hwVsyncTick(int count) {
        native_legacy_hw_vsync[0]=native_legacy_hw_vsync[0]+1;
        if(count>1)native_legacy_hw_vsync[3]=native_legacy_hw_vsync[3]+1;
        const uint64_t stamp=hwStamp;
        const uint64_t interval=hwLast && stamp>hwLast?(stamp-hwLast)/(count>0?(uint64_t)count:1):vsyncInterval;
        hwLast=stamp;
        {uint64_t ns=0;absolutetime_to_nanoseconds(interval,&ns);native_legacy_hw_vsync[5]=(uint32_t)(ns/1000);}
        sendFrameInfo(interval);
        if(!vsyncWanted()){native_legacy_hw_vsync[2]=native_legacy_hw_vsync[2]+1;return;}
        native_legacy_vsync_notify(this,stamp,mach_continuous_time(),interval);
        native_legacy_hw_vsync[1]=native_legacy_hw_vsync[1]+1;
    }
    bool startHwVsync() {
        unsigned on=0;PE_parse_boot_argn("fajita-hw-vsync",&on,sizeof(on));
        if(!on || !fajita_dpu_vsync_attach || !primary || hwVsync)return false;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return false;
        IOInterruptEventSource *source=IOInterruptEventSource::interruptEventSource(this,&NativeLegacyDisplay::hwVsyncFired,nullptr,0);
        if(!source)return false;
        if(loop->addEventSource(source)!=kIOReturnSuccess){source->release();return false;}
        nanoseconds_to_absolutetime(1000000000ull/60,&vsyncInterval);vsyncHz=60;
        hwVsync=source;
        const int a=fajita_dpu_vsync_attach(&NativeLegacyDisplay::hwVsyncInterrupt,this);
        native_legacy_hw_vsync[4]=(uint32_t)(a+100);
        if(a){loop->removeEventSource(source);hwVsync=nullptr;source->release();return false;}
        native_legacy_vsync_observed(4,60);
        return true;
    }
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+NATIVE_LEGACY_VSYNC_CLIENTS,sizeof(head));return head;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        if(!vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    void vsyncTick() {
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncWanted()){native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        if(startHwVsync())return;   // the DPU's frame interrupt paces the compositor; no timer
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(IOInterruptEventSource *source=hwVsync) {   // the board's handler finds no source from here on
            hwVsync=nullptr;
            if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(source);
            source->release();
        }
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;} args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->finishGpuSwap(a->status);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else finishGpuSwap(status);
    }
    void free() override {releaseScale();fajita_display_logical.valid=0;
        if(shadow){IOFreeAligned(shadow,(vm_size_t)shadowBytes);shadow=nullptr;shadowBytes=0;}
        stopVsync();if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    // The original start publishes NATIVE_LEGACY_DEBUG_INFO_KEY, an OSSerializer whose callback runs the debug-info slot
    // (NATIVE_LEGACY_DEBUG_INFO_SLOT: 289 on 20H392, 303 on 21H461) through the command gate at NATIVE_LEGACY_COMMAND_GATE, which the
    // original init creates. The original stop releases and clears that gate but leaves the property, so the next serialization of the
    // stopped object faulted on the cleared gate (20H392: pc IOMobileGraphicsFamily+0x2a60c, the boot log's property dump of a display
    // stopped at start). The property goes before every original stop. reason/detail: native_legacy_stop_observed (LegacyDisplayProbe).
    void stopOriginal(IOService *provider,unsigned reason,unsigned detail,void *caller) {
        removeProperty(NATIVE_LEGACY_DEBUG_INFO_KEY);
        IOMobileFramebufferLegacy::stop(provider);
        native_legacy_stop_observed(reason,detail,caller);
    }
    void stop(IOService *provider) override {
        stopVsync();
        transferred=false;for(auto &entry: validated)entry=nullptr;gpuRequest=nullptr;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;stopOriginal(provider,1,0,__builtin_return_address(0));OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840
    // on 21H461, the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part
    // of this port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is
    // the original state setter (virtual 354 on 21H461, NATIVE_LEGACY_STATE_SETTER_SLOT, the one the original code calls with this same
    // request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[NATIVE_LEGACY_STATE_SETTER_SLOT];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
        if(!startDisplay(provider))return false;
        startVsync();return true;
    }
    bool startDisplay(IOService *provider) {
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            const unsigned refused=(enabled!=1)|(phase!=4)<<1|(!provider)<<2|(!bootMapping)<<3|(!primary)<<4;
            native_legacy_pm_observed(1,kIOReturnNotReady,0);stopOriginal(provider,2,refused,__builtin_return_address(0));return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);stopOriginal(provider,3,0,__builtin_return_address(0));return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;stopOriginal(provider,4,(unsigned)status,__builtin_return_address(0));return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && ((hz>60 && hz<=240) || panelRefreshUs()))native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh (int-209: no vsync client ever
    // registered, frames came every 16.7 ms), and the generic slot 301 fixes it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's
    // own rate instead; the maximum interval and the step keep the original values. Words in 32.32 seconds, then their mach times.
    // fajita: with the panel's TE pacing the frames (fajita-hw-vsync), the interval is the panel's own: fajita-refresh-us=<us> (default
    // 16685, 59.94 Hz, the TE period measured by the DPU's rd_ptr; 0 keeps the family's 1/60 s). The render server's display timer runs
    // at this interval; at 1/60 s it ran 0.1% fast against the panel and slipped a frame every ~17 s.
    uint64_t panelRefreshUs() const {
        unsigned hw=0;PE_parse_boot_argn("fajita-hw-vsync",&hw,sizeof(hw));
        if(!hw)return 0;
        unsigned us=16685;PE_parse_boot_argn("fajita-refresh-us",&us,sizeof(us));
        return us>=4000 && us<=34000?us:0;
    }
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        if(const uint64_t us=panelRefreshUs()) {
            const uint64_t interval=(us<<32)/1000000ull;   // 32.32 seconds
            uint64_t machTime=0;nanoseconds_to_absolutetime(us*1000ull,&machTime);
            words[0]=interval;words[1]=machTime;
            if(fixed){words[2]=interval;words[3]=machTime;}
            native_legacy_vsync_observed(5,(unsigned)(1000000ull/us));
            return true;
        }
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<=60 || hz>240)return true;
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+NATIVE_LEGACY_REQUEST_SURFACES,sizeof(surface));
        if(!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            // The panel, whole: with a logical display this also blackens the letterbox.
            uint32_t bw=panelWidth,bh=panelHeight;
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            shadowValid=false;letterboxValid=true;
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            {const uint32_t colors[4]={0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=native_legacy_surface_stride(surface);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=panelWidth || video.v_height!=panelHeight || video.v_depth!=32 || video.v_rotate ||
           (!logical && (width!=panelWidth || height!=panelHeight)) ||
           !native_legacy_surface_bounds(panelWidth,panelHeight,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
        // The GPU presenter copies a panel-sized frame one to one and scales a logical one into the fitted rectangle with the same taps as
        // the CPU copy below (GpuScanoutPresenter.h); it clears the letterbox itself, so the CPU copy's letterbox stays valid if it falls back.
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;return;
        }
        // fajita-fb-src-cached=1: read the surface through a cacheable mapping. Its default cache mode is the creator's (CoreAnimation
        // asks for write-combined surfaces), and the copy reads every source pixel (the scaled copy several times): uncached reads.
        // The lines are cleaned and invalidated first, so nothing cached from an earlier frame is copied.
        static int srcCached=-1;
        if(srcCached<0){unsigned v=0;PE_parse_boot_argn("fajita-fb-src-cached",&v,sizeof v);srcCached=v==1;}
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly|(srcCached?kIOMapCopybackCache:0));
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(srcCached) {
            uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
            const uintptr_t line=(uintptr_t)4<<((ctr>>16)&15);
            uintptr_t at=(uintptr_t)source->getVirtualAddress()&~(line-1);
            const uintptr_t end=(uintptr_t)source->getVirtualAddress()+(uintptr_t)allocation;
            for(;at<end;at+=line)__asm__ volatile("dc civac, %0"::"r"(at):"memory");
            __asm__ volatile("dsb sy" ::: "memory");
        }
        if(logical ? !native_legacy_scale_destination(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,&scaleMap) ||
            !native_legacy_surface_bounds(width,height,sourceStride,source->getLength(),source->getLength()) ||
            scaleMap.lw!=width || scaleMap.lh!=height :
            !native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        if(!native_legacy_prepare_scanout()) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=UINT64_MAX;
        if(logical) {
            stored=scaledCopy(video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride);
            if(stored==UINT64_MAX) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
        } else {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            const bool diffOff=!diffCopyEnabled();
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            if(!native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        OSSafeReleaseNULL(source);transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            finishTransfer(request,true);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(retire==1 || cpuPresent==1 || force) {
            // Only after the real copy and store barrier: select the actual
            // pending head and let the unchanged family release it and wake
            // waiters. This reports memory scanout completion, not a panel
            // vblank or hardware fence. Producer completion must precede
            // submission; this does not implement asynchronous GPU sync.
            if(!activateOnlyPendingSwap(request)) {
                // The request is no longer the family's pending head: its client went away while the frame was on its way (the render
                // server crashed during a GPU copy and the family discarded its swaps), or another current swap holds the slot. Returning
                // with the latch set wedged the queue for good (user-hn-freeze, 2026-10-26: backboardd died at 141 s, the restarted one's
                // swaps were validated but never copied, swap_wait_gated timed out every 5 s). The request is not touched (it may be
                // freed); drop it, count it, and take the next pending swap.
                const void *current=nullptr,*head=nullptr;
                memcpy(&current,reinterpret_cast<const unsigned char *>(this)+4872,8);   // the current-swap slot and the pending head (activateOnlyPendingSwap)
                memcpy(&head,reinterpret_cast<const unsigned char *>(this)+4880,8);
                const unsigned why=head==request?3:current?2:1;
                native_legacy_retire_lost_observed(why,request);
                if(why==1)removeValidated(request);   // gone; a head that is still there keeps its validation for the retry
                transferred=false;
                // Another head (or none): take it now. The same head still refused (2, 3) would be copied and refused again at once, so
                // it waits for the family's next kick instead of spinning here.
                if(why==1)kickSwapQueue();
                return;
            }
            removeValidated(request);
#if NATIVE_LEGACY_REQUEST_SWAP_ID >= 0
            // Read before the family completes (and may free) it: the request is the pending head activateOnlyPendingSwap selected.
            memcpy(&completedSwapId,reinterpret_cast<const unsigned char *>(request)+NATIVE_LEGACY_REQUEST_SWAP_ID,4);completedSwapIdValid=true;
#endif
            IOMobileFramebufferLegacy::completeCurrentSwap();
            native_legacy_retirement_observed(this);
            // The unchanged family has retired this generation and released
            // its current reference. Permit the next independently validated
            // single pending frame. Keep the latch set while completion can
            // reenter this method, and whenever retirement remains disabled.
            transferred=false;
            kickSwapQueue();
        }
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+NATIVE_LEGACY_REQUEST_SURFACES,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,native_legacy_surface_stride(surface),
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+NATIVE_LEGACY_REQUEST_SURFACES,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted)addValidated(request);else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property) {
            // fajita-display-ppi=<N>: the pixel density of the logical display (iPad7,1: 264), for a provider without dot-pitch.
            unsigned ppi=0;PE_parse_boot_argn("fajita-display-ppi",&ppi,sizeof(ppi));
            if(ppi && ppi<=2000){*pitch=ppi;return kIOReturnSuccess;}
            return kIOReturnNotFound;
        }
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    // 20H392 has no mask output (its caller passes index, count and the 64-byte key).
#if NATIVE_LEGACY_FRAME_INFO_MASK
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
#else
    // fajita-frame-info (default on): the keys sendFrameInfo reports, by index; the render server asks for its frame-info keys only when
    // this answers (QuartzCore EAGLServer: IOMobileFramebufferSupportedFrameInfo, then the type-3 run loop source and the callback).
    IOReturn supportedFrameInfo(uint32_t index,uint32_t *count,char *key) override {
        if(!count || !key)return kIOReturnBadArgument;
        bzero(key,64);
        if(!frameInfoEnabled() || !&fajita_dpu_latch_n){*count=0;return kIOReturnUnsupported;}
        *count=(uint32_t)(sizeof kFrameInfoKeys/sizeof *kFrameInfoKeys);
        if(index>=*count)return kIOReturnBadArgument;
        strlcpy(key,kFrameInfoKeys[index],64);
        return kIOReturnSuccess;
    }
#endif
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height)return false;
        // fajita-fb-wc=1: map the boot framebuffer write-combined (normal non-cacheable) instead of device memory, so the per-frame CPU
        // copy's stores are gathered. With kIOMapInhibitCache that copy was ~59% of all CPU samples on fajita (native_legacy_copy_pixels,
        // in backboardd). Each copy still ends with dsb sy before the swap completes.
        unsigned wc=0;PE_parse_boot_argn("fajita-fb-wc",&wc,sizeof wc);
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,wc==1?kIOMapWriteCombineCache:kIOMapInhibitCache);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            panelWidth=(uint32_t)video.v_width;panelHeight=(uint32_t)video.v_height;
            // fajita-display-logical=<W>x<H>: the original family reports W x H everywhere; frames are scaled onto the panel.
            uint32_t reportWidth=panelWidth,reportHeight=panelHeight,lw=0,lh=0;
            releaseScale();
            if(fajitaLogicalSize(&lw,&lh) && (lw!=panelWidth || lh!=panelHeight) && planScale(lw,lh,panelWidth,panelHeight)) {
                reportWidth=lw;reportHeight=lh;
            }
            installBootDimensions(reportWidth,reportHeight);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==reportWidth && height==reportHeight;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=video.v_display==1;
                // The mapping observer (and its display-scale policy) checks the panel against the console: it gets the panel size.
                valid=native_legacy_gop_mapping_observed(this,bootMapping,panelWidth,panelHeight);
                if(valid)publishLogical();
            }
        }
        OSSafeReleaseNULL(mapping);return valid;
    }
// A leaf the render server reaches logs loudly and fails the call (x0 = kIOReturnUnsupported) instead of panicking the kernel.
// With frame info on (fajita-frame-info=1/2) the two frame-info backend calls succeed instead (x0 = kIOReturnSuccess): on 20H392 slot
// 292, made when the render server registers its frame-info notification, and 211, ChangeFrameInfo (selector 73). Failing them made
// IOMobileFramebufferFrameInfo fail, so QuartzCore's EAGLServer called IOMFBDisplay::set_frame_info_enabled(false): CA kept no
// per-swap records and ignored every frame info sent (watcher check, session-144333: D+0x9f2 bit 35 clear; console: UNIMPLEMENTED
// SLOT 292, then 211 three times at 163 s). The sender needs nothing from them: it walks the family's client queue 3 itself.
static bool frameInfoLeaf(unsigned slot) {return frameInfoEnabled() && NATIVE_LEGACY_REQUEST_SWAP_ID>=0 && (slot==211 || slot==292);}
#define LEAF(n) __attribute__((noinline)) void opaque##n() override { \
    if(frameInfoLeaf(n)){native_legacy_frame_info_leaf[n==211]=native_legacy_frame_info_leaf[n==211]+1;__asm__ volatile("mov w0, #0":::"x0","memory");return;} \
    native_legacy_backend_miss(n,this,__builtin_return_address(0)); __asm__ volatile("mov w0, #0x2c7\n\tmovk w0, #0xe000, lsl #16":::"x0","memory"); }
    // 211, 245, 246, 306, 307, 338 and 345 on 21H461 (OriginalLegacyLayout.h).
    NATIVE_LEGACY_GUARDED_LEAVES(LEAF)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
