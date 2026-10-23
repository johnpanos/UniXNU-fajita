/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoVm.h"
#include "NativeAdrenoVmAlias.h"
extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" {int (*native_adreno_dram_read32)(uint64_t,uint32_t *);int (*native_adreno_cached_read32)(uint64_t,uint32_t *);
    int (*native_adreno_line_audit)(uint64_t,size_t,uint32_t *,uint32_t *);}
#include <string.h>
#include <IOKit/IOLib.h>
/* 1 once the board proved the GPU's cacheable inner-shareable accesses coherent with the CPU's caches in both directions (the A740's
 * ADRENO_QUIRK_HAS_CACHED_COHERENT behind the dma-coherent GPU SMMU, which Linux maps with IOMMU_CACHE): a mapping with write-back
 * attributes then needs no CPU cache maintenance. 0 keeps every mapping maintained as before. Counters: maintenance passes skipped for
 * coherent mappings at map, unmap, submission and completion. */
extern "C" {volatile uint32_t native_adreno_io_coherent;volatile uint32_t native_adreno_coherent_skips[4];}
static bool coherentMapping(unsigned writeback) {return writeback && native_adreno_io_coherent;}
static bool overlap(uint64_t a,uint64_t ab,uint64_t b,uint64_t bb)
{return a<b+bb && b<a+ab;}
/* Mappings of one cache/shareability/privilege class may share pages; only pages shared across classes refuse. */
static unsigned aliasClass(unsigned permissions,unsigned writeback,unsigned shareability)
{return writeback|(shareability<<1)|((permissions&ArmSmmu4KPrivileged)?8u:0u);}
static void reverse(ArmSmmu4KRange *first,ArmSmmu4KRange *last)
{while(first<last && first<--last) {const ArmSmmu4KRange t=*first;*first++=*last;*last=t;}}
static bool validBinding(const NativeAdrenoVmRegion &r,unsigned upper)
{
    return r.tables && r.arena && r.state && !r.tables->quarantined() &&
        r.tables->deviceReferences() && r.tables->spanCount()==1 &&
        r.tables->spans()[0].bytes==r.tables->bytes() && !r.tables->spans()[0].offset &&
        r.arena->mapping==r.tables->mapping() && r.arena->upper_region==upper &&
        r.arena->config.table_physical==r.tables->spans()[0].physical &&
        r.arena->capacity_pages==r.tables->bytes()/4096 && r.state->prepared &&
        !r.state->poisoned && !r.state->tables.leaf_pages;
}
ArmSmmu4KLiveRegion NativeAdrenoVm::region(unsigned upper) const
{
    return {regions_[upper].arena,regions_[upper].state,
        ranges_+(upper?lower_count_:0),upper?upper_count_:lower_count_};
}
/* Board hook and per-phase time of the two table transactions, for the log: [0] map, [1] unmap; each row is count, the phases in order, the slowest call
 * and the bytes mapped (int-134: BoMap averaged 135 ms and the desktop crawled). */
extern "C" {uint64_t (*native_adreno_vm_now_us)(void)=nullptr;volatile uint64_t native_adreno_vm_phase_us[2][8];}
namespace {
struct PhaseClock {
    uint64_t last=native_adreno_vm_now_us?native_adreno_vm_now_us():0,began=last;
    void lap(unsigned row,unsigned slot) {
        if(!native_adreno_vm_now_us)return;
        const uint64_t now=native_adreno_vm_now_us();native_adreno_vm_phase_us[row][slot]+=now-last;last=now;
    }
    void done(unsigned row,uint64_t bytes) {
        if(!native_adreno_vm_now_us)return;
        const uint64_t total=native_adreno_vm_now_us()-began;
        native_adreno_vm_phase_us[row][0]+=1;native_adreno_vm_phase_us[row][7]+=bytes;
        if(total>native_adreno_vm_phase_us[row][6])native_adreno_vm_phase_us[row][6]=total;
    }
};
}
ArmSmmu4KStatus NativeAdrenoVm::observe() const
{
    ArmSmmu4KStreamResult routes;
    ArmSmmu4KStatus status=ArmSmmu4KObserveStreams(&io_,streams_,context_,&routes);if(status)return status;
    ArmSmmu4KLiveRegion lower=region(0),upper=region(1);ArmSmmu4KLiveResult audit;
    status=ArmSmmu4KLiveAuditContext(context_,lower.arena?&lower:nullptr,upper.arena?&upper:nullptr,&audit);
    if(status)return status;
    return ArmSmmu4KObserveStreams(&io_,streams_,context_,&routes);
}
/* Global policy, the context image and the owned routes only. The scan of every other slot for a conflicting route read each
 * SMR/S2CR of the SMMU twice per map and unmap (int-152: 0.75 ms a pass, 1.3 s of the 3.2 s of 807 maps); observe(), which
 * observeCheap runs every 64 operations, keeps that scan. */
