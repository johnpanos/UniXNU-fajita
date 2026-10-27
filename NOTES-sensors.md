# fajita sensors: the SLPI sensor DSP under UniXNU (iOS 16.7.16)

Goal: iOS's own auto-brightness (ambient light), screen-off during calls (proximity) and rotation (accelerometer/gyro) on
the OnePlus 6T. On SDM845 these sensors are not on the AP's buses: they hang off the **SLPI** (Sensor Low Power Island, a
Hexagon DSP), which runs Qualcomm's sensor firmware (SEE, "Sensors Execution Environment") and serves samples to the AP
over QRTR/QMI with protobuf payloads. Linux/postmarketOS is the reference: `qcom_q6v5_pas` (PAS id 12) boots it,
`hexagonrpcd -s` serves its configuration files over FastRPC, and `libssc` reads the samples.

Everything is behind `fajita-sensors=<stage>` (default 0: nothing is mapped, voted or poked).

## 1. Hardware facts (cited)

| Fact | Value | Source |
| --- | --- | --- |
| Remote processor | `slpi_pas: remoteproc@5c00000` `qcom,sdm845-slpi-pas` | `sdm845.dtsi:3378` |
| PAS id | **12**, firmware `slpi.mdt`, crash reason SMEM item 424, SSR name `dsps`, sysmon `slpi`, SSCTL instance 0x16 | `qcom_q6v5_pas.c:1223` (`sdm845_slpi_resource_init`) |
| AOSS QMP | `load_state` name `slpi` (on before boot, off after stop) | same, `.load_state = "slpi"` |
| Proxy power | power domains **LCX, LMX** (rpmhpd: cmd-db `lcx.lvl`, `lmx.lvl`) at the top level, clock `xo` (RPMH_CXO_CLK: cmd-db `xo.lvl`, ARC level 3), all dropped at handover | `sdm845.dtsi:3390-3397`, `qcom_q6v5_pas.c adsp_start/qcom_pas_handover` |
| Carveout | `slpi_mem` **0x96700000 + 0x1400000** (no-map) | `sdm845.dtsi:877` |
| Watchdog | GIC SPI 494 edge | `sdm845.dtsi:3382` |
| SMP2P | `smp2p-slpi`: SMEM items **481 (inbound, SLPI->AP) / 430 (outbound, AP->SLPI)**, local pid 0, remote pid **3**, IRQ SPI 172, doorbell `apss_shared` bit **26** | `sdm845.dtsi:1170` |
| SMP2P bits (inbound "slave-kernel") | 0 fatal, 1 ready, 2 handover, 3 stop-ack; outbound "master-kernel" bit 0 = stop | `sdm845.dtsi:3383-3402` |
| GLINK edge | label `dsps`, remote pid **3**, IRQ SPI 170, doorbell `apss_shared` bit **24**; SMEM items 478/479/480 in the APPS<->SSC partition | `sdm845.dtsi:3406`, `qcom_glink_smem.c` |
| SMEM partition | host 0 <-> host 3 exists on the phone: offset 0x15f000, 0x30000 bytes (snapshot `.cache/smem-snapshot/smem.bin`, `$TOC` entry 3) | parsed 2026-10-27 |
| FastRPC | GLINK channel **`fastrpcglink-apps-dsp`**, domain `sdsp`, non-secure, no SMMU context bank (sid 0); buffers come from the remote heap `fastrpc_mem` (16 MiB, 4 MiB aligned, below 4 GiB) which is SCM-assigned to **HLOS(3)+MSS_MSA(15)+SSC_Q6(5)+ADSP_Q6(6) RWX** at probe | `sdm845.dtsi:3412-3427`, `fastrpc.c:2310` |
| fajita | `&slpi_pas { firmware-name = "qcom/sdm845/oneplus6/slpi.mbn"; status = "okay"; }` | `sdm845-oneplus-common.dtsi:572` |

Linux sources: `/home/john/src/ref/linux` (v6.16 sparse checkout; `drivers/remoteproc/qcom_q6v5_pas.c`, `drivers/misc/fastrpc.c`
added for this work), `/home/john/src/ref/hexagonrpc` (linux-msm/hexagonrpc da7a374), `/home/john/src/ref/libssc` (033e2a3),
`/home/john/src/ref/libqmi-qmi-service-ssc.json` (libqmi's SSC service definition).

## 2. Firmware (read from the phone at runtime, never committed)

The phone's own `modem_a`/`modem_b` partition (FAT16, read through NativeUfs `native-ufs-partread=1` and the Wi-Fi FAT reader
`drivers/FajitaWifi/WifiFat.c`, exactly as the modem boot reads `image/modem.mdt`) holds the split image:
`image/slpi.mdt` (7964 B = ELF header + phdrs 0x314 B, hash segment 0x1c08 B at 0x1000) and `image/slpi.b00`..`slpi.b21`
(b18 and b22 do not exist: zero file size), plus the pd-mapper tables `image/slpir.jsn`, `image/slpius.jsn`
(domain `msm/slpi/root_pd` and `msm/slpi/sensor_pd`, qmi instance 90, service `tms/servreg`).

