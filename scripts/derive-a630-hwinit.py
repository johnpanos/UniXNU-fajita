#!/usr/bin/env python3
"""Derive the Adreno 630 register table of drivers/FajitaAdreno/A630Regs.h from a Linux source tree.

Offsets come from drivers/gpu/drm/msm/registers/adreno (a6xx.xml first, then adreno_common.xml; a6xx_gmu.xml for the GMU, RSCC
and PDC blocks) with exact A6XX variant selection; the hwcg and protect tables from adreno/a6xx_catalog.c (a630_hwcg,
a630_protect_regs). All offsets are dword indices: GPU registers from the GPU block (gpu@5000000), GMU registers from the GMU
block (gmu@506a000, a6xx_gmu.c gmu_read), RSCC registers from the RSCC block (GMU + 0x23000 on A630), PDC registers from
gmu_pdc (0x0b280000) and gmu_pdc_seq (0x0b480000).

  derive-a630-hwinit.py --linux /home/john/src/ref/linux            writes drivers/FajitaAdreno/A630Regs.h
  derive-a630-hwinit.py --linux ... --check                         compares instead (exit 1 on a difference)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'drivers/FajitaAdreno/A630Regs.h'
ORDER = ['A2XX', 'A3XX', 'A4XX', 'A5XX', 'A6XX', 'A7XX', 'A8XX']
CHIP = 'A6XX'


def matches(variants):
    if variants is None:
        return True
    for part in variants.split(','):
        part = part.strip()
        if '-' in part:
            left, right = part.split('-', 1)
            low = ORDER.index(left) if left else 0
            high = ORDER.index(right) if right else len(ORDER) - 1
            if low <= ORDER.index(CHIP) <= high:
                return True
        elif part == CHIP:
            return True
    return False


def resolve(xml, name, files):
    for f in files:
        text = (xml / f).read_text()
        hits = set()
        for m in re.finditer(r'<(reg32|reg64|array) offset="(0x[0-9a-fA-F]+|[0-9]+)" name="' + re.escape(name) + r'"([^>]*)>', text):
            v = re.search(r'variants="([^"]+)"', m.group(3))
            if matches(v.group(1) if v else None):
                hits.add(int(m.group(2), 0))
        if len(hits) > 1:
            raise SystemExit('ambiguous %s in %s: %s' % (name, f, sorted(hits)))
        if hits:
            return hits.pop()
    raise SystemExit('missing ' + name)


GPU = '''RBBM_SECVID_TSB_CNTL RBBM_SECVID_TSB_TRUSTED_BASE RBBM_SECVID_TSB_TRUSTED_SIZE CP_ADDR_MODE_CNTL VSC_ADDR_MODE_CNTL
GRAS_ADDR_MODE_CNTL RB_ADDR_MODE_CNTL PC_ADDR_MODE_CNTL HLSQ_ADDR_MODE_CNTL VFD_ADDR_MODE_CNTL VPC_ADDR_MODE_CNTL
UCHE_ADDR_MODE_CNTL SP_ADDR_MODE_CNTL TPL1_ADDR_MODE_CNTL RBBM_SECVID_TSB_ADDR_MODE_CNTL RBBM_CLOCK_CNTL
RBBM_VBIF_CLIENT_QOS_CNTL VBIF_GATE_OFF_WRREQ_EN RBBM_PERFCTR_GPU_BUSY_MASKED UCHE_WRITE_RANGE_MAX UCHE_TRAP_BASE
UCHE_WRITE_THRU_BASE UCHE_GMEM_RANGE_MIN UCHE_GMEM_RANGE_MAX UCHE_FILTER_CNTL UCHE_CACHE_WAYS CP_ROQ_THRESHOLDS_2
CP_ROQ_THRESHOLDS_1 CP_MEM_POOL_SIZE PC_DBG_ECO_CNTL CP_AHB_CNTL RBBM_PERFCTR_CNTL CP_PERFCTR_CP_SEL RB_NC_MODE_CNTL
TPL1_NC_MODE_CNTL SP_NC_MODE_CNTL UCHE_MODE_CNTL RBBM_NC_MODE_CNTL RBBM_INTERFACE_HANG_INT_CNTL UCHE_CLIENT_PF
CP_PROTECT_CNTL CP_PROTECT RBBM_INT_0_MASK CP_SQE_INSTR_BASE CP_RB_BASE CP_RB_CNTL CP_RB_RPTR_ADDR CP_SQE_CNTL CP_RB_RPTR
CP_RB_WPTR RBBM_STATUS RBBM_STATUS1 RBBM_STATUS2 RBBM_STATUS3 RBBM_INT_0_STATUS RBBM_INT_CLEAR_CMD CP_HW_FAULT
CP_INTERRUPT_STATUS RBBM_SECVID_TRUST_CNTL CP_SCRATCH CP_ALWAYS_ON_COUNTER RBBM_PERFCTR_CP CP_SQE_STAT_ADDR
CP_SQE_STAT_DATA VBIF_XIN_HALT_CTRL0 VBIF_XIN_HALT_CTRL1 RBBM_SW_RESET_CMD RBBM_PERFCTR_SRAM_INIT_CMD
RBBM_PERFCTR_SRAM_INIT_STATUS CP_STATUS_1 CP_ROQ_DBG_ADDR CP_ROQ_DBG_DATA CP_IB1_BASE CP_IB1_REM_SIZE
RBBM_CLOCK_CNTL_GMU_GX CP_CHICKEN_DBG CP_PROTECT_STATUS CP_SQE_INSTR_BASE CP_ME_NRT_ADDR CP_ME_NRT_DATA'''.split()

GMU = '''GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL GMU_GX_SPTPRAC_POWER_CONTROL GMU_CM3_ITCM_START GMU_CM3_DTCM_START
GMU_BOOT_SLUMBER_OPTION GMU_GX_VOTE_IDX GMU_MX_VOTE_IDX GMU_DCVS_ACK_OPTION GMU_DCVS_PERF_SETTING GMU_DCVS_BW_SETTING
GMU_DCVS_RETURN GMU_CORE_FW_VERSION GMU_ICACHE_CONFIG GMU_DCACHE_CONFIG GMU_SYS_BUS_CONFIG GMU_CM3_SYSRESET
GMU_CM3_BOOT_CONFIG GMU_CM3_FW_BUSY GMU_CM3_FW_INIT_RESULT GMU_CM3_CFG GMU_CX_GMU_POWER_COUNTER_ENABLE
GMU_CX_GMU_POWER_COUNTER_SELECT_0 GMU_PWR_COL_INTER_FRAME_CTRL GMU_PWR_COL_INTER_FRAME_HYST GMU_PWR_COL_SPTPRAC_HYST
GMU_SPTPRAC_PWR_CLK_STATUS GMU_GPU_NAP_CTRL GMU_RPMH_CTRL GMU_RPMH_HYST_CTRL GPU_GMU_CX_GMU_RPMH_POWER_STATE
GPU_GMU_CX_GMU_PWR_COL_CP_MSG GPU_GMU_CX_GMU_PWR_COL_CP_RESP GMU_GMU_PWR_COL_KEEPALIVE GMU_HFI_CTRL_STATUS
GMU_HFI_VERSION_INFO GMU_HFI_SFR_ADDR GMU_HFI_MMAP_ADDR GMU_HFI_QTBL_INFO GMU_HFI_QTBL_ADDR GMU_HFI_CTRL_INIT
GMU_GMU2HOST_INTR_SET GMU_GMU2HOST_INTR_CLR GMU_GMU2HOST_INTR_INFO GMU_GMU2HOST_INTR_MASK GMU_HOST2GMU_INTR_SET
GMU_HOST2GMU_INTR_CLR GMU_HOST2GMU_INTR_RAW_INFO GMU_GENERAL_0 GMU_GENERAL_1 GMU_GENERAL_6 GMU_GENERAL_7
GMU_AO_INTERRUPT_EN GMU_AO_HOST_INTERRUPT_CLR GMU_AO_HOST_INTERRUPT_STATUS GMU_AO_HOST_INTERRUPT_MASK
GPU_GMU_AO_GPU_CX_BUSY_STATUS GPU_GMU_AO_GPU_CX_BUSY_STATUS2 GPU_GMU_AO_GPU_CX_BUSY_MASK GMU_AO_AHB_FENCE_CTRL
GMU_AHB_FENCE_STATUS GMU_AHB_FENCE_STATUS_CLR GMU_RBBM_INT_UNMASKED_STATUS GMU_AO_SPARE_CNTL GMU_RSCC_CONTROL_REQ
GMU_RSCC_CONTROL_ACK GMU_AHB_FENCE_RANGE_0 GMU_AHB_FENCE_RANGE_1 GPU_CC_GX_GDSCR GPU_CC_GX_DOMAIN_MISC
GPU_GMU_AO_GMU_CGC_MODE_CNTL GPU_GMU_AO_GMU_CGC_DELAY_CNTL GPU_GMU_AO_GMU_CGC_HYST_CNTL GMU_ALWAYS_ON_COUNTER_L
GMU_ALWAYS_ON_COUNTER_H'''.split()

RSCC = '''GPU_RSCC_RSC_STATUS0_DRV0 RSCC_PDC_SEQ_START_ADDR RSCC_PDC_MATCH_VALUE_LO RSCC_PDC_MATCH_VALUE_HI
RSCC_PDC_SLAVE_ID_DRV0 RSCC_HIDDEN_TCS_CMD0_ADDR RSCC_HIDDEN_TCS_CMD0_DATA RSCC_OVERRIDE_START_ADDR RSCC_SEQ_BUSY_DRV0
RSCC_SEQ_MEM_0_DRV0 RSCC_TCS0_DRV0_STATUS RSCC_TCS1_DRV0_STATUS RSCC_TCS2_DRV0_STATUS RSCC_TCS3_DRV0_STATUS'''.split()

PDC = '''GPU_ENABLE_PDC GPU_SEQ_START_ADDR GPU_TCS1_CONTROL GPU_TCS1_CMD_ENABLE_BANK GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK
GPU_TCS1_CMD0_MSGID GPU_TCS1_CMD0_ADDR GPU_TCS1_CMD0_DATA GPU_TCS3_CONTROL GPU_TCS3_CMD_ENABLE_BANK
GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK GPU_TCS3_CMD0_MSGID GPU_TCS3_CMD0_ADDR GPU_TCS3_CMD0_DATA'''.split()


def domain_text(xml, file, name):
    text = (xml / file).read_text()
    m = re.search(r'<domain name="' + name + r'"[^>]*>(.*?)</domain>', text, re.S)
    if not m:
        raise SystemExit('no domain ' + name)
    return m.group(1)


def resolve_in(text, name):
    hits = set()
    for m in re.finditer(r'<(reg32|reg64|array) offset="(0x[0-9a-fA-F]+|[0-9]+)" name="' + re.escape(name) + r'"([^>]*)>', text):
        v = re.search(r'variants="([^"]+)"', m.group(3))
        if matches(v.group(1) if v else None):
            hits.add(int(m.group(2), 0))
    if len(hits) != 1:
        raise SystemExit('%s: %s' % (name, sorted(hits) or 'missing'))
    return hits.pop()


def table(catalog, name):
    m = re.search(r'static const struct adreno_reglist ' + name + r'\[\] = \{(.*?)\n\};', catalog, re.S)
    if not m:
        raise SystemExit('no ' + name)
    return re.findall(r'\{REG_A6XX_([A-Z0-9_]+), (0x[0-9a-fA-F]+)\}', m.group(1))


def protect(catalog, name):
    m = re.search(r'static const u32 ' + name + r'\[\] = \{(.*?)\n\};', catalog, re.S)
    out = []
    for kind, reg, length in re.findall(r'A6XX_PROTECT_(NORDWR|RDONLY)\((0x[0-9a-fA-F]+), (0x[0-9a-fA-F]+)\)', m.group(1)):
        reg, length = int(reg, 16), int(length, 16)
        value = ((length & 0x3fff) << 18) | (reg & 0x3ffff)
        if kind == 'NORDWR':
            value |= 1 << 31
        out.append(value)
    return out


def render(linux):
    msm = linux / 'drivers/gpu/drm/msm'
    xml = msm / 'registers/adreno'
    catalog = (msm / 'adreno/a6xx_catalog.c').read_text()
    lines = ['/* SPDX-License-Identifier: MIT */', '#ifndef FAJITA_A630_REGS_H', '#define FAJITA_A630_REGS_H',
             '/* Generated by scripts/derive-a630-hwinit.py from the Linux msm register XMLs (A6XX variants) and a6xx_catalog.c',
             ' * (a630_hwcg, a630_protect_regs). Dword offsets: A630_* from the GPU block, A630_GMU_* from the GMU block,',
             ' * A630_RSCC_* from the RSCC block, A630_PDC_* from gmu_pdc / gmu_pdc_seq. Do not edit by hand. */']
    seen = set()
    for n in GPU:
        if n in seen:
            continue
        seen.add(n)
        lines.append('#define A630_%s 0x%05x' % (n, resolve(xml, n, ['a6xx.xml', 'adreno_common.xml'])))
    gmu_text = (xml / 'a6xx_gmu.xml').read_text()
    for n in GMU:
        name = n if n.startswith('GPU_') else n
        lines.append('#define A630_%s 0x%05x' % (name if name.startswith(('GMU_', 'GPU_')) else 'GMU_' + name,
                                                 resolve_in(gmu_text, n)))
    rscc = gmu_text[gmu_text.index('starts at offset 0x8c00'):]
    for n in RSCC:
        lines.append('#define A630_RSCC_%s 0x%05x' % (n, resolve_in(rscc, n)))
    pdc = domain_text(xml, 'a6xx.xml', 'A6XX_PDC')
    for n in PDC:
        lines.append('#define A630_PDC_%s 0x%05x' % (n, resolve_in(pdc, n)))
    lines.append('#define A630_PDC_SEQ_MEM_0 0x%05x' % resolve_in(domain_text(xml, 'a6xx.xml', 'A6XX_PDC_GPU_SEQ'), 'MEM_0'))
    hwcg = table(catalog, 'a630_hwcg')
    lines.append('#ifdef A630_WANT_TABLES   /* only the hw_init unit includes the tables */')
    lines.append('#include <stdint.h>')
    lines.append('#define A630_HWCG_COUNT %du' % len(hwcg))
    lines.append('static const uint32_t A630Hwcg[A630_HWCG_COUNT][2] = {')
    for name, value in hwcg:
        lines.append('    {0x%05x, %s},   /* %s */' % (resolve(xml, name, ['a6xx.xml']), value.lower(), name))
    lines.append('};')
    regs = protect(catalog, 'a630_protect_regs')
    count_max = int(re.search(r'DECLARE_ADRENO_PROTECT\(a630_protect, (\d+)\)', catalog).group(1))
    lines.append('/* a6xx_set_cp_protect: entries 0..count-2 into CP_PROTECT(i) (zero entries skipped), the last into')
    lines.append(' * CP_PROTECT(count_max - 1) with LAST_SPAN_INF_RANGE. */')
    lines.append('#define A630_PROTECT_COUNT %du' % len(regs))
    lines.append('#define A630_PROTECT_COUNT_MAX %du' % count_max)
    lines.append('static const uint32_t A630Protect[A630_PROTECT_COUNT] = {')
    for i in range(0, len(regs), 6):
        lines.append('    ' + ', '.join('0x%08x' % v for v in regs[i:i + 6]) + ',')
    lines.append('};')
    lines.append('#endif')
    lines.append('#endif')
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--linux', type=Path, required=True)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    text = render(a.linux)
    if a.check:
        if not OUT.is_file() or OUT.read_text() != text:
            sys.exit('A630Regs.h differs from the derivation')
        print('A630Regs.h matches')
        return
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
