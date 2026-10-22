#!/usr/bin/env python3
"""Build the owner-signed userland of the SSH-over-USB channel for plain arm64 iOS
(not arm64e: the A10X iPadOS 17.7.11 userland has no pointer authentication):

  * fajita-reboot  (package/ssh-tools/fajita-reboot.c)
  * dropbearmulti  (dropbear SSH server + dropbearkey + scp + dbclient), from a
    pinned, sha256-checked source tarball
  * an ed25519 host key and the launchd job org.blu-sharky.sshd.plist

Everything is ad-hoc code-signed with 4 KiB pages (the kernel validates 4 KiB pages;
see native-4k-signature-hash-cache), exactly as scripts/build-adreno-metal-plugin.py
signs the Adreno probe daemon. The output and the cdhashes (codesign -dvvv) go to the
output directory with a JSON report; scripts/make-fajita-owner-trust.py adds the
cdhashes to an owner trust module on the Linux host.

This needs an iOS SDK and codesign, so it runs on macOS CI (xcrun --sdk iphoneos),
not on the Linux build host. See .github/workflows/ssh-userland.yml.

  build-ssh-userland.py --dropbear-tarball FILE [--out DIR] [--ios-version 17.0]

The tarball is untrusted: it is checked against DROPBEAR_SHA256, extracted into its
own directory, and built there; this script lives elsewhere and is passed paths.
"""
import argparse
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Pinned dropbear source. Update both together.
DROPBEAR_VERSION = '2024.86'
DROPBEAR_SHA256 = 'e78936dffc395f2e0db099321d6be659190966b99712b55c530dd0a1822e0a5e'

# Where the binaries and host key land on the device (under the GPU-addon mount root
# /private/var/ngpu; the daemons/ dir is unioned over /System/Library/LaunchDaemons).
DEVICE_BIN = '/private/var/ngpu/bin'
DEVICE_ETC = '/private/var/ngpu/etc'


def sdk_path():
    return subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()


