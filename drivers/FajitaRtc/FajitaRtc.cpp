// SPDX-License-Identifier: BSD-2-Clause-Patent
// FajitaRtc: the OnePlus 6T wall clock. Reads the PM8998 RTC over SPMI (drivers/NativeSpmi) and serves it as the "IORTC" resource,
// the service the platform expert asks for the UTC time of day: as on Apple's ARM platforms (AppleARMPE, whose PMU RTC answers
// the same calls), IOPlatformExpert::getUTCTimeOfDay calls its platform function "getGMTTimeOfDay" (clock_sec_t *, clock_nsec_t *),
// so clock_initialize_calendar (bsd_init) starts the calendar from it, and settimeofday reaches "setGMTTimeOfDay" through
// PESetUTCTimeOfDay. Both, and IOKitInitializeTime's wait for IORTC in the BCM2837 configuration, come from
// patches/xnu/zzu-fajita-rtc-calendar.patch; without it the calendar started at 1970 on every boot (NOTES-daemons.md).
//
// The RTC counter is read-only here (and to mainline Linux on this board): wall time is the counter plus an offset kept in a
// two-block record at the end of userdata (NativeUfs native_ufs_rtc_*, format in RtcCore.h). No PMIC register is ever written.
//
// Matches the /arm-io/rtc node of the ADT (compatible "qcom,pm8941-rtc", spmi-sid, spmi-base). Boot argument fajita-rtc:
//   0 (default) publish IORTC but answer "not ready", so the platform expert does not wait and the time stays as before;
//   1 read the counter and the record, serve the time; a set changes the offset in memory only;
//   2 as 1, and a set that moves the offset by 2 s or more writes the record (forced unit access).
// fajita-rtc-offset=<seconds>: an explicit anchor, wall time minus counter (constant while the RTC runs, so a build-time value
// does not go stale): it replaces the recorded offset, and in mode 2 is written as the record. A minute after start the driver
// logs the calendar against counter + offset.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#include "RtcCore.h"

extern "C" {
int native_spmi_init(void) __attribute__((weak_import));
int native_spmi_read(unsigned sid, unsigned addr, void *buf, unsigned len) __attribute__((weak_import));
int native_spmi_channel(unsigned sid, unsigned addr, unsigned *owner_ee) __attribute__((weak_import));
int native_ufs_rtc_open(int writable, const char **why) __attribute__((weak_import));
int native_ufs_rtc_read(unsigned slot, void *dst) __attribute__((weak_import));
int native_ufs_rtc_write(unsigned slot, const void *src) __attribute__((weak_import));
// Diagnostics: [0] stage (0 off, 1 starting, 2 published, 3 failed), [1] mode, [2] counter at start, [3] RTC_CTRL,
// [4] owner EE of the RTC channel, [5] time source (RtcCore RTC_TIME_*), [6] reads, [7] sets, [8] record writes, [9] errors.
volatile uint32_t fajita_rtc_stats[10];
char fajita_rtc_row[160];
}

namespace {
constexpr uint32_t kSlack = 2;           // seconds of offset change that are written
constexpr unsigned kUfsWaitMs = 20000;   // the platform expert waits 30 s for IORTC
}

class FajitaRtc : public IOService {
  OSDeclareDefaultStructors (FajitaRtc)
public:
  bool start (IOService *provider) override;
  using IOService::callPlatformFunction;
  IOReturn callPlatformFunction (const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4) override;
private:
  static void bringUp (void *arg, wait_result_t);
  int readCounter (uint32_t *raw);
  void loadRecord (void);
  void summarise (const char *note);
  IOReturn getTime (clock_sec_t *secs, clock_nsec_t *nsecs);
  IOReturn setTime (clock_sec_t secs);
  IOLock *fLock = nullptr;
  unsigned fMode = 0, fSid = 0, fBase = RTC_PM8941_RW_BASE;
  bool fReady = false, fUfs = false;
  int fHave = 0;             // fRecord valid (from the record, the seed or a set)
  rtc_record fRecord = {};
  int fSource = RTC_TIME_NONE;
};

OSDefineMetaClassAndStructors (FajitaRtc, IOService)

static uint32_t u32Property (IOService *nub, const char *key, uint32_t fallback)
{
  OSData *d = OSDynamicCast (OSData, nub->getProperty (key));
  return (d != nullptr && d->getLength () == 4) ? *static_cast<const uint32_t *> (d->getBytesNoCopy ()) : fallback;
}

static int spmiRead (void *ctx, unsigned addr, uint8_t *buf, unsigned len)
{
  return native_spmi_read (*static_cast<unsigned *> (ctx), addr, buf, len);
}

