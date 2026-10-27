#!/usr/bin/env python3
"""Generate the minimal fajita (OnePlus 6T, SDM845) Apple-format DT for the first boot.

Adapted from make-sheng-adt.py of UniXNU-sheng: the same required nodes and properties, with the
SDM845 values of the mainline sdm845.dtsi:

  - GIC-500 (GICv3) distributor 0x17a00000/0x10000, redistributor 0x17a60000/0x100000
    (arm-io child offsets under an APSS range at 0x17000000), stride 0x20000: eight
    GICv3 redistributors of two 64 KiB frames, no GICv4 VLPI frames
  - CPU timebase 19200000 Hz (Qualcomm XO architectural counter)
  - DRAM base 0x80000000; size from board.toml (8 GiB on the 256 GB model).

This is first-boot bring-up data: the platform has no matching platform
expert yet, so the kernel is expected to reach the video console and report
the genuine missing-driver panic on screen. No Apple device identity is
claimed and no property is invented beyond these pinned sources.
"""
from pathlib import Path
import argparse
import os
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

DRAM_BASE = 0x80000000
ARMIO_BASE = 0x17000000          # APSS peripheral block containing the GIC
GIC_DIST = 0x17a00000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x17a60000
GIC_REDIST_SIZE = 0x100000
GIC_REDIST_STRIDE = 0x20000
TIMEBASE = 19200000


# The devices the tree can describe (MobileGestalt reads the root model and target-type and the /product artwork traits).
# ipad7,1: the unchanged iPad7,1 tree (iPadOS 17.7.11 board). iphone10,6: the iPhone X (D221AP) of the iOS 16.7.16 board,
# 1125x2436 at scale 3.
IDENTITIES = {
    'ipad7,1': {'model': b'iPad7,1\0', 'target-type': b'J120\0', 'product-name': b'iPad Pro (12.9-inch) (2nd generation)\0',
                'idiom': b'pad\0', 'subtype': 2732, 'scale': 2, 'fallback': b'iPad6,7\0'},
    'iphone10,6': {'model': b'iPhone10,6\0', 'target-type': b'D221\0', 'product-name': b'iPhone X\0',
                   'idiom': b'phone\0', 'subtype': 2436, 'scale': 3, 'fallback': b'iPhone10,3\0'},
}


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def _adt_parse(d, o=0):
    n, c = struct.unpack_from('<II', d, o); o += 8; props = []
    for _ in range(n):
        name = d[o:o + 32].split(b'\0')[0].decode(); length, = struct.unpack_from('<I', d, o + 32); o += 36
        length &= 0x7fffffff; props.append((name, d[o:o + length])); o += (length + 3) & ~3
    kids = []
    for _ in range(c):
        k, o = _adt_parse(d, o); kids.append(k)
    return (props, kids), o


def backlight_node(source):
    """/backlight of the iPhone X tree: AppleARMBacklight matches IONameMatch "backlight" and calls the service with phandle 0x80
    for function-backlight_update ('bklU') and function-backlight_enable ('bklE'). Scalars below are the 20H392 D221 values."""
    u32 = lambda v: struct.pack('<I', v)
    props = [('compatible', b'backlight\0'), ('device_type', b'backlight\0'), ('backlight-id', u32(0xffffffff)),
             ('AAPL,phandle', u32(0xd3)),
             ('function-backlight_update', u32(0x80) + b'Uklb'), ('function-backlight_enable', u32(0x80) + b'Elkb'),
             ('DBVMin', u32(139)), ('DBVMax', u32(0x7ff)), ('override-boot-brightness', u32(740)), ('max-nit-value', u32(750)),
             ('backlight-update-policy', u32(4)), ('use-AAB-architecture', u32(1)), ('use-Nits2DACTable', u32(1)), ('use-AAP', u32(1)),
             ('use-new-AAP-ramp', u32(1)), ('pre-strobe-nit', u32(674)), ('panel-gamma', u32(0x898)),
             ('LminProduct', u32(0x20000)), ('LmidProduct', u32(0x9b0000)), ('LmaxProduct', u32(0x2a20741))]
    if source and not Path(source).is_file():
        # Without the copied properties (backlight-marketing-table above all) AppleARMBacklight's start fails ("Need maketing table for
        # the new Architecture") and iOS never finds a backlight: refuse instead of building a tree that cannot work.
        raise SystemExit(f'--backlight: {source} missing (ipsw img4 im4p extract DeviceTree.d221ap.im4p; build.sh sets FAJITA_BACKLIGHT_FROM)')
    if source:
        # the /backlight node is a direct child of the root
        data = Path(source).read_bytes()
        (rprops, kids), _ = _adt_parse(data)
        have = {k for k, _ in props} | {'name'}
        for kprops, _ in kids:
            if dict(kprops).get('name', b'').startswith(b'backlight\0'):
                props += [(k, v) for k, v in kprops if k not in have and not k.startswith('function-') and k != 'AAPL,phandle']
    return node('backlight', props)


