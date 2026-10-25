// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm signed firmware images (.mbn / .mdt): ELF32 with a hash segment. Freestanding C, host-tested
// (scripts/check-fajita-wifi-host.py). After drivers/soc/qcom/mdt_loader.c (qcom_mdt_read_metadata) and
// drivers/remoteproc/qcom_q6v5_mss.c (q6v5_phdr_valid, q6v5_mpss_load) of Linux v6.16.
#ifndef FAJITA_WIFI_MDT_H
#define FAJITA_WIFI_MDT_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WMDT_MAX_PHDRS 64u
#define WMDT_TYPE_MASK (7u << 24)       // QCOM_MDT_TYPE_MASK, in p_flags
#define WMDT_TYPE_HASH (2u << 24)       // QCOM_MDT_TYPE_HASH
#define WMDT_RELOCATABLE (1u << 27)     // QCOM_MDT_RELOCATABLE
#define WMDT_PT_LOAD 1u

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} wmdt_phdr;

typedef struct {
    uint32_t entry;
    unsigned phnum;
    wmdt_phdr ph[WMDT_MAX_PHDRS];
    int hash_index;                     // the hash segment, -1 if none
    uint32_t ehdr_bytes;                // ELF header + program headers (metadata part 1)
    uint32_t min_addr, max_addr;        // loadable segments: lowest p_paddr, highest end aligned to 4 KiB
    int relocatable;
} wmdt_image;

enum {
    WMDT_OK = 0,
    WMDT_ERR_SHORT = -1,                // fewer bytes than the headers need
    WMDT_ERR_MAGIC = -2,                // not ELF32 little-endian
    WMDT_ERR_PHDRS = -3,                // phentsize/phnum out of range
    WMDT_ERR_NOHASH = -4,               // no hash segment
    WMDT_ERR_RANGE = -5,                // a segment outside the file or the region
    WMDT_ERR_SPACE = -6,                // output buffer too small
};

// Parse the headers. `bytes` may be just a prefix of the file (the first page is enough for the program headers).
int wmdt_parse(const uint8_t *data, size_t bytes, wmdt_image *out);
// A segment Linux loads (q6v5_phdr_valid): PT_LOAD, not the hash segment, memsz != 0.
int wmdt_segment_loads(const wmdt_phdr *ph);
// Metadata for authentication (qcom_mdt_read_metadata): ELF header + program headers, then the hash segment's file bytes.
// Needs the whole file prefix up to the end of the hash segment. Returns the size written, or a negative WMDT_ERR.
long wmdt_metadata(const uint8_t *data, size_t bytes, const wmdt_image *img, uint8_t *out, size_t cap);
long wmdt_metadata_size(const wmdt_image *img);
// Where segment i lands in a region at region_phys of region_bytes (q6v5_mpss_load): offset from the region start.
// Returns WMDT_OK or WMDT_ERR_RANGE (outside the region, filesz > memsz).
int wmdt_segment_offset(const wmdt_image *img, unsigned i, uint64_t region_phys, uint64_t region_bytes, uint64_t *offset);
// The boot address the RMB wants (RMB_PMI_CODE_START_REG): the region base when relocatable, else min_addr.
uint64_t wmdt_boot_addr(const wmdt_image *img, uint64_t region_phys);
// The segment index whose file bytes cover file offset `at`, the earliest such in load order; -1 if none.
int wmdt_segment_at(const wmdt_image *img, uint64_t at);

#ifdef __cplusplus
}
#endif
#endif
