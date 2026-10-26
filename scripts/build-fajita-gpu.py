#!/usr/bin/env python3
"""Build libFajitaGpu.a: the fajita Adreno 630 bring-up (package/FajitaGpuProbe.cpp, drivers/FajitaAdreno), the NativeAdreno
objects it drives (the GMU and GPU SMMU contexts, memory, VM, ring) and the ArmSmmu4K kit, plus the A630 firmware.

Runs as a [kernel] archives builder after the bootstrap pass left the compile records (as build-fajita-touch.py). C++ units take
the flags of iokit/IOPlatformExpert.cpo, C units those of osfmk/startup.o with -mgeneral-regs-only. Linked with -force_load
through CONFIG_XNU_EXTRA_LD_ARCHIVES.

Firmware: a630_gmu.bin, a630_sqe.fw (linux-firmware qcom/) and the OnePlus-signed a630_zap.mbn (qcom/sdm845/oneplus6/, the path
of sdm845-oneplus-common.dtsi) are read from .cache/firmware (the device repository's ignored copy) and must match the sha256
pins below; they are embedded as fajita_gpu_{gmu,sqe,zap}_fw and never enter Git. A missing file leaves that symbol out (the
probe's weak reference then reports it) instead of failing the kernel.
"""
import hashlib
import json
import os
import shutil
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/fajita-gpu'
OBJ_ROOT = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
KIT = ROOT / 'kits/fajita'
A630 = ROOT / 'drivers/FajitaAdreno'
ADRENO = ROOT / 'drivers/NativeAdreno'
SMMU = ROOT / 'kits/ArmSmmu4K'
PM4 = ROOT / 'kits/AdrenoPM4'
FIRMWARE = ROOT / '.cache/firmware'
PINS = {
    'fajita_gpu_gmu_fw': ('qcom/a630_gmu.bin', 32768, 'da8d9b1b1f5c1a0b311f32567093b4828f3c80031dd8435f91ac13c664e173a6'),
    'fajita_gpu_sqe_fw': ('qcom/a630_sqe.fw', 34188, '1c21b527d9183487cc550dabbb3f43e555df5a977a461934fc61f0635a9aa90c'),
    'fajita_gpu_zap_fw': ('qcom/sdm845/oneplus6/a630_zap.mbn', 14256,
                          '9780956733759d24740b3df5bd804e5dddc48bc0e956271b0288631890080df9'),
}
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
            '-Wno-missing-designated-field-initializers', '-Wno-incompatible-sysroot']
INCLUDES = ['-I', str(A630), '-I', str(ADRENO), '-I', str(SMMU), '-I', str(PM4), '-I', str(ROOT / '.cache/xnu-work/osfmk')]
# NativeAdreno sources left out: the staged accelerator (built separately below: its initializer must not run at boot), the service
# and client, exactly as sheng's build-native-adreno-kits.py leaves them out.
ADRENO_SKIP = {'NativeAdrenoAccelerator.cpp', 'NativeAdrenoService.cpp', 'NativeAdrenoClient.cpp',
               # sheng's A740 GMU (ShengGmu, the modern HFI kit): the A630 GMU is drivers/FajitaAdreno
               'NativeAdrenoGmuConfig.cpp', 'NativeAdrenoGmuPlatform.cpp', 'NativeAdrenoGmuService.cpp', 'NativeAdrenoFirmware.cpp',
               'NativeAdrenoFirmwareDefaults.cpp', 'NativeAdrenoKernelTime.cpp'}


def record(relative):
    return json.loads((OBJ_ROOT / relative).read_text().strip().rstrip(','))


def command_for(rec, source, obj):
    command, args, i = [], rec['arguments'], 0
    while i < len(args):
        arg = args[i]
        if arg == rec['file']:
            command.append(str(source))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif arg == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W') and arg not in ('-MD', '-MP'):
            command.append(arg)
        i += 1
    return kit_release(command)


def kit_release(command):
    """FAJITA_KIT_RELEASE=1: the kit without the DEVELOPMENT kernel's checks (minimal UBSan, IOKit asserts), with NDEBUG.
    The configuration defines (DEVELOPMENT, IOTRACKING, ...) stay: they shape the kernel's structures the kit shares."""
    if __import__('os').environ.get('FAJITA_KIT_RELEASE') != '1':
        return command
    out, i = [], 0
    while i < len(command):
        arg = command[i]
        if arg.startswith('-fsanitize'):
            pass
        elif arg == '-D' and i + 1 < len(command) and command[i + 1] == 'IOASSERT=1':
            out += ['-D', 'IOASSERT=0']
            i += 1
        else:
            out.append(arg)
        i += 1
    return out + ['-DNDEBUG']


