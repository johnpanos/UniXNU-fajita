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
SOURCES = ['WifiMdt.c', 'WifiQmi.c', 'WifiQrtr.c', 'WifiBoard.c', 'tests/test_wifi.c']
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
    print(f'(tests {"sanitized" if san else "without sanitizers"})')


if __name__ == '__main__':
    main()