bool FajitaRtc::start (IOService *provider)
{
  if (!IOService::start (provider)) return false;
  if (!PE_parse_boot_argn ("fajita-rtc", &fMode, sizeof (fMode)) || fMode > 2) fMode = 0;
  fSid = u32Property (provider, "spmi-sid", 0);
  fBase = u32Property (provider, "spmi-base", RTC_PM8941_RW_BASE);
  fLock = IOLockAlloc ();
  fajita_rtc_stats[1] = fMode;
  if (fLock == nullptr) return false;
  if (fMode == 0) {
    summarise ("off");
    registerService ();
    publishResource ("IORTC", this);
    return true;
  }
  fajita_rtc_stats[0] = 1;
  retain ();
  thread_t thread;
  if (kernel_thread_start (bringUp, this, &thread) != KERN_SUCCESS) {
    release ();
    fajita_rtc_stats[0] = 3;
    summarise ("no thread");
    registerService ();
    publishResource ("IORTC", this);   // answers "not ready": the platform expert need not wait
    return true;
  }
  thread_deallocate (thread);
  return true;
}

int FajitaRtc::readCounter (uint32_t *raw)
{
  if (native_spmi_read == nullptr) return -45;
  int rc = rtc_read_counter (spmiRead, &fSid, fBase, raw);
  if (rc) fajita_rtc_stats[9]++;
  return rc;
}

void FajitaRtc::loadRecord (void)
{
  if (native_ufs_rtc_open == nullptr || native_ufs_rtc_read == nullptr) return;
  const char *why = "";
  unsigned waited = 0;
  while (!native_ufs_rtc_open (fMode == 2, &why)) {
    if (strcmp (why, "no root layout") != 0 || waited >= kUfsWaitMs) {
      IOLog ("FajitaRtc: no offset record (%s)\n", why);
      return;
    }
    IOSleep (20);
    waited += 20;
  }
  fUfs = true;
  uint8_t *block = static_cast<uint8_t *> (IOMalloc (RTC_RECORD_BLOCK));
  if (block == nullptr) return;
  rtc_record r[2] = {};
  int valid[2] = {};
  for (unsigned slot = 0; slot < 2; ++slot) {
    valid[slot] = native_ufs_rtc_read (slot, block) && rtc_record_decode (block, &r[slot]);
  }
  IOFree (block, RTC_RECORD_BLOCK);
  int pick = rtc_record_pick (valid[0], &r[0], valid[1], &r[1]);
  if (pick >= 0) {
    fRecord = r[pick];
    fHave = 1;
  }
  IOLog ("FajitaRtc: record %s (slots %d%d, %u ms)\n", pick >= 0 ? "found" : "blank", valid[0], valid[1], waited);
}

void FajitaRtc::bringUp (void *arg, wait_result_t)
{
  FajitaRtc *self = static_cast<FajitaRtc *> (arg);
  uint32_t raw = 0;
  int rc = native_spmi_init ? native_spmi_init () : -45;
  if (rc == 0) {
    unsigned ee = 0xff;
    native_spmi_channel (self->fSid, self->fBase + RTC_REG_READ, &ee);
    fajita_rtc_stats[4] = ee;
    uint8_t ctrl = 0;
    if (native_spmi_read (self->fSid, self->fBase + RTC_REG_CTRL, &ctrl, 1) == 0) fajita_rtc_stats[3] = ctrl;
    rc = self->readCounter (&raw);
    if (rc == 0 && !(ctrl & RTC_CTRL_ENABLE)) rc = -5;   // a stopped RTC: Linux would enable it; this driver never writes
  }
  fajita_rtc_stats[2] = raw;
  if (rc == 0) {
    self->loadRecord ();
    IOLockLock (self->fLock);
    self->fReady = true;
    IOLockUnlock (self->fLock);
    // An explicit anchor (the host's offset, given by the payload build): wins over the record, and in mode 2 becomes it.
    uint64_t seed = 0;
    if (PE_parse_boot_argn ("fajita-rtc-offset", &seed, sizeof (seed)) && seed) {
      IOLog ("FajitaRtc: offset %llu from fajita-rtc-offset (record %lld)\n", seed, self->fHave ? static_cast<long long> (self->fRecord.offset) : 0LL);
      self->setTime (static_cast<clock_sec_t> (seed + raw));
    }
  } else {
    fajita_rtc_stats[9]++;
    IOLog ("FajitaRtc: RTC read failed (%d)\n", rc);
  }
  fajita_rtc_stats[0] = rc == 0 ? 2 : 3;
  uint64_t t = 0;
  self->fSource = rc == 0 ? rtc_wall_time (raw, self->fHave, &self->fRecord, &t) : RTC_TIME_NONE;
  fajita_rtc_stats[5] = static_cast<uint32_t> (self->fSource);
  self->summarise ("up");
  IOLog ("FajitaRtc: %s, wall %llu\n", fajita_rtc_row, t);
  self->registerService ();
  self->publishResource ("IORTC", self);
  // One check a minute later: the calendar against counter + offset (they differ only when something set the time since).
  IOSleep (60000);
  clock_sec_t now = 0;
  clock_usec_t us = 0;
  clock_get_calendar_microtime (&now, &us);
  clock_nsec_t ns = 0;
  clock_sec_t rtc = 0;
  IOReturn st = self->getTime (&rtc, &ns);
  self->summarise ("check");
  IOLog ("FajitaRtc: calendar %lu.%06u, RTC wall %lu (0x%x), %s\n", static_cast<unsigned long> (now), us, static_cast<unsigned long> (rtc), st,
         fajita_rtc_row);
  self->release ();
}