`slpi.mdt` (checked against the dumped partition `.cache/fw-modem-part/modem_a.img`): 23 phdrs, entry 0x94a00000, phdr 1 is the
hash segment (PT_NULL, flags type 2), 21 loadable segments all **relocatable**, span 0x94a00000..0x95b80000 (0x1180000 B), so
they fit the 0x1400000 carveout. `qcom_mdt_load` relocates: `pas_mem_setup(12, 0x96700000, 0x1180000)`, segment i lands at
0x96700000 + (p_paddr - 0x94a00000).

The carveout 0x96700000..0x97b00000 is below DarwinBoot's window (starts 0xa01c8000, NOTES-wifi.md "Memory reservation"),
so XNU never hands its pages out; it only needs a write-combined mapping.

## 3. Boot sequence (Linux `adsp_start` for sdm845 SLPI; `fajita-sensors>=2`)

1. AOSS QMP `{class: image, res: load_state, name: slpi, val: on}` (`fajita_wifi_qmp_load_state("slpi", 1)`, shared QMP link).
2. Proxy votes through `fajita_rpmh_send`: `xo.lvl` (ARC on), `lcx.lvl` and `lmx.lvl` to their top level (cmd-db level tables,
   as WifiMss does for mx/cx/mss); the previous level is read back first and restored on failure.
3. Metadata (ELF header + phdrs, then the hash segment) into a physically contiguous buffer below 4 GiB;
   `qcom_scm_pas_init_image(12, meta)` (SIP svc 2 cmd 1, ARGS(2, VAL, RW)).
4. `qcom_scm_pas_mem_setup(12, 0x96700000, 0x1180000)` (cmd 2).
5. Copy every loadable segment from `slpi.bNN` into the carveout, zero the bss tail (memsz > filesz).
6. `qcom_scm_pas_auth_and_reset(12)` (cmd 5).
7. Wait (5 s, `qcom_q6v5_wait_for_start`) for SMP2P inbound bit 1 (ready); fatal bit 0 -> crash reason SMEM item 424.
8. Handover (bit 2): drop the proxy votes (xo, lcx, lmx back to what they were).
9. Failure unwinding: `pas_shutdown(12)` (cmd 6), votes restored, load_state off.

The same SCM call wrapper as the GPU zap load (FajitaGpuProbe.cpp stage 8) and the same freestanding, ops-table structure as
`WifiMss.c`, so the sequence is host-tested against a simulated SLPI.

## 4. Link and AP services (`fajita-sensors>=2`)

- SMEM: its own `wsmem` instance (WifiSmem.c) on the shared SMEM with the TCSR hwlock; SMP2P 430/481 (pid 0 <-> 3, doorbell 26).
- GLINK over SMEM to remote pid 3 (WifiGlink.c, doorbell 24), the `IPCRTR` channel, a QRTR node (WifiRouter.c) for the
  SLPI (node 9 in Linux logs) separate from the modem's link.
- pd-mapper (WifiPdmap.c, service 0x40) on that link with `slpir.jsn`/`slpius.jsn` from the modem partition.
- **Proof of stage 2**: the SLPI's NEW_SERVER announcements in the log (`SN2 qrtr new svc ...`). Expected (to be confirmed):
  SSC **0x190 (400)**, SSCTL 0x2b instance 0x16, servreg notifier 0x42.

## 5. Sensor configuration over FastRPC (`fajita-sensors>=3`)

The SEE firmware does not know which sensors the board has: at boot it reads its **registry** and configuration from the AP's
filesystem through a FastRPC reverse call ("apps_std" fopen/fread/...), served on Android by `sscrpcd sensorspd` and on pmOS by
`hexagonrpcd -f /dev/fastrpc-sdsp -s`. Without it the SUID lookups find no physical sensors.

AP side, all in the kernel (no daemon):
- GLINK channel `fastrpcglink-apps-dsp`. Messages: `fastrpc_msg {s32 client_id, s32 tid, u64 ctx, u32 handle, u32 sc, u64 addr,
  u64 size}` out, `fastrpc_invoke_rsp {u64 ctx, s32 retval}` back; arguments in a buffer at physical `addr`
  (`fastrpc_get_args` layout: remote_arg[n], invoke_buf[n], phy_page[n], fdlist[16], crclist[64], then inline args 128-aligned).
- Remote heap: a 4 MiB, 4 MiB-aligned range inside XNU's window held by `phys-reserve` (next to the Wi-Fi DMA arena), SCM-assigned
  HLOS -> HLOS+MSS_MSA+SSC_Q6+ADSP_Q6 RWX once, mapped uncached.
- `INIT_ATTACH_SNS`: handle 1 (init), method 0, one in-buffer (client id), `ctx | SENSORS_PD (2)`.
- The reverse tunnel (hexagonrpcd `listener.c`): `remotectl` open of the listener, `adsp_listener_init2`, then the
  `adsp_listener_next2` loop; the requests are served by a local `remotectl` (handle 0), `apps_std` (file ops) and `apps_mem`.