def cdhash(path):
    out = subprocess.run(['codesign', '-d', '--verbose=4', str(path)], capture_output=True, text=True).stderr
    m = re.search(r'^CDHash=([a-f0-9]{40})$', out, re.M)
    if not m:
        raise SystemExit(f'no CDHash from codesign for {path}:\n{out}')
    return m.group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dropbear-tarball', required=True, help='pinned dropbear source tarball (sha256-checked)')
    parser.add_argument('--out', default=str(ROOT / 'build/ssh-userland'))
    parser.add_argument('--ios-version', default='17.0', help='minimum iOS/iPadOS version (the userland is 17.7.11)')
    args = parser.parse_args()

    out = Path(args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    sdk = sdk_path()
    target = f'arm64-apple-ios{args.ios_version}'
    clang = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', target, '-isysroot', sdk,
             '-arch', 'arm64', '-O2', '-Wall', '-Wextra', '-Werror', '-fstack-protector-strong']
    report = {'target': target, 'sdk': sdk, 'dropbear_version': DROPBEAR_VERSION,
              'dropbear_sha256': DROPBEAR_SHA256, 'cdhashes': {}}

    # 1. fajita-reboot. reboot(2) is allowed for root; no entitlements needed.
    reboot_bin = out / 'fajita-reboot'
    subprocess.run(clang + [str(ROOT / 'package/ssh-tools/fajita-reboot.c'), '-o', str(reboot_bin)], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
                    '--identifier', 'org.blu-sharky.fajita-reboot', str(reboot_bin)], check=True)
    subprocess.run(['codesign', '--verify', '--strict', str(reboot_bin)], check=True)
    report['cdhashes']['fajita-reboot'] = cdhash(reboot_bin)

    # 2. dropbear. Verify, extract, cross-build a static multi-call binary.
    tar = Path(args.dropbear_tarball).resolve()
    digest = hashlib.sha256(tar.read_bytes()).hexdigest()
    if digest != DROPBEAR_SHA256:
        raise SystemExit(f'dropbear tarball sha256 {digest} != pinned {DROPBEAR_SHA256}')
    src_root = out / 'dropbear-src'
    src_root.mkdir()
    with tarfile.open(tar) as t:
        t.extractall(src_root, filter='data')
    src = next(p for p in src_root.iterdir() if p.is_dir())

    # The server/host-outside-every-sandbox grant, like the Adreno probe daemon.
    ent = out / 'dropbear-entitlements.plist'
    ent.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True,
                                    'platform-application': True}))

    env = dict(os.environ, CC='xcrun --sdk iphoneos clang',
               CFLAGS=f'-target {target} -isysroot {sdk} -arch arm64 -O2',
               LDFLAGS=f'-target {target} -isysroot {sdk} -arch arm64')
    # --host marks a cross build so configure skips run-tests. iOS has no utmp/lastlog/wtmp.
    subprocess.run(['./configure', '--host=aarch64-apple-darwin', '--disable-zlib',
                    '--disable-lastlog', '--disable-utmp', '--disable-utmpx',
                    '--disable-wtmp', '--disable-wtmpx', '--disable-pututline', '--disable-pututxline'],
                   cwd=src, env=env, check=True)
    subprocess.run(['make', 'PROGRAMS=dropbear dropbearkey scp dbclient', 'MULTI=1',
                    '-j', str(os.cpu_count() or 2), 'dropbearmulti'], cwd=src, env=env, check=True)
    multi = out / 'dropbearmulti'
    shutil.copyfile(src / 'dropbearmulti', multi)
    multi.chmod(0o755)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
                    '--identifier', 'org.blu-sharky.dropbear', '--entitlements', str(ent), str(multi)], check=True)
    subprocess.run(['codesign', '--verify', '--strict', str(multi)], check=True)
    report['cdhashes']['dropbearmulti'] = cdhash(multi)

    # 3. An ed25519 host key, generated with the just-built dropbearkey on the CI host
    #    (dropbearmulti is an iOS binary and cannot run here; dropbearkey must come from
    #    a host build). We build a host dropbearkey too, only to make the key.
    host_key = out / 'dropbear_ed25519_host_key'
    env_host = dict(os.environ, CC='xcrun clang', CFLAGS='-O2', LDFLAGS='')
    build_host = out / 'dropbear-host'
    shutil.copytree(src, build_host)
    subprocess.run(['./configure', '--disable-zlib'], cwd=build_host, env=env_host, check=True)
    subprocess.run(['make', 'PROGRAMS=dropbearkey', '-j', str(os.cpu_count() or 2), 'dropbearkey'],
                   cwd=build_host, env=env_host, check=True)
    keygen = subprocess.run([str(build_host / 'dropbearkey'), '-t', 'ed25519', '-f', str(host_key)],
                            capture_output=True, text=True, check=True)
    shutil.rmtree(build_host)
    fp = re.search(r'(SHA256:\S+)', keygen.stdout + keygen.stderr)
    report['host_key_fingerprint'] = fp.group(1) if fp else None

    # 4. The launchd job. dropbear in the foreground, logging to stderr, with the owner
    #    host key, listening on port 22 on every interface (the CDC-ECM ifnet, once it
    #    exists). -R would write keys to a read-only path, so a fixed -r key is shipped.
    plist_name = 'org.blu-sharky.sshd.plist'
    plist = out / plist_name
    plist.write_bytes(plistlib.dumps(dict(
        Label='org.blu-sharky.sshd',
        ProgramArguments=[f'{DEVICE_BIN}/dropbearmulti', 'dropbear', '-F', '-E',
                          '-r', f'{DEVICE_ETC}/dropbear_ed25519_host_key', '-p', '22'],
        RunAtLoad=True, KeepAlive=True, UserName='root',
        StandardOutPath='/dev/console', StandardErrorPath='/dev/console'), fmt=plistlib.FMT_XML))

    # The addon layout the device consumes (scripts/make-fajita-ssh-addon.py).
    report['device_layout'] = {
        f'{DEVICE_BIN}/dropbearmulti': 'dropbearmulti',
        f'{DEVICE_BIN}/fajita-reboot': 'fajita-reboot',
        f'{DEVICE_ETC}/dropbear_ed25519_host_key': 'dropbear_ed25519_host_key',
        '/private/var/ngpu/daemons/' + plist_name: plist_name,
    }
    (out / 'trust.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: SSH userland built and signed')
    print(json.dumps(report['cdhashes'], indent=2))


if __name__ == '__main__':
    main()
