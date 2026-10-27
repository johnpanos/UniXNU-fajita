#!/usr/bin/env python3
"""Build and run the host tests of the fajita board policies, header-only logic shared with the kernel (and the HAL plug-in), as
C++17 with the address and undefined-behaviour sanitizers when the compiler has them:
  fajita-thermal-test  package/FajitaThermalPolicy.h: TSENS decoding and the CPU cluster step governor (package/FajitaThermal.cpp)
  fajita-haptic-test   package/FajitaHapticPolicy.h: iOS haptic waveforms -> levels -> PMI8998 motor commands

  scripts/check-fajita-policy-host.py [--cxx clang++] [--out DIR] [TEST...]   (default DIR: .cache/fajita-out/host-tests; all tests)
"""
import argparse
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ['fajita-thermal-test', 'fajita-haptic-test']


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cxx', default=shutil.which('clang++') or 'c++')
    parser.add_argument('--out', default=str(ROOT / '.cache/fajita-out/host-tests'))
    parser.add_argument('tests', nargs='*', default=TESTS)
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    probe = out / 'probe.cpp'
    probe.write_text('int main() { return 0; }\n')
    if subprocess.run([args.cxx, *san, str(probe), '-o', str(out / 'probe')], capture_output=True).returncode:
        san = []
    for name in args.tests:
        binary = out / name
        subprocess.run([args.cxx, '-std=c++17', '-g', '-O1', '-Wall', '-Wextra', '-Werror', *san, '-I', str(ROOT / 'package'),
                        str(ROOT / 'package/tests' / f'{name}.cpp'), '-o', str(binary)], check=True)
        if subprocess.run([str(binary)]).returncode:
            raise SystemExit(f'FAIL: {name}')
        print(f'PASS: {name} ({"sanitized" if san else "without sanitizers"})')

if __name__ == '__main__':
    main()
