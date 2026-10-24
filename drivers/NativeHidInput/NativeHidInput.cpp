// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit offline keyboard/consumer input source. Uses the unchanged original
// HID parser and event dispatch; no user-client entitlement is substituted.
#include "OriginalHidDeviceABI.h"
// fajita: the IOHIDFamily slots and field the edge wrapper uses, generated for the kernelcache the kernel is prelinked with
// (scripts/build-native-legacy-display.py, HID_EVENT: 21H461 for the iPad board, 20H392 for the ios16 board).
#include "OriginalHidEventLayout.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
extern "C" void native_hid_input_observed(unsigned,int);
extern "C" uint64_t mach_absolute_time(void);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);
extern "C" void native_touch_set_sink(void (*)(void *), void *) __attribute__((weak_import));
// fajita: the PMIC keys (package/FajitaPmic.cpp, fajita-pmic-keys=1): power, volume up, volume down as bits 0..2 of
// native_button_state; the sink runs after each change. With native-hid-buttons=1 report 2 carries them as Consumer Power,
// Volume Increment and Volume Decrement next to Menu.
extern "C" void native_button_set_sink(void (*)(void *), void *) __attribute__((weak_import));
extern "C" volatile uint32_t native_button_state __attribute__((weak_import));
// [0] reports sent, [1] last IOReturn, [2] last report byte, [3] sink installed
extern "C" {volatile uint32_t native_hid_button_stats[4];}
// Optional touch digitizer fed by the board touch probe (kits/sheng/TouchProbe.cpp): finger set published with a seqlock.
extern "C" {
extern volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
// fajita-touch-ts=1 (FajitaTouchProbe.cpp): the sample's mach_absolute_time, published inside the seqlock, handed to the original
// IOHIDDevice::handleReportWithTime (vtable index 197 of 20H392: handleReport at 181 reads the clock and calls it) instead of
// letting handleReport stamp the report with the time the work loop got to it. native_touch_ts_mode is 0 unless the arg is set.
extern volatile uint64_t native_touch_ts __attribute__((weak_import));
extern volatile uint32_t native_touch_ts_mode __attribute__((weak_import));
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
}
// Keyboard Application collection ensures the original generic keyboard
// personality can match. Report 2 carries Consumer Menu, not a claimed result
// that the system accepted Home. Report 1 is an ordinary boot keyboard report.
static const unsigned char descriptor[]={
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,
    0x75,0x08,0x95,0x06,0x81,0x00,
    0x85,0x02,0x05,0x0c,0x09,0x40,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x01,0x81,0x02,
    0x75,0x07,0x95,0x01,0x81,0x03,0xc0
};
// The same with report 2 widened: Menu, Power, Volume Increment, Volume Decrement (bits 0..3), four bits padding.
static const unsigned char buttonDescriptor[]={
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,
    0x75,0x08,0x95,0x06,0x81,0x00,
    0x85,0x02,0x05,0x0c,0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x04,0x81,0x02,
    0x75,0x04,0x95,0x01,0x81,0x03,0xc0
};
// Report 3: ten-finger Touch Screen collection (Tip Switch, Untouch (set when the tip is up), Contact Identifier, X, Y per finger, then Contact Count).
// Coordinates are normalised to 0..32767 on both axes by the producer.
#define FINGER_DESC \
    0x09,0x22,0xa1,0x02, 0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02, 0x09,0x47,0x81,0x02, 0x75,0x06,0x95,0x01,0x81,0x03, \
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x01,0x81,0x02, 0x09,0x31,0x81,0x02, 0x05,0x0d, 0xc0,
static const unsigned char touchDescriptor[]={
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x03,
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02,0xc0
};
#define TOUCH_FINGERS 10
// Edge contacts (int-163). A contact that starts within ~3 mm of a panel edge carries kIOHIDDigitizerEventFromEdgeTip (0x800) in its
// finger events' event mask for its whole life, as Apple's multitouch stack reports it (MultitouchHID). UIKit derives UITouch's edge
// type from that bit alone (_UIEventHIDTouchEdgeTypeForHIDEvent: 0x800/0x1000/0x2000/0x40000 -> 1..4), and every screen-edge
// recognizer of SpringBoard fails at once on edge type 0: int-162 logged edge=0 for touches starting 2 pt above the bottom edge and
// SBHomeGesturePanGestureRecognizer / SBFluidSwitcherScreenEdgePanGestureRecognizer going 0->5, so no home or switcher gesture.
// The original IOHIDEventDriver (IOHIDFamily-2031.120.4) never sets the bit. Its dispatchEvent (IOHIDEventService slot 198, the slot
// 37 of IOHIDFamily's indirect calls use) is wrapped for this device's driver only, in a copy of its vtable; the wrapper adds the bit
// to the finger children of each digitizer collection and calls the original. IOHIDEvent virtuals of that version: getType 18,
// getIntegerValue 23, setIntegerValue 24, getFixedValue 25 (setIntegerValue/getIntegerValue at +0xc0/+0xb8 in the driver's own
// calls); _children at +24 (IOHIDEvent.h; appendChild stores into it and free releases it). _parent is never set (appendChild only
// stores the child, int-163 skipped every finger by comparing it), so each child is an IOHIDEvent by its class instead.
// fajita: those numbers come from OriginalHidEventLayout.h, qualified against the candidate of the board's kernelcache. 20H392 has
// the same ones (its IOHIDEventDriver has 219 slots, one fewer than 21H461's 220, past slot 216).
namespace edge {
enum {kSlots=256,kDispatch=NATIVE_HID_DISPATCH_EVENT_SLOT,kGetType=NATIVE_HID_EVENT_GET_TYPE_SLOT,
      kGetInteger=NATIVE_HID_EVENT_GET_INTEGER_SLOT,kSetInteger=NATIVE_HID_EVENT_SET_INTEGER_SLOT,
      kGetFixed=NATIVE_HID_EVENT_GET_FIXED_SLOT,kChildren=NATIVE_HID_EVENT_CHILDREN,kContacts=256};
static_assert(NATIVE_HID_EVENT_DRIVER_SLOTS<=kSlots,"the vtable copy covers the original IOHIDEventDriver table");
static_assert(kDispatch<NATIVE_HID_EVENT_DRIVER_SLOTS,"dispatchEvent is a slot of the original IOHIDEventDriver table");
enum : uint32_t {kHandleReportWithTime=197,kDigitizer=11,kX=0xB0000,kY=0xB0001,kIndex=0xB0005,kMask=0xB0007,kTouch=0xB0009,kMaskTouch=2,kFromEdgeTip=0x800};
// 48 panel pixels of 3048 x 2032 (the sweet reference uses 48 px), in the driver's normalised IOFixed coordinates. A board can
// set its own zone in thousandths of the screen: native-touch-edge-side, -top, -bottom (the fajita iOS 16 board: HackiOS's
// iPhone X zone, 16/375, 16/812 and 34/812, the strip of the home bar, as 43, 20 and 42).
static int32_t kEdgeX=48*65536/3048,kEdgeTop=48*65536/2032,kEdgeBottom=48*65536/2032;
static void configure() {
    unsigned v=0;
    if(PE_parse_boot_argn("native-touch-edge-side",&v,sizeof v) && v<500)kEdgeX=(int32_t)(v*65536/1000);
    if(PE_parse_boot_argn("native-touch-edge-top",&v,sizeof v) && v<500)kEdgeTop=(int32_t)(v*65536/1000);
    if(PE_parse_boot_argn("native-touch-edge-bottom",&v,sizeof v) && v<500)kEdgeBottom=(int32_t)(v*65536/1000);
}
static uintptr_t originalDispatch;
static bool fromEdge[kContacts];
extern "C" {volatile uint32_t native_touch_edge_stats[4];}   // [0] installed, [1] contacts begun, [2] of them at an edge, [3] skipped events
// Timer ticks: [0] reports, [1] sum and [2] max of (handleReport start - sample time) in ticks (timestamp mode only), [3] sum and [4] max
// of the handleReport duration, [5] reports sent with the sample time.
extern "C" {volatile uint32_t native_hid_lat[6];}
template <typename R,typename... A> static R call(void *object,unsigned slot,A... args) {
    return reinterpret_cast<R (*)(void *,A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object,args...);
}
// One finger event; true when it now carries the bit.
static bool mark(void *child) {
    if(call<uint32_t,IOOptionBits>(child,kGetType,0)!=kDigitizer)return false;   // the scan-time vendor child
    const uint32_t index=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kIndex,0);
    if(index>=kContacts) {native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;return false;}
    const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kMask,0);
    const bool touch=call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kTouch,0)!=0;
    if((mask&kMaskTouch) && touch) {   // the contact begins: its position now decides
        const int32_t x=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kX,0),y=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kY,0);
        fromEdge[index]=x<kEdgeX || x>65536-kEdgeX || y<kEdgeTop || y>65536-kEdgeBottom;
        native_touch_edge_stats[1]=native_touch_edge_stats[1]+1;
        if(fromEdge[index])native_touch_edge_stats[2]=native_touch_edge_stats[2]+1;
    }
    const bool edged=fromEdge[index];
    if(edged)call<void,uint32_t,int32_t,IOOptionBits>(child,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
    if(!touch && (mask&kMaskTouch))fromEdge[index]=false;   // the lift keeps the bit; the next contact with this index starts fresh
    return edged;
}
static void dispatch(IOService *self,OSObject *event,IOOptionBits options) {
    if(event && call<uint32_t,IOOptionBits>(event,kGetType,0)==kDigitizer) {
        OSArray *children=*reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event)+kChildren);
        if(children && OSDynamicCast(OSArray,children) && children->getCount()) {
            // A collection (the driver's touch-screen dispatch): its mask is the union of its fingers', so it takes the bit as well.
            bool any=false;
            for(unsigned i=0;i<children->getCount();++i) {
                OSObject *child=children->getObject(i);
                if(child && child->metaCast("IOHIDEvent"))any|=mark(child);
                else native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;
            }
            if(any) {
                const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(event,kGetInteger,kMask,0);
                call<void,uint32_t,int32_t,IOOptionBits>(event,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
            }
        } else if(!children) mark(event);   // one transducer dispatched on its own
    }
    reinterpret_cast<void (*)(IOService *,OSObject *,IOOptionBits)>(originalDispatch)(self,event,options);
}
// This device's own event driver gets the wrapped copy; any other driver keeps the original table.
static void install(IORegistryEntry *driver) {
    if(originalDispatch || !driver)return;
    configure();
    uintptr_t *table=*reinterpret_cast<uintptr_t **>(driver);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,table,kSlots*sizeof(uintptr_t));
    originalDispatch=copy[kDispatch];copy[kDispatch]=(uintptr_t)&dispatch;
    __asm__ volatile("dmb ish":::"memory");
    *reinterpret_cast<uintptr_t **>(driver)=copy;
    native_touch_edge_stats[0]=1;
}
}  // namespace edge
class NativeHidInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHidInput)
    IOWorkLoop *inputLoop=nullptr;
    IOTimerEventSource *timer=nullptr;
    bool down=false;
    bool touchEnabled=false;
    // The digitizer is a separate service from the keyboard: backboardd matches digitizers by primary usage page/usage
    // (Digitizer/Touch Screen) and the Built-In flag, which a combined keyboard-first descriptor would not provide.
    bool touchOnly=false;
    bool eventDriverFound=false;
    bool buttons=false;            // native-hid-buttons=1: report 2 also carries the PMIC keys
    uint8_t buttonBits=0;          // keys last sent (bit 0 power, 1 volume up, 2 volume down)
    uint32_t lastSeq=0;
    // Fingers sent as down in the previous report; a vanished identifier gets one explicit tip-up report.
    int prevIds[TOUCH_FINGERS]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    int prevX[TOUCH_FINGERS]={},prevY[TOUCH_FINGERS]={};
    static unsigned transformCoordinate(int value,int maximum) {
        if(value<0)value=0;if(value>=maximum)value=maximum-1;
        return (unsigned)((long long)value*32767/(maximum-1));
    }
    // One finger set to the event driver. The touch thread (kits/sheng/TouchProbe.cpp) calls touchNotify after it publishes a frame; there is no timer
    // watching the seqlock any more (it woke the work loop every 8 ms for nothing).
    static void touchNotify(void *context) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::touchAction,self);
    }
    static IOReturn touchAction(OSObject *owner,void *,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        self->touchSend();
        return kIOReturnSuccess;
    }
    void touchSend() {
        NativeHidInput *self=this;
        if(!self->hasEventDriverCached())return;
        uint32_t before=native_touch_seq;
        if((before&1) || before==self->lastSeq)return;
        __asm__ volatile("dmb ish":::"memory");
        unsigned count=native_touch_count;if(count>TOUCH_FINGERS)count=TOUCH_FINGERS;
        int ids[TOUCH_FINGERS],xs[TOUCH_FINGERS],ys[TOUCH_FINGERS];
        for(unsigned i=0;i<count;++i){ids[i]=native_touch_ids[i];xs[i]=native_touch_xs[i];ys[i]=native_touch_ys[i];}
        const bool stamped=&native_touch_ts_mode && native_touch_ts_mode && &native_touch_ts;
        const uint64_t sampleTime=stamped?native_touch_ts:0;
        __asm__ volatile("dmb ish":::"memory");
        if(native_touch_seq!=before)return;      // torn read: retry on the next tick
        self->lastSeq=before;
        // Sizes follow the axis maxima of the touch core: 30480 long, 20320 short, mapped to the same 0..32767 range.
        // A contact sits in the report slot of its controller id, so the event driver's finger index (the position of the finger
        // collection) stays the same for the whole contact; packing them into slots 0..n-1 moved a surviving finger to index 0 when
        // an earlier one lifted, and lifted the wrong index.
        unsigned char report[1+TOUCH_FINGERS*6+1]={3};
        bool present[TOUCH_FINGERS]={};
        unsigned records=0;
        for(unsigned i=0;i<count;++i) {
            if(ids[i]<0 || ids[i]>=(int)TOUCH_FINGERS || present[ids[i]])continue;
            const unsigned slot=(unsigned)ids[i];
            unsigned x=transformCoordinate(xs[i],30480),y=transformCoordinate(ys[i],20320);
            unsigned char *f=report+1+slot*6;
            f[0]=1;f[1]=(unsigned char)slot;f[2]=(unsigned char)x;f[3]=(unsigned char)(x>>8);f[4]=(unsigned char)y;f[5]=(unsigned char)(y>>8);
            present[slot]=true;self->prevIds[slot]=(int)slot;self->prevX[slot]=(int)x;self->prevY[slot]=(int)y;++records;
        }
        // Slots that were down and are gone: tip up at the last known position.
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            if(present[j] || self->prevIds[j]<0)continue;
            unsigned char *f=report+1+j*6;
            f[0]=2;f[1]=(unsigned char)j;
            f[2]=(unsigned char)self->prevX[j];f[3]=(unsigned char)(self->prevX[j]>>8);
            f[4]=(unsigned char)self->prevY[j];f[5]=(unsigned char)(self->prevY[j]>>8);
            ++records;
            self->prevIds[j]=-1;
        }
        if(!records)return;
        report[1+TOUCH_FINGERS*6]=(unsigned char)records;
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=kIOReturnNoMemory;
        if(buffer) {
            const uint64_t t0=mach_absolute_time();
            if(sampleTime && (*reinterpret_cast<uintptr_t **>(this))[edge::kHandleReportWithTime]) {
                result=edge::call<IOReturn,uint64_t,IOMemoryDescriptor *,unsigned,IOOptionBits>(this,edge::kHandleReportWithTime,sampleTime,buffer,0,0);
                native_hid_lat[5]=native_hid_lat[5]+1;
                const uint32_t late=(uint32_t)(t0>sampleTime?t0-sampleTime:0);
                native_hid_lat[1]=native_hid_lat[1]+late;if(late>native_hid_lat[2])native_hid_lat[2]=late;
            } else result=self->handleReport(buffer,0,0);
            const uint32_t took=(uint32_t)(mach_absolute_time()-t0);
            native_hid_lat[0]=native_hid_lat[0]+1;native_hid_lat[3]=native_hid_lat[3]+took;if(took>native_hid_lat[4])native_hid_lat[4]=took;
        }
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6,result);
        native_touch_hid_reports=native_touch_hid_reports+1;
    }
    uint8_t report2() const {return (uint8_t)((down?1:0) | (buttons?(buttonBits&7)<<1:0));}
    static void buttonNotify(void *context) {
        NativeHidInput *self=static_cast<NativeHidInput *>(context);
        if(self && self->inputLoop)self->inputLoop->runAction(&NativeHidInput::buttonAction,self);
    }
    static IOReturn buttonAction(OSObject *owner,void *,void *,void *,void *) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return kIOReturnSuccess;
        self->buttonSend();
        return kIOReturnSuccess;
    }
    void buttonSend() {
        if(!buttons || !&native_button_state)return;
        uint8_t bits=(uint8_t)(native_button_state&7);
        if(bits==buttonBits)return;
        if(!hasEventDriverCached())return;   // nobody to tell yet; the next change retries
        buttonBits=bits;
        unsigned char report[]={2,report2()};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_button_stats[0]=native_hid_button_stats[0]+1;
        native_hid_button_stats[1]=(uint32_t)result;
        native_hid_button_stats[2]=report[1];
    }
    bool hasEventDriverCached() {
        if(!eventDriverFound) {
            IORegistryEntry *driver=eventDriver(this,4);
            eventDriverFound=driver!=nullptr;
            if(driver && (touchEnabled || touchOnly))edge::install(driver);
        }
        return eventDriverFound;
    }
    static bool hasEventDriver(IORegistryEntry *parent,unsigned depth) {return eventDriver(parent,depth)!=nullptr;}
    // The IOHIDEventDriver below this device, if one has attached.
    static IORegistryEntry *eventDriver(IORegistryEntry *parent,unsigned depth) {
        if(!depth)return nullptr;
        OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
        IORegistryEntry *found=nullptr;
        while(OSObject *child=it->getNextObject()) {
            IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
            if(!entry)continue;
            found=entry->metaCast("IOHIDEventDriver")?entry:eventDriver(entry,depth-1);
            if(found)break;
        }
        it->release();return found;
    }
    static void tick(OSObject *owner,IOTimerEventSource *source) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return;
        native_hid_live_checkpoint(self);
        if(self->touchOnly){source->setTimeoutMS(1000);return;}
        if(!self->down && !hasEventDriver(self,4)) {
            native_hid_input_observed(3,0);
            source->setTimeoutMS(1000);
            return;
        }
        native_hid_input_observed(3,1);
        if(!self->down && !native_hid_input_requested()) {source->setTimeoutMS(1000);return;}
        unsigned char report[]={2,static_cast<unsigned char>((self->down?0:1) | (self->buttons?(self->buttonBits&7)<<1:0))};
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->handleReport(buffer,0,0):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(self->down?5:4,result);
        // Always attempt release, even if down returned an error: input must
        // never be left pressed by an ambiguous report completion.
        if(!self->down) {self->down=true;source->setTimeoutMS(100);}
        else {self->down=false;source->setTimeoutMS(1000);}
    }
