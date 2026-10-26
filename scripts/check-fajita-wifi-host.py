#!/usr/bin/env python3
"""Build and run the host tests of the fajita Wi-Fi protocol code (no hardware): the mbn/ELF loader, QMI TLV
encode/decode and the wlfw messages, QRTR/GLINK framing, the servreg-locator and rmtfs servers, and board-2.bin
selection (drivers/FajitaWifi, tests/test_wifi.c). C11 with the address and undefined-behaviour sanitizers when the
compiler has them.

  scripts/check-fajita-wifi-host.py [--cc clang] [--out DIR] [--firmware DIR] [--board board-2.bin]

With --firmware (a directory holding mba.mbn, modem.mbn, wlanmdsp.mbn) and --board (board-2.bin) the tests also parse
the real downloaded firmware. The default looks in .cache/firmware-wifi (gitignored; pulled from pmOS's
firmware-oneplus-sdm845, never committed); the parsing tests are skipped if it is absent.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIFI = ROOT / 'drivers/FajitaWifi'
SOURCES = ['WifiMdt.c', 'WifiQmi.c', 'WifiQrtr.c', 'WifiBoard.c', 'WifiFat.c', 'tests/test_wifi.c']
# The stack above a booted modem (tests/test_stack.c + one t_*.c per layer).
STACK_LIBS = ['WifiSmem.c', 'WifiGlink.c', 'WifiRouter.c', 'WifiPdmap.c', 'WifiTftp.c', 'WifiRmtfs.c', 'WifiSnoc.c', 'WifiWlfw.c', 'WifiCe.c', 'WifiHtc.c', 'WifiWpa.c', 'WifiQmp.c', 'WifiMss.c']
STACK_TESTS = ['tests/test_stack.c', 'tests/t_smem.c', 'tests/t_glink.c', 'tests/t_qrtr.c', 'tests/t_wlfw.c', 'tests/wsim.c', 'tests/wfw.c', 'tests/t_ce.c', 'tests/t_wpa.c', 'tests/t_qmp.c', 'tests/t_mss.c']
LIB_FOR_STACK = ['WifiQmi.c', 'WifiQrtr.c', 'WifiFat.c', 'WifiMdt.c', 'WifiBoard.c']
DEFAULT_EFS = ROOT / '.cache/efs-backup'
DEFAULT_SMEM = ROOT / '.cache/smem-snapshot/smem.bin'      # scripts/fajita/test/smem-snap.py (read-only, from the phone)
DEFAULT_MODEM = ROOT / '.cache/fw-modem-part/modem_b.img'   # ufspart.py backup --passes 1 --out .cache/fw-modem-part modem_b
FAT_FILES = ['image/mba.mbn', 'image/modem.mdt', 'image/modem.b20', 'image/modem.b28', 'image/wlanmdsp.mbn', 'image/bdwlan.102',
             'image/modemr.jsn']
DEFAULT_FW = ROOT / '.cache/firmware-wifi/src/lib/firmware/qcom/sdm845/OnePlus/enchilada'
DEFAULT_BOARD = ROOT / '.cache/firmware-wifi/src/lib/firmware/postmarketos/ath10k/WCN3990/hw1.0/board-2.bin'


def sanitizers(cc, out):
    probe = out / 'probe.c'
    probe.write_text('int main(void){return 0;}\n')
    flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    ok = subprocess.run([cc, *flags, str(probe), '-o', str(out / 'probe')], capture_output=True).returncode == 0
    return flags if ok else []


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cc', default=shutil.which('clang') or 'cc')
    parser.add_argument('--out')
    parser.add_argument('--firmware', default=str(DEFAULT_FW) if DEFAULT_FW.is_dir() else None)
    parser.add_argument('--board', default=str(DEFAULT_BOARD) if DEFAULT_BOARD.is_file() else None)
    parser.add_argument('--modem', default=str(DEFAULT_MODEM) if DEFAULT_MODEM.is_file() else None,
                        help='a dumped modem_a/modem_b partition: the FAT reader is checked against mtools (mcopy)')
    parser.add_argument('--efs', help='the verified EFS backup (ufspart.py backup), default .cache/efs-backup')
    parser.add_argument('--smem', default=str(DEFAULT_SMEM) if DEFAULT_SMEM.is_file() else None,
                        help='a SMEM snapshot from the phone (smem-snap.py): the parser runs on the real image')
    args = parser.parse_args()
    out = Path(args.out or tempfile.mkdtemp(prefix='fajita-wifi-host-'))
    out.mkdir(parents=True, exist_ok=True)
    san = sanitizers(args.cc, out)
    binary = out / 'test_wifi'
    cmd = [args.cc, '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', *san,
           *[str(WIFI / s) for s in SOURCES], '-o', str(binary)]
    subprocess.run(cmd, check=True)
    run = [str(binary)]
    if args.firmware:
        run.append(args.firmware)
        run.append(args.board or '')
    result = subprocess.run(run)
    if result.returncode:
        raise SystemExit('FAIL: fajita Wi-Fi host tests')
    stack = out / 'test_stack'
    subprocess.run([args.cc, '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', *san,
                    *[str(WIFI / x) for x in STACK_LIBS + LIB_FOR_STACK + STACK_TESTS], '-o', str(stack)], check=True)
    efs = args.efs if args.efs else (str(DEFAULT_EFS) if DEFAULT_EFS.is_dir() else '')
    if subprocess.run([str(stack), args.modem or '', efs, args.board or '', args.smem or '']).returncode:
        raise SystemExit('FAIL: fajita Wi-Fi stack host tests')
    if args.modem and shutil.which('mcopy'):
        fat = out / 'test_fat'
        subprocess.run([args.cc, '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', *san, str(WIFI / 'WifiFat.c'),
                        str(WIFI / 'WifiMdt.c'), str(WIFI / 'tests/test_fat.c'), '-o', str(fat)], check=True)
        mine, ref = out / 'fat-mine', out / 'fat-ref'
        mine.mkdir(exist_ok=True); ref.mkdir(exist_ok=True)
        if subprocess.run([str(fat), args.modem, str(mine), *FAT_FILES]).returncode:
            raise SystemExit('FAIL: FAT reader')
        env = dict(__import__('os').environ, MTOOLS_SKIP_CHECK='1')
        for f in FAT_FILES:
            subprocess.run(['mcopy', '-n', '-o', '-i', args.modem, '::/' + f, str(ref / Path(f).name)], check=True, env=env)
            if (mine / Path(f).name).read_bytes() != (ref / Path(f).name).read_bytes():
                raise SystemExit(f'FAIL: FAT reader: {f} differs from mcopy')
        print(f'PASS: FAT reader matches mcopy for {len(FAT_FILES)} files')
    print(f'(tests {"sanitized" if san else "without sanitizers"})')


if __name__ == '__main__':
    main()
