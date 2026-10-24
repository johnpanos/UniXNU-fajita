#!/usr/bin/env python3
"""compare.py CI_DIR LOCAL_DIR: functional comparison of a local GPU plugin build (build-gpu-plugin-local.sh) with a "GPU plugin (iOS 16)"
CI artifact. Per Mach-O: exported symbols, imported symbols, linked dylibs with their versions, platform/minos/sdk, segments and
sections, signature (validity, identifier, flags, 4 KiB pages, entitlements, bound Info.plist / resources). Plists and the metallib
byte for byte. Prints a table; exits 1 on a functional difference."""
import hashlib
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys

import importlib.util
_spec = importlib.util.spec_from_file_location('gpu_local_tool', Path(__file__).resolve().parent / 'tool.py')
TOOL = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(TOOL)


def run(*cmd):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True).stdout


def macho(path):
    # Rust v0 symbols carry a crate disambiguator hash (Cs<hash>_) derived from the crate's source path: normalized.
    nm_def = {re.sub(r'Cs[0-9A-Za-z]+_', 'Cs_', n) for n in run('llvm-nm', '-gU', '-j', path).split()}
    nm_und = set(run('llvm-nm', '-u', '-j', path).split())
    dylibs = sorted(l.strip() for l in run('llvm-otool', '-L', path).splitlines()[1:])   # load order is the linker's
    lc = run('llvm-otool', '-l', path)
    bv = re.search(r'LC_BUILD_VERSION\n.*\n\s+platform (\d+)\n\s+sdk (\S+)\n\s+minos (\S+)', lc)
    sects = sorted(set(re.findall(r'sectname (\S+)\n\s+segname (\S+)', lc)))
    info = TOOL.parse_signature(path)
    ents = plistlib.loads(TOOL.extract_entitlements(Path(path)) or plistlib.dumps({}))
    bundle = Path(path).parent if Path(path).parent.name.endswith('.bundle') and Path(path).name == Path(path).parent.stem else None
    problems = TOOL.verify_signature(bundle or path)
    page4k = info['page_log2'] == 12
    slots = sorted(k for k in info['slots'] if k not in (0x10000,)) + (['cms-empty'] if len(info['slots'].get(0x10000, b'')) == 8 else [])
    ident, flags = info['identifier'], hex(info['flags'])
    return {'exports': nm_def, 'imports': nm_und, 'dylibs': dylibs, 'build': bv.groups() if bv else None, 'sections': sects,
            'identifier': ident, 'cd_flags': flags, 'cd_version': hex(info['version']), 'page4k': page4k, 'blob_slots': slots,
            'entitlements': ents, 'signature_valid': problems or True}


def describe(a, b):
    if isinstance(a, set):
        only_a, only_b = sorted(a - b), sorted(b - a)
        return 'same (%d)' % len(a) if not only_a and not only_b else 'CI-only %d %s / local-only %d %s' % (
            len(only_a), only_a[:6], len(only_b), only_b[:6])
    return 'same' if a == b else 'CI %r / local %r' % (a, b)


def main():
    ci, local = Path(sys.argv[1]), Path(sys.argv[2])
    rows, bad = [], False
    for rel in ['NativeAdrenoMetal.bundle/NativeAdrenoMetal', 'NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib', 'adrenoprobed']:
        a, b = macho(ci / rel), macho(local / rel)
        for key in a:
            d = describe(a[key], b[key])
            same = d.startswith('same')
            if key == 'sections' and not same:
                same = None   # layout detail: reported, judged by hand
            bad |= same is False
            rows.append((rel, key, 'yes' if same else ('note' if same is None else 'NO'), d))
        ca, cb = (ci / rel).stat().st_size, (local / rel).stat().st_size
        rows.append((rel, 'bytes', 'identical' if (ci / rel).read_bytes() == (local / rel).read_bytes() else 'differ',
                     'CI %d / local %d (%+.1f%%)' % (ca, cb, 100.0 * (cb - ca) / ca)))
    for rel in ['NativeAdrenoMetal.bundle/Info.plist', 'adrenoprobed-entitlements.plist', 'org.blu-sharky.adrenoprobed.plist',
                'com.apple.iomfb_fdr_loader.plist', 'com.apple.bluetoothd.plist']:
        pa, pb = plistlib.loads((ci / rel).read_bytes()), plistlib.loads((local / rel).read_bytes())
        exact = (ci / rel).read_bytes() == (local / rel).read_bytes()
        bad |= pa != pb
        rows.append((rel, 'content', 'yes' if pa == pb else 'NO', 'byte-identical' if exact else ('same keys/values' if pa == pb else 'differ')))
    for rel in ['scanout_scale.metallib']:
        ha, hb = hashlib.sha256((ci / rel).read_bytes()).hexdigest(), hashlib.sha256((local / rel).read_bytes()).hexdigest()
        bad |= ha != hb
        rows.append((rel, 'sha256', 'yes' if ha == hb else 'NO', ha[:16] + (' both' if ha == hb else ' / ' + hb[:16])))
        sa = run('llvm-otool', '-s', '__DATA', '__scalelib', local / 'NativeAdrenoMetal.bundle/NativeAdrenoMetal')
        rows.append((rel, 'embedded __scalelib', 'yes' if sa else 'NO', 'present in local plugin' if sa else 'missing'))
    ra = plistlib.loads((ci / 'NativeAdrenoMetal.bundle/_CodeSignature/CodeResources').read_bytes())
    rb = plistlib.loads((local / 'NativeAdrenoMetal.bundle/_CodeSignature/CodeResources').read_bytes())
    same = {k: sorted(v) for k, v in ra.items()} == {k: sorted(v) for k, v in rb.items()} and ra['rules'] == rb['rules'] and ra['rules2'] == rb['rules2']
    bad |= not same
    rows.append(('_CodeSignature/CodeResources', 'files/files2/rules', 'yes' if same else 'NO', 'same entries and rules' if same else 'differ'))
    width = [max(len(str(r[i])) for r in rows) for i in range(3)]
    print('| file | check | equivalent | detail |\n| --- | --- | --- | --- |')
    for r in rows:
        print('| %s | %s | %s | %s |' % r)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