public:
    void setTouchOnly() {touchOnly=true;}
    IOWorkLoop *getWorkLoop() const override {return inputLoop;}
    OSString *newTransportString() const override {return OSString::withCString(touchOnly?"SPI":"Virtual");}
    OSString *newProductString() const override {return OSString::withCString(touchOnly?"Offline touch digitizer":"Offline HID input probe");}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if(!out)return kIOReturnBadArgument;
        unsigned char combined[sizeof(buttonDescriptor)+sizeof(touchDescriptor)];
        size_t size;
        if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);}
        else{
            if(buttons){size=sizeof(buttonDescriptor);memcpy(combined,buttonDescriptor,size);}
            else{size=sizeof(descriptor);memcpy(combined,descriptor,size);}
            // Default: one device, keyboard collection first, digitizer second (the arrangement whose event driver
            // and user client attach on hardware); the separate digitizer device is the opt-in sheng-touch-split.
            if(touchEnabled){memcpy(combined+size,touchDescriptor,sizeof(touchDescriptor));size+=sizeof(touchDescriptor);}
        }
        *out=IOBufferMemoryDescriptor::withBytes(combined,size,kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        inputLoop=IOWorkLoop::workLoop();if(!inputLoop)return false;
        // Request the normal generic HID event personality for this owned
        // keyboard. This is a matching property, not a client entitlement.
        if(!setProperty("HIDDefaultBehavior",true))return false;
        unsigned touch=0;
        touchEnabled=PE_parse_boot_argn("sheng-touch-hid",&touch,sizeof(touch)) && touch==1;
        if(touchOnly||touchEnabled){setProperty("Built-In",true);setProperty("DisplayIntegrated",true);}
        unsigned keys=0;
        buttons=!touchOnly && PE_parse_boot_argn("native-hid-buttons",&keys,sizeof(keys)) && keys==1;
        if(!IOHIDDevice::start(provider))return false;
        timer=IOTimerEventSource::timerEventSource(this,tick);
        if(!timer || inputLoop->addEventSource(timer)!=kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);IOHIDDevice::stop(provider);return false;
        }
        if(touchOnly||touchEnabled) {
            if(&native_touch_set_sink)native_touch_set_sink(&NativeHidInput::touchNotify,this);
        }
        if(buttons && &native_button_set_sink){native_button_set_sink(&NativeHidInput::buttonNotify,this);native_hid_button_stats[3]=1;}
        setProperty("OfflineInputProbe",true);
        native_hid_input_observed(2,0);
        timer->setTimeoutMS(1000);return true;
    }
    void stop(IOService *provider) override {
        if((touchOnly||touchEnabled) && &native_touch_set_sink)native_touch_set_sink(nullptr,nullptr);
        if(buttons && &native_button_set_sink)native_button_set_sink(nullptr,nullptr);
        if(timer){timer->cancelTimeout();inputLoop->removeEventSource(timer);OSSafeReleaseNULL(timer);}
        IOHIDDevice::stop(provider);
    }
    void free() override {OSSafeReleaseNULL(inputLoop);IOHIDDevice::free();}
};
OSDefineMetaClassAndStructors(NativeHidInput,IOHIDDevice)
extern "C" IOService *native_hid_input_allocate(void) {return new NativeHidInput;}
extern "C" IOService *native_hid_touch_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setTouchOnly();return device;}