ABI_RECORD = ROOT / 'scripts/data/fajita-ioaccelerator-base-abi.json'


def staged_accelerator(cpp_rec, commands):
    """The staged AGXAccelerator (FajitaGpuProbe stage 11): NativeAdrenoAccelerator.cpp with its static initializer moved out of
    __mod_init_func and exported, and its IOAcceleratorES imports set to the addresses audit-fajita-accelerator-base.py derived from
    the 20H392 kernelcache this kernel is prelinked with. Left out (with a warning) when the record is for another kernelcache."""
    record = json.loads(ABI_RECORD.read_text())
    pinned = os.environ.get('UNIXNU_KERNELCACHE_SHA256', '')
    if pinned != record['kernelcache_sha256']:
        print('WARNING: the IOAcceleratorES record is for kernelcache %s, this kernel is prelinked with %s: staged accelerator left out'
              % (record['kernelcache_sha256'][:16], pinned[:16] or 'an unpinned one'))
        return []
    if record['operational_overrides'] or record['instance_bytes'] != 136 or record['virtual_entries'] != 168:
        raise SystemExit('the IOAcceleratorES record is not a 136-byte, 168-entry base without operational overrides')
    source = ADRENO / 'NativeAdrenoAccelerator.cpp'
    obj = OUT / 'NativeAdrenoAccelerator.o'
    # Its "../NativeMetal/OriginalAcceleratorABI.h" include: this tree has no NativeMetal module, so the copy in drivers/FajitaAdreno
    # is placed where that relative path resolves from an include directory.
    shim = OUT / 'include/NativeMetal'
    shim.mkdir(parents=True, exist_ok=True)
    (shim / 'OriginalAcceleratorABI.h').write_bytes((A630 / 'OriginalAcceleratorABI.h').read_bytes())
    (OUT / 'include/here').mkdir(exist_ok=True)
    command = (command_for(cpp_rec, source, obj) + WARNINGS + ['-Wno-overloaded-virtual'] + INCLUDES +
               ['-I', str(OUT / 'include/here')])
    commands[source.name] = command
    result = subprocess.run(command, cwd=cpp_rec['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
    (OUT / 'NativeAdrenoAccelerator.log').write_text(result.stdout)
    if result.returncode:
        raise SystemExit(f'{source}:\n' + result.stdout[-4000:])
    raw = obj.read_bytes()
    symoff, nsyms, stroff = next(struct.unpack_from('<3I', raw, p + 8) for c, p in _load_commands(raw) if c == 2)
    undefined = set()
    for i in range(nsyms):
        n, kind, _, _, _ = struct.unpack_from('<IBBHQ', raw, symoff + 16 * i)
        name = raw[stroff + n:raw.index(b'\0', stroff + n)].decode()
        if kind & 0x0e == 0 and kind & 1 and 'IOAcceleratorES' in name:
            undefined.add(name)
    bindings = {'__ZN15IOAcceleratorESC2EPK11OSMetaClass': record['protected_compatible_constructor_aliases'][0],
                '__ZN15IOAcceleratorESD2Ev': record['nondeleting_destructor'],
                '__ZN15IOAcceleratorES10gMetaClassE': record['metaclass']}
    if undefined - set(bindings) or not undefined:
        raise SystemExit('unexpected IOAcceleratorES imports of the staged accelerator: %s' % sorted(undefined))
    asm = OUT / 'accelerator-bindings.S'
    # Every bound name is defined, the metaclass too (stage 11 compares it with the live metaclass), even when not imported.
    asm.write_text(''.join('.globl %s\n.set %s, %s\n' % (n, n, a) for n, a in sorted(bindings.items())))
    target = [a for a in cpp_rec['arguments'] if a.startswith('--target=')][-1]
    bound = OUT / 'accelerator-bindings.o'
    subprocess.run([cpp_rec['arguments'][0], target, '-c', str(asm), '-o', str(bound)], check=True, timeout=60, cwd=cpp_rec['directory'])
    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    late = OUT / 'NativeAdrenoAccelerator.late.o'
    subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func', '--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp',
                    str(obj), str(late)], check=True, timeout=60)
    late_raw = late.read_bytes()
    if b'__mod_init_func' in late_raw[:8192]:
        raise SystemExit('the staged accelerator still registers its initializer')
    if b'__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp' not in late_raw:
        raise SystemExit('the staged accelerator initializer is not exported')
    check_object(late)
    check_object(bound)
    return [late, bound]


