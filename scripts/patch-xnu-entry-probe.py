#!/usr/bin/env python3
# From UniXNU-sheng scripts/patch-xnu-entry-probe.py (BSD-2-Clause-Patent).
"""Put boot-progress probes into a COPY of the source XNU kernel (never the original).

There is no serial console on the device and the kernel cannot draw before its
own video console exists, so the early boot stages are reported by side
channels. The stage code lives in the zero padding after the kernel entry
(_start is one `b`) and displaced instructions are re-executed there:

  stage 1 (kernel entry, MMU off)       blue bar, then continues
  stage 2 (just before the MMU is on)   magenta bar, then continues
  stage 3 (right after the MMU is on)   waits --reset-delay seconds, then PSCI
                                        SYSTEM_RESET. A reboot by itself after
                                        that time shows the CPU got through the
                                        MMU enable; the frame buffer is not
                                        mapped by then, so time is the signal.
  panic screen (--panic-screen)         panic() and its common panic funnel draw
                                        their format string on the
                                        kernel's own frame buffer mapping and
                                        stops (scripts/xnu-panic-screen.c).
                                        --no-reset leaves out stages 3 and 4.
  stage 4 (--reset-after SYMBOL)        the same wait and reset, but placed after
                                        the call to SYMBOL in --in FUNCTION (the
                                        Nth call, --occurrence). It replaces
                                        stage 3, so one run asks one question:
                                        did the kernel get past that call?

The bars are the 1/16th bands of the frame buffer below the green one that
DarwinBoot paints (band index = stage). Each patch site is checked against its
expected original instruction first, and the tool refuses a kernel that does
not match. Offsets are specific to the kernel built by build-xnu-4k-baseline.sh.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ENTRY_VA = 0xFFFFFFF00B9D0488        # LC_UNIXTHREAD pc, `b start_first_cpu`
START_FIRST_CPU = 0xFFFFFFF00B9D205C
MMU_ENABLE_VA = 0xFFFFFFF00B9D2330   # msr SCTLR_EL1, x0
AFTER_MMU_VA = 0xFFFFFFF00B9D2350    # mov x0, #0x300000
PANIC_VA = 0xFFFFFFF00BF69B04      # _panic
VINFO_VA = 0xFFFFFFF00C0A2970      # _vinfo
PANIC_CAVE_END = 0xFFFFFFF00B9D0FF0
CAVE_SIZE = 0x60
CAVES = {1: 0xFFFFFFF00B9D0500, 2: 0xFFFFFFF00B9D0560, 3: 0xFFFFFFF00B9D05C0, 4: 0xFFFFFFF00B9D0620,
         5: 0}   # 5: the early watchdog, placed by early_wdog_cave()
TIMEBASE = 19200000

PAINT = """
  ldr x1, [{args}, #0x28]
  ldr x2, [{args}, #0x38]
  ldr x3, [{args}, #0x48]
  mul x2, x2, x3
  lsr x2, x2, #4
  bic x2, x2, #7
  mov x4, #{band}
  mul x5, x2, x4
  add x1, x1, x5
  movz w6, #{lo}
  movk w6, #{hi}, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
"""

SOURCES = {
    1: ".text\n.global _start\n_start:\n" + PAINT.format(args='x0', band=1, lo='0x00FF', hi='0xFF00') + "  b target\n",
    2: ".text\n.global _start\n_start:\n" + PAINT.format(args='x20', band=2, lo='0x00FF', hi='0xFFFF')
       + "  msr SCTLR_EL1, x0\n  b target\n",
    3: """.text
.global _start
_start:
  mov x0, #0x300000
  mrs x1, cntpct_el0
  ldr x2, ={ticks}
  add x2, x1, x2
1:
  isb
  mrs x1, cntpct_el0
  cmp x1, x2
  b.lo 1b
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
2:
  wfe
  b 2b
""" + ".ltorg\n",
    4: """.text
.global _start
_start:
  bl callee
  mrs x1, cntpct_el0
  ldr x2, ={ticks}
  add x2, x1, x2
1:
  isb
  mrs x1, cntpct_el0
  cmp x1, x2
  b.lo 1b
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
2:
  wfe
  b 2b
""" + ".ltorg\n",
}

# fajita: stage 5 (FAJITA_EARLY_WDOG=1), chained after stage 1 with the MMU still off: the unattended-run backstop from the kernel
# entry on, for hangs before ArmCpu takes it over (fajita-wdog=3 reprograms and pets it). First what ArmCpu's scmDisableCrashdump
# does, so a bite lands in fastboot and not in Qualcomm CrashDump Mode: SCM SDI off (0xC2000109), download mode off (0xC2000110),
# and if TCSR_BOOT_MISC_DETECT (0x01FD3000, secure read 0xC2000501) still has bit 0x10, the secure write (0xC2000502). Then the
# restart reason bootloader (IMEM 0x146bf65c = 0x77665500, the cookie the ABL reads) and the APSS watchdog (0x17980000) with bark
# 16 s and bite 32 s, counting without its interrupt. x0 (the boot arguments) is kept in x28 across the SMCs.
EARLY_WDOG = """.text
.global _start
_start:
  mov x28, x0
  movz x0, #0x0109
  movk x0, #0xc200, lsl #16
  mov x1, #2
  mov x2, #1
  mov x3, xzr
  mov x4, xzr
  mov x5, xzr
  smc #0
  movz x0, #0x0110
  movk x0, #0xc200, lsl #16
  mov x1, #2
  mov x2, xzr
  mov x3, xzr
  mov x4, xzr
  mov x5, xzr
  smc #0
  movz x0, #0x0501
  movk x0, #0xc200, lsl #16
  mov x1, #1
  movz x2, #0x3000
  movk x2, #0x01fd, lsl #16
  mov x3, xzr
  mov x4, xzr
  mov x5, xzr
  smc #0
  cbnz x0, 1f
  tbz x1, #4, 1f
  bic x3, x1, #0x10
  movz x0, #0x0502
  movk x0, #0xc200, lsl #16
  mov x1, #2
  movz x2, #0x3000
  movk x2, #0x01fd, lsl #16
  mov x4, xzr
  mov x5, xzr
  smc #0
1:
  mov x0, x28
  movz x9, #0xf65c
  movk x9, #0x146b, lsl #16
  movz w10, #0x5500
  movk w10, #0x7766, lsl #16
  str w10, [x9]
  movz x9, #0x1798, lsl #16
  str wzr, [x9, #8]
  movz w10, #0xffff
  movk w10, #0x7, lsl #16
  str w10, [x9, #0x10]
  movz w10, #0xffff
  movk w10, #0xf, lsl #16
  str w10, [x9, #0x14]
  mov w10, #1
  str w10, [x9, #4]
  str w10, [x9, #8]
  dsb sy
  b target
"""


def amfi_skip_pmgr(data, plan):
    """fajita iOS 16: set the 20H392 AMFI's 'running on a VMM' byte, so it skips its ApplePMGRNub wait and PMGRAON latch.

    AMFI's early initializer sets the byte when sysctlbyname("kern.hv_vmm_present") answers nonzero, but it runs before
    the sysctl tree exists, so the lookup fails and the byte stays 0; the PMGR wait (30 s, then panic) follows. The byte is
    found from the code that sets it (ldr w8,[sp,#0xc]; cbz w8; adrp x8; mov w9,#1; strb w9,[x8,#imm]) in AMFI's text.
    """
    text = next(e for e in plan['extents']
                if e['module'] == 'com.apple.driver.AppleMobileFileIntegrity' and e['segment'] == '__TEXT_EXEC')
    found = []
    for va in range(text['va'], text['va'] + text['size'] - 20, 4):
        ldr, cbz, adrp, mov, strb = struct.unpack_from('<5I', data, file_offset(va))
        if (ldr == 0xb9400fe8 and cbz & 0xff00001f == 0x34000008 and adrp & 0x9f00001f == 0x90000008 and mov == 0x52800029
                and strb & 0xffc003ff == 0x39000109):
            immediate = ((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3)
            if immediate & (1 << 20):
                immediate -= 1 << 21
            found.append(((va + 8) & ~4095) + (immediate << 12) + ((strb >> 10) & 4095))
    if len(found) != 1 or data[file_offset(found[0])] != 0:
        raise SystemExit(f'AMFI vmm byte not uniquely found unset: {[hex(v) for v in found]}')
    data[file_offset(found[0])] = 1
    return found[0]


def img4_skip_pmap_cs(data, plan):
    """fajita iOS 16: leave out the 20H392 AppleImage4's "pmap cs" image4 runtime (the PPL code signing runtime).

    Its init asks the pmap for a mix-n-match policy, which this kernel (no PPL) does not answer, and panics ("failed to get
    mix-n-match policy"). The runtimes are the __DATA_CONST,__img4_rt entries {version, name, init, ...}; the start skips an
    entry whose init is NULL (as it does for "pmap dtless"), so the init pointer of "pmap cs" is cleared.
    """
    extent = lambda segment: next(e for e in plan['extents']
                                  if e['module'] == 'com.apple.security.AppleImage4' and e['segment'] == segment)
    text, const = extent('__TEXT'), extent('__DATA_CONST')
    blob = bytes(data[file_offset(text['va']):file_offset(text['va']) + text['size']])
    at = blob.find(b'\0pmap cs\0')
    if at < 0 or blob.find(b'\0pmap cs\0', at + 1) >= 0:
        raise SystemExit('AppleImage4 "pmap cs" name not found once')
    name = text['va'] + at + 1
    entries = [va for va in range(const['va'], const['va'] + const['size'] - 24, 8)
               if struct.unpack_from('<Q', data, file_offset(va + 8))[0] == name]
    if len(entries) != 1 or not struct.unpack_from('<Q', data, file_offset(entries[0] + 16))[0]:
        raise SystemExit(f'AppleImage4 "pmap cs" runtime not found once with an init: {[hex(v) for v in entries]}')
    struct.pack_into('<Q', data, file_offset(entries[0] + 16), 0)
    return entries[0]


def early_wdog_cave(data, size):
    """The first 0x100-aligned run of zero padding of `size` bytes after the panic screen's padding, within 16 KiB."""
    va = (PANIC_CAVE_END + 0xff) & ~0xff
    while va + size <= PANIC_CAVE_END + 0x4000:
        try:
            if not any(data[file_offset(va):file_offset(va) + size]) and not any(data[file_offset(va) - 4:file_offset(va)]):
                return va
        except SystemExit:
            break
        va += 0x100
    raise SystemExit('No zero padding for the early watchdog after the panic screen padding')


RESET_BEFORE = """.text
.global _start
_start:
  movz w0, #0x0009
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
1:
  wfe
  b 1b
"""


def kernel_symbols(kernel):
    """Address -> name for the kernel's symbols, from llvm-nm."""
    nm = shutil.which('llvm-nm') or '/opt/homebrew/opt/llvm/bin/llvm-nm'
    table = {}
    # fajita: FAJITA_SYMBOL_KERNEL names the unstripped XNU image of this kernel (same addresses) when the prelinked one has lost
    # its function symbols (the 20H392 prelink keeps data symbols only).
    kernel = os.environ.get('FAJITA_SYMBOL_KERNEL') or kernel
    for line in subprocess.run([nm, '-n', kernel], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table.setdefault(int(parts[0], 16), parts[2])
    return table


def function_range(symbols, name):
    """Start and end (next symbol) of a function by its symbol name."""
    starts = sorted(symbols)
    for start, symbol in symbols.items():
        if symbol == name:
            return start, next(address for address in starts if address > start)
    raise SystemExit(f'No symbol {name} in the kernel')


def find_call(data, symbols, function, name, occurrence):
    """The VA and target of the Nth `bl name` inside FUNCTION."""
    hits = []
    low, high = function_range(symbols, function)
    for va in range(low, high, 4):
        word = struct.unpack_from('<I', data, file_offset(va))[0]
        if word >> 26 == 0b100101:
            delta = word & 0x3FFFFFF
            delta -= (1 << 26) if delta & (1 << 25) else 0
            target = va + delta * 4
            if symbols.get(target) == name:
                hits.append((va, target))
    if len(hits) < occurrence:
        raise SystemExit(f'{function} has {len(hits)} call(s) to {name}, not {occurrence}')
    return hits[occurrence - 1]


def assemble_c(source, va, workdir, symbols):
    """Build a freestanding C file for VA; returns (bytes, load address of the first byte, address of panic_screen).

    The linker may start .text a little after VA (it keeps the ELF headers in front), so the
    load address is read back from the ELF and is what the caller must write the bytes to."""
    clang = shutil.which('clang') or '/opt/homebrew/opt/llvm/bin/clang'
    lld = shutil.which('ld.lld') or '/opt/homebrew/opt/llvm/bin/ld.lld'
    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    nm = shutil.which('llvm-nm') or '/opt/homebrew/opt/llvm/bin/llvm-nm'
    # UNIXNU_UEFI is the Mu-Silicium checkout when the unixnu driver runs this script.
    font_dir = Path(os.environ.get('UNIXNU_UEFI') or ROOT / 'uefi-sheng') / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'
    obj, elf, binary = (workdir / name for name in ('panic.o', 'panic.elf', 'panic.bin'))
    definitions = []
    for macro, name, width in (('MAX_MEM', '_max_mem', 8),
                               ('PAGE_TOTAL', '_vm_page_pages', 4),
                               ('PAGE_FREE', '_vm_page_free_count', 4)):
        address = symbols.get(name)
        if address is None or address % width or not any(
                base <= address and address + width <= base + size
                for base, size, _, _ in SEGMENTS):
            raise SystemExit(f'Missing or invalid diagnostic symbol {name}')
        definitions.append(f'-D{macro}={address:#x}UL')
    # Optional: the board's banner buffer for the panic message (kits/sheng/BootProgress.c); kernels without it get 0 (no copy).
    text = symbols.get('_boot_progress_panic_text')
    if text is not None:
        if not any(base <= text and text + 256 <= base + size for base, size, _, _ in SEGMENTS):
            raise SystemExit('Invalid _boot_progress_panic_text')
        definitions.append(f'-DPANIC_TEXT={text:#x}UL')
    # fajita: the boot log's panic flush (package/BootLog.c), when linked, and the reset delay of --panic-reset.
    flush = symbols.get('_boot_log_panic_flush')
    if flush is not None and PANIC_RESET_SECONDS:
        if not any(base <= flush < base + size for base, size, _, _ in SEGMENTS):
            raise SystemExit('Invalid _boot_log_panic_flush')
        definitions.append(f'-DPANIC_FLUSH={flush:#x}UL')
    definitions.append(f'-DPANIC_RESET_S={PANIC_RESET_SECONDS}')
    print('panic screen:', 'flush' if flush is not None and PANIC_RESET_SECONDS else 'no flush', f'reset {PANIC_RESET_SECONDS}s' if PANIC_RESET_SECONDS else 'no reset')
    subprocess.run([clang, '--target=aarch64-none-elf', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                    '-mgeneral-regs-only', '-fno-pic', '-mcmodel=tiny', '-Oz', f'-DVINFO={VINFO_VA:#x}UL',
                    *definitions, f'-I{font_dir}', '-c', source, '-o', obj], check=True)
    script = workdir / 'panic.ld'
    script.write_text('SECTIONS { . = %#x; .text : { *(.text.a_entry) *(.text.a_common) *(.text*) *(.rodata*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.*) } }\n' % va)
    size = shutil.which('llvm-size') or '/opt/homebrew/opt/llvm/bin/llvm-size'
    sizes = subprocess.run([size, '--format=berkeley', obj], capture_output=True, text=True, check=True).stdout.splitlines()[-1].split()
    if int(sizes[1]) or int(sizes[2]):
        # The code runs from the kernel's read-only text: a global would fault on its first store.
        raise SystemExit(f'{source.name} has writable data (data {sizes[1]}, bss {sizes[2]}); use the stack')
    subprocess.run([lld, '-T', script, '-e', 'panic_screen', obj, '-o', elf], check=True)
    # Only .text: a plain binary dump would start at the ELF headers kept in front of it.
    subprocess.run([objcopy, '-O', 'binary', '-j', '.text', elf, binary], check=True)
    readelf = shutil.which('llvm-readelf') or '/opt/homebrew/opt/llvm/bin/llvm-readelf'
    load = None
    for line in subprocess.run([readelf, '-S', elf], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.replace('[', ' ').replace(']', ' ').split()
        if len(parts) >= 4 and parts[1] == '.text':
            load = int(parts[3], 16)
    if load is None or load < va:
        raise SystemExit('Cannot find where the linker put .text')
    entry = None
    for line in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == 'panic_screen':
            entry = int(parts[0], 16)
    if entry != load:
        raise SystemExit(f'panic_screen at {entry:#x} is not the first byte {load:#x}')
    return binary.read_bytes(), load, entry


def panic_common_entry(workdir, code, load):
    """Resolve the real alternate entry and verify its distinct argument ABI."""
    nm = shutil.which('llvm-nm') or '/opt/homebrew/opt/llvm/bin/llvm-nm'
    names = {parts[2]: int(parts[0], 16)
             for line in subprocess.check_output([nm, workdir / 'panic.elf'], text=True).splitlines()
             if len(parts := line.split()) == 3}
    entry = names.get('panic_screen_common')
    main = names.get('panic_screen_main')
    if entry is None or main is None or not load <= entry <= load + len(code) - 8:
        raise SystemExit('Common panic renderer entry is missing or out of bounds')
    at = entry - load
    if code[at:at + 8] != struct.pack('<II', 0xf9400021, branch(entry + 4, main)):
        raise SystemExit('Common panic entry does not dereference the Darwin va_list pointer and branch to the renderer')
    return entry


ROOT = Path(__file__).resolve().parents[1]


SEGMENTS = []   # (vmaddr, vmsize, fileoff, filesize) of the kernel being patched


def load_segments(data):
    """Read the Mach-O segment table; the file layout differs between kernel builds."""
    magic, _, _, _, ncmds = struct.unpack_from('<IiiII', data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit('Not a 64-bit Mach-O')
    offset = 32
    SEGMENTS.clear()
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x19:
            _, vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<16sQQQQ', data, offset + 8)
            if filesize:
                SEGMENTS.append((vmaddr, vmsize, fileoff, filesize))
        offset += size


def unixthread_pc(data):
    """LC_UNIXTHREAD entry pc (arm64 thread state: flavor, count, x0..x28, fp, lr, sp, pc)."""
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, offset)
        if cmd == 0x5:
            return struct.unpack_from('<Q', data, offset + 16 + 32 * 8)[0]
        offset += size
    raise SystemExit('No LC_UNIXTHREAD in kernel')


def rebase(kernel, data):
    """The probe sites are fixed offsets from the entry; shift them for another kernel build.

    _panic and _vinfo come from the symbol table when the kernel has one (the plain source
    kernel does); the retained candidate layout is the unshifted case.
    """
    global ENTRY_VA, START_FIRST_CPU, MMU_ENABLE_VA, AFTER_MMU_VA, PANIC_VA, VINFO_VA, PANIC_CAVE_END, CAVES
    delta = unixthread_pc(data) - ENTRY_VA
    if delta:
        ENTRY_VA += delta
        START_FIRST_CPU += delta
        MMU_ENABLE_VA += delta
        AFTER_MMU_VA += delta
        PANIC_CAVE_END += delta
        CAVES = {stage: va + delta for stage, va in CAVES.items()}
        PANIC_VA += delta
        VINFO_VA += delta
    names = {name: va for va, name in kernel_symbols(kernel).items()}
    if '_start_first_cpu' in names:
        START_FIRST_CPU = names['_start_first_cpu']
        # Native bootstrap table-capacity checks can grow before SCTLR setup.
        # Locate the unique instruction sequence in this bounded entry body;
        # do not assume a constant offset from the Mach-O entry point.
        matches = []
        for va in range(START_FIRST_CPU, START_FIRST_CPU + 0x1000, 4):
            def instruction(at):
                return struct.unpack_from('<I', data, file_offset(at))[0]
            if (instruction(va) == 0xD5181000 and
                    instruction(va + 4) == 0xD5033FDF and
                    instruction(va + 0x20) == 0xD2A00600 and
                    instruction(va + 0x24) == 0xD5181040):
                matches.append(va)
        if len(matches) != 1:
            raise SystemExit('Require one verified SCTLR/ISB/CPACR entry sequence')
        MMU_ENABLE_VA = matches[0]
        AFTER_MMU_VA = MMU_ENABLE_VA + 0x20
    if '_panic' in names and '_vinfo' in names:
        PANIC_VA = names['_panic']
        # Board diagnostics seed this ABI-compatible prefix from valid console
        # mappings, then pin it to their retained GOP mapping. Ordinary source
        # kernels without the observer retain the original vinfo path.
        VINFO_VA = names.get('_boot_progress_panic_vinfo', names['_vinfo'])
    return names


def file_offset(va):
    for vmaddr, vmsize, fileoff, filesize in SEGMENTS:
        if vmaddr <= va < vmaddr + min(vmsize, filesize):
            return fileoff + (va - vmaddr)
    raise SystemExit(f'{va:#x} is not in a file-backed segment of this kernel')


def branch(from_va, to_va):
    delta = (to_va - from_va) // 4
    if (to_va - from_va) % 4 or not -(1 << 25) <= delta < (1 << 25):
        raise SystemExit('Branch out of range')
    return 0x14000000 | (delta & 0x3FFFFFF)


def assemble(source, va, symbols, workdir):
    asm, obj, elf, binary = (workdir / name for name in ('cave.S', 'cave.o', 'cave.elf', 'cave.bin'))
    asm.write_text(source)
    clang = shutil.which('clang') or '/opt/homebrew/opt/llvm/bin/clang'
    lld = shutil.which('ld.lld') or '/opt/homebrew/opt/llvm/bin/ld.lld'
    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    subprocess.run([clang, '--target=aarch64-none-elf', '-c', asm, '-o', obj], check=True)
    subprocess.run([lld, f'-Ttext={va:#x}', *[f'--defsym={name}={value:#x}' for name, value in symbols.items()], '-e', '_start', obj, '-o', elf], check=True)
    subprocess.run([objcopy, '-O', 'binary', elf, binary], check=True)
    return binary.read_bytes()


PANIC_RESET_SECONDS = 0
BLR_REGISTER = [None]   # fajita: the register of a raw blr call site (FAJITA_RESET_AFTER=0x...)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('kernel', type=Path, help='the COPY of the kernel to patch in place')
    parser.add_argument('--reset-delay', type=int, default=6, help='seconds before the reset')
    parser.add_argument('--reset-after', metavar='SYMBOL', help='place the reset after this call in arm_init instead of stage 3')
    parser.add_argument('--reset-before', action='store_true', help='reset at the call site before invoking SYMBOL (requires --reset-after)')
    parser.add_argument('--panic-screen', action='store_true', help='draw panic() messages on the screen')
    parser.add_argument('--no-reset', action='store_true', help='no reset probe at all')
    parser.add_argument('--panic-reset', type=int, default=int(os.environ.get('FAJITA_PANIC_RESET', '0')),
                        help='fajita: after drawing a panic, flush the boot log and reset after this many seconds (0 = stay; '
                             'default from FAJITA_PANIC_RESET)')
    parser.add_argument('--occurrence', type=int, default=1, help='which call to SYMBOL (1 = first)')
    parser.add_argument('--in', dest='function', default='_arm_init', help='function that makes the call (default _arm_init)')
    args = parser.parse_args()
    global PANIC_RESET_SECONDS
    PANIC_RESET_SECONDS = max(0, args.panic_reset)
    if args.reset_before and not args.reset_after:
        raise SystemExit('--reset-before requires --reset-after')
    data = bytearray(args.kernel.read_bytes())
    load_segments(data)
    kernel_names = rebase(args.kernel, data)

    def word(va):
        return struct.unpack_from('<I', data, file_offset(va))[0]

    expected = {ENTRY_VA: branch(ENTRY_VA, START_FIRST_CPU), MMU_ENABLE_VA: 0xD5181000, AFTER_MMU_VA: 0xD2A00600}
    for va, value in expected.items():
        if word(va) != value:
            raise SystemExit(f'{va:#x} holds {word(va):#010x}, expected {value:#010x}: not the kernel these probes are for')
    for stage, va in CAVES.items():
        if stage != 5 and any(data[file_offset(va):file_offset(va) + CAVE_SIZE]):
            raise SystemExit(f'Padding at {va:#x} is not empty')

    early_wdog = os.environ.get('FAJITA_EARLY_WDOG') == '1'
    # fajita: FAJITA_MMU_PROBE=<seconds> places stage 3 (the timed reset right after the MMU enable) even under --no-reset, to ask
    # whether a kernel that stops after the magenta bar got through the MMU enable (reset after <seconds>) or not (watchdog, 32 s).
    mmu_probe = int(os.environ.get('FAJITA_MMU_PROBE', '0'))
    # fajita: FAJITA_RESET_AFTER=<symbol>[:<function>] places stage 4 (reset after that call) even under --no-reset, for bisecting.
    reset_after = os.environ.get('FAJITA_RESET_AFTER')
    if reset_after:
        # A leading '<' places the reset before the call instead (--reset-before).
        args.reset_before = reset_after.startswith('<')
        symbol, _, function = reset_after.lstrip('<').partition(':')
        args.no_reset, args.reset_after, args.panic_screen = False, symbol, False
        args.function = function or args.function
    if mmu_probe:
        # The panic screen does not fit the padding after cave 3; a probe run asks only about the MMU enable.
        args.no_reset, args.reset_after, args.reset_delay, args.panic_screen = False, None, mmu_probe, False
    # (site, cave stage, symbols the cave is linked with)
    if early_wdog:
        CAVES[5] = early_wdog_cave(data, 0x200)
    sites = [(ENTRY_VA, 1, {'target': CAVES[5] if early_wdog else START_FIRST_CPU}),
             (MMU_ENABLE_VA, 2, {'target': MMU_ENABLE_VA + 4})]
    if args.no_reset:
        description = 'no reset probe'
    elif args.reset_after and args.reset_after.startswith('0x'):
        # fajita: a raw call site (code without symbols, such as a prelinked kext): the `bl` at that address.
        call_va = int(args.reset_after, 16)
        word = struct.unpack_from('<I', data, file_offset(call_va))[0]
        if word & 0xFFFFFC1F == 0xD63F0000:
            # blr xN: the cave re-executes the indirect call (stage 4 with blr in place of bl callee).
            BLR_REGISTER[0] = (word >> 5) & 31
            callee = call_va
        elif word >> 26 == 0b100101:
            delta = word & 0x3FFFFFF
            delta -= (1 << 26) if delta & (1 << 25) else 0
            callee = call_va + delta * 4
        else:
            raise SystemExit(f'{call_va:#x} holds {word:#010x}, not a bl or blr')
        sites.append((call_va, 4, {'callee': callee, 'target': call_va + 4}))
        description = f'stage 4 reset {"before" if args.reset_before else "after"} the bl at {call_va:#x} (to {callee:#x})'
    elif args.reset_after:
        symbols = kernel_symbols(args.kernel)
        call_va, callee = find_call(data, symbols, args.function, args.reset_after, args.occurrence)
        sites.append((call_va, 4, {'callee': callee, 'target': call_va + 4}))
        description = f'stage 4 reset {"before" if args.reset_before else "after"} call #{args.occurrence} to {args.reset_after} in {args.function} (at {call_va:#x})'
    else:
        sites.append((AFTER_MMU_VA, 3, {'target': AFTER_MMU_VA + 4}))
        description = f'stage 3 reset {args.reset_delay} s after the MMU is on'

    with tempfile.TemporaryDirectory() as tmp:
        for site, stage, symbols in sites:
            source = RESET_BEFORE if (stage == 4 and args.reset_before) else SOURCES[stage].format(ticks=TIMEBASE * args.reset_delay)
            if stage == 4 and BLR_REGISTER[0] is not None and args.reset_before and os.environ.get('FAJITA_RESET_IF'):
                # fajita: reset at once when this indirect call is about to go to that address; others pass through.
                source = (f'.text\n.global _start\n_start:\n  ldr x16, ={os.environ["FAJITA_RESET_IF"]}\n'
                          f'  cmp x{BLR_REGISTER[0]}, x16\n  b.eq 3f\n  blr x{BLR_REGISTER[0]}\n  b target\n3:\n'
                          + RESET_BEFORE.split('_start:\n', 1)[1] + '.ltorg\n')
            elif stage == 4 and BLR_REGISTER[0] is not None:
                call = f'  blr x{BLR_REGISTER[0]}\n'
                only = os.environ.get('FAJITA_RESET_IF')
                if only:
                    # fajita: reset only when the indirect call goes to this address (one kext's start routine at the
                    # blr in OSKext::start); every other call passes straight through.
                    call = (f'  ldr x16, ={only}\n  cmp x{BLR_REGISTER[0]}, x16\n  b.eq 3f\n{call}  b target\n3:\n{call}')
                source = source.replace('  bl callee\n', call)
            code = assemble(source, CAVES[stage], symbols, Path(tmp))
            # The last cave (stage 4) may use the padding up to the panic screen's when that is not placed.
            room = PANIC_CAVE_END - CAVES[stage] if stage == 4 and not args.panic_screen else CAVE_SIZE
            if len(code) > room or (room > CAVE_SIZE and any(data[file_offset(CAVES[stage]):file_offset(CAVES[stage]) + len(code)])):
                raise SystemExit(f'Stage {stage} code is too large')
            data[file_offset(CAVES[stage]):file_offset(CAVES[stage]) + len(code)] = code
    if early_wdog:

        with tempfile.TemporaryDirectory() as tmp:
            code = assemble(EARLY_WDOG, CAVES[5], {'target': START_FIRST_CPU}, Path(tmp))
        if len(code) > 0x200:
            raise SystemExit('Early watchdog code is too large')
        data[file_offset(CAVES[5]):file_offset(CAVES[5]) + len(code)] = code
        description += ', early watchdog from the kernel entry'
    for site, stage, _ in sites:
        struct.pack_into('<I', data, file_offset(site), branch(site, CAVES[stage]))
    if os.environ.get('FAJITA_AMFI_SKIP_PMGR') == '1':
        plan = json.loads(args.kernel.with_name('kernel.json').read_text()) if args.kernel.with_name('kernel.json').is_file() else \
            json.loads(Path(os.environ['FAJITA_KERNEL_PLAN']).read_text())
        description += f', AMFI vmm byte at {amfi_skip_pmgr(data, plan):#x}'
        if os.environ.get('FAJITA_IMG4_SKIP_PMAP_CS') == '1':
            description += f', AppleImage4 pmap cs runtime off at {img4_skip_pmap_cs(data, plan):#x}'
    if args.panic_screen:
        # Right after the last probe cave in use, so the padding is shared as tightly as possible.
        PANIC_CAVE = max(CAVES[stage] for _, stage, _ in sites) + CAVE_SIZE
        if word(PANIC_VA) in (0, 0xFFFFFFFF) or any(data[file_offset(PANIC_CAVE):file_offset(PANIC_CAVE_END)]):
            raise SystemExit('panic() or the panic screen padding is not as expected')
        with tempfile.TemporaryDirectory() as tmp:
            code, load, entry = assemble_c(ROOT / 'scripts/xnu-panic-screen.c', PANIC_CAVE, Path(tmp), kernel_names)
            common_entry = panic_common_entry(Path(tmp), code, load)
        if load + len(code) > PANIC_CAVE_END:
            raise SystemExit(f'panic screen is {len(code):#x} bytes at {load:#x}, the padding ends at {PANIC_CAVE_END:#x}')
        if any(data[file_offset(load):file_offset(load) + len(code)]):
            raise SystemExit(f'Padding at {load:#x} is not empty')
        data[file_offset(load):file_offset(load) + len(code)] = code
        struct.pack_into('<I', data, file_offset(PANIC_VA), branch(PANIC_VA, entry))
        common = kernel_names.get('_panic_trap_to_debugger')
        if common is None or word(common) in (0, 0xFFFFFFFF):
            raise SystemExit('Common panic funnel is missing; refuse a partially covered panic screen')
        struct.pack_into('<I', data, file_offset(common), branch(common, common_entry))
        description += f', panic screen ({len(code):#x} bytes at {entry:#x}; common funnel at {common:#x})'
    args.kernel.write_bytes(data)
    print(f'patched {args.kernel.name}: stage 1 blue bar, stage 2 magenta bar, {description}')

if __name__ == '__main__':
    main()