def empty_nvram_proxy():
    """A fresh volatile CHRP bank for upstream IODTNVRAM, with no controller."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/fajita/boot/kernel.macho.adt')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), required=True,
                        help='Actual observed DRAM size, e.g. 0x230000000')
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=None,
                        help='Explicit DarwinBoot contiguous window request, 256 MiB..8 GiB in 2 MiB steps')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching: select the root by the UUID of an IOMedia (the standard source mechanism)')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None,
                        help='/chosen/unique-chip-id (8 bytes) and --chip-id (4 bytes) seed XNU IOPlatformUUID; software identity seeds, not Apple silicon IDs')
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true',
                        help='/product and /buttons describe an iPad-class device without a Home button (MobileGestalt reads these from the tree); '
                             'values follow the unchanged iPad7,1 tree')
    parser.add_argument('--identity', choices=sorted(IDENTITIES), default=None,
                        help='the device the tree describes; ipad7,1 is --ipad-identity, iphone10,6 the iPhone X of the iOS 16 board')
    parser.add_argument('--vmm-present', action='store_true',
                        help='/defaults vmm-present = 1 (kern.hv_vmm_present): the 20H392 AMFI then skips its ApplePMGRNub wait and '
                             'PMGRAON latch (it panics after 30 s without a PMGR), so the iOS 16 board needs no PMGR stub')
    parser.add_argument('--manifest-bnch', action='store_true',
                        help='/chosen/manifest-properties BNCH (48 zero bytes): the 20H392 AppleImage4 pmap runtime reads the boot nonce hash '
                             'there and panics without it ("failed to read boot nonce hash") at secure boot level 0')
    parser.add_argument('--chosen-display-scale', action='store_true',
                        help='/chosen/display-scale = the identity\'s scale from the start. MobileGestalt\'s main-screen scale comes from it and '
                             'iOS 16 FrontBoardServices asserts on scale 0 (SpringBoard died at once, 2026-10-23); the display probe accepts an '
                             'equal value it finds there (publishOrMatchWord)')
    parser.add_argument('--cpu-regs', default='0',
                        help='Comma-separated CPU MPIDR affinity values (the /cpus reg of each core); the first is the running boot CPU. '
                             'SDM845 numbers its cores 0x000..0x700 in Affinity1 (sdm845.dtsi). More than one adds /psci')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default='smc',
                        help='/psci method used by the secondary-CPU start (the board DT declares smc)')
    parser.add_argument('--packed-data-ramdisk', action='store_true', help='Hand off validated DBRDISK1 bytes to the native sparse block driver, without a dense memdev alias')
    parser.add_argument('--data-ramdisk', action='store_true',
                        help='/chosen/memory-map RAMDisk placeholder; the loader fills in the real range for a scratch Data seed')
    parser.add_argument('--persistent-data', action='store_true',
                        help='The Data disk keeps its writes across boots (sheng-data-persist): no ephemeral-storage, so first-boot data migration runs')
    parser.add_argument('--rtc', action='store_true',
                        help='/arm-io/rtc: the PM8998 RTC (pm8998.dtsi rtc@6000, SID 0) for FajitaRtc; device_type rtc makes ArmDTPlatform wait '
                             'for the IORTC resource before the calendar starts')
    parser.add_argument('--backlight', action='store_true',
                        help='/backlight (IONameMatch backlight: the AppleARMBacklight of AppleARMPlatform), as the iPhone X tree has it: scalar properties '
                             'and function-backlight_update/enable = phandle 0x80 + \'Uklb\'/\'Elkb\'. Phandle 0x80 is the panel service of '
                             'package/FajitaPanel.cpp (fajita-brightness=1). --backlight-from PATH copies the rest of the node from an iPhone X ADT payload')
    parser.add_argument('--haptics', action='store_true',
                        help='/product/haptics closed-loop (the Taptic path of package/fajita-audio, fajita-haptics=3); FAJITA_HAPTICS=1 does the same')
    parser.add_argument('--ringer', action='store_true',
                        help='/buttons gains the ringer switch (ringerab, function-button_ringerab) for package/FajitaAlertSlider.cpp (fajita-alert=2); '
                             'FAJITA_RINGER=1 does the same')
    parser.add_argument('--backlight-from', type=Path, default=None,
                        help='ADT payload of the 20H392 DeviceTree.d221ap (ipsw img4 im4p extract): the /backlight properties not listed here (the '
                             'marketing table, restriction tables) are copied from it at build time; nothing of it is stored in the repository')
    parser.add_argument('--trust-cache', action='store_true', help='Zeroed TrustCache sidecar handoff placeholder')
    parser.add_argument('--gpu-addon', action='store_true', help='Zeroed GpuAddon sidecar handoff placeholder (the loader fills in kernel.macho.gpuaddon)')
    parser.add_argument('--board-args', default='',
                        help='/chosen/board-args: kernel arguments beyond the command line ([payload] board_args), read by PE_parse_boot_argn '
                             'when the command line lacks them (UniXNU patches/xnu-8796/board-args.patch)')
    parser.add_argument('--text-fixup', action='append', default=[],
                        help='/chosen/text-fixups entry <file name>,<file offset>,<original>,<replacement> for the kernel\'s signed-text fixups')
    args = parser.parse_args()
    # FAJITA_BACKLIGHT=1 is --backlight for builds that pass no argument (the board's args list stays unchanged until the slider is proven)
    args.backlight = args.backlight or os.environ.get('FAJITA_BACKLIGHT') == '1'
    args.ringer = args.ringer or os.environ.get('FAJITA_RINGER') == '1'
    args.haptics = args.haptics or os.environ.get('FAJITA_HAPTICS') == '1'
    if args.backlight and args.backlight_from is None:
        args.backlight_from = Path(os.environ.get('FAJITA_BACKLIGHT_FROM', Path(__file__).resolve().parents[1] / '.cache/ipsw/20H392/DeviceTree.d221ap.payload'))
    if args.ipad_identity:
        args.identity = args.identity or 'ipad7,1'
    identity = IDENTITIES.get(args.identity)
    if (args.unique_chip_id is None) != (args.chip_id is None):
        parser.error('--unique-chip-id and --chip-id must be supplied together')
    if args.unique_chip_id is not None and not (0 < args.unique_chip_id < 1 << 64 and 0 < args.chip_id < 1 << 32):
        parser.error('--unique-chip-id must fit 8 bytes and --chip-id 4 bytes, both nonzero')
    if args.owned_memory_size is not None and (
            not 0x10000000 <= args.owned_memory_size <= 0x200000000
            or args.owned_memory_size % 0x200000):
        parser.error('Owned memory must be 256 MiB to 8 GiB, aligned to 2 MiB')

    try:
        cpu_regs = [int(value, 0) for value in args.cpu_regs.split(',')]
    except ValueError:
        parser.error('--cpu-regs must be integers')
    if (not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs) or len(cpu_regs) > 8
            or any(reg < 0 or reg > 0xffff or reg & 0xff for reg in cpu_regs)):
        parser.error('--cpu-regs: up to eight distinct Affinity1-numbered cores, boot CPU 0 first')
    cpus = [node('cpu%d' % index, [('state', b'running\0' if index == 0 else b'waiting\0'),
                                   ('reg', struct.pack('<I', reg)),
                                   ('device_type', b'cpu\0'), ('compatible', b'arm,cortex-a75\0' if index >= 4 else b'arm,cortex-a55\0'),
                                   ('timebase-frequency', struct.pack('<I', TIMEBASE))])
            for index, reg in enumerate(cpu_regs)]
    tree = node('device-tree', [
        # hw.product/hw.machine/hw.target come from the root 'model'/'target-type' (IODTPlatformExpert);
        # libMobileGestalt prefix-matches hw.product against iPhone/iPod/iPad, so the iPad identity needs both here.
        ('model', identity['model'] if identity else b'fajita-first-boot\0'),
        *([('target-type', identity['target-type'])] if identity else []),
        # Root compatible strings of sdm845-oneplus-fajita.dts.
        ('compatible', b'oneplus,fajita\0qcom,sdm845\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', [('random-seed', os.urandom(64)),
                        # Without these XNU never publishes IOPlatformUUID and gethostuuid() blocks forever
                        # for callers (configd) that pass a zero timeout.
                        *([('unique-chip-id', struct.pack('<Q', args.unique_chip_id)), ('chip-id', struct.pack('<I', args.chip_id))]
                          if args.unique_chip_id is not None else []),
                        # This boots the normal target OS. Volatile Data is
                        # described separately by ephemeral-storage below.
                        ('osenvironment', b'main\0'),
                        *([('display-scale', struct.pack('<I', identity['scale']))] if args.chosen_display_scale and identity else []),
                        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1), as read by MobileGestalt: mobileactivationd's
                        # DeviceType then takes its native "Hactivation" branch (Activated without an activation record; no SEP or
                        # baseband). Values established on hardware by github.com/dipa-studio/ios-sweet (tools/mkadt.py).
                        *([('certificate-production-status', struct.pack('<I', 0)), ('effective-production-status-ap', struct.pack('<I', 0)),
                           ('certificate-security-mode', struct.pack('<I', 1)), ('effective-security-mode-sep', struct.pack('<I', 1))]
                          if identity else []),
                        # This property describes the actual volatile Data RAM disk. A persistent one is not ephemeral: with the property
                        # datamigrator skips every migration (os_variant_uses_ephemeral_storage), so the staged system apps never install.
                        *([('ephemeral-storage', struct.pack('<I', 1))]
                          if (args.data_ramdisk or args.packed_data_ramdisk) and not args.persistent_data else []),
                        *( [('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))]
                           if args.owned_memory_size else [] ),
                        ('dram-base', struct.pack('<Q', DRAM_BASE)),
                        ('dram-size', struct.pack('<Q', args.dram_size)),
                        # Image4 hash algorithm. The target iPad's DT specifies sha2-384 in /chosen; this
                        # selects the algorithm only and asserts nothing about Apple silicon or fuses.
                        ('crypto-hash-method', b'sha2-384\0'),
                        *( [('root-matching', ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
                                               '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                                               args.root_match_volume_uuid.upper() + '</string></dict></dict>\0').encode())]
                           if args.root_match_volume_uuid else [] ),
                        # Strict policy for the original AMFI: platform code only. This keeps AMFI
                        # enforcing; it does not relax it.
                        ('amfi-only-platform-code', struct.pack('<I', 1)),
                        *([('board-args', args.board_args.encode() + b'\0')] if args.board_args else []),
                        *([('text-fixups', b''.join(f.encode() + b'\0' for f in args.text_fixup))] if args.text_fixup else []),
                        *empty_nvram_proxy()],
             [node('memory-map', ([("PackedRAMDisk" if args.packed_data_ramdisk else "RAMDisk", bytes(16))] if (args.data_ramdisk or args.packed_data_ramdisk) else []) +
                                 ([('TrustCache', bytes(16))] if args.trust_cache else []) +
                                 ([('GpuAddon', bytes(16))] if args.gpu_addon else [])),
              # Read by the original AppleImage4 at start-up; present and empty here.
              node('manifest-properties', [('BNCH', bytes(48))] if args.manifest_bnch else [])]),
        node('cpus', children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', args.psci_method.encode() + b'\0')])] if len(cpus) > 1 else []),
        # Suspend/resume is not implemented for this board. Use the original
        # root-domain policy input rather than allowing the CPU to park forever.
        # kern.maxvnodes: this kernel is built with CONFIG_VNODES=1024 (xsmall), so desiredvnodes clips to 1024 and the desktop's
        # ~1950 live vnodes made vnode jetsam kill idle daemons about once a second (int-102 log). XNU reads the value from /defaults
        # before it clips; this is its own formula, memory/64K + 1024, for the iPad7,1's 4 GiB.
        node('defaults', [('sleep-disabled', struct.pack('<I', 1)), ('kern.maxvnodes', struct.pack('<I', 66560)),
                          *([('vmm-present', struct.pack('<I', 1))] if args.vmm_present else [])]),
        node('options', []),
        *([backlight_node(args.backlight_from)] if args.backlight else []),
        # The original AppleImage4 panics without a product node. Name and model are the
        # board DT's own strings.
        *( [node('buttons', [('compatible', b'buttons\0'), ('device_type', b'buttons\0'),
                             # No 'menu' entry: fajita has no Home button. 2 = the non-mechanical answer BSPlatform passes through.
                             ('button-names', b'hold\0voldown\0volup\0' + (b'ringerab\0' if args.ringer else b'')),
                             # MobileGestalt's ringer-switch answer (SpringBoard's SBRingerControl acts on the switch only when it
                             # is true) is whether /buttons has function-button_ringerab (libMobileGestalt reads it by its hashed
                             # name Ah5yfSlY2yycIQb92HbmrA). The iPhone X's points at its GPIO provider; no AppleM68Buttons runs
                             # here (the switch reaches IOHIDFamily through NativeHidInput), so a null phandle stands in.
                             *([('function-button_ringerab', struct.pack('<I', 0) + b'RntbNIRb')] if args.ringer else []),
                             ('home-button-type', struct.pack('<I', 2))])] if identity else [] ),
        node('product', [('product-name', identity['product-name'] if identity else b'OnePlus 6T\0'),
                         ('model', b'oneplus,fajita\0'),
                         *( [('product-description', identity['product-name']),
                             ('artwork-device-idiom', identity['idiom']),
                             ('artwork-device-subtype', struct.pack('<I', identity['subtype'])),
                             ('artwork-scale-factor', struct.pack('<I', identity['scale'])),
                             ('artwork-display-gamut', b'P3\0'),
                             ('compatible-device-fallback', identity['fallback']),
                             ('thin-bezel', struct.pack('<I', 1))] if identity else [] )],
             # /product/haptics as the iPhone X has it: closed-loop is MobileGestalt's DeviceSupportsClosedLoopHaptics, with which iOS
             # plays its feedback and vibrations as waveforms on the "Actuator" audio device (package/fajita-audio, fajita-haptics=3).
             [node('haptics', [('closed-loop', struct.pack('<I', 1)), ('supports-3rd-party-haptics', struct.pack('<I', 1))])]
             if args.haptics else []),
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, 0x08000000))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q',
                                                GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                                GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
                            ('redistributor-stride', struct.pack('<Q', GIC_REDIST_STRIDE))]),
              # No reg: the RTC sits behind the SPMI arbiter, at spmi-base of PMIC slave spmi-sid.
              *([node('rtc', [('compatible', b'qcom,pm8941-rtc\0'), ('device_type', b'rtc\0'),
                              ('spmi-sid', struct.pack('<I', 0)), ('spmi-base', struct.pack('<I', 0x6000))])] if args.rtc else [])])

    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'fajita ADT: {len(tree)} bytes, dram {args.dram_size:#x}, timebase {TIMEBASE}')


if __name__ == '__main__':
    main()
