#!/usr/bin/env python3
"""Build libFajitaRtc.a: the PM8998 RTC wall clock (drivers/FajitaRtc: FajitaRtc.cpp over RtcCore.h, fajita-rtc=<n>, default off)
against the recorded XNU compile flags (those of iokit/IOPlatformExpert.cpo, as build-native-spmi.py).

Linked with -force_load through CONFIG_XNU_EXTRA_LD_ARCHIVES, after libNativeSpmi.a, whose API it reads the RTC with. Its built-in
personality (patches/xnu/zzw-fajita-rtc-personality.patch) matches the /arm-io/rtc node of the ADT (make-fajita-adt.py --rtc).
The kernel build does not track the extra archives, so a changed archive removes the linked kernel to make the next link use it.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/fajita-rtc'
OBJ_ROOT = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
RTC = ROOT / 'drivers/FajitaRtc'
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
            '-Wno-missing-designated-field-initializers']
INCLUDES = ['-I', str(RTC), '-I', str(ROOT / '.cache/xnu-work/osfmk')]


def record(relative):
    return json.loads((OBJ_ROOT / relative).read_text().strip().rstrip(','))


def command_for(rec, source, obj):
    """The recorded compile with another source and object, and without the record's own warnings."""
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
        elif not arg.startswith('-W'):
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


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rec = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
    source = RTC / 'FajitaRtc.cpp'
    obj = OUT / 'FajitaRtc.o'
    command = command_for(rec, source, obj) + WARNINGS + INCLUDES
    result = subprocess.run(command, cwd=rec['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
    (OUT / 'FajitaRtc.log').write_text(result.stdout)
    if result.returncode:
        raise SystemExit(result.stdout[-4000:])
    data = obj.read_bytes()
    if struct.unpack_from('<4I', data) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit('FajitaRtc.o: not an arm64 Mach-O object')
    (OUT / 'compile-commands.json').write_text(json.dumps({source.name: command}, indent=2) + '\n')
    archive = OUT / 'libFajitaRtc.a'
    previous = hashlib.sha256(archive.read_bytes()).hexdigest() if archive.is_file() else None
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True, timeout=60)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != previous:
        (OBJ_ROOT / 'mach.development.bcm2837.unstripped.noctf').unlink(missing_ok=True)
    sources = [RTC / 'FajitaRtc.cpp', RTC / 'RtcCore.h']
    (OUT / 'build-report.json').write_text(json.dumps({
        'archive_sha256': digest,
        'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        'hardware_execution': False,
    }, indent=2) + '\n')
    print(f'PASS: libFajitaRtc.a (sha256 {digest[:16]})')


if __name__ == '__main__':
    main()
