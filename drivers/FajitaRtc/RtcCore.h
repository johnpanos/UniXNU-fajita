// SPDX-License-Identifier: BSD-2-Clause-Patent
// PM8998 RTC wall-clock logic of FajitaRtc, with no kernel dependencies (host-tested by tests/rtc-test.cpp).
//
// Hardware (Linux v6.16 drivers/rtc/rtc-pm8xxx.c, pm8941_regs; pm8998.dtsi rtc@6000, compatible "qcom,pm8941-rtc"): the RTC_RW
// peripheral at 0x6000 of SID 0 holds the seconds counter at 0x48..0x4b (little endian) and RTC_CTRL at 0x46 (bit 7 enable).
// The counter is read as four bytes, then the low byte again: when it went down a carry ran through the read and the four bytes
// are read once more (pm8xxx_rtc_read_raw).
//
// On the OnePlus 6T the counter is not wall time: sdm845-oneplus-common.dtsi gives the RTC neither allow-set-time nor an
// offset nvmem cell, so mainline Linux only reads it, and Android kept its offset in /data (Qualcomm time-services). Measured
// 2026-10-23 from pmOS (regmap debugfs): 0x0001ae31 s, about 30 h since the PMIC last lost power. Nothing here ever writes a
// PMIC register. Wall time = counter + offset, and the offset is kept in a record of two alternate 4096-byte copies:
//
//   0   "FJRTCOFS"   8 bytes
//   8   version 1    u32
//   12  bytes 48     u32
//   16  seq          u64   higher wins; copy (seq & 1) is the one written
//   24  offset       i64   wall seconds - counter
//   32  wall         u64   the wall time it was set to (floor after an RTC reset)
//   40  raw          u32   the counter then
//   44  crc32        u32   of bytes 0..43 (zlib polynomial)
// All little endian; the rest of the block is zero.
#ifndef FAJITA_RTC_CORE_H
#define FAJITA_RTC_CORE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define RTC_PM8941_RW_BASE 0x6000u
#define RTC_REG_CTRL 0x46u
#define RTC_REG_READ 0x48u
#define RTC_CTRL_ENABLE 0x80u

#define RTC_RECORD_BLOCK 4096u
#define RTC_RECORD_BYTES 48u
#define RTC_RECORD_VERSION 1u

// Reads len bytes at the 16-bit PMIC address; 0 or a negative errno.
typedef int (*rtc_read_fn)(void *ctx, unsigned addr, uint8_t *buf, unsigned len);

static inline int rtc_read_counter(rtc_read_fn rd, void *ctx, unsigned base, uint32_t *secs)
{
    uint8_t v[4], lsb;
    int rc = rd(ctx, base + RTC_REG_READ, v, 4);
    if (rc) return rc;
    if ((rc = rd(ctx, base + RTC_REG_READ, &lsb, 1)) != 0) return rc;
    if (lsb < v[0] && (rc = rd(ctx, base + RTC_REG_READ, v, 4)) != 0) return rc;
    *secs = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
    return 0;
}

typedef struct rtc_record {
    uint64_t seq;
    int64_t offset;
    uint64_t wall;
    uint32_t raw;
} rtc_record;

static inline uint32_t rtc_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static inline void rtc_put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void rtc_put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static inline uint32_t rtc_get32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = v << 8 | p[i]; return v; }
static inline uint64_t rtc_get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | p[i]; return v; }

static inline void rtc_record_encode(const rtc_record *r, uint8_t block[RTC_RECORD_BLOCK])
{
    memset(block, 0, RTC_RECORD_BLOCK);
    memcpy(block, "FJRTCOFS", 8);
    rtc_put32(block + 8, RTC_RECORD_VERSION);
    rtc_put32(block + 12, RTC_RECORD_BYTES);
    rtc_put64(block + 16, r->seq);
    rtc_put64(block + 24, (uint64_t)r->offset);
    rtc_put64(block + 32, r->wall);
    rtc_put32(block + 40, r->raw);
    rtc_put32(block + 44, rtc_crc32(block, 44));
}

// 1 and *r for a valid copy, 0 otherwise (blank, torn or another layout).
static inline int rtc_record_decode(const uint8_t block[RTC_RECORD_BLOCK], rtc_record *r)
{
    if (memcmp(block, "FJRTCOFS", 8) || rtc_get32(block + 8) != RTC_RECORD_VERSION || rtc_get32(block + 12) != RTC_RECORD_BYTES ||
        rtc_get32(block + 44) != rtc_crc32(block, 44)) return 0;
    r->seq = rtc_get64(block + 16);
    r->offset = (int64_t)rtc_get64(block + 24);
    r->wall = rtc_get64(block + 32);
    r->raw = rtc_get32(block + 40);
    return 1;
}

// The newer of two copies: 0 or 1, or -1 when neither is valid.
static inline int rtc_record_pick(int valid0, const rtc_record *r0, int valid1, const rtc_record *r1)
{
    if (valid0 && valid1) return r1->seq > r0->seq ? 1 : 0;
    return valid0 ? 0 : valid1 ? 1 : -1;
}

enum {
    RTC_TIME_NONE = 0,     // no offset known: no wall time
    RTC_TIME_OK = 1,       // counter + offset
    RTC_TIME_RESET = 2,    // the counter is below the one recorded: the RTC lost power since; the recorded wall time plus the
                           // counter is a floor (the time is at least that), not the time
};

// Wall seconds from the counter and the offset source (have: 0 none, 1 record). A negative result is clipped to RTC_TIME_NONE.
static inline int rtc_wall_time(uint32_t raw, int have, const rtc_record *rec, uint64_t *secs)
{
    if (!have) return RTC_TIME_NONE;
    if (raw < rec->raw) {
        *secs = rec->wall + raw;
        return RTC_TIME_RESET;
    }
    int64_t t = (int64_t)raw + rec->offset;
    if (t <= 0) return RTC_TIME_NONE;
    *secs = (uint64_t)t;
    return RTC_TIME_OK;
}

// The record after setting the wall time to secs at counter raw. Returns 1 when it must be written: there was none, the RTC was
// reset, or the offset moved by at least `slack` seconds (smaller corrections stay in memory, as Linux defers drift updates).
static inline int rtc_record_set(uint32_t raw, uint64_t secs, int have, const rtc_record *prev, uint32_t slack, rtc_record *next)
{
    next->seq = have ? prev->seq + 1 : 1;
    next->offset = (int64_t)secs - (int64_t)raw;
    next->wall = secs;
    next->raw = raw;
    if (!have || raw < prev->raw) return 1;
    int64_t d = next->offset - prev->offset;
    if (d < 0) d = -d;
    return d >= (int64_t)slack;
}
#endif
