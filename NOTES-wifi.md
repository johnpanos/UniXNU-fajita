# fajita Wi-Fi: WCN3990 on SDM845 under UniXNU (iOS 16.7.16)

## Current state (keep this updated; a restart starts here)

Round 7 (lead): the AOSS QMP link-up, AP side only (no load_state or other message). Modem boot, SCM, memory
assignment, the SMMU, WMI and the network interface stay on hold.

**AOSS QMP link-up (done 2026-10-25, on the phone, PASS twice).** `drivers/FajitaWifi/WifiQmp.[ch]` is qcom_aoss.c
`qmp_open` step for step: check magic/version/mcore mailbox size; write UCORE_LINK_STATE into UCORE_LINK_STATE_ACK; MCORE
link = UP (0xffff), kick, wait MCORE_LINK_ACK == UP; MCORE channel = UP, kick, wait UCORE_CH == UP; UCORE_CH_ACK = UP,
kick, wait MCORE_CH_ACK == UP; 1 s per wait (HZ); a timeout closes the channel (once opened) and the link, then kicks,
like qmp_open's error path. Kick = `writel(BIT(0), 0x17990000 + 0xc)` (apss_shared chan 0). No mailbox word is ever
written. The AOSS doorbell interrupt (SPI 389) is not used: the waits poll every 50 us. Kernel side: `fajita-wifi-qmp=1`
(default off; with `fajita-wifi=1`) runs it once from the fajita-wifi worker after FW1/the survey (FajitaWifiLower.cpp,
writable maps of the message RAM and the apss_shared page only for this), with one `FWQ <n> <step> rc val us` row per
step, `FWQ done` and an `FWQ state` row of both ends; `kcmd wifi qmp` re-reads and prints the state plus the last
link-up's rows. Host tests `tests/t_qmp.c` (in `test_stack`): a simulated AOSS starting in the phone's round 6 state,
reacting only to kicks after 0..39 polls: the exact write/kick sequence, no write outside the four AP-owned/ack words,
mailboxes untouched, a second open on an up link; dead AOSS / no link ack / no channel / no channel ack each time out at
the right step and close correctly; bad magic, version or mailbox size 0 touch nothing. wifi-test.sh stage `qmp`
(`wifi-test.sh lower qmp`). On the phone (two boots, identical): before, AP link/ch down, AOSS link up and everything
else down (the state is fresh on every boot: a reset brings it back down); steps 0-7 rc 0, link ack after 77-78 us,
AOSS channel 3 us, channel ack 3 us, 3 kicks; after: **`FWQ state ap link up/ack up ch up/ack up | aoss link up/ack up
ch up/ack up ... mbox 128+2d8 => UP`**, and `kcmd wifi qmp` reports the same. The mcore mailbox (0x128, 0x2d8 bytes) is
where the later `load_state` message goes (qmp_send: 64-byte payload at +4, length at +0, kick, wait for length 0).
Also fixed: wifi-test.sh's hub path pointed at scripts/fajita/fajita-usbdbg-hub.py (absent), so its kcmd/iOS probes
never answered; it is scripts/fajita-usbdbg-hub.py.

Round 6 (lead): 1 = validate the lower layers on the phone, read only; 2 = the WPA2-PSK supplicant core, host only.
Modem boot, SCM, memory assignment, the SMMU, ath10k WMI and the iOS network interface stay on hold.

**1, lower layers read-only (done 2026-10-25, on the phone, PASS twice).** `fajita-wifi=1 fajita-wifi-ro=1` runs
`package/FajitaWifiLower.cpp` after FW1; `kcmd wifi ro|rows|snap` re-runs it, prints its rows, or copies the
AP-readable SMEM ranges to RAM for `scripts/fajita/test/smem-snap.py` (readv + CRC32 -> `.cache/smem-snapshot/smem.bin`,
mode 400, never committed). wifi-test.sh stage `lower` (`wifi-test.sh power lower`) checks the FWR rows, pulls the
snapshot and runs the host tests on it (`t_smem_real`). Nothing is written: SMEM and AOSS are mapped read-only, and the
only RPMh traffic is a READ request on an idle apps TCS (no vote). Only the SMEM header, the ptable and the partitions
whose hosts include APPS or GLOBAL are read (the others may be XPU-protected from the AP). Results (identical both runs):
- SMEM @0x86000000: initialized 1, SBL version word 0x000c0000 = **v12 (global partition)**, as WifiSmem expects. The SIII
  info says size 0x200000, base 0x86000000 and **615 items** (above the legacy 512; global items 602..614 exist).
- ptable: 15 entries, **cacheline 0x20** (the synthetic tests used 0x80), flags 3. Two entries have hosts 0xffff/0xffff
  (the header 0+0x1000 and the ptable 0x1ff000+0x1000; skipped). APPS pairs: modem 0xb9000+0x7c000, adsp 0x1af000+0x30000,
  cdsp 0x18f000+0x20000, slpi 0x15f000+0x30000, spss(8) 0x14f000+0x4000; global 0x1000+0x88000. The rest: adsp-slpi,
  tz(7)-spss, tz-modem, modem-spss, modem-cdsp, modem-adsp, modem-slpi.
- Every APPS private partition is **empty** (free-uncached 0x20, free-cached = size; APPS<->MODEM 507872 B free). The
  global partition holds 14 uncached items (469, 473, 602, 404, 403, 81, 137, 136, 604, 474, 402, 603, 614, 612), some
  with data padding (usable sizes not a multiple of 8). Item walk == wsmem_get for each.
- SMP2P: all eight items of the four APPS pairs (mpss 435/428, lpass 443/429, slpi 481/430, cdsp 94/432) are absent:
  no remote processor is up and nothing on the AP has allocated its out item. Expected before the modem boots.