- Files: the phone's own **persist** partition (ext4, read-only, NativeUfs): `/mnt/vendor/persist/sensors/registry/registry/*`
  (277 files on this phone, generated by OxygenOS from the vendor JSON, with its calibration), `.../sns_reg_config`,
  `sns_reg_ctrl` (`END:AP`); `/vendor/etc/sensors/config` served as an empty directory first (the registry is already
  built; hexagonrpcd does the same with `map_or_empty`); `/sys/devices/soc0/*` synthesised from SMEM socinfo.
  **Writes by the DSP go to RAM only; persist is never written.** Every path asked for is logged with its result.
- Needs a read-only ext4 reader (freestanding, host-tested against `.cache/efs-backup/persist.img`, checked with `debugfs`).

## 6. Samples: the SSC protocol (`fajita-sensors>=3`)

QMI service **0x190** on the SLPI node (libqmi `qmi-service-ssc.json`):
- request/response `0x0020` Control: TLV 0x01 = u16 length + protobuf `SscClientRequest`; TLV 0x10 = u8 report type
  (1 = large); response TLV 0x02 result, 0x10 client id (u64), 0x11 u32.
- indications `0x0021` (small) / `0x0022` (large): TLV 0x01 client id (u64), TLV 0x02 = u16 length + protobuf `SscClientResponse`.

Protobuf (proto2, reverse-engineered, `libssc/data/*.proto`):
- `SscClientRequest {1 SscUid uid (fixed64 low=1, high=2); 2 fixed32 msg_id; 3 SscClientConfig {1 processor=1 (APSS), 2 suspend_mode=0};
  4 SscClientRequestBody {2 bytes msg; 3 bool is_passive}}`.
- `SscClientResponse {1 SscUid uid; 2 repeated SscClientResponseBody {1 fixed32 msg_id; 2 fixed64 timestamp (19.2 MHz); 3 bytes msg}}`.
- SUID lookup: to uid 0xABABABABABABABAB/0xABABABABABABABAB, msg 512 `SscSuidRequest {1 data_type, 2 enable_updates}`; event 768
  `SscSuidResponse {1 data_type, 2 repeated SscUid}`. Data types: `ambient_light`, `proximity`, `accel`, `gyro`, `mag`.
- Attributes: msg 1 -> event 128 `SscAttrResponse` (0 name, 1 vendor, 2 type, 6 rates, 16 stream type 0 continuous / 1 on-change).
- Enable: msg 513 continuous (`SscEnableConfigRequest {1 float sample_rate}`, accel/gyro) or 514 on-change (light, proximity);
  config event 768; disable msg 10.
- Samples: event **1025** `{1 repeated float values, 2 int32 accuracy}` (light: [0] lux; accel: x,y,z m/s^2; gyro rad/s);
  proximity event **769** `{1 int32 near, 2 int32 distance, 3 int32 accuracy}`.

Which parts are on this phone is answered by the SUID/attribute queries; the persist registry carries drivers for several
(stk3x3x, tmd3702, tcs3400, apds9251 light/prox; bmi160, lsm6dsm, lsm6ds3c accel/gyro; ak0991x, mmc5603nj, mmc3530kj mag).

Rows `SN3 ...`, `kcmd sensors [rows|light|prox|accel|suid|files]`; the decoder is host-tested against hand-built messages
(and, once captured, real ones from the phone).

## 7. iOS side (`fajita-sensors>=4`, phase 4: plan reported before building)

To be surveyed against the 20H392 kernelcache and dyld cache: the IOKit/HID services that backlightd/CoreBrightness
(ambient light), the proximity path (SpringBoard/backboardd, `AppleProxHIDEventDriver`-style HID events) and
CoreMotion/locationd (accelerometer, gyro) look for.

## 8. Staged plan and code map

| Arg | Stage | Writes HW | Rows |
| --- | --- | --- | --- |
| `fajita-sensors=1` | read-only: firmware headers from the partition, SMEM partition 0<->3, ARC levels of xo/lcx/lmx | nothing | `SN1` |
| `fajita-sensors=2` | SLPI boot (section 3), SMP2P, GLINK, QRTR, pd-mapper; services logged | QMP, RPMh, SCM PAS, carveout | `SN2` |
| `fajita-sensors=3` | FastRPC reverse tunnel (registry from persist), SSC client: light, proximity, accel samples | SCM assign of the heap | `SN3` |
| `fajita-sensors=4` | iOS services | — | `SN4` |

Code (planned): `drivers/FajitaSensors/` freestanding C (`SnsPas` boot sequence, `SnsRpc` FastRPC + listener, `SnsExt4`
read-only ext4, `SnsFiles` the served tree, `SnsSsc` QMI+protobuf client) with host tests run by
`scripts/check-fajita-sensors-host.py`; kernel glue `package/FajitaSensors.cpp`, built into `libFajitaWifi.a`
(`build-fajita-wifi.py`, it reuses the Wi-Fi SMEM/GLINK/QRTR/QMI/FAT objects) and started from `fajita_wifi_poll`.
Test runner `scripts/fajita/test/sensors-test.sh`.
