#!/usr/bin/env python3
"""smmu-survey.py: read-only dump of an ARM SMMUv2 (default apps_smmu 0x15000000) over the USB debug hub.

  smmu-survey.py [--base PA] [--raw FILE] [--from-raw FILE]

Reads (never writes) sCR0, sACR, ID0..ID2, sGFSR, every SMR/S2CR pair, every CBAR/CBA2R and each context bank's SCTLR, TCR,
TTBR0/1, MAIR0, FSR and FAR, through `hc read32` (the hub of a running cycle.sh/session.sh). It prints the decoded table:
which streams are matched to which bank, the bank's translation state, the unmatched-stream policy (sCR0.USFCFG) and the
banks no SMR points at. --raw saves the words read (ADDR: VALUE lines), --from-raw decodes such a file without the phone.
Bit layouts: ARM IHI 0062 (SMMUv2), as Linux drivers/iommu/arm/arm-smmu/arm-smmu.h.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

HC = Path(__file__).resolve().parent / 'hc'
words = {}


def rd(pa, n=1):
    missing = [pa + 4 * i for i in range(n) if pa + 4 * i not in words]
    if missing and not args.from_raw:
        out = subprocess.run([str(HC), 'read32', hex(pa), str(n)], capture_output=True, text=True,
                             env=dict(os.environ, T=os.environ.get('T', '20')))
        for m in re.finditer(r'^([0-9a-f]+): ([0-9a-f]+)$', out.stdout, re.M):
            words[int(m[1], 16)] = int(m[2], 16)
        if pa not in words:
            sys.exit(f'read32 {pa:#x} {n} failed: {out.stdout.strip()} {out.stderr.strip()}')
    return [words.get(pa + 4 * i) for i in range(n)]


p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
p.add_argument('--base', type=lambda v: int(v, 0), default=0x15000000)
p.add_argument('--raw')
p.add_argument('--from-raw')
args = p.parse_args()
if args.from_raw:
    for m in re.finditer(r'^([0-9a-f]+): ([0-9a-f]+)$', Path(args.from_raw).read_text(), re.M):
        words[int(m[1], 16)] = int(m[2], 16)
B = args.base
scr0, _scr1, _, _, sacr = rd(B + 0x0, 5)
id0, id1, id2 = rd(B + 0x20, 3)
gfsr, _, gfsynr0, gfsynr1 = rd(B + 0x48, 4)
nsmr = id0 & 0xff
page = 0x10000 if id1 >> 31 else 0x1000
npage = 1 << (((id1 >> 28) & 7) + 1)
ncb, ns2cb = id1 & 0xff, (id1 >> 16) & 0xff
cbase = B + npage * page
print(f'apps SMMU at {B:#x}: sCR0 {scr0:08x} sACR {sacr:08x} ID0 {id0:08x} ID1 {id1:08x} ID2 {id2:08x} sGFSR {gfsr:08x} '
      f'sGFSYNR0/1 {gfsynr0:08x}/{gfsynr1:08x}')
print(f'  sCR0: CLIENTPD={scr0 & 1} GFRE={scr0 >> 1 & 1} GFIE={scr0 >> 2 & 1} EXIDENABLE={scr0 >> 3 & 1} '
      f'GCFGFRE={scr0 >> 4 & 1} USFCFG={scr0 >> 10 & 1} ({"unmatched streams FAULT" if scr0 >> 10 & 1 else "unmatched streams BYPASS"})'
      f' VMIDPNE={scr0 >> 11 & 1} PTM={scr0 >> 12 & 1} FB={scr0 >> 13 & 1} BSU={scr0 >> 14 & 3} SMCFCFG={scr0 >> 21 & 1}'
      f' (Linux arm_smmu_device_reset wants (sCR0 & 0x407) == 0x406: {"yes" if scr0 & 0x407 == 0x406 else "no"})')
print(f'  ID0: NUMSMRG={nsmr} SMS={id0 >> 27 & 1} NUMSIDB={id0 >> 9 & 0xf} EXIDS={id0 >> 8 & 1} S1TS={id0 >> 30 & 1} '
      f'S2TS={id0 >> 29 & 1} CTTW={id0 >> 14 & 1}')
print(f'  ID1: PAGESIZE={page:#x} NUMPAGENDXB -> {npage} pages, context banks at {cbase:#x}, NUMCB={ncb} NUMS2CB={ns2cb}')
print(f'  ID2: IAS={id2 & 0xf} OAS={id2 >> 4 & 0xf} UBS={id2 >> 8 & 0xf} PTFS 4K={id2 >> 12 & 1} 16K={id2 >> 13 & 1} 64K={id2 >> 14 & 1}')
smr = rd(B + 0x800, nsmr)
s2cr = rd(B + 0xc00, nsmr)
cbar = rd(B + page, ncb)
cba2r = rd(B + page + 0x800, ncb)
TYPE = {0: 'translate', 1: 'bypass', 2: 'fault', 3: 'reserved'}
print(f'\nStream match groups ({nsmr}; valid ones, then the S2CR of every invalid group that is not all-zero):')
users = {}
for i in range(nsmr):
    v, sid, mask = smr[i] >> 31, smr[i] & 0x7fff, (smr[i] >> 16) & 0x7fff
    t, cb = (s2cr[i] >> 16) & 3, s2cr[i] & 0xff
    if v:
        users.setdefault(cb if t == 0 else None, []).append(i)
        print(f'  SMR{i:3d} {smr[i]:08x} id {sid:#06x} mask {mask:#06x}  S2CR {s2cr[i]:08x} {TYPE[t]}'
              + (f' -> cb{cb}' if t == 0 else '') + f' privcfg {s2cr[i] >> 24 & 3} instcfg {s2cr[i] >> 26 & 3}')
    elif smr[i] or s2cr[i]:
        print(f'  SMR{i:3d} {smr[i]:08x} (invalid)  S2CR {s2cr[i]:08x} {TYPE[t]}' + (f' cb{cb}' if t == 0 else ''))
print(f'\nContext banks ({ncb}):')
CBT = {0: 'S2', 1: 'S1/S2-bypass', 2: 'S1/S2-fault', 3: 'S1+S2'}
free = []
for n in range(ncb):
    sctlr, actlr, _, _, tcr2 = rd(cbase + n * page, 5)
    ttbr0lo, ttbr0hi, ttbr1lo, ttbr1hi, tcr, cidr, mair0 = rd(cbase + n * page + 0x20, 7)
    fsr, _, farlo, farhi = rd(cbase + n * page + 0x58, 4)
    smrs = users.get(n, [])
    on = sctlr & 1
    if not smrs and not cbar[n] and not cba2r[n]:   # never programmed: CBAR/CBA2R reset to 0, the bank registers hold power-on noise
        free.append(n)
    print(f'  cb{n:2d} CBAR {cbar[n]:08x} {CBT[cbar[n] >> 16 & 3]} vmid {cbar[n] & 0xff} irpt {cbar[n] >> 24} '
          f'CBA2R {cba2r[n]:08x}{" VA64" if cba2r[n] & 1 else ""} SCTLR {sctlr:08x} (M={on}) TCR {tcr:08x} TCR2 {tcr2:08x} '
          f'TTBR0 {ttbr0hi:08x}{ttbr0lo:08x} TTBR1 {ttbr1hi:08x}{ttbr1lo:08x} MAIR0 {mair0:08x} FSR {fsr:08x} '
          f'FAR {farhi:08x}{farlo:08x} SMRs {",".join(map(str, smrs)) or "-"}')
print(f'\nBanks no valid SMR uses, CBAR and CBA2R 0 (never programmed; free for a new stream): {free}')
if users.get(None):
    print(f'Bypass/fault groups (no bank): {users[None]}')
if args.raw:
    Path(args.raw).write_text(''.join(f'{a:08x}: {v:08x}\n' for a, v in sorted(words.items())))
