// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WifiMdt.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint16_t rd16(const uint8_t *p) {return (uint16_t)(p[0] | p[1] << 8);}

int wmdt_segment_loads(const wmdt_phdr *ph)
{
    return ph->type == WMDT_PT_LOAD && (ph->flags & WMDT_TYPE_MASK) != WMDT_TYPE_HASH && ph->memsz != 0;
}

int wmdt_parse(const uint8_t *d, size_t bytes, wmdt_image *out)
{
    memset(out, 0, sizeof *out);
    out->hash_index = -1;
    if (bytes < 52) return WMDT_ERR_SHORT;
    if (d[0] != 0x7f || d[1] != 'E' || d[2] != 'L' || d[3] != 'F' || d[4] != 1 || d[5] != 1) return WMDT_ERR_MAGIC;
    const uint32_t phoff = rd32(d + 28);
    const unsigned phent = rd16(d + 42), phnum = rd16(d + 44);
    if (phent != 32 || phnum == 0 || phnum > WMDT_MAX_PHDRS) return WMDT_ERR_PHDRS;
    if ((uint64_t)phoff + (uint64_t)phnum * 32 > bytes) return WMDT_ERR_SHORT;
    out->entry = rd32(d + 24);
    out->phnum = phnum;
    out->ehdr_bytes = phoff + phnum * 32;
    uint64_t lo = ~0ull, hi = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t *p = d + phoff + i * 32;
        wmdt_phdr *ph = &out->ph[i];
        ph->type = rd32(p); ph->offset = rd32(p + 4); ph->vaddr = rd32(p + 8); ph->paddr = rd32(p + 12);
        ph->filesz = rd32(p + 16); ph->memsz = rd32(p + 20); ph->flags = rd32(p + 24); ph->align = rd32(p + 28);
        if ((ph->flags & WMDT_TYPE_MASK) == WMDT_TYPE_HASH && out->hash_index < 0) out->hash_index = (int)i;
        if (!wmdt_segment_loads(ph)) continue;
        if (ph->flags & WMDT_RELOCATABLE) out->relocatable = 1;
        if (ph->paddr < lo) lo = ph->paddr;
        const uint64_t end = ((uint64_t)ph->paddr + ph->memsz + 0xfff) & ~0xfffull;
        if (end > hi) hi = end;
    }
    if (out->hash_index < 0) return WMDT_ERR_NOHASH;
    out->min_addr = lo == ~0ull ? 0 : (uint32_t)lo;
    out->max_addr = (uint32_t)hi;
    return WMDT_OK;
}

long wmdt_metadata_size(const wmdt_image *img)
{
    if (img->hash_index < 0) return WMDT_ERR_NOHASH;
    return (long)img->ehdr_bytes + (long)img->ph[img->hash_index].filesz;
}

long wmdt_metadata(const uint8_t *d, size_t bytes, const wmdt_image *img, uint8_t *out, size_t cap)
{
    const long size = wmdt_metadata_size(img);
    if (size < 0) return size;
    if ((size_t)size > cap) return WMDT_ERR_SPACE;
    const wmdt_phdr *h = &img->ph[img->hash_index];
    if (img->ehdr_bytes > bytes || (uint64_t)h->offset + h->filesz > bytes) return WMDT_ERR_RANGE;
    memcpy(out, d, img->ehdr_bytes);
    memcpy(out + img->ehdr_bytes, d + h->offset, h->filesz);
    return size;
}

int wmdt_segment_offset(const wmdt_image *img, unsigned i, uint64_t region_phys, uint64_t region_bytes, uint64_t *offset)
{
    if (i >= img->phnum) return WMDT_ERR_RANGE;
    const wmdt_phdr *ph = &img->ph[i];
    if (ph->filesz > ph->memsz) return WMDT_ERR_RANGE;
    const uint64_t reloc = img->relocatable ? img->min_addr : region_phys;
    if (ph->paddr < reloc) return WMDT_ERR_RANGE;
    const uint64_t at = ph->paddr - reloc;
    if (at + ph->memsz > region_bytes) return WMDT_ERR_RANGE;
    *offset = at;
    return WMDT_OK;
}

uint64_t wmdt_boot_addr(const wmdt_image *img, uint64_t region_phys)
{
    return img->relocatable ? region_phys : img->min_addr;
}

int wmdt_segment_at(const wmdt_image *img, uint64_t at)
{
    for (unsigned i = 0; i < img->phnum; ++i) {
        const wmdt_phdr *ph = &img->ph[i];
        if (!wmdt_segment_loads(ph) || !ph->filesz) continue;
        if (at >= ph->offset && at < (uint64_t)ph->offset + ph->filesz) return (int)i;
    }
    return -1;
}
