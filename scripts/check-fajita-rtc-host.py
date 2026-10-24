#!/usr/bin/env python3
"""Build and run the host test of the fajita RTC wall-clock logic (drivers/FajitaRtc/RtcCore.h) against a PM8998 RTC model (no
hardware), as C++17 with the address and undefined-behaviour sanitizers when the compiler has them.

  scripts/check-fajita-rtc-host.py [--cxx clang++] [--out DIR]
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST = ROOT / 'drivers/FajitaRtc/tests/rtc-test.cpp'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cxx', default=shutil.which('clang++') or 'c++')
    parser.add_argument('--out')
    args = parser.parse_args()
    out = Path(args.out or tempfile.mkdtemp(prefix='fajita-rtc-host-'))
    out.mkdir(parents=True, exist_ok=True)
    san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    probe = out / 'probe.cpp'
    probe.write_text('int main() { return 0; }\n')
    if subprocess.run([args.cxx, *san, str(probe), '-o', str(out / 'probe')], capture_output=True).returncode:
        san = []
    binary = out / 'rtc-test'
    subprocess.run([args.cxx, '-std=c++17', '-g', '-O1', '-Wall', '-Wextra', '-Werror', *san, str(TEST), '-o', str(binary)], check=True)
    if subprocess.run([str(binary)]).returncode:
        raise SystemExit('FAIL: rtc-test')
    print(f'PASS: fajita RTC host test ({"sanitized" if san else "without sanitizers"})')


if __name__ == '__main__':
    main()
