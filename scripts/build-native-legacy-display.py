#!/usr/bin/env python3
"""fajita's CPU-rendered display archive: NativeLegacyDisplay bound to the unchanged original IOMobileFramebufferLegacy, with the
HID input device and the keystore notification service, in the archive the kernel link expects for CONFIG_XNU_NATIVE_LEGACY_DISPLAY
(build/native-legacy-display/libNativeLegacyDisplay.late.a and its build-report.json, scripts/build-xnu-4k-baseline.sh).

A port of UniXNU-sheng scripts/build-native-legacy-display.py (d92e799) that also runs on Linux:
- the tools: "xcrun clang" and "xcrun ar" are the shims of scripts/unixnu/host on Linux; llvm-objcopy comes from PATH, or from
  Homebrew's llvm on macOS. scripts/build-native-hid-input.py of UniXNU asks "brew --prefix llvm" for its llvm-objcopy: on a host
  without Homebrew it gets a one-line brew stand-in (build/native-legacy-display/host-bin) that names the prefix of PATH's llvm-objcopy.
- the reference candidate: the IOMobileGraphicsFamily and IOHIDFamily instruction checks need a prelinked kernel of the 21H461
  (iPad board) or 20H392 (ios16 board) kernelcache. FAJITA_DISPLAY_CANDIDATE (default FAJITA_STORAGE_CANDIDATE, as scripts/build-fajita-storage.py) names a fajita
  kernel.macho with its kernel.json beside it; the audits accept it because it carries the identical original family executables
  (checked by hash in audit-mobile-graphics-legacy.py and build-native-hid-input.py).
- the runtime observation: audit-mobile-graphics-legacy.py carries the hash of the pruned QEMU observation of the 21H461 class
  (identity, parent, object size 0x18a0) over from its previous output. A fresh tree has none, so the recorded hash is seeded.
  20H392 has no such observation; the audit qualifies its identity, parent and size (0x1660) from the metaclass registration.
- the build: the original class differs between the two kernelcaches (LEGACY below), so the class header, the layout header
  OriginalLegacyLayout.h (field offsets and slots NativeLegacyDisplay.cpp and LegacyDisplayProbe.cpp use) and the bindings are
  generated for the kernelcache the candidate's plan names.

Contents (sheng's list without its board parts):
- drivers/NativeMobileDisplay (fajita's copy in this repository, with fajita-display-logical; see its README) NativeLegacyDisplay.cpp
  and NativeGOPPresenter.cpp (LegacyDisplayProbe registers both metaclasses;
  the presenter only acts with sheng-fb-gpu=1, which fajita does not set);
- drivers/NativeHidInput/NativeHidInput.cpp with its original-IOHIDDevice bindings (scripts/build-native-hid-input.py);
- drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp (the owned AppleKeyStore of sheng-aks=1);
- kits/fajita LegacyDisplayProbe.cpp, HidInputProbe.cpp, KeyStoreProbe.cpp, BootProcessProbe.c and BootUiTraceProbe.c, copies of
  UniXNU-sheng package/ (d92e799); LegacyDisplayProbe.cpp without the cpufreq, GPU power and touch probe calls.
Left out: BootLog.c and BootReset.c (scripts/build-boot-progress.py links fajita's own), and sheng's TouchProbe.cpp with the Novatek
and GENI SPI libraries, CpuFreqProbe.cpp and GpuPowerProbe.cpp (SM8550 hardware). The touch snapshot NativeHidInput reads
(native_touch_seq/count/xs/ys/ids, native_touch_set_sink) is defined by libFajitaTouch.a (scripts/build-fajita-touch.py, force-loaded
through CONFIG_XNU_EXTRA_LD_ARCHIVES), which writes it only with fajita-touch=4. The other diagnostics that
BootProgress.c reads from sheng's touch, Adreno and cpufreq code stay the weak stand-ins of kits/fajita/BootProgressStubs.c.

The 21H461 kernelcache addresses below are sheng's; fajita prelinks the same kernelcache (board.toml [kernel.prelink]
kernelcache_sha256 f18b4df8..., slide -0x1000000). The 20H392 ones are their counterparts in that kernelcache (ios16/board.toml,
kernelcache_sha256 2ec297ba..., the same slide). Every address is checked against the instruction bytes of the candidate.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)
KIT = ROOT / 'kits/fajita'
LOGS = ROOT.parent / 'logs'
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
from native_prelink import macho, backed_offset, exports   # noqa: E402


# The original IOMobileFramebufferLegacy of each accepted kernelcache (selected by the original kernelcache that
# audit-mobile-graphics-legacy.py qualified): the instance and vtable extents, the slots NativeLegacyDisplay declares or calls,
# the private fields it touches, and the original code its bindings name, each with the instruction bytes that qualify it.
# 21H461 is sheng's set. 20H392 was matched against it function by function (vtable order, call graph, string references,
# instruction shapes): its instance is 0x240 bytes smaller and its generic class 10 slots shorter, so every private field and
# every Legacy slot from 213 on moved; supportedFrameInfo (selector 74) has no mask output there; IOSurface returns its sizes as
# 32-bit words (ldr w0, which zero-extends into the 64-bit return the bindings declare).
LEGACY = {
    '21H461': dict(
        size=0x18a0, slots=358,
        # slot -> prototype; 213 with the trace-key mask output (four arguments).
        prototypes={184: 'isMainDisplay', 188: 'getDotPitch', 213: 'supportedFrameInfo', 249: 'getDisplayDimensions',
                    288: 'startHardware', 290: 'kickSwapQueue', 295: 'completeCurrentSwap', 296: 'validateSwap',
                    301: 'getRefreshAttributes', 337: 'collectCompletionReports'},
        frame_info_mask=True, state_setter=354, state_setter_address=0xfffffff0057970cc,
        # Display: current swap, pending queue head and tail, vsync client queue (type 5 of the per-type queues at +5512),
        # reported width/height, completed and presented generations, retired frame count, the requested and powered state words.
        # Request: four layer surfaces, queue links.
        fields=dict(current=5472, head=5480, tail=5488, vsync_clients=5592, width=5808, height=5812, completed=5820, presented=5824,
                    frames=252, power_requested=5836, powered=5840, surfaces=2624, next=2960, previous=2968),
        constructor=0xfffffff0057865cc, destructor=0xfffffff005786618, metaclass=0xfffffff006b35940,
        base_calls=((0xfffffff0057865fc, 0xfffffff00577b510), (0xfffffff005786630, 0xfffffff00577b55c)),
        # IOSurface getters: (name, address, field, 64-bit load).
        surface=[('descriptor', 0xfffffff004b9ead0, 48, True), ('width', 0xfffffff004b9ebbc, 88, True),
                 ('height', 0xfffffff004b9ebc4, 96, True), ('stride', 0xfffffff004b9ebd4, 104, True),
                 ('allocation', 0xfffffff004b9ebfc, 144, True), ('format', 0xfffffff004b9ebac, 128, False),
                 ('planes', 0xfffffff004b9ebf4, 152, False)],
        vsync_notify=0xfffffff005796720,
        vsync_checks=[(0xfffffff005796720, 'ff0302d1fd7b07a9fdc30191a0831ff8'), (0xfffffff005796830, '08ed4af9'),
                      (0xfffffff00579685c, 'e2230091c3008052240080521684ff97'), (0xfffffff005795fbc, '0ab182d2')],
        refresh_attributes=0xfffffff00579ea5c,
        refresh_checks=[(0xfffffff00579ea68, '888888d28888a0f2'), (0xfffffff00579ec54, '08b544f9')],
        set_refresh_properties=0xfffffff00579ebb8,
        set_refresh_checks=[(0xfffffff00579ebb8, 'ff0302d1fd7b07a9fdc30191a0031ff8'),
                            (0xfffffff00579ed54, '086540f90173ff90218c0c9100013fd6')],
        start_hardware_call=(0xfffffff005787ef8, '080040f9088144f900013fd6e0000037'),
        complete_current=(0xfffffff005795b88, 0xfffffff005795b98, 'e00740f9280080520101001272d3ff97'),
        completion_report_call=(0xfffffff00578aa7c, '080040f9084545f9030080d2e20303aa0400805200013fd6'),
        main_display_call=(0xfffffff005777e9c, '080040f908e142f900013fd6c0000036')),
    '20H392': dict(
        size=0x1660, slots=338,
        # 212 without the mask output: its selector-74 caller passes (index, &count, key) and returns one scalar.
        prototypes={184: 'isMainDisplay', 188: 'getDotPitch', 212: 'supportedFrameInfo', 246: 'getDisplayDimensions',
                    274: 'startHardware', 276: 'kickSwapQueue', 281: 'completeCurrentSwap', 282: 'validateSwap',
                    287: 'getRefreshAttributes', 321: 'collectCompletionReports'},
        frame_info_mask=False, state_setter=336, state_setter_address=0xfffffff004c1c1d8,
        # The per-type client queues at +4912, the state words at +5208.. (21H461 +5808.., the same order).
        fields=dict(current=4872, head=4880, tail=4888, vsync_clients=4992, width=5208, height=5212, completed=5220, presented=5224,
                    frames=172, power_requested=5236, powered=5240, surfaces=2184, next=2408, previous=2416),
        constructor=0xfffffff004c0bbbc, destructor=0xfffffff004c0bc08, metaclass=0xfffffff006a45540,
        base_calls=((0xfffffff004c0bbec, 0xfffffff004c0186c), (0xfffffff004c0bc20, 0xfffffff004c018b8)),
        surface=[('descriptor', 0xfffffff004a4e724, 56, True), ('width', 0xfffffff004a4e7f4, 144, False),
                 ('height', 0xfffffff004a4e7fc, 148, False), ('stride', 0xfffffff004a4e80c, 152, False),
                 ('allocation', 0xfffffff004a4e834, 172, False), ('format', 0xfffffff004a4e7e4, 164, False),
                 ('planes', 0xfffffff004a4e82c, 176, False)],
        vsync_notify=0xfffffff004c1b838,
        vsync_checks=[(0xfffffff004c1b838, 'ff0302d1fd7b07a9fdc30191a0831ff8'), (0xfffffff004c1b948, '08c149f9'),
                      (0xfffffff004c1b974, 'e2230091c300805224008052368aff97'), (0xfffffff004c1b0d4, '0a6682d2')],
        refresh_attributes=0xfffffff004c234b4,
        refresh_checks=[(0xfffffff004c234c0, '888888d28888a0f2'), (0xfffffff004c236ac, '087d44f9')],
        set_refresh_properties=0xfffffff004c23610,
        set_refresh_checks=[(0xfffffff004c23610, 'ff0302d1fd7b07a9fdc30191a0031ff8'),
                            (0xfffffff004c237ac, '086540f9219aff902148359100013fd6')],
        start_hardware_call=(0xfffffff004c0d5d4, '080040f9084944f900013fd6e0000037'),
        complete_current=(0xfffffff004c1ace4, 0xfffffff004c1acf4, 'e00740f928008052010100120bd5ff97'),
        completion_report_call=(0xfffffff004c1023c, '080040f9080545f9030080d2e20303aa0400805200013fd6'),
        main_display_call=(0xfffffff004bfe528, '080040f908e142f900013fd6c0000036')),
}


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=300, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


# Host tools.
objcopy = shutil.which('llvm-objcopy')
env = dict(os.environ)
if not shutil.which('brew'):
    require(objcopy, 'llvm-objcopy not found on PATH')
    shim = OUT / 'host-bin'
    shim.mkdir(exist_ok=True)
    (shim / 'brew').write_text('#!/bin/sh\n# Stand-in for "brew --prefix llvm" (scripts/build-native-hid-input.py): the prefix of PATH\'s llvm-objcopy.\n'
                               f'[ "$1" = --prefix ] && [ "$2" = llvm ] && exec echo {Path(objcopy).resolve().parent.parent}\n'
                               'echo "brew stand-in: only --prefix llvm" >&2; exit 1\n')
    (shim / 'brew').chmod(0o755)
    env['PATH'] = os.pathsep.join([str(shim), env.get('PATH', '')])
else:
    objcopy = objcopy or str(Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin/llvm-objcopy')

# The reference candidate, under the names audit-mobile-graphics-legacy.py and build-native-hid-input.py look for.
candidate = Path(os.environ.get('FAJITA_DISPLAY_CANDIDATE') or os.environ.get('FAJITA_STORAGE_CANDIDATE') or '')
require(candidate.is_file() and candidate.with_suffix('.json').is_file(),
        'FAJITA_DISPLAY_CANDIDATE (or FAJITA_STORAGE_CANDIDATE) must name a prelinked fajita kernel.macho with its kernel.json beside it')
prelink = ROOT / 'build/native-prelink'
prelink.mkdir(parents=True, exist_ok=True)
for name in ('native-owner-mobile-graphics-candidate', 'native-owner-ufs-cpu-desktop-fajita-candidate'):
    for suffix in ('.macho', '.json'):
        shutil.copyfile(candidate.with_suffix(suffix), prelink / (name + suffix))
analysis = ROOT / 'build/display-analysis/mobile-graphics-legacy-abi.json'
ORIGINAL_KERNELCACHES = {'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7': '21H461',
                         '2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221': '20H392'}
build = ORIGINAL_KERNELCACHES.get(json.loads(candidate.with_suffix('.json').read_text())['original_sha256'])
require(build, 'The candidate is not prelinked from an accepted original kernelcache (21H461 or 20H392)')
# ... and from the one the kernel is prelinked with (unixnu: board.toml [kernel.prelink] kernelcache_sha256).
pinned = os.environ.get('UNIXNU_KERNELCACHE_SHA256')
require(not pinned or json.loads(candidate.with_suffix('.json').read_text())['original_sha256'] == pinned,
        f'The candidate is not prelinked from the pinned original kernelcache {pinned}')
legacy = LEGACY[build]
if build == '21H461' and not analysis.exists():
    analysis.parent.mkdir(parents=True, exist_ok=True)
    analysis.write_text(json.dumps({'runtime_metadata_checkpoint_sha256': '3044c2beff552911605994c0dc7aa73182066cbb5aed0b396999ceeb5a96da2b',
                                    'seeded_by': 'scripts/build-native-legacy-display.py (fajita): recorded hash, no observation'}) + '\n')
print(run(['python3', ROOT / 'scripts/audit-mobile-graphics-legacy.py'], text=True), end='')
audit = json.loads(analysis.read_text())
require(audit.get('original_build') == build and audit['legacy_instance_bytes'] == legacy['size'] and
        audit['legacy_virtual_entries'] == legacy['slots'] and audit['legacy_metaclass'] == hex(legacy['metaclass']),
        'The audit qualified the same original class')
pure = audit['abstract_slots']
methods = audit['methods']
slots, size, field = legacy['slots'], legacy['size'], legacy['fields']
slot = {name: i for i, name in legacy['prototypes'].items()}
leaves = [i for i in pure if i not in (slot['supportedFrameInfo'], slot['collectCompletionReports'])]

# The private layout the driver and the display probe use (both include it), from the table above.
layout = f'''// Generated by scripts/build-native-legacy-display.py: the original {build} IOMobileFramebufferLegacy layout.
#pragma once
#define NATIVE_LEGACY_ORIGINAL_BUILD "{build}"
#define NATIVE_LEGACY_INSTANCE_BYTES {size:#x}
// Display: current swap, pending queue head and tail, vsync (type 5) client queue, reported width and height,
// completed and presented generations, retired frame count. The state words the probe dumps start at the width.
#define NATIVE_LEGACY_CURRENT_SWAP {field['current']}
#define NATIVE_LEGACY_PENDING_HEAD {field['head']}
#define NATIVE_LEGACY_PENDING_TAIL {field['tail']}
#define NATIVE_LEGACY_VSYNC_CLIENTS {field['vsync_clients']}
#define NATIVE_LEGACY_WIDTH {field['width']}
#define NATIVE_LEGACY_HEIGHT {field['height']}
#define NATIVE_LEGACY_COMPLETED_GENERATION {field['completed']}
#define NATIVE_LEGACY_PRESENTED_GENERATION {field['presented']}
#define NATIVE_LEGACY_RETIRED_FRAMES {field['frames']}
// Swap request: the four layer surfaces and the pending queue links (next, previous-link).
#define NATIVE_LEGACY_REQUEST_SURFACES {field['surfaces']}
#define NATIVE_LEGACY_REQUEST_NEXT {field['next']}
#define NATIVE_LEGACY_REQUEST_PREVIOUS {field['previous']}
// The original display state setter: the slot the family calls with the four-word power state request.
#define NATIVE_LEGACY_STATE_SETTER_SLOT {legacy['state_setter']}
// 1 when supportedFrameInfo (selector 74) has the trace-key mask output.
#define NATIVE_LEGACY_FRAME_INFO_MASK {int(legacy['frame_info_mask'])}
// The abstract backend leaves the driver guards: every abstract slot but supportedFrameInfo and collectCompletionReports.
#define NATIVE_LEGACY_GUARDED_LEAVES(X) {' '.join(f'X({i})' for i in leaves)}
'''
(OUT / 'OriginalLegacyLayout.h').write_text(layout)

# The original class ABI header, as sheng generates it, with the slots and fields of the qualified build.
header = f'''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
#include "OriginalLegacyLayout.h"
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {{
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[{size:#x}-sizeof(IOService)];
public:
 // Both fields are qualified by the complete original slot-{slot['getDisplayDimensions']} getter.
 // This modifies this object's own mode state, never a vtable or executable.
 void installBootDimensions(uint32_t width,uint32_t height) {{
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+{field['width']})=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+{field['height']})=height;
 }}
 const OriginalLegacySwapRequest *pendingSwapRequest() const {{
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+{field['head']},sizeof(request));return request;
 }}
 // The real normal scanout backend removes its pending head before calling
 // slot {slot['completeCurrentSwap']}. The discard path (terminal state 8) is not presentation.
 // The queue is a tail queue (head at +{field['head']}, tail pointer at +{field['tail']}, each request's next at +{field['next']} and previous-link at +{field['previous']}): taking the head
 // moves the next request, if a client already queued one behind it (int-132: swap 3 arrived during swap 2's GPU copy and the old
 // single-entry form refused to retire swap 2, so every wait on it stayed busy), up to the head. The original queue's reference
 // moves to the current-swap slot for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+{field['current']},8);memcpy(&head,self+{field['head']},8);memcpy(&tail,self+{field['tail']},8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+{field['next']},8);memcpy(&previous,bytes+{field['previous']},8);
  if(previous!=self+{field['head']})return false;
  if(next) {{
   const void *link=self+{field['head']};
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+{field['previous']},&link,8);
  }} else if(tail!=bytes+{field['next']})return false;
  memcpy(self+{field['current']},&request,8);
  if(next) {{memcpy(self+{field['head']},&next,8);}}
  else {{const void *empty=nullptr,*emptyTail=self+{field['head']};memcpy(self+{field['head']},&empty,8);memcpy(self+{field['tail']},&emptyTail,8);}}
  return true;
 }}
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
PROTOTYPES = {
    'isMainDisplay': 'virtual bool isMainDisplay();',
    'getDotPitch': 'virtual IOReturn getDotPitch(uint32_t *);',
    'supportedFrameInfo': ('virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;' if legacy['frame_info_mask'] else
                           'virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *)=0;'),
    'getDisplayDimensions': 'virtual void getDisplayDimensions(uint32_t *,uint32_t *);',
    'kickSwapQueue': 'virtual void kickSwapQueue();',
    'validateSwap': 'virtual bool validateSwap(const OriginalLegacySwapRequest *);',
    'completeCurrentSwap': 'virtual void completeCurrentSwap();',
    'collectCompletionReports': 'virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;',
    'startHardware': 'virtual bool startHardware();',
    'getRefreshAttributes': 'virtual bool getRefreshAttributes(uint64_t *,bool);'}
for i in range(168, slots):
    if i in legacy['prototypes']:
        header += ' ' + PROTOTYPES[legacy['prototypes'][i]] + '\n'
    elif i in pure:
        header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:
        header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header += f'}};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)=={size:#x},"Actual original instance extent");\n'
(OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True, env=env), end='')

# The compiled vtable: exactly the original entry count, only qualified own methods, every other entry bound to the original.
obj = OUT / 'NativeLegacyDisplay.o'
raw = obj.read_bytes()
image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
relocations = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]
compiled = [relocations.get(vtable + 16 + i * 8) for i in range(slots)]
require(all(compiled) and vtable + 16 + slots * 8 not in relocations, f'Exact owned {slots}-entry vtable')
reference = prelink / 'native-owner-mobile-graphics-candidate.macho'
old = reference.read_bytes()
old_image = macho(old)
names = {n: v for v, group in exports(old, old_image).items() for n in group}


def read(va, n):
    at = backed_offset(old_image['segments'], va, n, len(old))
    return old[at:at + n]


def instructions(checks, why):
    for va, expected in checks:
        require(read(va, len(expected) // 2) == bytes.fromhex(expected), why)


def load_slot(va, index, why):
    """The vtable load ldr x8, [x8, #index*8] at va reads the given slot."""
    require(struct.unpack('<I', read(va, 4))[0] == 0xf9400108 | index << 10, why)


bindings = {'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': legacy['constructor'],
            '__ZN25IOMobileFramebufferLegacyD2Ev': legacy['destructor'],
            '__ZN25IOMobileFramebufferLegacy10gMetaClassE': legacy['metaclass'],
            '__ZTV25IOMobileFramebufferLegacy': int(audit['legacy_vptr'], 16) - 16}
OWNED = tuple(sorted((0, 1, 7, 14, 84, 86, 87, 141, 155) + tuple(slot[name] for name in (
    'isMainDisplay', 'getDotPitch', 'startHardware', 'kickSwapQueue', 'validateSwap', 'getRefreshAttributes'))))
owned = []
native = []
for i, name in enumerate(compiled):
    expected = int(methods[i]['address'], 16)
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i)
        require(i in OWNED or i in pure, 'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, 'No unresolved original abstract backend target')
        bindings[name] = expected
    else:
        require(i < 168 and names.get(name) == expected, 'Actual inherited native base symbol ' + str(i))
        native.append(i)
require(owned == sorted(list(OWNED) + pure), 'Exact owned method set')
undefined = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
require(undefined == set(bindings), 'All and only actual original-class imports qualified')
# The original IOSurface getters, observed through the real original swap request on sheng (21H461) and, on 20H392, the ones the
# family calls from the same places: each is one load of the named field.
for name, address, offset, wide in legacy['surface']:
    expected = (0xf9400000 if wide else 0xb9400000) | ((offset // (8 if wide else 4)) << 10)
    require(read(address, 8) == struct.pack('<2I', expected, 0xd65f03c0), 'Exact original surface getter ' + name)
    bindings['_native_legacy_surface_' + name] = address
# The original display-refresh notification sender (vsync message type 5 to the clients queued at the vsync client queue).
instructions(legacy['vsync_checks'], 'Original vsync sender, its client queue and six-word drop-allowed async send')
require(struct.unpack('<I', read(legacy['vsync_checks'][1][0], 4))[0] == 0xf9400108 | (field['vsync_clients'] // 8) << 10,
        'Original vsync sender walks the client queue the driver tests')
bindings['_native_legacy_vsync_notify'] = legacy['vsync_notify']
# The getRefreshAttributes slot fills the refresh words with the 1/60 s minimum; setDisplayRefreshProperties publishes them as
# IOMFBDisplayRefresh.
require(int(methods[slot['getRefreshAttributes']]['address'], 16) == legacy['refresh_attributes'],
        'Original refresh-attributes slot')
load_slot(legacy['refresh_checks'][1][0], slot['getRefreshAttributes'], 'setDisplayRefreshProperties calls the refresh-attributes slot')
instructions(legacy['refresh_checks'], 'Original refresh words with the 1/60 s minimum, called by setDisplayRefreshProperties')
instructions(legacy['set_refresh_checks'], 'Original setDisplayRefreshProperties publishing IOMFBDisplayRefresh through setProperty')
key = legacy['set_refresh_checks'][1][0] + 4
page, add = struct.unpack('<2I', read(key, 8))
pages = ((page >> 5) & 0x7ffff) << 2 | (page >> 29) & 3
key = (key & ~4095) + ((pages - (1 << 21) if pages >> 20 else pages) << 12) + (add >> 10 & 4095)
require(read(key, 20) == b'IOMFBDisplayRefresh\0', 'setDisplayRefreshProperties sets IOMFBDisplayRefresh')
bindings['_native_legacy_set_refresh_properties'] = legacy['set_refresh_properties']


def branch(pc):
    word = struct.unpack('<I', read(pc, 4))[0]
    require(word & 0xfc000000 == 0x94000000, 'Actual protected base call')
    value = word & 0x3ffffff
    return pc + 4 * (value - (1 << 26) if value & (1 << 25) else value)


require(all(branch(pc) == target for pc, target in legacy['base_calls']),
        'Original protected Legacy C2/D2 really call their original Service bases')
for check, name, why in [
        (legacy['start_hardware_call'], 'startHardware', 'Original bool start_hardware() call and result check'),
        (legacy['completion_report_call'], 'collectCompletionReports',
         'Original completion calls optional report slot with null outputs and zero limit'),
        (legacy['main_display_call'], 'isMainDisplay', 'Original isMainDisplay dispatch uses only self and tests boolean bit zero')]:
    load_slot(check[0] + 4, slot[name], why)
    instructions([check], why)
getter = int(methods[slot['getDisplayDimensions']]['address'], 16)
for offset, word in [(0x30, field['width']), (0x58, field['height'])]:
    require(struct.unpack('<I', read(getter + offset, 4))[0] == 0xb9400000 | ((word // 4) << 10) | (8 << 5) | 8,
            'Actual original width/height getter fields')
complete, at, expected = legacy['complete_current']
require(int(methods[slot['completeCurrentSwap']]['address'], 16) == complete and read(at, 16) == bytes.fromhex(expected),
        'Actual original completeCurrentSwap passes true to the normal current completion body')
# The state setter the driver calls by slot (announcePoweredByFirmware): the original one, writing the requested and powered words.
setter = int(methods[legacy['state_setter']]['address'], 16)
require(setter == legacy['state_setter_address'], 'Actual original display state setter slot')
for offset, word, encoding in [(0x68, field['power_requested'], 0xb9400129), (0x88, field['power_requested'], 0xb9000128),
                               (0xbc, field['powered'], 0xb9400129), (0xdc, field['powered'], 0xb9000128)]:
    require(struct.unpack('<I', read(setter + offset, 4))[0] == encoding | (word // 4) << 10, 'Original state setter power words')
asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(bindings.items())))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True, env=env).strip()
# The deployment target of the kernel objects (scripts/build-arm-driver.py).
ios_min = os.environ.get('UNIXNU_XNU_IOS_MIN', '17.0')
run(['xcrun', 'clang', '-target', f'arm64-apple-ios{ios_min}', '-isysroot', sdk, '-c', asm, '-o', OUT / 'original-bindings.o'], env=env)


def code(data, info):
    sec = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[sec['fileoff']:sec['fileoff'] + sec['size']]


def late_init(directory, name):
    """Drop the object's static initializer registration and make it callable: the board probe runs it when the class registers."""
    source, late = directory / f'{name}.o', directory / f'{name}.late.o'
    run([objcopy, '--remove-section=__DATA,__mod_init_func', f'--globalize-symbol=__GLOBAL__sub_I_{name}.cpp', source, late])
    require(code(source.read_bytes(), macho(source.read_bytes())) == code(late.read_bytes(), macho(late.read_bytes())),
            f'Owned {name} instructions unchanged by late init staging')
    return late


late = late_init(OUT, 'NativeLegacyDisplay')

# Board probes. C++ with the IOKit flags of IOPlatformExpert.cpp, C with the osfmk flags of startup.c, as sheng's script.
r = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
cpp = []
args = r['arguments']
i = 0
placeholder_src, placeholder_obj = '@SOURCE@', '@OBJECT@'
while i < len(args):
    value = args[i]
    if value == r['file']:
        cpp.append(placeholder_src)
    elif value == '-o':
        cpp += ['-o', placeholder_obj]
        i += 1
    elif value == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif not value.startswith('-W'):
        cpp.append(value)
    i += 1
cpp += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        # As scripts/build-arm-driver.py: upstream clang's -Wextra has it and XNU's kalloc_type macros trip it.
        '-Wno-unknown-warning-option', '-Wno-missing-designated-field-initializers',
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
        # OriginalLegacyLayout.h: LegacyDisplayProbe.cpp reads the display's state words and the swap request.
        '-I', str(OUT)]
pr = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
c = []
pa = pr['arguments']
j = 0
while j < len(pa):
    value = pa[j]
    if value == pr['file']:
        c.append(placeholder_src)
    elif value in ('-o', '-MF', '-MT', '-MJ'):
        if value == '-o':
            c += ['-o', placeholder_obj]
        j += 1
    elif value == '-Xclang' and pa[j + 1].startswith('-W'):
        j += 1
    elif not value.startswith('-W'):
        c.append(value)
    j += 1
c += ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only', '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(KIT)]


def compile_with(template, record, source, extra=()):
    obj = OUT / (source.stem + '.o')
    command = [str(source) if a == placeholder_src else str(obj) if a == placeholder_obj else a for a in template] + list(extra)
    run(command, cwd=record['directory'], env=env)
    return obj


probes = [KIT / 'LegacyDisplayProbe.cpp', KIT / 'HidInputProbe.cpp', KIT / 'KeyStoreProbe.cpp']
probe_objs = [compile_with(cpp, r, source) for source in probes]
c_sources = [KIT / 'BootProcessProbe.c', KIT / 'BootUiTraceProbe.c']
probe_objs += [compile_with(c, pr, source) for source in c_sources]

# The drivers: NativeHidInput through UniXNU's qualification of the original IOHIDDevice, then NativeKeyStoreEvents and the presenter.
print(run(['python3', ROOT / 'scripts/build-native-hid-input.py'], text=True, env=env), end='')
hid_dir = ROOT / 'build/native-hid-input'
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeKeyStoreEvents'], text=True, env=env), end='')
keyevents_late = late_init(ROOT / 'build/native-keystore-events', 'NativeKeyStoreEvents')
print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeGOPPresenter'], text=True, env=env), end='')
gop_late = late_init(ROOT / 'build/native-gop-presenter', 'NativeGOPPresenter')

members = [late, OUT / 'original-bindings.o', *probe_objs, hid_dir / 'NativeHidInput.late.o', hid_dir / 'hid-original-bindings.o',
           keyevents_late, gop_late]
archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, *members], env=env)
# The kernel link force-loads the archive next to libBootProgress.a; their strong symbols must not collide.
def strong_symbols(path):
    return {line.split()[-1] for line in run(['xcrun', 'nm', '-gUm', path], text=True, env=env).splitlines()
            if ' external ' in line and ' weak external ' not in line}


defined = {}
for member in members:
    for name in strong_symbols(member):
        require(name not in defined, f'{name} defined by both {defined.get(name)} and {member.name}')
        defined[name] = member.name
# Its weak stand-ins (kits/fajita/BootProgressStubs.c) give way to these definitions; a strong symbol there would collide.
progress = ROOT / 'build/boot-progress/libBootProgress.a'
if progress.is_file():
    clash = sorted(set(defined) & strong_symbols(progress))
    require(not clash, 'Strong symbols of libBootProgress.a defined again: ' + ', '.join(clash[:8]))

sources = [ROOT / 'drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp', ROOT / 'drivers/NativeMobileDisplay/NativeLegacyRequest.h',
           ROOT / 'drivers/NativeMobileDisplay/NativeLegacyScale.h', ROOT / 'drivers/NativeMobileDisplay/FajitaDisplayLogical.h',
           ROOT / 'drivers/NativeMobileDisplay/NativeGOPPresenter.cpp', ROOT / 'drivers/NativeMobileDisplay/NativeGOPWire.h',
           *probes, *c_sources, KIT / 'BootUiTraceCore.h', KIT / 'BootUiTraceDetails.h',
           ROOT / 'drivers/NativeHidInput/NativeHidInput.cpp', ROOT / 'scripts/build-native-hid-input.py',
           ROOT / 'drivers/NativeKeyStoreEvents/NativeKeyStoreEvents.cpp', Path(__file__).resolve()]
report = dict(
    source_sha256={str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p): sha(p) for p in sources},
    original_build=build, generated_abi_sha256=sha(OUT / 'OriginalLegacyFramebufferABI.h'),
    generated_layout_sha256=sha(OUT / 'OriginalLegacyLayout.h'),
    original_candidate_sha256=sha(reference), original_audit_sha256=sha(analysis),
    compiled_object_sha256=sha(obj), late_object_sha256=sha(late), archive_sha256=sha(archive),
    members=[m.name for m in members],
    bindings={n: hex(v) for n, v in bindings.items()}, virtual_entries=slots, owned_entries=owned, inherited_native_entries=native,
    guarded_backend_leaves=leaves, original_instructions_modified=False,
    left_out=['BootLog.c', 'BootReset.c', 'TouchProbe.cpp', 'GeniSpi.c', 'NvtProtocol.c', 'NvtTouchCore.c', 'NvtFirmware.c',
              'CpuFreqProbe.cpp', 'GpuPowerProbe.cpp'],
    driver_initialized=False, display_presented=False, desktop=False)
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
LOGS.mkdir(parents=True, exist_ok=True)
shutil.copyfile(OUT / 'build-report.json', LOGS / 'native-legacy-display-build-report.json')
print(f'PASS: fajita Legacy display archive {archive.name} ({len(members)} members), original Legacy and HID bindings qualified '
      f'against candidate {report["original_candidate_sha256"][:16]}; runtime lifecycle unqualified')
