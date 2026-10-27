#!/usr/bin/env python3
"""Build and run the host test of the fajita thermal policy (package/FajitaThermalPolicy.h: TSENS decoding and the CPU cluster step
governor of package/FajitaThermal.cpp), as C++17 with the address and undefined-behaviour sanitizers when the compiler has them.

  scripts/check-fajita-thermal-host.py [--cxx clang++] [--out DIR]   (default DIR: .cache/fajita-out/host-tests)
"""
import argparse
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST = ROOT / 'package/tests/fajita-thermal-test.cpp'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cxx', default=shutil.which('clang++') or 'c++')
    parser.add_argument('--out', default=str(ROOT / '.cache/fajita-out/host-tests'))
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    san = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    probe = out / 'probe.cpp'
    probe.write_text('int main() { return 0; }\n')
    if subprocess.run([args.cxx, *san, str(probe), '-o', str(out / 'probe')], capture_output=True).returncode:
        san = []
    binary = out / 'fajita-thermal-test'
    subprocess.run([args.cxx, '-std=c++17', '-g', '-O1', '-Wall', '-Wextra', '-Werror', *san, '-I', str(ROOT / 'package'), str(TEST),
                    '-o', str(binary)], check=True)
    if subprocess.run([str(binary)]).returncode:
        raise SystemExit('FAIL: fajita-thermal-test')
    print(f'PASS: fajita thermal host test ({"sanitized" if san else "without sanitizers"})')


if __name__ == '__main__':
    main()