ArmSmmu4KStatus NativeAdrenoVm::observeStreamsOnly() const
{
    ArmSmmu4KStreamResult routes;
    return ArmSmmu4KObserveOwnedStreams(&io_,streams_,context_,&routes);
}
/* The owned routes and context are observed in full every 64th table operation; the ones between read no SMMU register before the
 * table transaction (int-159: a map read the owned routes before and after its transaction, 0.4 ms of its 1.7 ms). */
ArmSmmu4KStatus NativeAdrenoVm::observeCheap()
{
    if(++since_audit_>=64) {since_audit_=0;return observe();}
    return ArmSmmu4KSuccess;
}
IOReturn NativeAdrenoVm::fail(ArmSmmu4KStatus status)
{
    poisoned_=true;result_.operation=status;result_.complete=false;
    for(size_t i=0;mappings_ && i<NativeAdrenoVmMaxObjects;++i)if(mappings_[i].memory)mappings_[i].memory->quarantine();
    for(auto &r:regions_)if(r.arena) {r.tables->quarantine();r.state->poisoned=1;}
    ArmSmmu4KStreamResult routes;
    result_.quarantine=ArmSmmu4KFaultStreams(&io_,streams_,&routes);
    return kIOReturnIOError;
}
IOReturn NativeAdrenoVm::attach(const ArmSmmu4KIo *io,ArmSmmu4KContextConfig *context,
    ArmSmmu4KStreamState *streams,const NativeAdrenoVmRegion *lower,const NativeAdrenoVmRegion *upper,
    size_t capacity,unsigned budget)
{
    if(attached_ || ranges_ || poisoned_)return kIOReturnBusy;
    if(!io || !context || !streams || (!lower && !upper) || !capacity || capacity>262144 ||
        !budget || budget>1048576 || (lower && !validBinding(*lower,0)) || (upper && !validBinding(*upper,1)))
        return kIOReturnBadArgument;
    ranges_=IONewData(ArmSmmu4KRange,capacity);if(!ranges_)return kIOReturnNoMemory;
    mappings_=IONewData(Mapping,NativeAdrenoVmMaxObjects);
    if(!mappings_) {IODeleteData(ranges_,ArmSmmu4KRange,capacity);return kIOReturnNoMemory;}
    memset(mappings_,0,sizeof(Mapping)*NativeAdrenoVmMaxObjects);
    capacity_=capacity;io_=*io;context_=context;streams_=streams;budget_=budget;
    if(lower)regions_[0]=*lower;if(upper)regions_[1]=*upper;
    for(unsigned i=0;i<2;++i)if(regions_[i].arena) {
        if(NativeAdrenoBindSmmuVisibility(regions_[i].tables,&visibility_[i])!=kIOReturnSuccess) {
            IODeleteData(ranges_,ArmSmmu4KRange,capacity_);IODeleteData(mappings_,Mapping,NativeAdrenoVmMaxObjects);
            regions_[0]={};regions_[1]={};return kIOReturnBadArgument;
        }
    }
    attached_=true;
    ArmSmmu4KStatus status=observe();if(status)return fail(status);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::detach()
{
    if(poisoned_ || objects_ || spanCount())return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    IODeleteData(ranges_,ArmSmmu4KRange,capacity_);IODeleteData(mappings_,Mapping,NativeAdrenoVmMaxObjects);
    attached_=false;regions_[0]={};regions_[1]={};context_=nullptr;streams_=nullptr;capacity_=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::findAddress(unsigned upper,size_t bytes,uint64_t alignment,uint64_t *out) const
{
    if(!out)return kIOReturnBadArgument;*out=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(upper>1 || !regions_[upper].arena || !bytes || (bytes&4095) || alignment<4096 ||
        (alignment&(alignment-1)))return kIOReturnBadArgument;
    const ArmSmmu4KLiveArena *a=regions_[upper].arena;
    uint64_t offset=0;
    for(size_t i=upper?lower_count_:0;;++i) {
        uint64_t rem=(a->iova+offset)&(alignment-1),padding=rem?alignment-rem:0;
        if(padding>a->bytes-offset)return kIOReturnNoResources;offset+=padding;
        if(bytes>a->bytes-offset)return kIOReturnNoResources;
        if(i==(upper?spanCount():lower_count_) ||
            (ranges_[i].iova-a->iova>=offset && bytes<=ranges_[i].iova-a->iova-offset))
            {*out=a->iova+offset;return kIOReturnSuccess;}
        uint64_t end=ranges_[i].iova-a->iova+ranges_[i].bytes;
        if(end>offset)offset=end;
    }
}
IOReturn NativeAdrenoVm::preflight(const NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability) const
{
    if(!memory || upper>1 || !regions_[upper].arena || !memory->backed() || memory->quarantined() ||
        !memory->bytes() || !memory->spanCount() || !(permissions&ArmSmmu4KRead) || (permissions&~15u) ||
        writeback>1 || (shareability!=2 && shareability!=3))return kIOReturnBadArgument;
    const ArmSmmu4KLiveArena *a=regions_[upper].arena;
    if((iova&4095) || iova<a->iova || iova-a->iova>=a->bytes || memory->bytes()>a->bytes-(iova-a->iova))
        return kIOReturnBadArgument;
    if(objects_>=NativeAdrenoVmMaxObjects || memory->spanCount()>capacity_-spanCount())return kIOReturnNoResources;
    for(size_t i=0;i<spanCount();++i) {
        /* Native addresses may end at 2^64; compare offsets in the region. */
        if((i<lower_count_)==!upper && iova<=ranges_[i].iova+(ranges_[i].bytes-1) &&
            ranges_[i].iova<=iova+(memory->bytes()-1))return kIOReturnBusy;
    }
    if(a->config.output_bits<32 || a->config.output_bits>48)return kIOReturnBadArgument;
    uint64_t offset=0,limit=UINT64_C(1)<<a->config.output_bits;
    for(size_t j=0;j<memory->spanCount();++j) {
        const auto &span=memory->spans()[j];
        if(span.offset!=offset || !span.physical || !span.bytes || ((span.physical|span.bytes)&4095) ||
            span.bytes>memory->bytes()-offset || span.physical>=limit || span.bytes>limit-span.physical)
            return kIOReturnBadArgument;
        for(const auto &r:regions_)if(r.arena && overlap(span.physical,span.bytes,
            r.arena->config.table_physical,r.arena->capacity_pages*UINT64_C(4096)))return kIOReturnBadArgument;
        offset+=span.bytes;
    }
    if(offset!=memory->bytes())return kIOReturnBadArgument;
    /* Only another cache/shareability/privilege class can conflict. Reuse the
     * arena's caller-owned temporary alias scratch before the table transaction
     * uses it. Index the smaller side and query the other in O(log n); neither
     * the resource spans nor the actual page tables are modified here. */
    const unsigned mine=aliasClass(permissions,writeback,shareability);
    size_t other=0;
    for(size_t i=0;i<spanCount();++i)
        if(aliasClass(ranges_[i].permissions,ranges_[i].writeback,ranges_[i].shareability)!=mine)++other;
    if(!other)return kIOReturnSuccess;
    const bool indexNew=memory->spanCount()<=other;
    const size_t smaller=indexNew?memory->spanCount():other;
    uint32_t *order=a->alias_scratch;
    if(order && smaller<=a->alias_scratch_entries) {
        if(indexNew) {
            for(size_t i=0;i<smaller;++i)order[i]=(uint32_t)i;
            NativeAdrenoPhysicalIndex index(memory->spans(),order,smaller);
            for(size_t i=0;i<spanCount();++i) {
                const auto &r=ranges_[i];
                if(aliasClass(r.permissions,r.writeback,r.shareability)!=mine && index.overlaps(r.physical,r.bytes))return kIOReturnBadArgument;
            }
        } else {
            size_t n=0;
            for(size_t i=0;i<spanCount();++i)
                if(aliasClass(ranges_[i].permissions,ranges_[i].writeback,ranges_[i].shareability)!=mine)order[n++]=(uint32_t)i;
            NativeAdrenoPhysicalIndex index(ranges_,order,n);
            for(size_t j=0;j<memory->spanCount();++j)
                if(index.overlaps(memory->spans()[j].physical,memory->spans()[j].bytes))return kIOReturnBadArgument;
        }
        return kIOReturnSuccess;
    }
    /* Small/unconfigured callers retain the conservative allocation-free path. */
    for(size_t i=0;i<spanCount();++i) {
        const ArmSmmu4KRange &r=ranges_[i];
        if(aliasClass(r.permissions,r.writeback,r.shareability)==mine)continue;
        for(size_t j=0;j<memory->spanCount();++j)
            if(overlap(memory->spans()[j].physical,memory->spans()[j].bytes,r.physical,r.bytes))return kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}
NativeAdrenoVm::Mapping *NativeAdrenoVm::find(uint64_t handle)
{if(handle && mappings_)for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i)if(mappings_[i].handle==handle && mappings_[i].memory)return &mappings_[i];return nullptr;}
/* The n ranges just past the table's end (one mapping's spans, consecutive in IOVA and all in one gap) take their sorted place. */
void NativeAdrenoVm::insert(size_t n,unsigned upper)
{
    const size_t count=spanCount();
    size_t at=0;while(at<count && ranges_[at].iova<ranges_[count].iova)++at;
    reverse(ranges_+at,ranges_+count);reverse(ranges_+count,ranges_+count+n);reverse(ranges_+at,ranges_+count+n);
    if(upper)upper_count_+=n;else lower_count_+=n;
}
void NativeAdrenoVm::erase(size_t at,size_t n,unsigned upper)
{
    const size_t count=spanCount();
    memmove(ranges_+at,ranges_+at+n,(count-at-n)*sizeof(*ranges_));
    if(upper)upper_count_-=n;else lower_count_-=n;
}
IOReturn NativeAdrenoVm::map(NativeAdrenoMemory *memory,unsigned upper,uint64_t iova,
    unsigned permissions,unsigned writeback,unsigned shareability,uint64_t *handle)
{
    if(!handle)return kIOReturnBadArgument;*handle=0;result_={};
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    PhaseClock clock;
    IOReturn checked=preflight(memory,upper,iova,permissions,writeback,shareability);if(checked)return checked;
    if(next_handle_==UINT64_MAX)return kIOReturnNoResources;
    clock.lap(0,5); /* Previously omitted preflight; map slot 5 was unused. */
    ArmSmmu4KStatus status=observeCheap();if(status)return fail(status);
    clock.lap(0,1);
    checked=memory->holdForDevice();if(checked)return checked;
    Mapping *record=nullptr;for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i)if(!mappings_[i].memory){record=&mappings_[i];break;}
    if(!record) {memory->quarantine();return fail(ArmSmmu4KStateMismatch);}
    *record={memory,0,iova,memory->spanCount(),0,permissions,writeback,shareability,0};++objects_;
    if(coherentMapping(writeback))native_adreno_coherent_skips[0]=native_adreno_coherent_skips[0]+1;
    else {
        checked=memory->publishForDevice(0,memory->bytes(),permissions&ArmSmmu4KWrite,distrust_write_combined_);
        if(checked)return fail(ArmSmmu4KIoError);
    }
    clock.lap(0,2);
    /* Every span in one table transaction, staged in the table's free tail (preflight checked the room). */
    const size_t spans=record->spans;
    ArmSmmu4KRange *changes=ranges_+spanCount();
    for(size_t i=0;i<spans;++i) {
        const auto &span=memory->spans()[i];
        changes[i]={iova+span.offset,span.physical,span.bytes,permissions,writeback,shareability};
    }
    ArmSmmu4KLiveRegion lower=region(0),high=region(1);ArmSmmu4KLiveResult update;
    status=ArmSmmu4KLiveUpdateContextBatch(&io_,streams_->bank,context_,lower.arena?&lower:nullptr,
        high.arena?&high:nullptr,upper,&visibility_[upper],changes,spans,1,budget_,&update);
    if(status)return fail(status);
    clock.lap(0,3);
    insert(spans,upper);record->mapped_spans=spans;result_.completed_spans=spans;
    clock.lap(0,4);clock.done(0,memory->bytes());
    record->handle=next_handle_++;*handle=record->handle;result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::unmap(uint64_t handle)
{
    native_adreno_trace_stage=10;
    result_={};if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record)return kIOReturnBadArgument;
    if(record->jobs)return kIOReturnBusy;
    PhaseClock clock;
    ArmSmmu4KStatus status=observeCheap();
    native_adreno_trace_stage=11;
    if(status)return fail(status);
    clock.lap(1,1);
    unsigned upper=regions_[1].arena && record->iova>=regions_[1].arena->iova;
    if(coherentMapping(record->writeback))native_adreno_coherent_skips[1]=native_adreno_coherent_skips[1]+1;
    else if(record->permissions&ArmSmmu4KWrite) {
        if(record->memory->acquireDeviceWrites(0,record->memory->bytes())!=kIOReturnSuccess)
            return fail(ArmSmmu4KIoError);
    }
    clock.lap(1,2);
    /* The mapping's spans are consecutive in the table; unmap them in one transaction. */
    const size_t spans=record->spans;
    size_t at=0;while(at<spanCount() && ranges_[at].iova!=record->iova)++at;
    if(spans>spanCount()-at)return fail(ArmSmmu4KStateMismatch);
    for(size_t i=0;i<spans;++i) {
        const auto &span=record->memory->spans()[i];
        const ArmSmmu4KRange &r=ranges_[at+i];
        if(r.iova!=record->iova+span.offset || r.physical!=span.physical || r.bytes!=span.bytes ||
            r.permissions!=record->permissions || r.writeback!=record->writeback || r.shareability!=record->shareability)
            return fail(ArmSmmu4KStateMismatch);
    }
    ArmSmmu4KLiveRegion lower=region(0),high=region(1);ArmSmmu4KLiveResult update;
    native_adreno_trace_stage=12;
    status=ArmSmmu4KLiveUpdateContextBatch(&io_,streams_->bank,context_,lower.arena?&lower:nullptr,
        high.arena?&high:nullptr,upper,&visibility_[upper],ranges_+at,spans,0,budget_,&update);
    native_adreno_trace_stage=13;
    if(status)return fail(status);
    clock.lap(1,3);
    erase(at,spans,upper);record->mapped_spans=0;result_.completed_spans=spans;
    native_adreno_trace_stage=14;
    clock.lap(1,4);
    if(record->memory->retireAfterQuiescence()!=kIOReturnSuccess)return fail(ArmSmmu4KStateMismatch);
    clock.lap(1,5);clock.done(1,record->memory->bytes());
    *record={};--objects_;result_.complete=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::describe(uint64_t handle,uint64_t *iova,size_t *bytes,
    unsigned *permissions) const
{
    if(!iova || !bytes || !permissions)return kIOReturnBadArgument;
    *iova=0;*bytes=0;*permissions=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(size_t m=0;mappings_ && m<NativeAdrenoVmMaxObjects;++m)if(mappings_[m].handle==handle && mappings_[m].memory) {
        const Mapping &mapping=mappings_[m];
        *iova=mapping.iova;*bytes=mapping.memory->bytes();*permissions=mapping.permissions;
        return mapping.mapped_spans==mapping.spans?kIOReturnSuccess:kIOReturnNotReady;
    }
    return kIOReturnBadArgument;
}
/* TLBIALL of the bank before a submission. Where a table update has observed the bank's image (each region's whole-verification
 * update remembers its register offset), the invalidation alone: the image and faults were observed there and every 64th table
 * operation since (int-159: a submission spent 1.3 ms in the kernel, most of it reading SMMU registers). */
IOReturn NativeAdrenoVm::invalidateTlb()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    ArmSmmu4KProgramResult r;
    uint64_t offset=0;
    for(const auto &region:regions_)if(region.state && region.state->context_offset)offset=region.state->context_offset;
    const ArmSmmu4KStatus status=offset?ArmSmmu4KSyncContextTlb(&io_,offset,context_->asid,1,budget_,&r):
        ArmSmmu4KInvalidateAllOwnedContext(&io_,streams_->bank,context_,budget_,&r);
    return status?fail(status):kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::publishMapped()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory)continue;
        if(coherentMapping(m.writeback)) {native_adreno_coherent_skips[2]=native_adreno_coherent_skips[2]+1;continue;}
        if(m.memory->publishForDevice(0,m.memory->bytes(),true,distrust_write_combined_))return fail(ArmSmmu4KIoError);
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::acquireImported()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || !m.memory->imported() || !(m.permissions&ArmSmmu4KWrite))continue;
        if(coherentMapping(m.writeback)) {native_adreno_coherent_skips[3]=native_adreno_coherent_skips[3]+1;continue;}
        if(m.memory->acquireDeviceWrites(0,m.memory->bytes()))return fail(ArmSmmu4KIoError);
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::sampleWord(uint64_t iova,uint32_t *cached,uint32_t *dram) const
{
    if(!cached || !dram)return kIOReturnBadArgument;
    *cached=*dram=~0u;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(iova&3)return kIOReturnBadArgument;
    for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || iova<m.iova || iova-m.iova>=m.memory->bytes())continue;
        const size_t offset=(size_t)(iova-m.iova);
        if(m.memory->mapping())*cached=static_cast<const volatile uint32_t *>(m.memory->mapping())[offset/4];
        for(size_t j=0;j<m.memory->spanCount();++j) {
            const auto &span=m.memory->spans()[j];
            if(offset<span.offset || offset-span.offset>=span.bytes)continue;
            uint32_t value=0;
            /* DRAM first: the cached reader writes its line back, so read after it DRAM would always agree (int-099). */
            if(native_adreno_dram_read32 && !native_adreno_dram_read32(span.physical+(offset-span.offset),&value))*dram=value;
            /* A write-combined BO's own mapping is non-cacheable: read the cacheable alias instead, so a dirty line is still seen. */
            if(native_adreno_cached_read32 && !native_adreno_cached_read32(span.physical+(offset-span.offset),&value))*cached=value;
            break;
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::reassertWriteCombined(uint32_t *reset) const
{
    if(!reset)return kIOReturnBadArgument;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory)continue;
        const IOReturn status=m.memory->reassertWriteCombined(reset);
        if(status)return status;
    }
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::auditRange(uint64_t iova,size_t bytes,uint32_t *lines,uint32_t *dirty,uint32_t *stale) const
{
    if(!lines || !dirty || !stale)return kIOReturnBadArgument;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!native_adreno_line_audit)return kIOReturnUnsupported;
    if(!bytes || iova>UINT64_MAX-bytes)return kIOReturnBadArgument;
    for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || iova<m.iova || iova-m.iova>=m.memory->bytes())continue;
        if(!m.memory->writeCombined())return kIOReturnSuccess;   /* maintained before every submission anyway */
        /* Whole lines; the range is clipped to its BO. */
        size_t at=(size_t)(iova-m.iova)&~(size_t)63;
        const size_t end=(size_t)(iova-m.iova)+bytes>m.memory->bytes()?m.memory->bytes():(size_t)(iova-m.iova)+bytes;
        while(at<end) {
            const NativeAdrenoPhysicalSpan *span=nullptr;
            for(size_t j=0;j<m.memory->spanCount();++j) {
                const auto &s=m.memory->spans()[j];
                if(at>=s.offset && at-s.offset<s.bytes) {span=&s;break;}
            }
            if(!span)return kIOReturnBadArgument;
            const uint64_t physical=span->physical+(at-span->offset);
            size_t chunk=4096-(size_t)(physical&4095);
            if(chunk>span->offset+span->bytes-at)chunk=span->offset+span->bytes-at;
            if(chunk>end-at)chunk=(end-at+63)&~(size_t)63;
            if(native_adreno_line_audit(physical,chunk,dirty,stale))return kIOReturnIOError;
            *lines+=(uint32_t)(chunk/64);at+=chunk;
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::peekWords(uint64_t iova,uint32_t *out,size_t count) const
{
    if(!out || !attached_ || poisoned_)return kIOReturnBadArgument;
    for(size_t k=0;k<count;++k)out[k]=0;
    for(size_t i=0;i<NativeAdrenoVmMaxObjects;++i) {
        const Mapping &m=mappings_[i];
        if(!m.memory || !m.memory->mapping() || iova<m.iova || iova-m.iova>=m.memory->bytes())continue;
        const size_t offset=(size_t)(iova-m.iova);
        for(size_t k=0;k<count && offset+4*k+4<=m.memory->bytes();++k)out[k]=static_cast<const volatile uint32_t *>(m.memory->mapping())[offset/4+k];
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}
IOReturn NativeAdrenoVm::holdJob(uint64_t handle)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record)return kIOReturnBadArgument;
    if(record->jobs==UINT32_MAX)return kIOReturnNoResources;
    IOReturn status=record->memory->holdForDevice();if(status)return status;
    ++record->jobs;return kIOReturnSuccess;
}
IOReturn NativeAdrenoVm::retireJobAfterCompletion(uint64_t handle)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    Mapping *record=find(handle);if(!record || !record->jobs)return kIOReturnBadArgument;
    IOReturn status=record->memory->retireAfterQuiescence();if(status)return status;
    --record->jobs;return kIOReturnSuccess;
}
