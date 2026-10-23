/* SPDX-License-Identifier: MIT */
/* Real timestamp reader over a software counter peer. No device access. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoTimer.h"
#include <cstdio>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Peer {
    uint32_t low=0,high=0;
    unsigned reads=0,fail_read=0,unstable=0;
    uint32_t last_low_address=0,last_high_address=0;
};
static Peer peer;
static int read32(void *cookie,uint32_t address,uint32_t *value)
{
    CHECK(cookie==&peer && value);++peer.reads;
    if(peer.reads==peer.fail_read)return 1;
    if(address==NativeAdrenoTimer::gmuLowOffset() || address==NativeAdrenoTimer::cpLowRegister()) {
        peer.last_low_address=address;*value=peer.low;return 0;
    }
    if(address==NativeAdrenoTimer::gmuHighOffset() || address==NativeAdrenoTimer::cpHighRegister()) {
        peer.last_high_address=address;
        /* Emulate a high-word change between the bracketing reads. */
        *value=peer.high;
        if(peer.unstable && peer.reads%3==1) {++peer.high;--peer.unstable;}
        return 0;
    }
    CHECK(0);return 1;
}
static NativeAdrenoTimeIo io{&peer,read32};
static unsigned unsettled_reads=0;
static int unsettled(void *cookie,uint32_t address,uint32_t *value)
{(void)cookie;(void)address;*value=++unsettled_reads;return 0;}
int main()
{
    /* The derived A740 GMU offsets are the pinned values. */
    {
        CHECK(NativeAdrenoTimer::gmuLowOffset()==0x14220);
        CHECK(NativeAdrenoTimer::gmuHighOffset()==0x14224);
        CHECK(NativeAdrenoTimer::cpLowRegister()==0x0980);
        CHECK(NativeAdrenoTimer::cpHighRegister()==0x0981);
        CHECK(NativeAdrenoTimer::gmuHighOffset()==NativeAdrenoTimer::gmuLowOffset()+4);
    }
    /* A stable 64-bit counter reads exactly, through both address spaces. */
    {
        peer=Peer{};peer.low=0x89abcdefu;peer.high=0x01234567u;
        NativeAdrenoTimer timer;
        CHECK(timer.attach(&io,NativeAdrenoTimeGmu)==kIOReturnSuccess);
        uint64_t ticks=0;
        CHECK(timer.read(&ticks)==kIOReturnSuccess);
        CHECK(ticks==UINT64_C(0x0123456789abcdef));
        CHECK(peer.last_low_address==0x14220 && peer.last_high_address==0x14224);
        CHECK(timer.detach()==kIOReturnSuccess && timer.detach()==kIOReturnNotReady);
        peer.reads=0;
        NativeAdrenoTimer cp;
        CHECK(cp.attach(&io,NativeAdrenoTimeCp)==kIOReturnSuccess);
        CHECK(cp.read(&ticks)==kIOReturnSuccess && ticks==UINT64_C(0x0123456789abcdef));
        CHECK(peer.last_low_address==0x0980 && peer.last_high_address==0x0981);
    }
    /* A high-word change is retried, then read consistently. */
    {
        peer=Peer{};peer.low=0xfffffff0u;peer.high=7;peer.unstable=1;
        NativeAdrenoTimer timer;CHECK(timer.attach(&io,NativeAdrenoTimeGmu)==kIOReturnSuccess);
        uint64_t ticks=0;
        CHECK(timer.read(&ticks)==kIOReturnSuccess);
        CHECK(peer.high==8 && ticks==UINT64_C(0x00000008fffffff0));
        CHECK(peer.reads==6); /* two bracketing triples */
    }
    /* Persistent instability and I/O failure are errors, not fabricated ticks. */
    {
        peer=Peer{};peer.low=1;peer.high=0;
        NativeAdrenoTimeIo missing{nullptr,nullptr};
        NativeAdrenoTimer unbound;
        CHECK(unbound.attach(nullptr,NativeAdrenoTimeCp)==kIOReturnBadArgument);
        CHECK(unbound.attach(&missing,NativeAdrenoTimeCp)==kIOReturnBadArgument);
        CHECK(unbound.attach(&io,7)==kIOReturnBadArgument);
        CHECK(unbound.attach(&io,0xffffffffu)==kIOReturnBadArgument);
        CHECK(unbound.read(nullptr)==kIOReturnBadArgument);
        NativeAdrenoTimer timer;CHECK(timer.attach(&io,NativeAdrenoTimeCp)==kIOReturnSuccess);
        uint64_t ticks=0;
        peer.fail_read=2;
        CHECK(timer.read(&ticks)==kIOReturnIOError && !ticks);
        peer.fail_read=0;peer.unstable=0;
        CHECK(timer.read(&ticks)==kIOReturnSuccess);
        CHECK(timer.read(nullptr)==kIOReturnBadArgument);
        CHECK(timer.attach(&io,NativeAdrenoTimeCp)==kIOReturnBusy);
        CHECK(timer.detach()==kIOReturnSuccess && timer.read(&ticks)==kIOReturnNotReady);
    }
    /* A torn loop that never settles is bounded and reported as a timeout. */
    {
        NativeAdrenoTimeIo never{nullptr,unsettled};
        unsettled_reads=0;
        NativeAdrenoTimer timer;CHECK(timer.attach(&never,NativeAdrenoTimeGmu)==kIOReturnSuccess);
        uint64_t ticks=0;
        CHECK(timer.read(&ticks)==kIOReturnTimeout && !ticks);
        CHECK(unsettled_reads==3*8);
    }
    std::printf("PASS: 1 derived-offset check, 2 stable reads, 1 rollover retry, 1 I/O failure and 1 bounded torn-loop timeout over a software counter peer\n");
}
