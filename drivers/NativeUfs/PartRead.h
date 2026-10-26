// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only access to any named GPT partition on LUNs 0..5, for backups and for the fajita Wi-Fi rmtfs (EFS) path.
// Off unless `native-ufs-partread=1`. Nothing here writes: it issues READ(10) through hostRead4k only.
//
//   kcmd ufspart find NAME            -> "NAME lun=L first=F last=L blocks=N type=GUID uniq=GUID attr=A ents=CRCOK"
//   kcmd ufspart list LUN [START]     -> "idx name first last" lines from entry START while they fit
//   kcmd ufspart read NAME OFF COUNT  -> reads COUNT (<= 32) 4096-byte blocks at block OFF of NAME into a static buffer;
//                                         "ok va=0x... bytes=N crc=XXXXXXXX" (the host fetches the bytes with readv and
//                                         checks the CRC32: scripts/fajita/test/ufspart.py)
// The C entry native_ufs_part_find / native_ufs_part_read give the same reads to in-kernel users (rmtfs, read-only).
// Included once by NativeUfs.cpp after RootLayout.h (le32/le64/rootCrc32/rootEntryUsed).
#ifndef NATIVE_UFS_PART_READ_H
#define NATIVE_UFS_PART_READ_H

extern "C" {
typedef size_t (*fajita_usbdbg_kcmd_fn) (const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd (const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__ ((weak));
}

namespace partread {
enum : unsigned { kLuns = 6, kMaxEntries = 128, kChunkBlocks = kDataMax / 4096 };
static uint8_t gEntries[kMaxEntries * 128];
static uint8_t gHeader[4096];
static uint8_t gData[kDataMax] __attribute__ ((aligned (4096)));
static IOLock *gLock;

struct Part {
  uint8_t lun;
  uint32_t index;
  uint64_t first, last, attr;
  uint8_t type[16], uniq[16];
  bool entriesCrcOk;
};

// Load LUN lun's primary GPT (header at LBA 1, 4096-byte blocks) into gHeader/gEntries. Returns the entry count, 0 on failure.
static uint32_t loadGpt (uint8_t lun, bool *crcOk)
{
  if (hostRead4k (lun, 1, 1, gHeader) != kIOReturnSuccess) return 0;
  if (le32 (gHeader) != 0x20494645U || le32 (gHeader + 4) != 0x54524150U || le32 (gHeader + 84) != 128) return 0;
  uint32_t headerBytes = le32 (gHeader + 12);
  if (headerBytes < 92 || headerBytes > 4096 || rootCrc32 (gHeader, headerBytes, true) != le32 (gHeader + 16)) return 0;
  uint64_t entriesLba = le64 (gHeader + 72);
  uint32_t count = le32 (gHeader + 80);
  if (entriesLba < 2 || entriesLba > 64 || !count) return 0;
  if (count > kMaxEntries) count = kMaxEntries;
  uint32_t blocks = (count * 128 + 4095) / 4096;
  if (hostRead4k (lun, entriesLba, blocks, gEntries) != kIOReturnSuccess) return 0;
  if (crcOk) *crcOk = le32 (gHeader + 80) <= kMaxEntries && rootCrc32 (gEntries, le32 (gHeader + 80) * 128) == le32 (gHeader + 88);
  return count;
}

static bool nameIs (const uint8_t *entry, const char *name, size_t len)
{
  if (!len || len > 36) return false;
  for (size_t c = 0; c < 36; ++c) {
    uint8_t lo = entry[56 + 2 * c], hi = entry[57 + 2 * c];
    if (c < len) {
      if (hi || lo != static_cast<uint8_t> (name[c])) return false;
    } else return !lo && !hi;
  }
  return true;
}

static void nameOf (const uint8_t *entry, char out[37])
{
  unsigned c = 0;
  for (; c < 36; ++c) {
    uint8_t lo = entry[56 + 2 * c], hi = entry[57 + 2 * c];
    if (!lo && !hi) break;
    out[c] = hi || lo < 0x20 || lo > 0x7e ? '?' : static_cast<char> (lo);
  }
  out[c] = 0;
}

// Caller holds gLock. First match over LUN 0..5 in order.
static bool find (const char *name, size_t len, Part *part)
{
  for (uint8_t lun = 0; lun < kLuns; ++lun) {
    bool crcOk = false;
    uint32_t count = loadGpt (lun, &crcOk);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t *e = gEntries + i * 128;
      if (!rootEntryUsed (e) || !nameIs (e, name, len)) continue;
      part->lun = lun;
      part->index = i;
      part->first = le64 (e + 32);
      part->last = le64 (e + 40);
      part->attr = le64 (e + 48);
      memcpy (part->type, e, 16);
      memcpy (part->uniq, e + 16, 16);
      part->entriesCrcOk = crcOk;
      return part->last >= part->first;
    }
  }
  return false;
}

static int guid (char *out, size_t cap, const uint8_t *g)
{
  return snprintf (out, cap, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", le32 (g),
                   unsigned (g[4] | (g[5] << 8)), unsigned (g[6] | (g[7] << 8)), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

static bool enabled ()
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t v = 0;
    cached = PE_parse_boot_argn ("native-ufs-partread", &v, sizeof (v)) && v ? 1 : 0;
  }
  return cached == 1;
}

static uint32_t crc32 (const uint8_t *data, size_t bytes) { return rootCrc32 (data, bytes); }

// Split "a b c" into at most n words (copied into buf, NUL-terminated).
static unsigned words (const char *args, size_t len, char *buf, size_t cap, const char **w, unsigned n)
{
  size_t l = len < cap - 1 ? len : cap - 1;
  memcpy (buf, args, l);
  buf[l] = 0;
  unsigned k = 0;
  char *p = buf;
  while (*p && k < n) {
    while (*p == ' ') *p++ = 0;
    if (!*p) break;
    w[k++] = p;
    while (*p && *p != ' ') ++p;
  }
  while (*p) { if (*p == ' ') *p = 0; ++p; }
  return k;
}

static uint64_t num (const char *s)
{
  uint64_t v = 0;
  unsigned base = 10;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
  for (; *s; ++s) {
    unsigned d = *s >= '0' && *s <= '9' ? unsigned (*s - '0') : *s >= 'a' && *s <= 'f' ? unsigned (*s - 'a' + 10) :
                 *s >= 'A' && *s <= 'F' ? unsigned (*s - 'A' + 10) : 99;
    if (d >= base) break;
    v = v * base + d;
  }
  return v;
}

static size_t kcmd (const char *args, size_t len, char *out, size_t cap)
{
  char buf[160];
  const char *w[4] = {};
  unsigned n = words (args, len, buf, sizeof (buf), w, 4);
  if (!gHost.ready) return snprintf (out, cap, "ufspart: host not ready\n");
  if (!n) return snprintf (out, cap, "ufspart find NAME | list LUN [START] | read NAME OFF COUNT (<= %u blocks)\n", kChunkBlocks);
  size_t at = 0;
  IOLockLock (gLock);
  if (!strcmp (w[0], "find") && n >= 2) {
    Part p;
    if (!find (w[1], strlen (w[1]), &p)) at = snprintf (out, cap, "ufspart: %s not found\n", w[1]);
    else {
      char t[40], u[40];
      guid (t, sizeof t, p.type);
      guid (u, sizeof u, p.uniq);
      at = snprintf (out, cap, "%s lun=%u index=%u first=%llu last=%llu blocks=%llu type=%s uniq=%s attr=%llx ents=%s\n", w[1],
                     p.lun, p.index, p.first, p.last, p.last - p.first + 1, t, u, p.attr, p.entriesCrcOk ? "crcok" : "crcBAD");
    }
  } else if (!strcmp (w[0], "list") && n >= 2) {
    uint8_t lun = static_cast<uint8_t> (num (w[1]));
    uint32_t start = n >= 3 ? static_cast<uint32_t> (num (w[2])) : 0;
    bool crcOk = false;
    uint32_t count = lun < kLuns ? loadGpt (lun, &crcOk) : 0;
    at = snprintf (out, cap, "lun %u entries %u %s\n", lun, count, crcOk ? "crcok" : "crcBAD");
    for (uint32_t i = start; i < count && at < cap; ++i) {
      const uint8_t *e = gEntries + i * 128;
      if (!rootEntryUsed (e)) continue;
      char name[37];
      nameOf (e, name);
      int k = snprintf (out + at, cap - at, "%u %s %llu %llu\n", i, name, le64 (e + 32), le64 (e + 40));
      if (k < 0 || size_t (k) >= cap - at) { at += snprintf (out + at, cap - at, "more %u\n", i) ; break; }
      at += size_t (k);
    }
  } else if (!strcmp (w[0], "read") && n >= 4) {
    Part p;
    uint64_t off = num (w[2]), count = num (w[3]);
    if (!find (w[1], strlen (w[1]), &p)) at = snprintf (out, cap, "ufspart: %s not found\n", w[1]);
    else if (!count || count > kChunkBlocks || off > p.last - p.first || count > p.last - p.first + 1 - off)
      at = snprintf (out, cap, "ufspart: range %llu+%llu outside %llu blocks\n", off, count, p.last - p.first + 1);
    else if (hostRead4k (p.lun, p.first + off, static_cast<uint32_t> (count), gData) != kIOReturnSuccess)
      at = snprintf (out, cap, "ufspart: read failed at lun %u lba %llu\n", p.lun, p.first + off);
    else
      at = snprintf (out, cap, "ok va=0x%llx bytes=%llu crc=%08x\n", static_cast<unsigned long long> (reinterpret_cast<uintptr_t> (gData)),
                     count * 4096, crc32 (gData, count * 4096));
  } else at = snprintf (out, cap, "ufspart: bad arguments\n");
  IOLockUnlock (gLock);
  return at < cap ? at : cap;
}

static void registerOnce ()
{
  static bool done;
  if (done || !enabled ()) return;
  done = true;
  gLock = IOLockAlloc ();
  if (gLock && fajita_usbdbg_register_kcmd)
    fajita_usbdbg_register_kcmd ("ufspart", kcmd, "read-only GPT partition reader: find NAME | list LUN | read NAME OFF COUNT");
}
}  // namespace partread

// In-kernel read-only access (rmtfs, the modem firmware). Returns 1 with the partition's LUN/first/blocks and GPT attributes
// (attr may be null; A/B: bit 50 is the active slot), 0 if absent or off.
extern "C" int native_ufs_part_find (const char *name, uint8_t *lun, uint64_t *first, uint64_t *blocks, uint64_t *attr)
{
  if (!partread::enabled () || !partread::gLock || !gHost.ready || !name) return 0;
  partread::Part p;
  IOLockLock (partread::gLock);
  bool ok = partread::find (name, strlen (name), &p);
  IOLockUnlock (partread::gLock);
  if (!ok) return 0;
  if (lun) *lun = p.lun;
  if (first) *first = p.first;
  if (blocks) *blocks = p.last - p.first + 1;
  if (attr) *attr = p.attr;
  return 1;
}

// Read `count` 4096-byte blocks at absolute `lba` of `lun` into dst, in kDataMax pieces. Read only.
extern "C" int native_ufs_part_read (uint8_t lun, uint64_t lba, uint32_t count, void *dst)
{
  if (!partread::enabled () || !gHost.ready || !dst || lun >= partread::kLuns) return 0;
  while (count) {
    uint32_t chunk = count > partread::kChunkBlocks ? partread::kChunkBlocks : count;
    if (hostRead4k (lun, lba, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    lba += chunk;
    count -= chunk;
  }
  return 1;
}

#endif
