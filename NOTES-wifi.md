# fajita Wi-Fi: WCN3990 on SDM845 under UniXNU (iOS 16.7.16)

## HANDOFF (Wi-Fi agent, Sonnet, 2026-10-25): read this first

Everything below is on `ios16-port` (branch `wifi-sonnet`, worktree `../UniXNU-fajita-wifi2`), behind `fajita-wifi=N` /
`fajita-net=usb` (both default off: the default boot is unchanged; the default build links). The next agent (Opus) starts here.

### Definition of done (user requirement, via the lead)

On the iOS side this must work **fully as a Wi-Fi network device**: visible in Settings > Wi-Fi, scanning and joining
networks through iOS's own stack (wifid, CoreWiFi, the IO80211 family), not a generic `en0`. Two main tasks follow from that:

1. **The iOS interface must be an IO80211-family interface that wifid accepts** (IO80211Family / Skywalk in 20H392). Survey
   below (read-only, from the kernelcache); the dyld-cache/CoreWiFi side is not surveyed yet.
2. **The real `/usr/sbin/wifid` has to run.** Today `fjdbg-agent` stands in for wifid: it is run as the `com.apple.wifid`
   launchd KeepAlive job through the boot-time addon union mount over `/usr/sbin` (`usb-debug=3`, kern_exec.c) and answers
   CoreWiFi's XPC endpoints with empty replies (`package/usb-debug-tools/fjdbg-agent.c`, "launchd runs this agent as the
   com.apple.wifid job"). Move the agent to another launchd job (its own label/plist) so that wifid is the real binary.

The BSD `en0` over USB (below) is the stepping stone that proves iOS's IP stack, not the end state.

### What exists and where it stands

| Piece | State | Code |
|---|---|---|
| Modem (MSS) boot, `fajita-wifi>=2` | **works on the phone** | `drivers/FajitaWifi/WifiMss.[ch]`, `package/FajitaWifiModem.cpp` |
| `fajita_wifi_msa_assign` / `_release` (HLOS <-> MSS_MSA+WLAN) | written, host-tested; **not exercised** (needs wlfw) | `FajitaWifiModem.cpp`, `WifiMss.c` ops |
| Modem services: GLINK, QRTR, pd-mapper, TFTP (wlanmdsp.mbn), rmtfs | **work on the phone** | `WifiGlink`, `WifiQrtr`, `WifiPdmap`, `WifiTftp`, `WifiRmtfs` |
| wlfw handshake (QMI service 0x45) | **blocked: the service never appears** | `WifiWlfw` (host-tested, never reached) |
| WCN3990 CE/HTC | host-tested (earlier rounds) | `WifiCe`, `WifiHtc`, `WifiSnoc` |
| WMI-TLV (init, ready, scan, vdev, peer, keys, mgmt tx) | written, host-tested against Linux struct layouts | `WifiWmi.[ch]`, `tests/t_wmi.c` |
| Station MLME (scan table, auth, assoc, peer assoc, vdev up, 4-way, keys) | written, host-tested with a simulated AP+target | `WifiMlme.[ch]`, `tests/t_mlme.c` |
| HTT data path (setup, in-order rx, native-wifi tx) | written, host-tested | `WifiHtt.[ch]`, `tests/t_htt.c` |
| BSD ifnet `en0` over the USB NET channel | **works on the phone** (DHCP + ping + CFNetwork GET) | `package/FajitaNet.cpp` |
| Kernel glue WMI/MLME/HTT <-> HTC/CE <-> `fajita_net_*` (stage 6/7) | **not written** | see "Next steps" |
| apps_smmu glue for the WLAN SIDs | **not written**; design below | |
| IO80211 interface + real wifid | **not started** (definition of done) | |

Host tests: `python3 scripts/check-fajita-wifi-host.py` (6144 checks, 0 failures at this commit). Kernel compile check:
`python3 scripts/check-fajita-wifi-kernel.py [UNIT]`. ABI constants/layouts are generated from Linux v6.16
(`tools/wifi/gen-ath10k-abi.py /home/john/src/ref/linux`).

### Modem boot (stage 2): design and results

Port of `qcom_q6v5_mss.c` (sdm845, MBA self-auth path) over an ops table (`wmss_ops`), so the host tests drive the exact same
code against a simulated modem that enforces the ordering constraints (`tests/t_mss.c`: failure injected at every op index,
17 behavioural faults, the real 28-segment modem image). Sequence (rows `FWM <n> <step> rc val us`): AOSS QMP `load_state`
-> RPMh ARC votes (mx, cx, mss; levels are indices into the cmd-db level table) -> GCC branch clocks (iface, snoc_axi, bus,
mem/boot_rom, gpll0_mss, mnoc_axi, prng) -> PDC modem sync reset + AOSS CC MSS_RESTART -> QDSP6SS sleep clock + BOOT FSM ->
RMB handshake with the MBA -> metadata authenticated -> 28 segments (125 MB) copied in about 0.5 s -> MPSS authenticated ->
SMP2P ready bit -> late `handover` bit (arrives only after stage-3 services answer). Every failure path returns memory to HLOS
(SCM `assign_mem`), restores clocks and ARC votes as found, drops load_state and halts the AXI ports.

Facts learned the hard way:
- SMP2P items: **428 is the AP's outbound, 435 is the modem's**; "slave-kernel" bits: fatal 0, ready 1, handover 2, stop-ack 3,
  shutdown-ack 7. (Roles were swapped in the first attempt: the modem never found its peer.)
- The metadata buffer must sit inside the MBA carveout (`meta_off` 0x100000): a buffer from XNU's pool gave an intermittent
  SoC reset during the meta-assign step.
- Never survey modem registers after boot (SError panic). The survey runs only before boot or on failure.
- Result row: `FW2 modem up: pbl1 mba2 meta3 seg28 auth4 ready1 handover0 (11649 ms)` then `FWM live 21 handover rc 0 val 6`.
  Steps: assign-mpss/mba ~2 ms each, q6-boot-fsm 75 ms, meta-auth 8 ms, segments 526 ms, mpss-auth 900 ms, ready at 1.64 s.

### GLINK / QRTR / services: results and fixes

GLINK v1 over SMEM, IPCRTR channel opens, QRTR hello, **19 QMI services announced**
(`FW3 ... servers 42,2b,f,33,15,18,17,16,24,31,36,11,4a,1002,22,29,37,e4,32`: no 0x45).
- wlanmdsp.mbn (3035876 B) is served in full over tqftpserv-style TFTP (`FW4 pdr 5 tftp rrq11 wrq3 2964kB err3 ...`; mcfg
  digests and all paths exist; the only ENOENTs are `ota_firewall/ruleset` and the first `mcfg.tmp`). It took three fixes:
  OACK format (no extra NUL after blksize), WRQ echoes the modem's `tsize`, and the GLINK **remote intent table** (16 x 128 B
  slots were full when a large grant arrived: now 64 slots with eviction of a smaller idle intent; `t_glink_full_intents`
  reproduces it).
- rmtfs: `open8 iov7 sect7434 wr-refused0` (reads served from the EFS backup, writes refused).
- Extra channels the modem opens: `glink_ssr` (we ack it; `fajita-wifi-ssr`, default on), `SSM_RTR_MODEM_APPS`,
  `apr_voice_svc`, `apr_apps2`, `LOOPBACK_CTL_MPSS`.

### The open problem: wlfw (0x45) never appears

After handover and the TFTP fetch of wlanmdsp.mbn the modem goes quiet; 150+ s of waiting never shows service 0x45.
`FW5 wlfw idle ind0 cap chip=0 board=0 fw=0 msa0 ... fw_ready0`; no QRTR drops, GLINK `err4` constant, no SMP2P fatal bit.

