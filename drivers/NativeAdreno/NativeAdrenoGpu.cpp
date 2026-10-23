/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoGpu.h"
#include <IOKit/IOLib.h>
IOReturn NativeAdrenoGpu::fail(IOReturn status)
{
    failed_=true;ready_=false;
    if(reserved_ && !quiesced_) {
        quiesced_=board_.quiesce(board_.cookie)==kIOReturnSuccess;
    }
    return status;
}
IOReturn NativeAdrenoGpu::start(const NativeAdrenoGpuBoard *board,
    const NativeAdrenoGpuConfig *config)
{
    if(started_)return kIOReturnBusy;
    if(!board || !config || !board->reserve || !board->bootGmu ||
        !board->programSmmu || !board->quiesce)return kIOReturnBadArgument;
    if(!config->mmio.write32 || !config->mmio.read32 || !config->mmio.barrier ||
        !config->cache.publish || !config->cache.acquire || !config->ring ||
        !config->fence || !config->sqe_iova || !config->pwrup_reglist_iova ||
        !config->clock.now_us || !config->start_timeout_us)
        return kIOReturnBadArgument;
    started_=true;board_=*board;
    IOReturn status=board_.reserve(board_.cookie);
    if(status) {failed_=true;return status;}
    reserved_=true;steps_=1;
    if((status=board_.bootGmu(board_.cookie)))return fail(status);
    steps_=2;
    if((status=board_.programSmmu(board_.cookie)))return fail(status);
    steps_=3;
    if((status=ring_.attach(&config->mmio,&config->cache,config->ring,config->ring_bytes,
        config->ring_iova,config->fence,config->fence_bytes,config->fence_iova)))return fail(status);
    steps_=4;
    NativeAdrenoCpInitConfig cp{config->sqe_iova,config->pwrup_reglist_iova,0};
    if((status=cp_.run(&config->mmio,&ring_,&cp,&fence_)))return fail(status);
    /* A started CP is not ready until its init fence is actually observed. */
    const NativeAdrenoRingFence started=ring_.wait(fence_,&config->clock,
        config->start_timeout_us,nullptr);
    if(started!=NativeAdrenoRingFenceComplete)
        return fail(started==NativeAdrenoRingFenceTimeout?kIOReturnTimeout:kIOReturnIOError);
    steps_=5;
    if((status=irq_.attach(&config->mmio)))return fail(status);
    if((status=irq_.programMask(nullptr)))return fail(status);
    steps_=6;
    if(config->time.read32) {
        if((status=timer_.attach(&config->time,NativeAdrenoTimeGmu)))return fail(status);
        timerTimeOk_=true;
    }
    ready_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoGpu::stop()
{
    if(!ready_)return kIOReturnNotReady;
    if(ring_.poisoned() || irq_.poisoned() || cp_.poisoned())return kIOReturnBusy;
    if(ring_.outstanding())return kIOReturnBusy;
    IOReturn status=board_.quiesce(board_.cookie);
    quiesced_=status==kIOReturnSuccess;
    if(status) {failed_=true;ready_=false;return status;}
    if(irq_.detach())return fail(kIOReturnIOError);
    if(timerTimeOk_ && timer_.detach())return fail(kIOReturnIOError);
    timerTimeOk_=false;
    if(ring_.detach())return fail(kIOReturnIOError);
    ready_=false;
    return kIOReturnSuccess;
}