- cmd-db: cx.lvl @0x30000 levels {0 16 48 64 128 192 256 384}, mx.lvl @0x30010 {0 16 16 256 384}, mss.lvl @0x30060
  {0 16 48 64 128 192 256 384}. RPMh READ: cx 6, mx 3, mss 0 (indices into those tables: cx 256 = NOM, mx 256 = NOM,
  mss off). Whether a READ returns the aggregate or only the apps DRV's vote is not confirmed.
- AOSS QMP @0x0c300000: magic 0x4d41494c ok, version 1, features 0; **ucore (AOSS) link up (0xffff), its link ack and
  both channels down; mcore (APSS) link and channel down**; mbox ucore 0xc0+0x68, mcore 0x128+0x2d8, nothing pending.
  AOSS is waiting for the AP's link-up handshake (qmp_open: mcore link up, kick, wait ucore ack, then the channels);
  UEFI never used QMP. So the modem's `load_state` message needs that handshake first.
- **Parser: no bug found on real data.** WifiSmem's init, walks and lookups agree with the snapshot. New host tests pin
  the real shape without the blob (`t_smem_phone_layout`: cacheline 0x20, 0xffff entries, SIII 615, items > 512 in
  global, a non-APPS partition with a bad header that must not be validated) and run what stage 3 will allocate (478,
  479, the 435 SMP2P out item, a cached 480) on that layout and on a copy of the real snapshot (`t_smem_real`).
- Also fixed on the way: the kernel did not link with `fajita-wifi` code since stage 5 (`_fajita_wifi_msa_assign`, an
  undefined weak reference from a static archive, which ld64 does not leave unresolved); a weak default returning -78
  now stands in, and FW5 still reports `msa-assign?` until the modem-boot side links its definition.

**2, WPA2-PSK supplicant core (done 2026-10-25, host only).** `drivers/FajitaWifi/WifiWpa.[ch]`, freestanding, with
its own SHA-1, HMAC-SHA1 and AES-128/192/256 (nothing suitable was in the tree): PBKDF2 PSK (8..63 printable chars,
SSID <= 32), the PRF and PTK (KCK | KEK | TK, min/max ordering), EAPOL-Key parse/build/MIC (descriptor type 2, key
descriptor version 2 = HMAC-SHA1-128 + AES key wrap; version 1 TKIP is refused), RFC 3394 wrap/unwrap, the KDE parser
(RSNE + GTK KDE, 0xdd padding), and the supplicant side of the 4-way handshake: M1 -> M2 (our RSNE, MIC), M3 -> M4
(MIC, same ANonce, replay counter strictly increasing, key data unwrapped, M3's RSNE must equal the beacon's, GTK
taken). A failed check drops the frame and leaves the state as it was; an M3 retransmission after success answers M4
again with `WWPA_M4_RESENT` and does not reinstall keys (KRACK). Keys are wiped after use; nothing logs. Host tests
`tests/t_wpa.c` (ASan/UBSan, in `test_stack`): SHA-1 FIPS vectors incl. a million 'a', RFC 2202 HMAC 1/2/6, FIPS-197
C.1-C.3, RFC 3394 4.1-4.6, J.4 PSK tests 1-3, J.3 PRF test 1 (tests 2-3 cross-checked with Python hmac), and a full
handshake whose M2 and M4 match byte for byte the frames of an independent reference, `tools/wifi/gen-wpa-vectors.py`
(hashlib/hmac + `cryptography` AES-KW; `--check` verifies the generated `tests/wpa_vectors.h`) on the J.4 PMK and the
J.7 inputs (AA a0..a5, SPA b0..b5, ANonce e0..ff, SNonce c0..df). The J.7 PTK digits were not available offline, so
the PTK is checked against that reference, not against transcribed Annex J output. Negative tests: tampered MIC,
wrong ANonce, replayed counters, RSNE downgrade, wrong passphrase, TKIP, no room for M2. Not done: the group key
handshake (GTK rekey), PMKID, PMF/IGTK, SHA-256 AKMs. Also built into libFajitaWifi.a (kernel compile check passes);
nothing calls it yet.

Round 5 (lead): A = an iOS Ethernet interface proven over USB (IOEthernetController subclass, frames tunnelled over the
fjdbg channel to a host bridge), B = the apps_smmu read-only survey. Modem boot, SCM and ath10k WMI are the user's.

**A, iOS Ethernet over USB (stopped 2026-10-25 by the safety filter; per the lead, not to be retried):** nothing of the
driver, gadget, hub or host bridge is written or committed. Findings for whoever picks it up:
- IONetworkingFamily is not in the prelink today. A trial prelink with `com.apple.iokit.IONetworkingFamily` added to the
  ios16/board.toml roots binds cleanly (dependencies: com.apple.kpi.* only; +16 KiB); not added to the board yet. Its
  symbols are stripped in the 20H392 kernelcache.
- Host backend without root: `passt` (installed) speaks the qemu stream format (u32 BE length + Ethernet frame) on a UNIX
  socket and gives the guest DHCP, DNS forwarding and NAT; unprivileged ICMP is allowed here (ping_group_range 0..2^31-1).
  A tap + dnsmasq + NAT route would need one sudo step instead.
- Tunnel design: a new fjdbg channel 7 (NET), one Ethernet frame per frame in both directions (a 1514-byte frame fits
  kMaxData 1536); device side a TX via queueFrame with a reserve so replies are never starved and an RX ring filled from
  onBulkOut; host side a hub op that turns a socket into the frame stream, relayed to passt by a small bridge.

**B, apps_smmu survey (done 2026-10-25, read only, on the phone):** `scripts/fajita/test/smmu-survey.py` (hub `read32`
only; `--raw` saves the words, `--from-raw` decodes offline). Run on the ios16-port test build (usb-debug=3, GPU on),
in iOS userland, so the state is UEFI's plus NativeUfs/NativeDwc3's:
- sCR0 0x002b0406: CLIENTPD 0, GFRE/GFIE 1, **USFCFG 1: an unmatched stream FAULTS** (it does not bypass), SMCFCFG 1;
  `(sCR0 & 0x407) == 0x406` holds, so ArmSmmu4KAttachStreams' precondition is met. sACR 0x0400001c. sGFSR 0.