**Tried (all with the exact rows above, none changed the outcome):**
1. Correct SMP2P roles/bits; ready and handover seen.
2. WCN3990 rails and RF clock voted from the AP (`fajita-wifi-rails=1`, `WIFI_EXTRA_ARGS`): `FWM rails-on ldoa5 800 mV`,
   `ldoa7 1800`, `ldoa17 1304`, `ldoa25 3312`, `ldoa23 3312`, `rfclka2`; read-back `ldoa5@40f00 800mV en1 mode7 ...
   rfclka2@4c400 en1`. (cmd-db shows the rails off at boot; VRM offsets: voltage +0, enable +4, mode +8, HPM = 7. The very
   first attempt wrote enable to +0: a 1 mV garbage vote, harmless, cleared by reboot.)
3. `glink_ssr` open/ack.
4. TFTP/GLINK fixes above (so wlanmdsp.mbn is fully delivered: the PD *is* started and loaded).
5. LFN theory for the mcfg paths: ruled out (plain 8.3 names).
6. Request-history logging (`FW4 tftp-req` history with `E<code>` markers, `FW3d` remote intents, `FW3t` GLINK command trace,
   `FW3c` channel list, `FW3q` QRTR drops).

**Open candidates NOT yet tried (from the lead):**
- (a) pd-mapper answers `tms/pdr_enabled` with **0 domains**. Should it report PDR enabled / the wlan PD domain
  (`msm/modem/wlan_pd`, instance id) so the modem registers the PD with the AP's servreg locator?
- (b) Does the wlan PD wait for a servreg/PDR **indication registration** (qcom_pdr `register_listener`, servreg-locator
  `get_domain_list`/`register_listener`, `pd_state` indications) from the AP before it publishes wlfw?