def _load_commands(raw):
    count, = struct.unpack_from('<I', raw, 16)
    at, out = 32, []
    for _ in range(count):
        command, size = struct.unpack_from('<II', raw, at)
        out.append((command, at))
        at += size
    return out


def check_object(path):
    magic, cpu, subtype, kind = struct.unpack_from('<4I', path.read_bytes())
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'{path.name}: not an arm64 Mach-O object')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cpp_rec, c_rec = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json')
    units = [(cpp_rec, KIT / 'FajitaGpuProbe.cpp', []),
             (c_rec, A630 / 'A630Gmu.c', ['-mgeneral-regs-only']),
             (c_rec, A630 / 'A630Gpu.c', ['-mgeneral-regs-only'])]
    units += [(cpp_rec, p, []) for p in sorted(ADRENO.glob('NativeAdreno*.cpp')) if p.name not in ADRENO_SKIP]
    units += [(c_rec, p, ['-mgeneral-regs-only']) for p in sorted(SMMU.glob('ArmSmmu4K*.c'))]
    units += [(c_rec, PM4 / 'AdrenoPM4.c', ['-mgeneral-regs-only'])]
    objects, commands = [], {}
    for rec, source, extra in units:
        obj = OUT / (source.stem + ('.c.o' if source.suffix == '.c' else '.o'))
        command = command_for(rec, source, obj) + WARNINGS + extra + INCLUDES
        commands[source.name] = command
        result = subprocess.run(command, cwd=rec['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source}:\n' + result.stdout[-4000:])
        check_object(obj)
        objects.append(obj)
    objects += staged_accelerator(cpp_rec, commands)
    # Firmware blobs
    assembly, embedded = ['.section __DATA_CONST,__const'], {}
    for label, (name, size, digest) in PINS.items():
        path = FIRMWARE / name
        blob = path.read_bytes() if path.is_file() else b''
        if len(blob) != size or hashlib.sha256(blob).hexdigest() != digest:
            # Fatal unless asked for: a firmware-less archive builds, then the kernel link fails on the weak references far from here
            # (a new worktree whose ios16/.cache was an empty directory instead of the shared one, 2026-10-26).
            if os.environ.get('FAJITA_GPU_NO_FIRMWARE') != '1':
                raise SystemExit(f'{path}: missing or not the pinned file ({label}). In a new worktree, link ios16/.cache to the '
                                 f'main tree\'s (ln -s ../../UniXNU-fajita-ios16/ios16/.cache ios16/.cache); '
                                 f'FAJITA_GPU_NO_FIRMWARE=1 builds without it on purpose.')
            print(f'WARNING: {name} missing or not the pinned file: {label} left out (FAJITA_GPU_NO_FIRMWARE=1)')
            continue
        embedded[label] = name
        assembly += ['.p2align 12', '.globl _' + label, '_' + label + ':', '.incbin ' + json.dumps(str(path)), '_' + label + '_end:',
                     '.p2align 3', '.globl _' + label + '_size', '_' + label + '_size:', '.quad _' + label + '_end-_' + label]
    (OUT / 'firmware.S').write_text('\n'.join(assembly) + '\n')
    fw_obj = OUT / 'firmware.o'
    # The recorded compiler and its last --target (the deployment version the kernel objects carry).
    target = [a for a in c_rec['arguments'] if a.startswith('--target=')][-1]
    fw_cmd = [c_rec['arguments'][0], target, '-c', str(OUT / 'firmware.S'), '-o', str(fw_obj)]
    subprocess.run(fw_cmd, check=True, timeout=60, cwd=c_rec['directory'])
    check_object(fw_obj)
    objects.append(fw_obj)
    (OUT / 'compile-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    archive = OUT / 'libFajitaGpu.a'
    previous = hashlib.sha256(archive.read_bytes()).hexdigest() if archive.is_file() else None
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=60)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != previous:
        (OBJ_ROOT / 'mach.development.bcm2837.unstripped.noctf').unlink(missing_ok=True)
    (OUT / 'build-report.json').write_text(json.dumps({'archive_sha256': digest, 'firmware': embedded,
                                                        'objects': [o.name for o in objects]}, indent=2) + '\n')
    print(f'PASS: libFajitaGpu.a ({len(objects)} objects, firmware {sorted(embedded.values())}, sha256 {digest[:16]})')


if __name__ == '__main__':
    main()