- ID0 0x4c017e4c: 76 SMR groups, SMS, 15 SID bits, S1 only (S2TS 0). ID1 0x5000002d: 4 KiB pages, 64-page global
  space, context banks at 0x15040000 (+0x1000 each), **45 banks**, 0 S2-only. ID2 0x5555: 48-bit IAS/OAS, 4K+64K granules.
- Valid SMRs (8 of 76), every bank in S1-with-S2-bypass, VA64, **SCTLR.M = 0 (all bypass, no translation)**:

  | SMR | Stream / mask | Device | Bank | CBAR | SCTLR |
  | --- | --- | --- | --- | --- | --- |
  | 0, 1 | 0x880 / 0x8, 0xc80 / 0x8 | MDSS (display) | cb0 | 0x10003 (vmid 3) | 0xe0 |
  | 2, 3 | 0xa0 / 0xf (sdhc_2), 0xc0 / 0xf (not in mainline DT) | | cb1 | 0x10000 | 0xe0 |
  | 4, 5 | 0xe0 / 0xf (not in mainline DT), 0x100 / 0xf (UFS) | UFS | cb2 | 0x10000 | 0xe0 |
  | 6 | 0x740 / 0 | USB prim (dwc3, our gadget) | cb3 | 0x10000 | 0x0 (NativeDwc3's VA64 bypass) |
  | 7 | 0x760 / 0 | USB sec | cb4 | 0x10000 | 0xe0 |

- SMR 8..75 invalid, their S2CR read 0x000200ff (FAULT, cb 0xff). **No SMR covers 0x40/0x41 (Wi-Fi): Wi-Fi DMA faults
  today.** Banks 5..44 (40 banks) have CBAR = CBA2R = 0 and power-on noise in SCTLR/TCR/TTBR (never programmed): free.
  A Wi-Fi bank: one of 5..36 (off bank 0, off NativeUfs's top-8 range 37..44), plus one free SMR (8..) for 0x40 mask 0x1
  (or two exact ones for ArmSmmu4KAttachStreams). Raw words: `.cache/fajita-work/smmu/apps-smmu-raw.txt`.

Layer 5 notes (from ath10k, for the restart): WCN3990 hw_params target_64bit, shadow_reg_support (src write index via
0x32000 + 4*ce for CE0/3/4/5/7), rri_on_ddr (read indices in a host DMA word per CE, enabled by ctrl1 bit 19; used for
DIS_INTR pipes), use_fw_tx_credits = false (one HTC credit total, all on WMI control), CE regs at membase+0x240000 +
0x1000*ce (wcn3990_ce_regs in hw.c), desc meta mask 0xfff0 lsb 4. Core start order: htc init, htt tx/rx alloc, hif start
(irq enable, rx post on every pipe with a dest ring), htc wait READY, htt connect, wmi connect, htc SETUP_COMPLETE_EX,
wmi service ready, wmi init, wmi ready, htt rx ring refill, htt setup. DMA for the CE rings goes through the apps SMMU
(SID 0x40): open hardware item, the glue takes a DMA allocator callback.

Round 3 (stack above a booted modem; the modem boot itself is the user's). Kernel hook: `FajitaWifiProbe.cpp` calls
`fajita_wifi_stack(stage)` (FajitaWifiStack.cpp) for `fajita-wifi>=3` after stage 2; it never returns (one poll loop owns
every protocol object). For the modem boot: `fajita_wifi_smp2p_bits(&v)` (inbound bits, ready = bit 2) and
`fajita_wifi_smp2p_set(mask, v)` (outbound stop bit 0). Compile check without a build: `scripts/check-fajita-wifi-kernel.py`.
Notes for the phone run: TFTP offers at most blksize 8192 (GLINK fifo is 16 KiB; tqftpserv allows 65464) and the modem
must accept the smaller OACK; stage 5 needs the boot side's `fajita_wifi_msa_assign(regions, n)` (weak; without it FW5 stops
before msa_ready with `msa-assign?`); the board file is `image/bdwlan.<id>` (cnss naming: .bin for 0xff, .bNN, .NNN); `/readwrite/` is RAM only; rmtfs refuses writes (counted as `wr-refused` in FW4). Worktree `../UniXNU-fajita-wifi`, branch
`wifi-stack`, merged into `ios16-port` after each layer. Host tests: `scripts/check-fajita-wifi-host.py` (two binaries:
`test_wifi` for the earlier protocol code, `test_stack` for the layers below).

Round 4 order (lead): 5a CE+HTC, 5b WMI-TLV, 5c association + HTT, then layer 6 (Ethernet interface, WPA2, DHCP), all
host-only; the apps_smmu glue comes LAST, after a read-only hardware survey on the phone.

apps_smmu facts for the Wi-Fi glue (round 3, read-only findings):
- The GPU's banks are on the separate adreno_smmu (0x5040000). Wi-Fi is on apps_smmu (0x15000000), shared with UFS, USB
  (SID 0x740) and display (0x880/0xc80).
- NativeUfs's SmmuTakeover.h takes apps_smmu banks from the top (8 deep) or adopts the UEFI UFS bank: a Wi-Fi bank stays
  off the top 8 and off bank 0.
- ArmSmmu4KAttachStreams does exact matches only: SID 0x40 mask 0x1 needs SMRs for 0x40 and 0x41. It requires
  (sCR0 & 0x407) == 0x406, which UEFI may not have left. Survey first, read-only: sCR0, ID0-2, every SMR/S2CR, each bank's
  CBAR/SCTLR.
- The Qualcomm S2CR quirk (NOTES-gpu.md section 2: FAULT-type S2CR writes dropped, BYPASS lands as FAULT) probably applies.
- IOVAs under 4 GiB; non-cacheable tables (the wifi node is not dma-coherent).
- The MSA is likely not mapped through apps_smmu on SDM845 (the SCM assign gives it to the WLAN VMIDs): no window for it.
- Use the kit in the stack worktree (kits/ArmSmmu4K at ios16/gpu-plugin.ref), not a second copy.

Round 4 status (final for round 4): 5a (CE + HTC) is done and merged (3f81617). 5b stands at the ABI constants only:
`tools/wifi/gen-ath10k-abi.py` and its generated `WifiAth10kIds.h` / `tests/ath10k_layout.h` are merged (3be19ca,
`--check` passes). The WMI-TLV module (`WifiWmi.[ch]`) is NOT written: round 4 was stopped by the safety filter while
writing `WifiWmi.h` (the partial file was deleted, nothing of it committed), and per the lead it is not to be retried.
5c, layer 6 and the apps_smmu glue were not started; wifi-test.sh is unchanged. Facts gathered for 5b: TLV header is le16 len then le16 tag; command header
le32 id (mask 0xffffff); main service bitmap uses 4 services per u32 word (WMI_SERVICE_IS_ENABLED, ids < 128), the
SERVICE_AVAILABLE ext bitmap is 32 per word from id 128; host memory chunks: num_units from NUM_UNITS_IS_* flags
(vdevs 4+1, peers 33+1), pool = num_units * roundup(unit_size, 4), ptr_high only with SUPPORT_EXTEND_ADDRESS (ext 179);
rx decap stays NATIVE_WIFI (ath10k never uses Ethernet rx); tx encap vdev param NATIVE_WIFI; a vdev must exist before a
scan; WMI-TLV start scan XORs WMI_SCAN_FILTER_PROBE_REQ.

| Layer | Code | Host test | Status |
| --- | --- | --- | --- |
| 1 SMEM + SMP2P | `WifiSmem.[ch]` (+ read-only inspection), survey `package/FajitaWifiLower.cpp` | `tests/t_smem.c` (synthetic SMEM, both ends; the phone's layout; the real snapshot) | done, validated read-only on the phone |
| 2 GLINK over SMEM | `WifiGlink.[ch]` (also fixed `glink_intent` to 16 B) | `tests/t_glink.c` (loopback, AP + simulated modem) | done |
| 3 QRTR + pd-mapper / TFTP / rmtfs (ro) | `WifiRouter`, `WifiPdmap`, `WifiTftp`, `WifiRmtfs` + kernel glue `package/FajitaWifiStack.cpp` | `tests/t_qrtr.c` (simulated modem router; real .jsn, real wlanmdsp.mbn via FAT, real EFS backup) | done |
| 4 wlfw client | `WifiWlfw.[ch]`, `WifiSnoc.[ch]` (snoc CE tables); stage 5 in FajitaWifiStack.cpp | `tests/t_wlfw.c` (fixture server, every request decoded; real bdwlan.102 == board-2.bin id 0x102) | done |
| 5a CE + HTC | `WifiCe.[ch]` (rings, RRI on DDR, shadow src idx, rx post/repost, gather; one DMA arena `wdma`), `WifiHtc.[ch]` (READY, connect, SETUP_COMPLETE_EX, credits/trailers, WMI queue, CE5 direct HTT) | `tests/t_ce.c` on `tests/wsim.[ch]` (CE register/DMA target) + `tests/wfw.[ch]` (firmware model) | done |
| 5b WMI-TLV init/ready/scan | constants: `WifiAth10kIds.h` + `tests/ath10k_layout.h`, generated by `tools/wifi/gen-ath10k-abi.py LINUX` (`--check` verifies); `WifiWmi.[ch]` not written yet | | in progress (stopped: see below) |
| 5c assoc + HTT | | | |
| 6 iOS Ethernet + WPA2 + DHCP | WPA2-PSK core: `WifiWpa.[ch]` | `tests/t_wpa.c` (FIPS/RFC/Annex J vectors; 4-way vs `tools/wifi/gen-wpa-vectors.py`) | WPA2 core done; Ethernet/DHCP on hold |

Goal: join a WPA2 network and pass IP traffic in iOS on the OnePlus 6T. The Wi-Fi radio (WCN3990) runs on the modem
DSP, so this is a modem-remoteproc bring-up before it is a Wi-Fi driver. Status 2026-10-25: host-only phase (no phone
yet). Research, design, protocol code and host tests are done; nothing runs on hardware.

Sources are mainline Linux v6.16 at `/home/john/src/ref/linux` (sparse: the ath10k, remoteproc, soc/qcom, rpmsg, qrtr,
reset, clk and dts paths), the linux-msm userspace daemons at `/home/john/src/ref/{rmtfs,tqftpserv,pd-mapper,qrtr}`, and
the firmware package below. Register constants with their sources are in `package/FajitaWifiBoard.h`.

## 1. Hardware facts (cited)

| Fact | Value | Source |
| --- | --- | --- |
| Wi-Fi radio | Qualcomm WCN3990, on-SoC (SNOC), driven by `ath10k_snoc` | `drivers/net/wireless/ath/ath10k/snoc.c`, `qmi.c` |
| Wi-Fi node | `wifi@18800000` `qcom,wcn3990-wifi`, membase 0x18800000+0x800000 | `sdm845.dtsi:5465` |
| CE interrupts | 12, GIC SPI 414..425 level-high (INTID 446..457) | `sdm845.dtsi` wifi node |
| Wi-Fi SMMU | `iommus = <&apps_smmu 0x40 0x1>` (SID 0x40) | `sdm845.dtsi` wifi node |
| Wi-Fi supplies | l5a 0.8 (cx/mx), l7a 1.8 (xo), l17a 1.3 (rfa), l25a 3.3 (ch0), l23a 3.3 (ch1); RF clk RPMH_RF_CLK2 | `sdm845-oneplus-common.dtsi:862` |
| SDM845 quirk | `qcom,snoc-host-cap-8bit-quirk` (host_cap uses the 8-bit EI table) | `sdm845-oneplus-common.dtsi:869` |
| Modem | `mss_pil remoteproc@4080000` `qcom,sdm845-mss-pil` — the MBA self-auth path (not PAS) | `sdm845.dtsi:3299`, `qcom_q6v5_mss.c:2311` (`MSS_SDM845`) |
| Modem firmware names | `qcom/sdm845/oneplus6/mba.mbn`, `.../modem.mbn` | `sdm845-oneplus-common.dtsi:455` |
| Carveouts (no-map) | mba 0x96500000+0x200000, mpss 0x8e000000+0x7800000, wlan_msa 0x8df00000+0x100000, smem 0x86000000+0x200000, mdata 0x4000 | `sdm845.dtsi` reserved-memory |
| rmtfs memory | fajita overrides it to 0xf5b01000+0x200000, with guard pages, `qcom,vmid = MSS_MSA` | `sdm845-oneplus-common.dtsi:79` |
| MSS reset path | QDSP6SS sleep-clk + BOOT_CORE_START/BOOT_CMD FSM, then PBL/MBA via RMB regs; AOSS reset MSS_RESTART, PDC modem sync bit 9 | `qcom_q6v5_mss.c:655` |

The modem is the SDM845 MBA variant: Linux copies `mba.mbn` into the mba carveout, hands the Q6 access to mba + mpss
(`qcom_scm_assign_mem` HLOS->MSS_MSA), kicks the boot FSM, waits PBL, writes the metadata (ELF header + hash segment)
address to the RMB, waits META_DATA_AUTH_SUCCESS, loads each `modem.mbn` segment into the mpss carveout, writes
CODE_START/CODE_LENGTH, and waits AUTH_COMPLETE. `need_mem_protection = true`, `has_alt_reset = true`. This is **not**
the PAS/SCM path the GPU zap shader uses (`FajitaGpuProbe.cpp stage8`); it is heavier and needs SMEM, SMP2P, AOSS QMP
and the EFS/rmtfs backing.

### Verified against the firmware (host test output)

`drivers/FajitaWifi/tests/test_wifi.c` parses the real images from `.cache/firmware-wifi`:

- `mba.mbn`: 7 phdrs, 5 loadable, hash segment index 1, entry 0x4417000, link span 0x80e00000..0x80e4a000, metadata 6940 B.
- `modem.mbn`: 30 phdrs, 28 loadable, relocatable, span 0x8e000000..0x95800000 (exactly 0x7800000 = the mpss carveout), one
  non-split ELF (no `.b00` segment files), metadata 8412 B.
- `wlanmdsp.mbn`: 7 phdrs, 3 loadable, span 0xb0000000..0xb06b6000. Loaded **by the modem**, not the AP: it is fetched from
  the AP over TFTP (tqftpserv) once the modem boots and brings up its wlan PD.
- `board-2.bin`: ath10k WCN3990 hw1.0; `bus=snoc,qmi-board-id=102` matches (19152 B of board data). The runtime board id
  comes from the wlfw cap response.

## 2. Boot chain (what must come up, in order)

```
1  regulators + RF clk           RPMh votes l5a/l7a/l17a/l25a/l23a, RPMH_RF_CLK2 (as FajitaBusVote/FajitaTouch LDO votes)
2  MSS remoteproc up             GCC MSS clocks, AOSS reset + PDC modem sync, QDSP6SS boot FSM, SCM mem assign,
                                 MBA auth (PBL->XPU_UNLOCKED), modem.mbn segment load (AUTH_COMPLETE); SMP2P "ready"
3  SMEM + GLINK + QRTR           SMEM partitions, GLINK over SMEM fifos (apss_shared 12), QRTR node, modem announces servers
4  AP-side QRTR services         pd-mapper (svc 0x40), tqftpserv (svc 4096 -> modem fetches wlanmdsp.mbn), rmtfs (svc 14 -> EFS)
5  wlfw handshake (svc 0x45)     ind_register, host_cap(8bit), msa_info, SCM assign MSA to MSS+WLAN, msa_ready, cap,
                                 bdf_download(board-2.bin), cal_report, wlan_cfg(CE rings), wlan_mode(mission); FW_READY ind
6  CE rings + HTC/HTT/WMI        copy-engine over membase 0x18800000, HTC handshake, WMI "ready", scan
7  connect + data                WMI connect (WPA2 4-way), then IP traffic
```

Nothing above stage 1 exists in the UniXNU stack today: no SMEM, GLINK, QRTR, QMI, pd-mapper, tqftpserv or rmtfs. They
are being built in `drivers/FajitaWifi` (protocol/parsing, host-tested) and `package/FajitaWifiProbe.cpp` (the staged
hardware bring-up, default off). The modem RMB/QDSP6 poke reuses the ml_io_map + dsb pattern of the touch/pmic probes;
the SCM assign reuses the smc wrapper of `FajitaGpuProbe.cpp`.

## 3. Firmware sources

pmOS `firmware-oneplus-sdm845` (GitLab `sdm845-mainline/firmware-oneplus-sdm845`, commit 3e31a0c) is cloned into
`.cache/firmware-wifi/` (gitignored; **never commit**). It carries, under `lib/firmware/qcom/sdm845/OnePlus/enchilada/`
(the enchilada = OnePlus 6 path; the 6T/fajita shares it): `mba.mbn` (242400 B), `modem.mbn` (60.3 MB), `wlanmdsp.mbn`
(3.0 MB); and under `lib/firmware/postmarketos/ath10k/WCN3990/hw1.0/`: `board-2.bin` (576048 B). The pd-mapper domain JSON
(`modemr.jsn`, `modemuw.jsn`) confirm the wlan PD domain `msm/modem/wlan_pd`, qmi_instance 180, providing `kernel/elf_loader`,
`tms/servreg`, `wlan/fw`. `a630_zap.mbn` in this package has the same sha256 as our pinned GPU zap, confirming it is the
same OnePlus firmware set. The vendor `modem` partition on the phone holds the same images.

Compressed, the three AP-loaded images are ~36 MB (modem 34.2 MB + mba 0.13 MB + board 0.4 MB zlib). `wlanmdsp.mbn`
(1.7 MB) is served to the modem over TFTP, so the AP must also hold it.

## 4. iOS side: two options

iOS networking expects Apple's Wi-Fi stack (`wifid` + `IO80211Family` + `AppleBCMWLAN*` for Broadcom). On this board
`/usr/sbin/wifid` is replaced by our USB debug agent (`package/usb-debug-tools/fjdbg-agent.c`, union-mounted over
`/usr/sbin` when `usb-debug=3`), which answers CoreWiFi XPC with empty replies so first-boot migration and the Settings
Wi-Fi pane do not hang.

**(a) Full Apple path — an `IO80211Controller` subclass `wifid` accepts.** The 20H392 kernelcache prelinks
`IO80211Family`, `IOSkywalkFamily`, `io80211peermanager`, and Apple's Broadcom driver (`AppleBCMWLANCore`,
`AppleBCMWLANBusInterfacePCIe`). The interface `wifid` drives is private, undocumented, Skywalk-based, and tied to
Broadcom chip/firmware semantics. Matching it for an Atheros/ath10k radio means reimplementing a large private surface
with no reference. Very hard; later target at best.

**(b) Ethernet-like `IOEthernetController` + our own supplicant + DHCP (recommended first target).** The kernelcache has
`IONetworkingFamily`, `IOEthernetController`/`IOEthernetInterface`, and `AppleUSBEthernetHost`, so iOS's IP stack
(configd, mDNSResponder, apps) runs over a plain Ethernet `en` interface without `wifid`. Plan: an `IOEthernetController`
whose data path is the WCN3990 CE/HTT/WMI (802.11 encap/decap in the driver, presenting 802.3 frames up); association and
the WPA2-PSK 4-way handshake done in the driver or a small kernel supplicant (keys derived with corecrypto, which is
already linked); DHCP left to iOS's own `configd` once the link is up, or seeded over the debug channel. `wifid` is left
replaced by the agent. The SSID/PSK are configured over the debug channel (a `kcmd` like the existing `bus`/`key`/`vibrate`)
or a boot arg for the first bring-up.

Recommendation: **(b)**. It reuses the proven CPU-driven, boot-arg-staged bring-up style of this board, gives real iOS
connectivity (IP, DNS, apps) at a bounded cost, and avoids the undocumented IO80211 surface. (a) stays a research item.

Trade-offs of (b): no Settings Wi-Fi UI integration (the link is an Ethernet interface, not a Wi-Fi one in Preferences);
roaming, power save and 802.11 management are minimal; the supplicant is ours to maintain. These are acceptable for "join
one WPA2 network and pass traffic".

## 5. Staged bring-up plan (`fajita-wifi=<N>`, default 0)

Default off: without `fajita-wifi=<n>` nothing is mapped, voted, or poked, so every other agent's build is unchanged. Each
stage runs all lower ones first; the first failure stops the sequence and is logged. Driven from the boot progress monitor
(`fajita_wifi_poll`, once a second, after IOKit and, with `fajita-wdog`, after ArmCpu armed the watchdog), like
`fajita_pmic_poll`. Rows go to `fajita_wifi_rows[]` and IOLog (`fajita-wifi: ...`); BootLog writes them to every UFS log
slot so the first phone session shows exactly where it stopped.

| Arg | Stage | Writes HW | Logs (prefix `FW`) |
| --- | --- | --- | --- |
| `fajita-wifi=1` | power: RPMh votes for l5a/l7a/l17a/l25a/l23a + RF_CLK2; read GCC MSS branch states; map RMB/QDSP6/SMEM/GCC/AOSS | RPMh TCS (shared vote, never un-voted) | `FW1 vote... clk... rmb<pbl>` |
| `fajita-wifi=2` | modem up: GCC MSS clocks, AOSS+PDC reset, SCM assign mba+mpss to MSS, QDSP6SS boot FSM, MBA PBL/XPU, load modem.mbn segments, AUTH_COMPLETE, SMP2P ready | GCC, reset, SCM, QDSP6SS, RMB, mpss carveout | `FW2 fsm<status> pbl<n> mba<n> seg<k/n> auth<n> ready<smp2p>` |
| `fajita-wifi=3` | SMEM partitions, GLINK over SMEM (version/open IPCRTR), QRTR node 1, read the modem's NEW_SERVER announcements | SMEM, GLINK fifos, APCS IPC | `FW3 smem<parts> glink<ver> qrtr servers<wlfw?servreg?>` |
| `fajita-wifi=4` | AP-side servers: pd-mapper (0x40) answers GET_DOMAIN_LIST; tqftpserv (4096) serves wlanmdsp.mbn; rmtfs (14) backs EFS read-only from the UFS modemst/fsg partitions | none new (QRTR messages) | `FW4 pdr<reqs> tftp<bytes> rmtfs<iovecs>` |
| `fajita-wifi=5` | wlfw handshake: ind_register, host_cap(8bit), msa_info, SCM assign MSA to MSS+WLAN, msa_ready, cap, bdf_download(board-2.bin), cal_report, wlan_cfg(CE), wlan_mode(mission); wait FW_READY | SCM (MSA), QRTR messages | `FW5 ind cap<chip/board> msa<regions> bdf<segs> cfg mode ready<fw_status>` |
| `fajita-wifi=6` | CE rings on membase 0x18800000, HTC handshake, HTT setup, WMI "ready", scan | WLAN membase, SMMU SID 0x40 | `FW6 ce<rings> htc wmi<ready> scan<bss>` |
| `fajita-wifi=7` | WMI connect (WPA2-PSK 4-way), publish the IOEthernetController, pass data | WLAN | `FW7 connect<aid> ptk gtk link data<rx/tx>` |

`fajita-wifi-ssid=`, `fajita-wifi-psk=` (or a debug-channel `wifi` kcmd) feed stage 7. Options mirror the other probes:
`fajita-wifi-dump=1` (register/SMEM dumps), read-only guards (`-ro`) for the SCM/rmtfs writes while bringing up.

### Memory reservation (lead point 1)

`rmtfs_mem` 0xf5b01000+0x200000 and the modem carveouts are `no-map` in Linux: the OS must never hand those pages out,
because the modem DMA-writes them. On this board DarwinBoot picks the kernel window from EfiConventionalMemory
(`AllocateWindow` in `drivers/DarwinBoot/.../DarwinBoot.c`) and the ADT carries `darwinboot-memory-size`
(`owned_memory_size` 0x130000000 = 4.75 GiB from DRAM base 0x80000000, i.e. 0x80000000..0x1b0000000). The modem carveouts
(0x86000000, 0x8df00000, 0x8e000000, 0x96500000) fall **inside** that owned window, and rmtfs at 0xf5b01000 does too. The
enchilada UEFI memory map (`Mu-Silicium .../enchiladaPkg MemoryMapLib.c`) already reserves QSEE (0x86200000+0x4900000)
which covers smem, but not the modem/mpss/wlan_msa/rmtfs ranges. **Before the modem boots**, stage 2 must claim those
pages so XNU/NativeRamBlock cannot reuse them. Options, behind `fajita-wifi` so the default is unchanged:
- shrink `owned_memory_size` is wrong (it is contiguous from the base); instead
- reserve the specific ranges. Cleanest: add `fw-reserved` entries to the ADT (`make-fajita-adt.py`, only when a
  `--wifi-reserve` flag is given) that XNU treats as carveouts, OR have stage 1 fault them in and pin them via an
  `IOBufferMemoryDescriptor` at those fixed physical addresses before stage 2. The ADT route is cleaner and is what
  stage 1 will log it did. **Open: confirm XNU honours an ADT carveout at a fixed PA; test on the phone.** Until then,
  stage 2 refuses to boot the modem if the ranges are not reserved (logged `FW2 reserve?`), so we never corrupt XNU RAM.

**Done 2026-10-25 (measured):** `ios16/patches/xnu-8796/fajita-phys-reserve.patch` adds `phys-reserve=<pa>.<size>,...`
(the payload's argument check allows only `[A-Za-z0-9_.,-]`, hence `.` as the separator). `vm_page_release_startup` keeps
every page of a listed range off the free lists (counted as wired); without the argument nothing changes. The probe's
`FW1 res` row checks each modem DMA range: fully held, or entirely outside the managed window; stage 2 refuses otherwise
(`FW2 reserve?`). wifi-test.sh passes `phys-reserve=$PHYS_RESERVE` (rmtfs with guard pages, msa, mpss, mba). On the phone:
`FW1 res win a01c8000..1d01c8000 avail ae185000 rmtfs held(516/516) msa outside mpss outside mba outside`. XNU runs on
4 KiB pages here, and DarwinBoot's window starts at 0xa01c8000, above all of the modem carve-outs (0x8df00000..0x96700000);
only rmtfs (0xf5b01000) lies inside it, and it is held. Changing the patch: the build applies patches idempotently, so
reset `osfmk/vm/vm_resident.c` in `.cache/<out>/unixnu/fajita-ios16/tree/.cache/xnu-work` first.

### EFS / rmtfs (lead point 2)

The modem reads and writes its EFS (NV) from the AP over rmtfs (QMI svc 14): `modemst1`, `modemst2`, `fsg`, `fsc` on UFS
(linux-msm/rmtfs `storage.c` partition_table; OnePlus also `oem_stanvbk`/`oem_dycnvbk`). The AP allocates a shared buffer
(`alloc_buf`), the modem issues `rw_iovec` (sector/offset/count triples), and the AP copies between that buffer and the
partition. **wlfw strictly needs at least read access**: the modem reads its calibration/NV at boot and may refuse to
bring up the wlan PD without it. Plan: **read-only first** — serve reads from the UFS partitions through NativeUfs, fail
writes (or buffer them in RAM and never flush). The modem tolerates read-only EFS for bring-up; persistent NV writes
(e.g. learned calibration) are a later concern. **Never write the real `modemst*` without a backup**: the first phone
session dumps `modemst1/2`, `fsg`, `fsc` to `.cache/` before rmtfs is enabled. NativeUfs exposes a store ABI
(`native_ufs_store_*`) but it is a dedicated region, not arbitrary partitions; serving these partitions needs a read path
to those GPT entries — an open item for the NativeUfs side (likely a small read-only partition-by-name accessor).

### EFS backup (done 2026-10-25, verified)

`drivers/NativeUfs/PartRead.h` (on with `native-ufs-partread=1`, off by default) is a read-only reader for any named GPT
partition on LUNs 0..5: `kcmd ufspart find NAME | list LUN | read NAME OFF COUNT` (READ(10) only), plus the in-kernel
`native_ufs_part_find` / `native_ufs_part_read` that rmtfs will use. `scripts/fajita/test/ufspart.py backup` reads each
partition twice through a held session (each 128 KiB chunk CRC32-checked between the kernel buffer and the host), requires
both passes identical, checks the size and GUIDs against the GPT before and after, and writes `.cache/efs-backup/NAME.img`
(mode 400) + `manifest.json`; `ufspart.py verify` re-checks a backup against the live GPT and the manifest sha256.

| Partition | LUN | first..last (4 KiB) | Size | Content |
| --- | --- | --- | --- | --- |
| modemst1 | 5 | 32..543 | 2 MiB | populated (EFS) |
| modemst2 | 5 | 544..1055 | 2 MiB | populated (EFS) |
| fsg | 5 | 1056..1567 | 2 MiB | **all zero** on this phone |
| fsc | 5 | 1568..1599 | 128 KiB | all zero |
| persist | 0 | 8..8199 | 32 MiB | populated |
| oem_stanvbk, oem_dycnvbk | 0 | 77064.., 74504.. | 10 MiB each | in `.cache/efs-backup-oem/` |

Type GUIDs are the Qualcomm ones (modemst1 `ebbeadaf-22c9-e33b-8f5d-0e81686a68cb`, ...); the sha256s are in the manifests.
Nothing has written these partitions; rmtfs stays read-only until a write path is reviewed.

## 6. Code map (host phase)

| File | What | Tested |
| --- | --- | --- |
| `drivers/FajitaWifi/WifiMdt.[ch]` | mbn/ELF32 parse, metadata (ELF hdr + hash seg), segment relocation into the carveout | yes, against real mba/modem/wlanmdsp |
| `drivers/FajitaWifi/WifiQmi.[ch]` | QMI header + TLV encode/decode; wlfw requests + response parsers; servreg-locator and rmtfs servers | yes (encode + decode round-trips) |
| `drivers/FajitaWifi/WifiQrtr.[ch]` | QRTR v1 header + control packets; GLINK native command framing | yes |
| `drivers/FajitaWifi/WifiQmp.[ch]` | AOSS QMP link-up, AP side (qcom_aoss qmp_open), no messages | yes, `tests/t_qmp.c` (simulated AOSS); on the phone (`fajita-wifi-qmp=1`) |
| `drivers/FajitaWifi/WifiBoard.[ch]` | ath10k board-2.bin selection by QMI board id | yes, matches qmi-board-id=102 |
| `drivers/FajitaWifi/tests/test_wifi.c` | the host tests | `scripts/check-fajita-wifi-host.py` |
| `package/FajitaWifiBoard.h` | hardware addresses/registers with sources (MSS, GCC, AOSS, SCM, SMEM, GLINK, SMP2P, CE) | — |
| `package/FajitaWifiProbe.cpp` | the staged bring-up (`fajita-wifi=<n>`, default off) | builds into the kernel (host-side logic shared with the tests) |
| `scripts/build-fajita-wifi.py` | `libFajitaWifi.a`, force-loaded like libFajitaPmic.a | — |

The QMI encoder deliberately omits the EI `OPT_FLAG` bytes (qmi_encdec sends only the value TLV for a set optional
field); the host tests check this against hand-built messages.

## 7. Phone test plan (for when the device is handed over)

Build: `FAJITA_OUT=.cache/fajita-out-wifi scripts/fajita/build.sh --test fajita-wifi=1` (then 2, 3, ... one stage at a
time). The Wi-Fi archive is force-loaded but idle without the arg. Run: `scripts/fajita/test/cycle.sh wifi-s1` (queues on
`phone.lock`); the `FW` rows come back in `.cache/fajita-work/logs/wifi-s1/`.

Expected per stage (success / how failure looks):
- **s1** `FW1 vote<ok> clk<mss branch states> rmb pbl<0>`: RPMh votes land, MSS AHB readable; failure = TCS busy/timeout
  (`FW1 ... CLOCKS/VOTE TIMEOUT`) or an unreserved-range refusal.
- **s2** `FW2 fsm1 pbl1 mba1 seg28/28 auth4 ready1`: boot FSM done, PBL success (1), MBA XPU unlocked (1 or 2), all 28
  modem.mbn segments loaded, AUTH_COMPLETE (4), SMP2P modem "ready" set. Failure modes: `fsm0` (boot FSM timeout — clocks
  or reset wrong), `mba<neg>` (XPU not unlocked — SCM assign failed), `auth<neg>` (segment auth failed — mpss load or a
  truncated image), `reserve?` (carveout not reserved; it refuses before booting).
- **s3** `FW3 smem<n parts> glink v1 qrtr servers wlfw,servreg`: the modem announces its QMI servers on QRTR. Failure =
  `glink<timeout>` (no GLINK version ack — mailbox/fifo wrong) or no servers (modem up but PD not started -> usually a
  missing rmtfs/tftp, i.e. do s4).
- **s4** `FW4 pdr<n> tftp<wlanmdsp bytes> rmtfs<iovecs ro>`: the modem fetched wlanmdsp.mbn and read EFS; failure =
  the modem resets (seen as an SMP2P/GLINK BYE) if a service is missing or EFS reads fail.
- **s5** `FW5 ind0 cap chip=... board=102 msa<2 regions> bdf<n segs> cfg0 mode0 ready fw_status=2`: the full wlfw
  handshake, FW_READY indicated. Failure at any step logs the QMI result/error.
- **s6/s7**: CE/WMI ready, then connect. Logged as the ring/WMI/connection counters above.

First session, before enabling rmtfs writes: dump `modemst1`, `modemst2`, `fsg`, `fsc` to `.cache/` (read-only) as a
backup. Keep every run unattended and self-recovering (`--test`, autoreset + watchdog), per AGENTS.md.

## 8. Open items

- Confirm XNU honours a fixed-PA ADT carveout for the modem/rmtfs ranges (reservation route); else pin via IOBuffer at
  stage 1. Stage 2 refuses to boot the modem until the ranges are reserved.
- NativeUfs read path for named partitions (modemst1/2, fsg, fsc) for rmtfs read-only backing.
- The boot-addon change to carry kernel-only firmware entries (modem/mba/board, ~36 MB compressed) touches the shared
  GPU addon path; keep it additive + default-off and clear it with the lead/GPU agent before committing.
- iOS option (a) IO80211 remains research; (b) IOEthernetController is the first target.