- (c) **MSA region initial permissions**: `qcom,msa-fixed-perm` or not on this board? The MSA/WLAN region might need to be
  assigned to MSS_MSA+WLAN *before* the PD starts (`fajita_wifi_msa_assign` exists but only runs after wlfw's msa_info).
Other ideas: ack/answer the other GLINK channels (`SSM_RTR_MODEM_APPS`, `apr_*`), SSCTL (service 0x2b is announced: the AP
side `ssctl` shutdown/get-id interplay), check the modem's `qmi 0x1002` / `0x4a` services for a wlan hint, compare the QRTR
service table against a Linux boot of the same board (pmOS on fajita is the reference; only on a failed capture per the mandate).

Test commands: `scripts/fajita/test/wifi-test.sh modem link servers wlfw` (stages), `WIFI_MSS_DRY=1` (read-only survey),
`WIFI_EXTRA_ARGS="fajita-wifi-rails=1"`, boot args `fajita-wifi=N`, `fajita-wifi-ssr`, `fajita-wifi-blk=N` (TFTP block cap),
`kcmd wifimss [rows|survey|stop|poll]`, `kcmd wifi ...`. Rows: `FW2` modem, `FWM` modem steps/live/smp2p/rails, `FW3*`
GLINK/QRTR, `FW4` services, `FW5` wlfw.

### WMI-TLV, MLME and HTT (host-tested only)

All three are freestanding, non-blocking, ops-table driven, and checked against sizeof/offsetof of the Linux structs. The
simulated target in `t_mlme.c` runs the whole connect: scan table from beacons (rssi = snr - 95), RSN check (CCMP/PSK/no
required PMF), vdev start -> peer create -> open-system auth -> assoc (legacy rates + own RSNE) -> peer assoc -> vdev up ->
4-way (WifiWpa) -> PTK+GTK install -> peer authorize. HTT: VERSION_REQ -> FRAG_DESC_BANK_CFG, RX_RING_CFG (64-bit ring, rx
desc offsets of `htt_rx_desc_v2`, 128-entry ring, 63 buffers posted), AGGR_CFG v2; in-order rx indication -> Ethernet II;
tx = native-wifi frame in a per-id buffer + 44-byte txbuf + 72-byte fragment descriptor, two-fragment CE gather without HTC.

### Next steps for the kernel glue (nothing of this is written)

1. Once wlfw gives FW_READY: `whtc_init` on `wce`; connect WMI and HTT services (`whtc_connect(WHTC_SVC_HTT_DATA_MSG)`),
   route CE5 to `h->htt_direct`; `wwmi_rx` on the WMI endpoint; on SERVICE_READY `wwmi_init_cmd` (set `w->num_msdu_desc` equal to
   `whtt_cfg.max_tx`), on READY `wmlme_start`, `wwmi_scan_chan_list` (`wmlme_channels`), then `whtt_start` once HTC is started.
2. HTT `tx_sg` op = `wce_send(..., gather=1)` + `wce_send(..., gather=0)` on the HTT upload pipe with `wce_send_revert` on
   failure; control messages via `whtc_send`; `dma_alloc` from the `wdma` arena; the arena must cover rings, 64 x 2 KB rx
   buffers, tx buffers (max_tx x 2 KB), 44/72-byte per-id areas, and WMI host-memory chunks (size from SERVICE_READY).
3. `whtt_ops.rx` -> EAPOL (ethertype 0x888e) to `wmlme_eapol_rx`, everything else to `fajita_net_input`; `fajita_net_set_backend`
   -> `whtt_tx(vdev, bssid, frame, len, protect)` with `protect` set once the PTK is installed (`wmlme` state CONNECTED/KEYS done);
   `wmlme_ops.eapol_tx` -> `whtt_tx(..., protect=0)`.
4. MLME key completion: on WCN3990 the install-key completion may arrive as the HTT `SEC_IND` (ath10k completes
   `install_key_done` there) instead of the WMI event: `whtt_ops.sec_ind` exists; check that `wmlme` does not wait on the WMI
   event only.
5. SSID from the boot arg `fajita-wifi-ssid=`, PSK read at run time from `/private/var/ngpu/wifi.psk` (credentials file on the
   laptop: `~/.config/unixnu/wifi-kittycorner`, mode 600; never commit or print the PSK; `wifi-test.sh` passes it at run time).
6. Add `WifiWmi.c WifiMlme.c WifiHtt.c` to `STACK` in `scripts/build-fajita-wifi.py` (they are host-only today) and keep the
   default build linking before every push.

### apps_smmu glue (design; not written)

The WLAN subsystem masters are SIDs **0x40 and 0x41** on apps_smmu. Program them with **exact SMRs** (mask 0, ID = SID), in a free
context bank (**banks 5..36 are free**), with the precondition that `(sCR0 & 0x407) == 0x406` (set by the existing survey
`scripts/fajita/test/smmu-survey.py` / the NativeUfs-style SMMU kit `kits/ArmSmmu4K` in the stack worktree). Map **only the
DMA arena** through a 4 KB-granule table (non-cacheable table walks, IOVAs below 4 GiB, S1 bypass off for those two SIDs).
Order: build tables -> context bank (TTBR0, TCR, MAIR, SCTLR.M) -> S2CR (CBNDX) -> SMR (VALID) for 0x40 then 0x41 -> only then
`wlan_enable`/CE bring-up; tear down in reverse and invalidate TLB (`TLBIALL` + `TLBSYNC`). Transactions outside the arena
must fault (check `CB_FSR`) rather than pass.

### The iOS network interface: what is done and what iOS does with it

**Deviation from the original spec:** I did not reconstruct the IONetworkingFamily vtables (IOEthernetController subclass)
from the kernelcache: with XNU source available, a BSD ifnet is a few hundred lines (as `bsd/net/if_fake.c`) and works
today; the IOKit route needs days of reverse engineering of the stripped 20H392 kernelcache and, per the user requirement
above, the real target is an IO80211 interface anyway.

`package/FajitaNet.cpp` (`fajita-net=usb`, with `usb-net=1`): `ifnet_allocate_extended` Ethernet interface `en0` (IFT_ETHER, MAC
02:fa:6a:17:4e:02, MTU 1500), `ether_demux`/`ifnet_input` on its own thread, transmit through `fajita_usbnet_send` (the USB NET
channel of the other agent, bridged to passt on the laptop). `kcmd net` prints counters. Test: `scripts/fajita/test/net-test.sh ios`
(builds `--test usb-net=1 fajita-net=usb`; reuse one `FAJITA_OUT` so the plugin builds incrementally).

iOS ships **no ifconfig, ipconfig or ping**: `package/usb-debug-tools/fjnet.c` (built by `scripts/fajita/build-usbdbg-agent.sh` to
`.cache/fajita-inputs/fjnet`, pushed by net-test.sh) provides `show IF`, `dhcp IF`, `sc [REGEX]` (SCDynamicStore keys, resolved with
dlsym because the SDK marks them unavailable), `http URL` (CFNetwork GET) and `ping IP`.

Phone result (PASS, `net-test.sh ios`): `en0 attached`; `fjnet dhcp en0` -> lease 192.168.0.183 gw 192.168.0.1 through passt;
ping 3/3; CFNetwork GET `http://192.168.0.1:8099/` (the laptop, via passt's gateway mapping) -> HTTP 200, 784 bytes.
Findings that matter for the definition of done:
- configd **sees** a bare BSD en0 (`State:/Network/Interface` lists lo0, en0, utun0; `State:/Network/Interface/en0/IPv4`
  mirrors the address once one is set) but does **not** start DHCP on it: after 12 s up, no inet address, no
  `State:/Network/Service/...` for en0 (only utun0 has one), no `State:/Network/Global/IPv4`, `Global/DNS` empty.
- `http://example.com/` through CFNetwork fails with POSIX error 50 (ENETDOWN): most likely the missing primary service/DNS,
  not tested separately.
- Conclusion: a bare BSD interface is not enough for iOS to manage the link; configd/IPConfiguration need a service (preferences
  or wifid-driven) bound to the interface. That supports the IO80211 requirement.

### IO80211 survey (read-only, kernelcache 20H392, `strings` of `.cache/ipsw/20H392/kernelcache.decompressed`)

What is in the kernelcache (iOS 16.7 internal-SDK build, `DTSDKName iphoneos16.7.internal`):
- Bundles: `com.apple.iokit.IO80211Family`, `com.apple.iokit.IOSkywalkFamily`, `com.apple.iokit.IONetworkingFamily` (1.5.0),
  `com.apple.driver.AppleBCMWLANCore` (+`AppleBCMWLANCoreV3`, `AppleBCMWLANBusInterfacePCIe`), `com.apple.driver.AppleOLYHAL`.
  AppleBCMWLANCore's personalities (`AppleBCMWLAN PCIe`/`SDIO`) match providers `AppleBCMWLANBusInterfacePCIe`/`SDIO` with
  `IOMatchCategory WiFiDriver`, `IOResourceMatch` on a resource, and libraries IO80211Family, IOSkywalkFamily, IONetworkingFamily.
  The Broadcom driver is the template to imitate: its provider is a bus-interface nub, so our driver needs its own nub/provider.
- IO80211Family classes: `IO80211Controller` (its `start` logs "Aborting WiFi init by boot-arg"), `IO80211Interface`,
  `IO80211SkywalkInterface`, `IO80211InfraInterface`, `IO80211VirtualInterface` (+Namer/Role), `IO80211P2P*`, `IO80211NAN*`,
  `IO80211Peer`/`PeerManager`, `IO80211ScanManager`, `IO80211LinkRecovery`, `IO80211LinkQualityMonitor`, `IO80211TrafficMonitor`,
  `IO80211CommandGate`/`CommandQueue`/`WorkQueue`, `IO80211Band`/`Channel*`/`CountryCode`/`SSID`/`BSSID`, `IO80211Buffer*`
  (`IO80211DataStream`, `IO80211NetworkPacket`), `IO80211FlowQueue*`.
- User clients wifid/CoreWiFi talk through: `IO80211APIUserClient`, `IO80211AsyncEventUserClient`, `IO80211IOUCEventPipe`,
  `IONetworkUserClient`, `IONetworkStackUserClient`, plus `AppleBCMWLANUserClient`; entitlements checked by the family:
  `com.apple.private.corewifi`, `.corewifi.internal`, `.corewifi.readonly`, `com.apple.private.corewifi-xpc`.
- Skywalk side: `IOSkywalkNetworkInterface`, `IOSkywalkEthernetInterface`, `IOSkywalkLegacyEthernetInterface`,
  `IOSkywalkNetworkController`, `IOSkywalk{Tx,Rx}{Submission,Completion}Queue`, `IOSkywalkPacketBufferPool`, `IOSkywalkNetworkBSDClient`;
  the Broadcom driver uses `AppleBCMWLANPCIeSkywalk*` queues and `AppleBCMWLANIO80211APSTAInterface` for the AP/STA interface.
  `IOUserEthernetController`/`IOUserEthernetInterface` (DriverKit) also exist.
- The command surface is the **APPLE80211_IOC_*** table: 418 names in the kernelcache (337 outside AWDL), e.g. SSID, BSSID,
  CHANNEL, SCAN_REQ/SCAN_REQ_MULTIPLE/SCAN_RESULT/ABORT_SCAN, ASSOCIATE/ASSOCIATE_RESULT/DISASSOCIATE/REASSOCIATE,
  AUTH_TYPE, CIPHER_KEY, RSN_IE/RSN_CONF, CUR_PMK/PMK_CACHE, CURRENT_NETWORK, STATE, RSSI, NOISE, RATE/TX_RATE/RX_RATE,
  OP_MODE, PHY_MODE, POWER/POWERSAVE, COUNTRY_CODE, SUPPORTED_CHANNELS/CHANNELS_INFO, HW_ADDR, CARD_CAPABILITIES,
  DRIVER_VERSION, HARDWARE_VERSION, LINK_CHANGED_EVENT_DATA, BSSID blacklist, ROAM*, BGSCAN*, VIRTUAL_IF_CREATE/DELETE,
  COMPANION_SKYWALK_IF_CREATE. wifid's `IO80211InterfaceWifidEnumeratedInfra` / `IO80211InterfaceWiFiPrimary` properties mark
  the primary infrastructure interface. The classic transport is the `SIOCSA80211`/`SIOCGA80211` interface ioctl pair plus
  the async event user client (not verified on 20H392: confirm from IO80211Family's `performCommand`/`apple80211Request`).
- Not surveyed yet: the dyld-cache side (CoreWiFi, wifid's own IOKit calls, `IO80211Interface` registry properties it requires),
  and the exact vtable layouts (needed for a subclass the way NativeHidInput reconstructs one: see
  `scripts/build-native-legacy-display.py` for the capstone-based, instruction-qualified address method and
  `OriginalHidEventLayout.h` for what a generated layout looks like).

### Main tasks for the next agent

1. **wlfw**: work the open candidates (a)(b)(c) above; this blocks every on-air step.
2. **IO80211 interface that wifid accepts** (definition of done 1): survey CoreWiFi/wifid in the dyld cache and the family's
   user-client protocol, then write a native driver (IO80211Controller + an IO80211 interface, backend = the HTT path or, first,
   the USB NET channel) the way NativeHidInput reconstructs IOKit vtables; add IO80211Family/IOSkywalkFamily/IONetworkingFamily to
   the prelink roots as needed.
3. **Real wifid** (definition of done 2): move `fjdbg-agent` off the `com.apple.wifid` job/union mount to its own launchd job.
4. The kernel glue and SMMU glue above, then `wifi-test.sh` stages beyond `wlfw` (scan, assoc, dhcp, ping).

### Deviations and housekeeping

- ifnet `en0` instead of an IOEthernetController (see above).
- Process notes: merge `origin/ios16-port` before each build/phone run; explicit `git add`; a full `scripts/fajita/build.sh --test`
  must link before pushing kernel-side changes; the phone is shared (global lock in cycle.sh; end sessions by name,
  `session.sh end NAME`; `to-fastboot.sh` if left in iOS); long jobs via `scripts/fajita/test/bg.sh`.

## Current state (keep this updated; a restart starts here)

### Band steering: why connect #1 to the 2.4 GHz BSS failed (branch `wifisae`, 2026-10-26)
Symptom: on many boots `FAIL assoc`, every time against KittyCorner's 2.4 GHz BSS `fa:d0:0e:a8:9f:89` (2412 MHz, -44 dBm):
four SAE commits, no answer (`sae c4/f0`). Boots that passed had joined the 5 GHz BSS `fa:d0:0e:af:9f:8a` (5220, later 5785 MHz).
**Cause: the AP band-steers this phone off 2.4 GHz.** Evidence:
- **History** (every session console under `.cache/fajita-work/logs`, first connect of each boot): 7/7 first connects
  on 2412 MHz joined within ~60 ms up to session 001344 (2026-10-26 00:13), **the phone's first 5 GHz association ever**.
  From 00:32 on, ~50 of 51 first connects on 2412 MHz failed, while every first connect on 5 GHz joined, and a later try
  on 2412 in the same boot (about 15 s on) usually joined (one boot, 003802, was let in on the second commit). The AP
  learned the phone is dual band and now holds it off 2.4 GHz until it insists.
- **The commits arrive and are ignored** (new FW7 link fields): WMI mgmt TX completion status 0 (acked) for all four
  commits (`mtx 4 ok4 noack0`), and the AP sends us nothing (`ap auth0 deauth0 disas0 act0`): no reject status, no SA
  Query, no deauth. hostapd itself would answer even an invalid commit with a status, so this is a drop below its SAE.
- **Not H2E**: both BSSes have the same RSN element (caps 00cc, MFPC+MFPR), **no RSNXE** (`rsnxe 0/00`) and no BSS
  membership selector 123 (`sel123 0`); the same hunting-and-pecking commits are accepted on 2.4 GHz on a later try and
  before 00:13. No H2E was implemented: nothing asks for it.
- **Ruled out on the phone** (wifi-test, forced 2.4 GHz with `fajita-wifi-band=2g`): the translating SMMU bank (run B,
  xlate: fails; run C, `fajita-wifi-smmu=bypass`: fails the same way; the xlate/bypass boundary in the history was a
  coincidence with 00:13), the management sequence number restarting at 0 each boot (run D, `fajita-wifi-mgmt-seq=random`:
  fails the same way), TX not leaving the chip (acked), wrong channel or rate (acked by the AP).
**Fix (2b870dd):** choose the BSS the way a supplicant does. `fajita-wifi-band=auto` (default): a 5 GHz BSS at >= -70 dBm
(`WMLME_5G_GOOD_DBM`) beats any 2.4 GHz one; `strongest` (the old rule), `2g`, `5g`. After a failed authentication the
retry takes the next best BSS (`wmlme_find_other`). The kernel rescans once when scan #1 saw the SSID only on 2.4 GHz (the
5 GHz beacon/probe response was missed: -67 dBm, 50 ms active dwell), and rescans (bounded, 8 scans) when a scan did not
see the SSID at all (the connect loop used to stall there: session 162319). Host test `t_mlme_band` (the field case).
Rows: `FW7 connect #N ...: BSSID MHz dBm (bss 2g N 5g N) rsn tail .. rsnxe len/octet sel123 N rates N seq N`; FW7 scan:
`bcn N prsp N` per BSS and `(bss 2g N 5g N)`; FW7 link: `mtx` (completions by status) and `ap ...` (frames the AP sent
us by subtype, action category, deauth reason).
**2.4 GHz is avoided, not fixed**: there was nothing to fix in our SAE (no H2E needed, commits accepted on a later try);
the AP withholds 2.4 GHz from this phone, and `auto` goes to 5 GHz first. When 2.4 GHz is the only band, the join
still depends on the retry (connect #2 after a rescan), which band steering usually lets through.
Proof: `scripts/fajita/test/wifi-soak.sh 8` on e0b3f89, default args (stopped after boot 7 at the lead's request, the
phone went to graphics). Full `build.sh --test` from e0b3f89: rc 0.

| boot | result | joined (first try every time) | session |
| --- | --- | --- | --- |
| 1 | PASS | fa:d0:0e:af:9f:8a 5785 MHz -67 dBm | 174121 |
| 2 | PASS | 5785 MHz -70 dBm | 174214 |
| 3 | FAIL dhcp, ping (harness) | 5785 MHz -67 dBm (scan #1 saw 2.4 GHz only; the rescan found 5 GHz) | 175131 |
| 4 | PASS | 5785 MHz -68 dBm | 180637 |
| 5 | PASS | 5785 MHz -67 dBm | 180740 |
| 6 | PASS | 5785 MHz -65 dBm | 180908 |
| 7 | PASS | 5785 MHz -64 dBm | 181016 |

Open items:
- **Boot 3: associated, then no DHCP lease reported.** Its log: assoc PASS at 56 s, then `push fjnet failed` (the hub
  push of the fjnet tool did not finish in 60 s), so `fjnet dhcp` and `fjnet ping` timed out on the hub (`run: timeout`).
  The link itself carried traffic (data rx 2040 / tx 1427, IPv6 TCP to the internet established) and the same boot's
  CFNetwork probe fetched http://example.com/ (http 200) over Wi-Fi. So it looks like a USB debug channel / harness
  failure, not Wi-Fi, but that is not proven: why the push stalled is not known (the session had waited 13 min behind
  a graphics session before booting).
- 5785 MHz is often near -67..-70 dBm here, right at `WMLME_5G_GOOD_DBM`; below it `auto` picks 2.4 GHz and joins on
  the retry. A lower threshold (-75) would keep 5 GHz; not tried.
- DFS 5 GHz channels (52-144) are not scanned, so an AP that moves there is seen on 2.4 GHz only.

**Opus round (branch `wifi-opus`, worktree `../UniXNU-fajita-wifi3`), 2026-10-25.**
- **Handover point (2026-10-26):** all work is on ios16-port. Opus stops here; the phone goes to the next agent. Open for done:
  (1) wifid seeing the interface (status icon, Settings > Wi-Fi, join from Settings) - the stopped IO80211.framework survey
  below; (2) Safari loads a page (after 1); (3) credentials from wifid/keychain instead of wifi.psk (after 1). The SMMU
  translating bank is done and default. Best test: `scripts/fajita/test/wifi-test.sh modem link servers wlfw scan assoc
  dhcp ping` (all PASS); add `WIFI_EXTRA_ARGS="fajita-wifi-iokit=1 fajita-wifid=1"` for iOS's own stack and the real wifid.
- **wlfw solved (on the phone).** The wlan PD was up all along: the modem announces 32 QMI services before wlfw, and
  the router's remote-server table had 32 slots, so wlfw's NEW_SERVER (the 33rd) was dropped without an event (77dd018).
  Candidates (a)(b)(c) were not needed. Table now 128, overflow still delivers the event. Result:
  `FW5 wlfw enabled ind2 cap chip=30214 board=0 fw=20028032 msa2 assigned bdf bdwlan.bin 19152/19152 seg4 fw_ready1`
  (msa_assign worked, BDF downloaded, FW_READY, wlan_cfg + wlan_mode). 46 services in all.
- **Fixed-size table audit** (f74ac6c): every overflow counted and shown (GLINK err sites in FW3 `err N[site:count]`,
  QRTR drop/flowfull/txqfull, SMP2P over, pd-mapper more/cut, TFTP busy, rmtfs full, MLME scan table 64 with
  weakest-non-target eviction, msa_info > 2 regions refused).
- **Stage 6, the device (fajita-wifi=6, on the phone):** `drivers/FajitaWifi/WifiDev.[ch]` (host-tested, `t_dev.c`)
  + `package/FajitaWifiDev.cpp`. DMA arena = `FW_WLAN_DMA_PHYS` 0xf6000000 + 8 MiB, held with phys-reserve (in
  wifi-test's PHYS_RESERVE), mapped **write-combine (Normal NC)**: a Device (InhibitCache) mapping panicked in bzero
  (unaligned data abort). IOVA = PA. apps_smmu: SMR id 0x40 mask 1 -> a free bypass bank (the NativeDwc3 recipe;
  phone: slot 8, bank 5). Deviation from the handoff's SMMU design: bypass, not a translating bank (proven recipe;
  a translating bank limited to the arena is a later hardening).
  First run: HTC READY, HTT+WMI connected, SERVICE_READY, INIT, READY, HTT setup, regdomain, channels, vdev in 36 ms;
  **first scan saw 29 BSSes including KittyCorner (-60 dBm)**. Then: the MGMT_RX channel field is a channel number
  (fixed: -> MHz), READY's MAC is zero (fixed: LAA 02:fa:6a:17:4e:03), KittyCorner's RSN parsed as not usable
  (`kcmd wifidev bss` now dumps the RSN element), and **the modem raised its SMP2P fatal bit ~1 s into the scan**
  (rows froze; `FW3 MODEM FATAL ... crash421 '...'` now logs the reason). Under investigation.
- Second scan (LAA MAC): 47 BSSes, scan completes, **no modem fatal** (the zero MAC was the likely cause), KittyCorner
  fa:d0:0e:a8:9f:89 2412 MHz -44 dBm and fa:d0:0e:af:9f:8a 5220 MHz -61 dBm.
- **KittyCorner is WPA3-Personal only**: RSNE `30 14 01 00 | 00-0f-ac:4 | 1x 00-0f-ac:4 | 1x 00-0f-ac:8 (SAE) | caps 00cc`
  (MFPC + MFPR) on both BSSIDs, no RSNXE (hunting-and-pecking, no H2E). Not TKIP/mixed.
- **WPA3 implemented (host-tested):** `WifiSae.[ch]` (SAE group 19 after hostap sae.c/dragonfly.c: >= 40 iterations,
  stub password, constant-time selection, blinded QR; peer validation; Montgomery P-256; SHA-256/HMAC/KDF/CMAC); IEEE
  802.11-2020 J.10 vector passes (commit, KCK, PMK, PMKID). Supplicant AKM 8 (key descriptor v0, KDF-SHA-256 PTK, CMAC
  MIC) matches an independent Python reference byte for byte (gen-wpa-vectors.py). MLME: SAE commit/confirm with the
  anti-clogging token, RSNE AKM 8 + MFPC|MFPR, PMF peer flag, PDEV PMF_QOS; end-to-end against the simulated AP
  (t_mlme_sae). Kernel: fajita-wifi=7 joins fajita-wifi-ssid with `/private/var/ngpu/wifi.psk` (pushed by wifi-test,
  read in the kernel, never logged); FW7 link row (row 11); `kcmd wifidev connect`.
- MAC: the phone's own, from persist:/wlan_mac.bin (only Intf2 filled), via `tools/wifi/persist-wlan-mac.py` on the EFS
  backup -> `fajita-wifi-mac=`; LAA 02:fa:6a:17:4e:03 fallback.
- **Done: apps_smmu translating bank (default since 2026-10-26).** `drivers/FajitaWifi/WifiSmmu.[ch]` (host-tested, t_smmu.c:
  a software walk maps the arena, faults its neighbours, other RAM and the tables) builds a stage-1 AArch64 context: 4 KiB
  granule, T0SZ 32, L1 + L2 with four 2 MiB block entries over the 8 MiB arena, IOVA = PA, Normal NC (MAIR 0x44), AF/RW/OSH;
  TCR 0x800020, TCR2 0x38002 (40-bit PA, SEP upstream), SCTLR 0x27 (M, TRE, AFE, CFRE; no context interrupt), CBAR S1+S2-bypass,
  CBA2R VA64, ASID 0x40. The kernel (FajitaWifiDev.cpp smmuTranslateWlan) keeps the tables in their own contiguous buffer
  (outside the arena, cleaned to PoC), TLBIALL+SYNC, SCTLR.M last, then two exact SMRs 0x40 and 0x41 -> the bank (phone:
  bank 5, slots 8/9). CB_FSR faults are logged as `FW6 SMMU FAULT fsr .. far ..`; wifi-test fails a run on one. Phone proof:
  modem..ping all PASS, no faults. `fajita-wifi-smmu=bypass` restores the old bypass bank.
- **TODO before done (lead): credentials from iOS, not a file.** `/private/var/ngpu/wifi.psk` is the passphrase in plain
  text on the Data volume (bring-up only). Once wifid drives the join, the passphrase must come through iOS's own path
  (wifid / keychain via the IO80211 associate request) and the file and its push in wifi-test.sh must go.
- **Data on the air (2026-10-25):** fajita-net=wifi (en0 over HTT, the station MAC): DHCP 192.168.0.84 gw 192.168.0.1,
  ping 3/3 ~9 ms, CFNetwork GET http://192.168.0.1/ 200. The data path was right from the start (TX completions all ok,
  the AP re-broadcast our encrypted DISCOVER); fjnet's DHCP sent from 169.254.0.1 to 169.254.255.255 (passt answered,
  a real server does not); it now uses BPF raw frames. Name lookups fail (ENETDOWN): configd does not manage a bare en0.
  Full suite PASS: `wifi-test.sh modem link servers wlfw scan assoc dhcp ping`.
- **The real wifid (fajita-wifid=1, on the phone):** iOS 16 launchd runs jobs from its signed cache
  (/System/Library/xpc/launchd.plist, 532 daemons; LaunchDaemons overlay plists never load) and com.apple.wifid is the only
  root RunAtLoad+KeepAlive job in /usr/sbin (its `_LimitLoadToDeviceTree` is wlan/marconi-wifi). So the agent still starts
  as that job and splits: a child (setsid, flock on /private/var/ngpu/fjdbg/agent.lock) keeps the debug channel, the job's
  process execs Apple's wifid from /private/var/ngpu/fjdbg/wifid.real (build.sh puts the 20H392 binary from
  .cache/mnt/os20H392/root into the boot addon with scripts/fajita/addon-put.py; trusted by cdhash). Phone: pid 42 = real
  wifid (7.3 MB RSS, ppid 1), pid 87 = agent (ppid 42, own session), channel works, WiFiCloudAssetsXPCService and
  wifianalyticsd start, lock screen normal. Default boots unchanged.
- **iOS's own network stack over the radio (fajita-wifi-iokit=1, on the phone, 2026-10-26):** `package/FajitaWifiIOKit.cpp`
  publishes our own minimal classes with the names configd tests for (IONetworkingFamily/IO80211Family are not in this
  kernel; no Apple layout): `IONetworkStack` (+ `IONetworkStackUserClient` accepting IOConnectSetCFProperties naming
  commands) at FajitaNet start, then `IO80211Controller` (IOMACAddress) with `IO80211Interface` -> `IOEthernetInterface` ->
  `IONetworkInterface` (BSD Name en0, IOInterfaceType 6, prefix/unit, IOBuiltin, IOPrimaryInterface) once en0 attaches.
  Result: NetworkInterfaces.plist `('en0', 'IEEE80211')`, a network service for en0, en0 UP with **192.168.0.84 from iOS's
  own IPConfiguration** (no fjnet), `State:/Network/Global/IPv4` PrimaryInterface en0 / Router 192.168.0.1, IPv6, DNS from
  DHCP, **CFNetwork GET http://example.com/ -> 200 by name**, and the lock screen clock set from the network. Without the
  IONetworkStack object InterfaceNamer never names anything ("No network stack object"). Not yet: the Wi-Fi status icon /
  Settings (wifid does not see the interface).
- **STOPPED (safety classifier, 2026-10-26), for another agent: the wifid / IO80211.framework survey.** My response was cut
  off while writing a read-only dyld-shared-cache reader to look at IO80211.framework (Apple80211*) and how it reaches the
  kernel. Per the rules I stopped that piece and did not retry it. Established before the cutoff:
  - wifid links /System/Library/PrivateFrameworks/IO80211.framework (the Apple80211* API), weak CoreWiFi, WiFiPolicy,
    SystemConfiguration, IOKit; its strings name IO80211SkywalkInterface (IOServiceAddMatchingNotification),
    IO80211APIUserClient (also in its iokit-user-client-class entitlement list), IO80211InterfaceRole,
    IO80211InterfaceWifidEnumeratedInfra, "Could not obtain IO80211Interface bsd name".
  - The open question for the next agent: does IO80211.framework reach the driver through the IO80211APIUserClient, through
    the BSD ioctl pair SIOCGA80211/SIOCSA80211 on the interface (which FajitaNet's ifnet could answer), or both, and with
    which request layout. Then publish what wifid matches (IO80211SkywalkInterface or the legacy interface class) from
    FajitaWifiIOKit.cpp and answer the requests from the station (FajitaWifiDev.cpp: scan table, connect, state).
  - Practical notes: the dyld cache is copied locally to .cache/dsc-20H392/ (3.2 GB; through the DMG FUSE mount at
    .cache/mnt/sysos20H392 random reads crawl). `ipsw dyld macho/info --dylibs` (3.1.680) hangs at the image table on this
    cache (0% CPU, not network), so it needs another tool. The 20H392 root is at .cache/mnt/os20H392/root (wifid,
    configd, the SystemConfiguration bundles); Apple's open-source configd-1241.140.6 is in /home/john/src/ref/configd.
- wifid's IOKit side (strings): Apple80211.framework (Apple80211Open/BindToInterfaceWithParams/ScanAsync/AssociateAsync/
  EventMonitoringInit2 ...), IOServiceAddMatchingNotification on **IO80211SkywalkInterface**, the **IO80211APIUserClient**,
  `_SCNetworkInterfaceCreateWithIONetworkInterfaceObject`. Next: what Apple80211 sends through the user client (dyld cache
  at .cache/mnt/sysos20H392/root/System/Library/Caches/com.apple.dyld/).
- kcmd `wifidev [state|scan [SSID]|connect|bss]`; rows FW6 (row 8, device), FW7 scan (row 9), FW3 MODEM FATAL (row 10),
  FW7 link (row 11).
- Test: `scripts/fajita/test/wifi-test.sh modem link servers wlfw scan` (via bg.sh; reuse FAJITA_OUT).

Round 8 (lead): a USB network transport, so a future iOS network interface can be tested against this laptop. The
interface itself, IONetworkingFamily, modem boot, SCM, memory assignment, the SMMU and WMI stay on hold.

**USB network transport (done 2026-10-25, on the phone, PASS twice).** fjdbg channel 7 `NET`: one Ethernet frame
(14..1536 bytes, no FCS) per fjdbg frame, both directions, behind `usb-net=1` (default off: NET frames from the host are
counted and ignored, `fajita_usbnet_send` returns -1). The other channels and the hub ops are unchanged.
- Kernel API (drivers/NativeDwc3/NativeDwc3.cpp; callers declare them weak): `int fajita_usbnet_send(const void *frame,
  size_t len)` queues on the gadget's TX ring from any thread (not interrupt context), never blocks; 0 queued, -1 off or
  no USB configuration, -2 bad length, -3 ring full (dropped, counted); NET never takes the ring's last 4 KiB, so command
  replies and SHELL output keep room. `int fajita_usbnet_register_rx(fn, ctx)`, `fn(ctx, frame, len)` runs on the USB poll
  thread for every frame the host sends: the frame is valid only for the call and fn must not block (copy it out); fn
  may call fajita_usbnet_send; fn = nullptr unregisters. `int fajita_usbnet_link_up(void)`.
- Core and test hook: `drivers/NativeDwc3/UsbNet.h` (freestanding, shared with the host simulator): echo mode, the
  dispatch (echo, then the probe's replies, then the registered receiver, else dropped), and a probe that is a tiny IPv4
  client: DHCP DISCOVER -> OFFER -> REQUEST -> ACK, ARP for the router, one ICMP echo (1 s retry, 4 tries per step, then
  `FAILED: <step>`). `kcmd usbnet [state] | echo on|off | dhcp [A.B.C.D] | show`; its rows (`usbnet: ...`) go to the
  kernel log. Probe MAC 02:fa:6a:17:4e:01 (locally administered).
- Host: `scripts/fajita-usbdbg-hub.py` op `net`: after the JSON reply line the hub connection carries frames in qemu
  stream format (u32 BE length + frame, what passt speaks); one NET client at a time; a slow client loses frames rather
  than stalling the hub's reader (queue of 1024); `call net-stats` prints the counters.
  `scripts/fajita/test/usbnet-host.py bridge [--pcap F] [-v]` starts passt (`-f -1 -s SOCK --mtu 1500`, no root: DHCP,
  DNS forwarding, NAT through the laptop's sockets) and relays both ways; `usbnet-host.py echo --count N` is the
  transport test (frames of 14..1536 bytes, every byte value including the fjdbg escape bytes, checked intact and in
  order). passt's default MTU is 65520: the bridge asks for 1500 and drops anything larger than 1536 from passt.
- Tests: `drivers/NativeDwc3/tests/UsbNet.cpp` (in tests/run.sh): builders and checksums (RFC 1071 example), parsers on
  good and malformed replies, dispatch, the probe against a scripted server including retries, NAK and silence.
  `scripts/fajita/test/net-test.sh sim` runs the same core in UsbDbgSim behind a private hub bridged to a real passt (no
  phone); `net-test.sh usbnet` builds `--test usb-net=1` (into .cache/fajita-out-net), holds one session, and runs the
  echo test, then the bridge and `kcmd usbnet dhcp`, checking the probe's PASS row in the kernel log.
- On the phone (two boots, both PASS): `kcmd usbnet` `link=1`; echo 300 frames (235772 bytes) in 0.25 s and 2000
  frames (1.5 MB) in 2.37 s, all intact and in order (about 0.6-0.9 MiB/s each way with 8 frames in flight; the 1 ms
  poll period bounds it); then the kernel's DISCOVER reached passt: **`usbnet: DHCP OFFER 192.168.0.183 from 192.168.0.1
  ... router 192.168.0.1 dns 192.168.0.1 mtu 1500, 1-2 ms`**, ACK, ARP for 192.168.0.1, ICMP echo reply rtt 1-2 ms,
  `usbnet: probe PASS: DHCP, ARP and ping over the NET channel in 5-6 ms`. Hub counters: no frame dropped.
- For the iOS interface (next, on hold): register an RX callback that copies into an mbuf and hands it to the
  interface's input path from a thread of its own (not the poll thread); transmit with fajita_usbnet_send and treat -3
  as an output drop. Address config can come from passt's DHCP (the lease is infinite; MTU 1500).

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
every protocol object). For the modem boot: `fajita_wifi_smp2p_bits(&v)` (inbound bits, ready = bit 1; the earlier constants were off by one) and
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

## 9. Safari: WebKit XPC services crashed at launch, the hang, the black page (all fixed 2026-10-26); the App cryptex mounted

Symptom: Safari opens; tapping a favorite gives a blank page, and every `com.apple.WebKit.Networking` / `WebContent` process dies
at once: `CRASH ... sig11 ... E92000005 F8` (NULL + 8 read in CoreFoundation, called from WebKit +0x1e6dac).

Cause (not a sandbox, entitlement or environment problem): the service entry point does
`CFBundleGetFunctionPointerForName(CFBundleGetBundleWithIdentifier(CFSTR("com.apple.WebKit")), CFSTR("WebContentServiceInitializer"))`
(and the Networking/GPU twins). `CFBundleGetBundleWithIdentifier` returned NULL because CFBundle never read WebKit.framework's
Info.plist: `readdir` of `/System/Library/Frameworks/WebKit.framework` (and JavaScriptCore.framework) failed with EINVAL.
SystemOS (the cryptex, `disk2s1`) is mounted as a union over the OS volume. When the upper directory is exhausted, getdirentries
traverses to the lower directory (`lookup_traverse_union`), and the OS volume has a non-directory at that path, so the traversal
handed a non-directory to `getdirentries64` (EINVAL; getattrlistbulk: ENOTDIR). Directories that exist only in one layer read fine, which is
why UIKit, CFNetwork and Foundation worked.

Fix: `ios16/patches/xnu-8796/zzx-union-readdir-lower-nondir.patch` makes `lookup_traverse_union` return ENOENT ("no lower directory",
the ordinary end of a union directory) when the lower object is not a directory. One place, so getdirentries, getdirentriesattr and
getattrlistbulk all end at the upper entries. (Keep it out of vfs_attrlist.c/vfs_syscalls.c: build-xnu-4k-baseline.sh re-applies
patches idempotently by reverse-checking, and editing lines of native-union-getattrlistbulk.patch breaks that check.)

Tools: `package/usb-debug-tools/fjbundle.c` (built by `scripts/fajita/build-usbdbg-agent.sh`, push with `session.sh push`; push a changed
build to a NEW name on the phone, an overwritten path ran the old binary):
`session.sh run /usr/sbin/fjbundle /System/Library/Frameworks/WebKit.framework/WebKit com.apple.WebKit WebContentServiceInitializer`
(prints `CFBundleGetBundleWithIdentifier = 0x0` when broken), `fjbundle --ls DIR...`, `fjbundle --stat PATH...` (open, getdirentries64 results,
filesystem). `tools/crash/dsc-disasm.py IMAGE OFFSET [N] [--back M]` disassembles a shared-cache image at an offset from a CRASH row
(`-` as the image takes an absolute unslid address; `--data` dumps words); `tools/crash/dsc-addr.py` maps the row's addresses first.

After the fix there are no WebKit CRASH rows; the Networking, WebContent and GPU services stay up, idle in their run loops.

Second blocker, the Safari hang (fixed 2026-10-26): after tapping a favorite MobileSafari's main thread blocked (spindump:
`dispatch_sync` onto `com.apple.SafariShared.WBSHistoryServiceStore`, whose block waits for an XPC reply). The reply comes from the
`com.apple.Safari.History` job (`/System/Library/LaunchDaemons/com.apple.Safari.History.plist`, Mach service
`com.apple.Safari.History.Service`) and that job's Program is `/System/Cryptexes/App/usr/libexec/com.apple.Safari.History`.
`/System/Cryptexes/App` and `/System/Cryptexes/OS` on the OS volume are symlinks to `/private/preboot/Cryptexes/{App,OS}`, and
`/private/preboot` is empty here: the board mounts OS and SystemOS (SystemOS as the union at `/`) but not the 10 MiB `Cryptex1,AppOS`
image (`087-86715-047.dmg`: Safari.History, webbookmarksd, passwordbreachd, webinspectord, safarifetcherd, webpushd,
AuthenticationServicesAgent, SafariBookmarksSyncAgent). launchd cannot start those jobs, so lookups of their Mach services never return.
`com.apple.fairplayd.H2` (the iPhone X label; the list had the iPad's `.A2`) did the same to an AccountStore thread.
First fix (c643ac2): `package/BootWatchdogPolicy.c` put the nine Cryptex-program jobs and `fairplayd.H2` on the `sheng-lite` disable
list, so their lookups failed at once instead of hanging.

Proper fix (2026-10-26, the App cryptex mounted): AppOS is the third root window of userdata, after SystemOS (`ios16/board.toml`
`[userdata] windows`; the OS and SystemOS offsets do not move), with its own owner-signed policy built and block-verified like the other two
(`build-fajita-verified-root.py --image appos:087-86715-047.dmg:9E10DDA7-90D9-4677-844A-E984BB1CCD0A:...:2560`, stack a43c5be
`build-verified-root-probe.py --extra-policy appos`). NativeUfs needed no change (up to four windows; policies match by window size). It
publishes SystemOS, AppOS, OS in that order (`NativeUfs: AppOS volume 9E10DDA7-... ready`). `package/BootCryptexMount.c` (`fajita-appos=1`,
on by default in board_args; called from the watchdog policy step after the Data mount, before launchd): the mount points live on Data
(`/private/var/db/fajita-preboot/Cryptexes/App`), a read-only bindfs view of them is mounted over `/private/preboot` (the sealed OS volume has
an empty directory there and no Preboot volume exists), then the AppOS APFS volume read-only at `/private/preboot/Cryptexes/App`, with the
SystemOS mount's payload; it checks read-only, apfs, the selected device, authorization, and a program reachable through
`/System/Cryptexes/App`. Never panics: the kernel log line `fajita appos: mode 1 stage 8 status 0 device /dev/disk5s1` and BootLog row `C`
report it (stage 8 = mounted and checked; stage 5 status 2 = no AppOS volume, e.g. a two-window userdata). The App jobs are disabled only
when the mount did not reach stage 8, so a kernel without the cryptex still never hangs Safari. `fajita-appos=2` also links
`Cryptexes/OS` to `/` (SystemOS is the union at `/`), for adattributiond's `/System/Cryptexes/OS/...` program.

Userdata: `scripts/fajita/userdata-windows.py` writes only the manifest block and the AppOS window (a 10 MiB sparse image;
`fastboot flash userdata .cache/fajita-out/userdata-windows/userdata-windows.simg`, under phone.lock). Its two-window manifest
(`--windows OS,SystemOS`) is byte-identical to the one read off the phone (`ufspart.py read userdata 4194304 1 --to F`, sha256
`7fcc6240...`); the three-window one is `7e82317e...`. The phone has the three-window image since 2026-10-26 07:17. **A kernel without the AppOS
policy (any build before this change) finds no policy for the AppOS window and publishes no root**: merge ios16-port first, or flash the
two-window manifest back (`userdata-windows.py --windows OS,SystemOS`, 4 KiB).

Result (sessions 071820, 072707, 072954, Wi-Fi up): `scripts/fajita/test/cryptex-jobs.sh --ping` (looks up each job's first Mach service,
sends it a message, watches the process table) shows Safari.History, webbookmarksd, webinspectord, safarifetcherd, passwordbreachd,
SafariBookmarksSyncAgent and AuthenticationServicesAgent run from the cryptex (webinspectord exits again when idle). MobileSafari's main thread
idles in its run loop (spindump) and apple.com loads and draws, product images included (`.cache/appos/safari-apple3.png`). Not solved:
**webpushd** (App) and **adattributiond** (OS, tried with `fajita-appos=2`): their services are found (look_up 0) but a message gets no answer
and launchd never spawns a process (20 ms sampling, no CRASH row); the binary runs when started by hand. Ruled out by an overlay of their
plists (the GPU addon's `daemons/` union over /System/Library/LaunchDaemons): webpushd without `StandardErrorPath`, adattributiond with an
explicit `Program`. Both stay disabled (a never-answered lookup is what hung Safari), as does `fairplayd.H2`. Next step there: launchd's own
view of the job (an xpc_pipe `print` like launchctl's), which this board has no tool for yet.

Third blocker, the black page (fixed 2026-10-26): the page loads (padlock, title "Apple" / "Example Domain", find-on-page counts its text)
but the content area is black. Earlier findings that still hold: not the GPU process as such, not the plugin's CPU-held textures
(`NGPU_CPU_HELD=0`), not the sandbox; and not the kernel's import maintenance either (`kcmd gpu set ro-skip 0`, `wpub-skip 0`,
`uncached-skip 0` live: still black). The `ngpu: import at iova ... unmapped (page now 0)` lines are not it: every session logs its
cap of eight, Safari or not (an import's pages are held by its own memory entry, so the owner's unmapped address is harmless there).

Cause: WebKit draws its tiles into deep-colour backing stores because this board's display is P3 (`artwork-display-gamut` P3 in the
device tree): opaque tiles are RGB10 IOSurfaces (`w30r`, 1125x1536 and 1125x1164), and CoreGraphics renders RGB10 IOSurface contexts
through Metal (CGIOSurfaceContextCreate there loads QuartzCore, IOSurfaceAccelerator and Metal, and Metal loads the GPU plugin). The plugin
gives a Metal device only to the render server and to the processes named in `addons/NativeAdrenoMetal.clients` (NativeAdrenoMetalPlugin.mm
`listedClient`), which held only `ngpu-metal`. With no device, CoreGraphics' drawing silently writes nothing: every tile stays zero (the
render server imports them fine and draws them, as black). Who draws depends on Safari's preferences: by default the WebKit GPU process
(`com.apple.WebKit.GPU`; the WebContent processes then never load Metal), with the GPU-process preferences off WebContent itself; neither
was listed, which is why turning the GPU process off changed nothing. With `WebKitAcceleratedDrawingEnabled=0` (bitmap backing stores
in shared memory, drawn by the CPU) pages drew, which pointed at the IOSurface path.

Fix: `scripts/fajita/build.sh` writes the client list into the boot add-on: `ngpu-metal com.apple.WebKit.GPU com.apple.WebKit.WebContent`
(`FAJITA_GPU_CLIENTS` replaces it; the build prints `gpu clients:`). A listed process takes one of the three 170 MiB client windows
(NativeAdrenoPublish.cpp `kClientWindows`) while its device lives: with Safari's defaults the GPU process takes one, an idle WebContent none.
The list on the phone is the add-on's, copied at boot (`/System/Library/Extensions/NativeAdrenoMetal.clients`): a push to
/private/var/ngpu/addons does not change the running view, rebuild the payload.

Proof (session-032805, Wi-Fi up, Safari defaults with the GPU process): apple.com drawn (`.cache/webpaint/safari-apple-fixed.png`);
with the GPU process off (WebContent drawing, session-031744) example.com and apple.com draw too, and scroll; there the probe's census showed
the w30r tiles at nz1000 (non-zero words per mille) against nz0 in the sessions before the fix. fps.sh unchanged: home motion 54.7 [50.0-57.2], App Library motion
51.8 [42.0-58.4]. settings-scroll.sh against the same image with only `ngpu-metal` listed (FAJITA_GPU_CLIENTS, Wi-Fi up, corr/wpbase2 vs
corr/webpaint): the resting screens 0, u4, d4 match (0 px over 8); the in-between drag steps differ by their scroll offset only (8-37 px).
Still open, separate: a one-pixel dark line at some tile seams with WebContent drawing (GPU process off); product images
on apple.com are missing (image decode, not the tiles); the tab overview's background is pink.

Tools: `package/usb-debug-tools/fjsurface.c` (built by build-usbdbg-agent.sh): `--cg w30r|BGRA` draws with CoreGraphics into a new surface
and counts the result (the w30r test reproduces the zero tiles in any unlisted process; list the tool's name in FAJITA_GPU_CLIENTS to see it
draw), `--share` checks cross-process IOSurface sharing (passes), `--images PID [FILTER]` lists another process's loaded images (whether
Metal and NativeAdrenoMetal are in it), `--scan` / `--dump` (IOSurfaceLookup by ID is refused for other processes' surfaces here: "IOSurface
disallowing global lookups"). `scripts/fajita/test/safari-page.sh OUT.png [open|apple|reload|wake]` wakes, opens Safari, screenshots and
prints the SI/HC rows; `tools/gpu/surface-png.py` turns a raw dump into a PNG. The plugin's SI/HC telemetry rows (NativeMetal
NativeBridgeMetalFrontend.mm `probeSurface`): the last large surface imports with format, size, seed and purgeable state; `NGPU_SURFACE_PROBE=1`
in gpu-env adds the pixel census. `fjbundle --prefs` now synchronizes per key (with one synchronize at the end, only the first of several
`=d` removals reached the plist).

### 9.x Images on web pages (checked 2026-10-26, closed)

On a default build (28a381d, Wi-Fi all PASS, App cryptex mounted), Safari draws pages with their images: news.ycombinator.com (the Y
logo), en.wikipedia.org (the featured-article photo, the logo) and www.apple.com (the product photo). The missing images seen before
the App cryptex mount are gone; nothing else was needed. Pages are opened with `fjbundle --open URL` (LaunchServices: openURL:
succeeds, openSensitiveURL:withOptions: is refused even with com.apple.springboard.opensensitiveurl), e.g.
`session.sh push .cache/fajita-inputs/fjbundle /private/var/ngpu/sbin/fjbopen1` then `session.sh run '/private/var/ngpu/sbin/fjbopen1
--open https://news.ycombinator.com/'`, then `session.sh call screenshot ABS.png` (wake the screen first: safari-page.sh ... wake).

Open: com.apple.Safari.History (now running from the App cryptex) aborted 4 times in that session: sig6 from an uncaught Objective-C
exception thrown on a dispatch queue in SafariCore (+0x5b930 / +0x5bd5c -> libdispatch -> libobjc +0x1aafc -> libc++abi -> abort).
Safari keeps working (history lookups fail). Likely the history store's path or container; not investigated yet.
