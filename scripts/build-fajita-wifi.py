#!/usr/bin/env python3
"""Build libFajitaWifi.a: the fajita Wi-Fi staged bring-up (package/FajitaWifiProbe.cpp, fajita-wifi=<stage>, default off)
and the freestanding protocol/parsing code it links (drivers/FajitaWifi: WifiMdt, WifiQmi, WifiQrtr, WifiBoard), against
the recorded XNU compile flags.

Runs as a [kernel] archives builder after the bootstrap pass left the compile records (like build-fajita-pmic.py).
FajitaWifiProbe.cpp takes the flags of iokit/IOPlatformExpert.cpo; the C units take osfmk/startup.o plus
-mgeneral-regs-only. Linked with -force_load through CONFIG_XNU_EXTRA_LD_ARCHIVES; it defines fajita_wifi_poll, which
takes precedence over the weak stand-in of BootProgressStubs.c. The protocol code is host-tested by
scripts/check-fajita-wifi-host.py. A changed archive removes the linked kernel so the next link uses it.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/fajita-wifi'
OBJ_ROOT = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
KIT = ROOT / 'kits/fajita'
WIFI = ROOT / 'drivers/FajitaWifi'
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
            '-Wno-missing-designated-field-initializers']
# The stack above a booted modem (SMEM/SMP2P, GLINK, QRTR services, wlfw, ath10k, WPA2), host-tested in tests/test_stack.c.
KIT_UNITS = ['FajitaWifiStack.cpp', 'FajitaWifiLower.cpp', 'FajitaWifiModem.cpp', 'FajitaNet.cpp', 'FajitaWifiDev.cpp']
STACK = ['WifiSmem.c', 'WifiGlink.c', 'WifiRouter.c', 'WifiPdmap.c', 'WifiTftp.c', 'WifiRmtfs.c', 'WifiFat.c', 'WifiSnoc.c', 'WifiWlfw.c', 'WifiCe.c', 'WifiHtc.c', 'WifiWpa.c', 'WifiQmp.c', 'WifiMss.c', 'WifiWmi.c', 'WifiMlme.c', 'WifiHtt.c', 'WifiDev.c']
INCLUDES = ['-I', str(KIT), '-I', str(WIFI), '-I', str(ROOT / '.cache/xnu-work/osfmk')]


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


def check_object(path):
    data = path.read_bytes()
    magic, cpu, subtype, kind = struct.unpack_from('<4I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
        raise SystemExit(f'{path.name}: not an arm64 Mach-O object')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cpp_rec, c_rec = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json')
    units = [
        (cpp_rec, KIT / 'FajitaWifiProbe.cpp', []),
        *[(cpp_rec, KIT / name, []) for name in KIT_UNITS],
        (c_rec, WIFI / 'WifiMdt.c', ['-mgeneral-regs-only']),
        (c_rec, WIFI / 'WifiQmi.c', ['-mgeneral-regs-only']),
        (c_rec, WIFI / 'WifiQrtr.c', ['-mgeneral-regs-only']),
        (c_rec, WIFI / 'WifiBoard.c', ['-mgeneral-regs-only']),
        *[(c_rec, WIFI / name, ['-mgeneral-regs-only']) for name in STACK],
    ]
    objects, commands = [], {}
    for rec, source, extra in units:
        obj = OUT / (source.stem + '.o')
        command = command_for(rec, source, obj) + WARNINGS + extra + INCLUDES
        commands[source.name] = command
        result = subprocess.run(command, cwd=rec['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-4000:])
        check_object(obj)
        objects.append(obj)
    (OUT / 'compile-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    archive = OUT / 'libFajitaWifi.a'
    previous = hashlib.sha256(archive.read_bytes()).hexdigest() if archive.is_file() else None
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True, timeout=60)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != previous:
        (OBJ_ROOT / 'mach.development.bcm2837.unstripped.noctf').unlink(missing_ok=True)
    sources = [KIT / 'FajitaWifiProbe.cpp', KIT / 'FajitaWifiBoard.h', *[KIT / n for n in KIT_UNITS], WIFI / 'WifiMdt.c', WIFI / 'WifiMdt.h',
               WIFI / 'WifiQmi.c', WIFI / 'WifiQmi.h', WIFI / 'WifiQrtr.c', WIFI / 'WifiQrtr.h', WIFI / 'WifiBoard.c',
               WIFI / 'WifiBoard.h', *[WIFI / n for name in STACK for n in (name, name[:-2] + '.h')]]
    (OUT / 'build-report.json').write_text(json.dumps({
        'archive_sha256': digest,
        'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
        'hardware_execution': False,
    }, indent=2) + '\n')
    print(f'PASS: libFajitaWifi.a ({len(objects)} objects, sha256 {digest[:16]})')


if __name__ == '__main__':
    main()