void FajitaRtc::summarise (const char *note)
{
  snprintf (fajita_rtc_row, sizeof fajita_rtc_row, "RTC %s m%u st%u raw %u ctrl %02x ee%u src%u off %lld seq %llu rd%u set%u wr%u err%u",
            note, fMode, fajita_rtc_stats[0], fajita_rtc_stats[2], fajita_rtc_stats[3], fajita_rtc_stats[4], fajita_rtc_stats[5],
            fHave ? static_cast<long long> (fRecord.offset) : 0LL, fHave ? static_cast<unsigned long long> (fRecord.seq) : 0ULL,
            fajita_rtc_stats[6], fajita_rtc_stats[7], fajita_rtc_stats[8], fajita_rtc_stats[9]);
}

IOReturn FajitaRtc::getTime (clock_sec_t *secs, clock_nsec_t *nsecs)
{
  IOLockLock (fLock);
  IOReturn status = kIOReturnNotReady;
  uint32_t raw = 0;
  uint64_t t = 0;
  if (fReady && readCounter (&raw) == 0) {
    int source = rtc_wall_time (raw, fHave, &fRecord, &t);
    fSource = source;
    fajita_rtc_stats[5] = static_cast<uint32_t> (source);
    if (source != RTC_TIME_NONE) {
      *secs = static_cast<clock_sec_t> (t);
      *nsecs = 0;
      status = kIOReturnSuccess;
    }
  } else if (fReady) {
    status = kIOReturnIOError;
  }
  fajita_rtc_stats[6]++;
  IOLockUnlock (fLock);
  return status;
}

IOReturn FajitaRtc::setTime (clock_sec_t secs)
{
  IOLockLock (fLock);
  uint32_t raw = 0;
  if (!fReady || readCounter (&raw) != 0) {
    IOLockUnlock (fLock);
    return kIOReturnNotReady;
  }
  rtc_record next;
  int write = rtc_record_set (raw, secs, fHave, &fRecord, kSlack, &next);
  int64_t before = fHave ? fRecord.offset : 0;
  fRecord = next;
  fHave = 1;
  fajita_rtc_stats[7]++;
  bool wrote = false;
  if (write && fMode == 2 && fUfs && native_ufs_rtc_write != nullptr) {
    uint8_t *block = static_cast<uint8_t *> (IOMalloc (RTC_RECORD_BLOCK));
    if (block != nullptr) {
      rtc_record_encode (&next, block);
      wrote = native_ufs_rtc_write (static_cast<unsigned> (next.seq & 1), block);
      IOFree (block, RTC_RECORD_BLOCK);
    }
    if (wrote) fajita_rtc_stats[8]++;
    else fajita_rtc_stats[9]++;
  }
  summarise (wrote ? "set, saved" : "set");
  IOLockUnlock (fLock);
  IOLog ("FajitaRtc: time set to %lu (counter %u, offset %lld -> %lld)%s\n", static_cast<unsigned long> (secs), raw,
         static_cast<long long> (before), static_cast<long long> (next.offset),
         wrote ? ", record written" : write && fMode == 2 ? ", record NOT written" : "");
  return kIOReturnSuccess;
}

IOReturn FajitaRtc::callPlatformFunction (const OSSymbol *name, bool wait, void *p1, void *p2, void *p3, void *p4)
{
  if (name != nullptr && name->isEqualTo ("getGMTTimeOfDay")) {
    if (p1 == nullptr || p2 == nullptr) return kIOReturnBadArgument;
    if (fMode == 0) return kIOReturnNotReady;
    return getTime (static_cast<clock_sec_t *> (p1), static_cast<clock_nsec_t *> (p2));
  }
  if (name != nullptr && name->isEqualTo ("setGMTTimeOfDay")) {
    if (p1 == nullptr) return kIOReturnBadArgument;
    if (fMode == 0) return kIOReturnNotReady;
    return setTime (*static_cast<clock_sec_t *> (p1));
  }
  return IOService::callPlatformFunction (name, wait, p1, p2, p3, p4);
}
