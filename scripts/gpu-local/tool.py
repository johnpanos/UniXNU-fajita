#!/usr/bin/env python3
"""Linux stand-ins for the Xcode tools the UniXNU GPU build scripts call (scripts/build-turnip-darwin.py, build-native-metal-bridge.py,
build-adreno-metal-plugin.py), so those scripts run unchanged on Linux. Dispatched on the name it is called by (bin/<name> symlinks):

  xcrun     --show-sdk-path / --find TOOL / `xcrun [-sdk S] TOOL ARGS` -> the CI's iPhoneOS SDK (GPU_LOCAL_SDK) and the tools below
  clang, clang++  LLVM clang for arm64-apple-ios with ld64.lld. A compile-and-link of several sources is split: each source is compiled
            on its own (in parallel, through ccache when present) and the objects are linked, so an unchanged .mm is not rebuilt.
  codesign  ad-hoc signing with rcodesign (GPU_LOCAL_RCODESIGN); --display / -d --entitlements / --verify in codesign's output format
  strip, nm, ar  llvm-strip / llvm-nm / llvm-ar
  metal, metallib  the scanout metallib is Apple-compiler output: served from GPU_LOCAL_METALLIB_CACHE by the sha256 of the .metal
            source and flags (seeded from a CI artifact by build-gpu-plugin-local.sh --seed-metallib RUN); on a miss, compiled with Apple's
            Metal Developer Tools for Windows under Wine (GPU_LOCAL_METAL_WINEPREFIX, set up by --setup-metal-win) into <cache>/wine/

Set up by scripts/build-gpu-plugin-local.sh; not meant to be run directly."""
import base64
import concurrent.futures
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
BIN = HERE / 'bin'
SDK = os.environ.get('GPU_LOCAL_SDK', '')
CLANG = os.environ.get('GPU_LOCAL_CLANG', shutil.which('clang', path='/usr/bin') or 'clang')
LLVM = os.environ.get('GPU_LOCAL_LLVM_BIN', str(Path(CLANG).resolve().parent))
RCODESIGN = os.environ.get('GPU_LOCAL_RCODESIGN', 'rcodesign')
OBJ = Path(os.environ.get('GPU_LOCAL_OBJ', '/tmp/gpu-local-obj'))
CCACHE = shutil.which('ccache', path='/usr/bin:/usr/local/bin')


def die(msg):
    sys.stderr.write('gpu-local %s: %s\n' % (Path(sys.argv[0]).name, msg))
    sys.exit(1)


def llvm(name):
    path = Path(LLVM) / name
    return str(path) if path.exists() else (shutil.which(name) or name)


# ---- clang ---------------------------------------------------------------------------------------------------------------------------
SOURCE_EXT = {'.c': 'c', '.m': 'objective-c', '.mm': 'objective-c++', '.cc': 'c++', '.cpp': 'c++', '.cxx': 'c++', '.s': 'assembler',
              '.S': 'assembler-with-cpp'}
# Options that take the next argument as their value.
WITH_VALUE = {'-o', '-x', '-target', '-isysroot', '-I', '-D', '-U', '-include', '-isystem', '-iquote', '-framework', '-F', '-L', '-arch',
              '-Xlinker', '-MF', '-MT', '-MQ', '-idirafter', '-iframework', '-install_name', '-rpath', '-compatibility_version',
              '-current_version', '-exported_symbols_list', '-unexported_symbols_list', '-weak_framework', '-resource-dir'}
LINK_ONLY = re.compile(r'^(-Wl,.*|-l.*|-bundle|-dynamiclib|-shared|-dead_strip|-framework|-weak_framework|-F.*|-L.*|-rpath|-install_name|'
                       r'-compatibility_version|-current_version|-headerpad_max_install_names|-Xlinker|-exported_symbols_list|'
                       r'-unexported_symbols_list|-undefined|-flat_namespace|-fuse-ld=.*)$')


def clang_args(cxx):
    driver = CLANG + ('++' if cxx and not CLANG.endswith('++') else '')
    if cxx and not Path(driver).exists():
        driver = CLANG
        extra = ['--driver-mode=g++']
    else:
        extra = []
    return driver, extra


def run_clang(argv, cxx):
    driver, mode = clang_args(cxx)
    args = list(argv)
    if '-target' not in args and not any(a.startswith('--target=') for a in args):
        args = ['-target', 'arm64-apple-ios' + os.environ.get('UNIXNU_IOS_TARGET', '17.0')] + args
    if '-isysroot' not in args and SDK:
        args = ['-isysroot', SDK] + args
    # The SDK's libc++ headers (the dylib's ABI), never the host's (/usr/include/c++/v1 sits next to /usr/bin/clang and wins otherwise).
    if SDK and (cxx or any(a in ('c++', 'objective-c++') for a in args)):
        args = ['-nostdinc++', '-isystem', SDK + '/usr/include/c++/v1'] + args
    # Apple clang 17 (Xcode 16.4) default kept: no sized deallocation (operator delete(void*), as the CI build imports it).
    if cxx:
        args = ['-fno-sized-deallocation'] + args
    compile_only = any(a in ('-c', '-E', '-S', '-M', '-MM', '-fsyntax-only') for a in args) or '-v' in args or '--version' in args
    # The darwin driver links libclang_rt.ios.a from its resource directory; LLVM's Linux build has none, so give it Xcode's (from the
    # exported SDK) for ___isPlatformVersionAtLeast and friends.
    rt = os.environ.get('GPU_LOCAL_CLANG_RT')
    link_extra = ['-fuse-ld=lld', '-Wno-unused-command-line-argument'] + ([rt] if rt and Path(rt).exists() else [])
    if compile_only:
        os.execv(driver, [driver, *mode, *args])
    # Split sources out of a compile-and-link command.
    sources, objects, compile_opts, link_opts = [], [], [], []
    lang, out, i = None, None, 0
    while i < len(args):
        a = args[i]
        if a == '-x':
            lang = None if args[i + 1] == 'none' else args[i + 1]
            i += 2
            continue
        if a == '-o':
            out = args[i + 1]
            i += 2
            continue
        if a in WITH_VALUE:
            pair = [a, args[i + 1]]
            (link_opts if LINK_ONLY.match(a) else compile_opts).extend(pair)
            if a in ('-target', '-isysroot', '-arch'):
                link_opts.extend(pair)
            i += 2
            continue
        if a.startswith('-'):
            if LINK_ONLY.match(a):
                link_opts.append(a)
            else:
                compile_opts.append(a)
                if a.startswith(('-target', '--target', '-isysroot', '-arch', '-mios', '-miphoneos', '-fobjc-arc', '-fstack-protector',
                                 '-stdlib', '-g', '-O', '-flto', '-fsanitize')):
                    link_opts.append(a)
            i += 1
            continue
        ext = Path(a).suffix
        if lang or ext in SOURCE_EXT:
            sources.append((a, lang or SOURCE_EXT[ext]))
        else:
            objects.append(a)
        i += 1
    if not sources:
        os.execv(driver, [driver, *mode, *args, *link_extra])
    OBJ.mkdir(parents=True, exist_ok=True)

    def compile_one(item):
        src, language = item
        key = hashlib.sha256(('\0'.join([str(Path(src).resolve()), language, *compile_opts])).encode()).hexdigest()[:20]
        obj = OBJ / ('%s-%s.o' % (Path(src).stem, key))
        cmd = ([CCACHE] if CCACHE and os.environ.get('GPU_LOCAL_CCACHE', '1') == '1' else []) + \
              [driver, *mode, *compile_opts, '-x', language, src, '-c', '-o', str(obj)]
        result = subprocess.run(cmd)
        if result.returncode:
            sys.exit(result.returncode)
        return str(obj)

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(sources)) as pool:
        compiled = list(pool.map(compile_one, sources))
    cmd = [driver, *mode, *link_opts, *compiled, *objects, *link_extra] + (['-o', out] if out else [])
    sys.exit(subprocess.run(cmd).returncode)


def target_version(args):
    for i, a in enumerate(args):
        t = args[i + 1] if a == '-target' and i + 1 < len(args) else (a.split('=', 1)[1] if a.startswith('--target=') else None)
        if t and 'ios' in t:
            return re.sub(r'^.*ios', '', t) or '17.0'
    return os.environ.get('UNIXNU_IOS_TARGET', '17.0')


def sdk_version():
    try:
        import json
        return json.loads((Path(SDK) / 'SDKSettings.json').read_text())['Version']
    except Exception:
        return '18.5'


# ---- codesign ------------------------------------------------------------------------------------------------------------------------
def macho_of(path):
    p = Path(path)
    if p.is_dir():
        info = plistlib.loads((p / 'Info.plist').read_bytes())
        return p / info['CFBundleExecutable']
    return p


def parse_signature(target):
    """The embedded signature of a thin arm64 Mach-O: {blob magic: bytes}, the CodeDirectory fields, and the file bytes."""
    data = Path(target).read_bytes()
    if int.from_bytes(data[0:4], 'little') != 0xfeedfacf:
        die('%s: not a thin 64-bit Mach-O' % target)
    ncmds, at = int.from_bytes(data[16:20], 'little'), 32
    sig = None
    for _ in range(ncmds):
        cmd, size = int.from_bytes(data[at:at + 4], 'little'), int.from_bytes(data[at + 4:at + 8], 'little')
        if cmd == 0x1d:   # LC_CODE_SIGNATURE
            sig = (int.from_bytes(data[at + 8:at + 12], 'little'), int.from_bytes(data[at + 12:at + 16], 'little'))
        at += size
    if not sig:
        return None
    off, _ = sig
    be = lambda o, n=4: int.from_bytes(data[o:o + n], 'big')
    if be(off) != 0xfade0cc0:
        die('%s: bad superblob' % target)
    blobs, slots = {}, {}
    for k in range(be(off + 8)):
        typ, boff = be(off + 12 + 8 * k), be(off + 16 + 8 * k)
        magic, length = be(off + boff), be(off + boff + 4)
        blobs.setdefault(magic, data[off + boff:off + boff + length])
        slots[typ] = data[off + boff:off + boff + length]
    cd = slots.get(0)
    if cd is None:
        die('%s: no CodeDirectory' % target)
    c = lambda o, n=4: int.from_bytes(cd[o:o + n], 'big')
    fields = dict(version=c(8), flags=c(12), hash_offset=c(16), ident_offset=c(20), n_special=c(24), n_code=c(28), code_limit=c(32),
                  hash_size=cd[36], hash_type=cd[37], page_log2=cd[39])
    fields['identifier'] = cd[fields['ident_offset']:cd.index(b'\0', fields['ident_offset'])].decode()
    fields['cdhash'] = hashlib.sha256(cd).hexdigest()
    return dict(data=data, blobs=blobs, slots=slots, cd=cd, **fields)


def sig_info(path):
    info = parse_signature(macho_of(path))
    if not info:
        die('%s: code object is not signed at all' % path)
    return dict(cdhash=info['cdhash'], identifier=info['identifier'], digests=info['n_code'], size=info['code_limit'], page=1 << info['page_log2'])


def verify_signature(path):
    """What codesign --verify checks for an ad-hoc signature: every code page hash, and the special slots (Info.plist, requirements,
    resources, entitlements, DER entitlements) against their current contents. Returns a list of problems."""
    target = macho_of(path)
    info = parse_signature(target)
    if not info:
        return ['not signed']
    problems, cd, data = [], info['cd'], info['data']
    if info['hash_type'] != 2 or info['hash_size'] != 32:
        problems.append('hash type %d' % info['hash_type'])
    page = 1 << info['page_log2']
    base = info['hash_offset']
    for n in range(info['n_code']):
        chunk = data[n * page:min((n + 1) * page, info['code_limit'])]
        if hashlib.sha256(chunk).digest() != cd[base + 32 * n:base + 32 * (n + 1)]:
            problems.append('page %d' % n)
            break
    special = {2: info['slots'].get(2), 5: info['slots'].get(5), 7: info['slots'].get(7)}
    if Path(path).is_dir():
        special[1] = (Path(path) / 'Info.plist').read_bytes()
        seal = Path(path) / '_CodeSignature/CodeResources'
        special[3] = seal.read_bytes() if seal.exists() else None
        if special[3] is not None and special[3] != code_resources(Path(path), target.name):
            problems.append('a sealed resource changed')
    for slot in range(1, info['n_special'] + 1):
        want = cd[base - 32 * slot:base - 32 * (slot - 1)]
        have = special.get(slot)
        if have is None:
            if want != bytes(32):
                problems.append('slot %d missing' % slot)
        elif hashlib.sha256(have).digest() != want:
            problems.append('slot %d differs' % slot)
    return problems


def code_resources(bundle, exe_name):
    """_CodeSignature/CodeResources the way codesign writes it for a flat bundle with only plain files: SHA-1 `files` for every file but
    the executable, SHA-256 `files2` for every file but the executable and Info.plist, and codesign's default iOS rules."""
    files, files2 = {}, {}
    for f in sorted(bundle.rglob('*')):
        rel = f.relative_to(bundle).as_posix()
        if f.is_dir() or rel == exe_name or rel.startswith('_CodeSignature/'):
            continue
        data = f.read_bytes()
        files[rel] = hashlib.sha1(data).digest()
        if rel != 'Info.plist':
            files2[rel] = {'hash2': hashlib.sha256(data).digest()}
    rules = {'^.*': True, '^.*\\.lproj/': {'optional': True, 'weight': 1000.0},
             '^.*\\.lproj/locversion.plist$': {'omit': True, 'weight': 1100.0}, '^Base\\.lproj/': {'weight': 1010.0},
             '^version.plist$': True}
    rules2 = {'.*\\.dSYM($|/)': {'weight': 11.0}, '^(.*/)?\\.DS_Store$': {'omit': True, 'weight': 2000.0}, '^.*': True,
              '^.*\\.lproj/': {'optional': True, 'weight': 1000.0}, '^.*\\.lproj/locversion.plist$': {'omit': True, 'weight': 1100.0},
              '^Base\\.lproj/': {'weight': 1010.0}, '^Info\\.plist$': {'omit': True, 'weight': 20.0},
              '^PkgInfo$': {'omit': True, 'weight': 20.0}, '^embedded\\.provisionprofile$': {'weight': 20.0},
              '^version\\.plist$': {'weight': 20.0}}
    xml = plistlib.dumps({'files': files, 'files2': files2, 'rules': rules, 'rules2': rules2}, fmt=plistlib.FMT_XML)
    return re.sub(rb'<real>(\d+)\.0</real>', rb'<real>\1</real>', xml)   # codesign writes whole reals without the fraction


def run_codesign(argv):
    opts, paths, i = {}, [], 0
    while i < len(argv):
        a = argv[i]
        if a in ('--sign', '-s', '--identifier', '-i', '--entitlements', '--pagesize', '--options', '-o'):
            opts[a.lstrip('-')[:1] if len(a) == 2 else a.lstrip('-')] = argv[i + 1]
            i += 2
        elif a.startswith('--verbose') or a.startswith('-v') and set(a[1:]) <= {'v'}:
            opts['verbose'] = True
            i += 1
        elif a.startswith('-'):
            opts[a.lstrip('-')] = True
            i += 1
        else:
            paths.append(a)
            i += 1
    sign = opts.get('sign') or opts.get('s')
    for path in paths:
        if sign:
            if sign != '-':
                die('only ad-hoc signing (--sign -) is supported')
            if opts.get('pagesize', '4096') != '4096':
                die('rcodesign signs with 4 KiB pages only')
            target = macho_of(path)
            cmd = [RCODESIGN, 'sign']
            ident = opts.get('identifier') or opts.get('i')
            if ident:
                cmd += ['--binary-identifier', ident]
            if opts.get('entitlements'):
                cmd += ['--entitlements-xml-file', opts['entitlements']]
            if Path(path).is_dir():
                bundle = Path(path)
                seal = bundle / '_CodeSignature'
                shutil.rmtree(seal, ignore_errors=True)
                seal.mkdir()
                (seal / 'CodeResources').write_bytes(code_resources(bundle, target.name))
                cmd += ['--info-plist-file', str(bundle / 'Info.plist'), '--code-resources-file', str(seal / 'CodeResources')]
            result = subprocess.run(cmd + [str(target)], capture_output=True, text=True)
            if result.returncode:
                die('rcodesign failed on %s:\n%s' % (target, result.stderr[-2000:]))
            sys.stderr.write('%s: replacing existing signature\n' % path)
        elif opts.get('verify'):
            target = macho_of(path)
            problems = verify_signature(path)
            if problems:
                die('%s: invalid signature (%s)' % (path, ', '.join(problems)))
        elif opts.get('entitlements') or opts.get('d') and 'entitlements' in opts:
            ents = extract_entitlements(macho_of(path))
            sys.stdout.buffer.write(ents)
        elif opts.get('display') or opts.get('d') or opts.get('dvvv') or opts.get('dv'):
            info = sig_info(path)
            target = macho_of(path)
            sys.stderr.write('Executable=%s\nIdentifier=%s\nFormat=Mach-O thin (arm64)\nCodeDirectory v=20400 size=0 flags=0x2(adhoc) '
                             'hashes=%d+7 location=embedded\nHash type=sha256 size=32\nCandidateCDHash sha256=%s\n'
                             'CandidateCDHashFull sha256=%s\nHash choices=sha256\nCMSDigest=%s\nCMSDigestType=2\nPage size=%d\n'
                             'CDHash=%s\nSignature=adhoc\nInfo.plist=%s\nTeamIdentifier=not set\nSealed Resources=%s\n'
                             'Internal requirements count=0 size=12\n'
                             % (target.resolve(), info['identifier'], info['digests'], info['cdhash'][:40], info['cdhash'],
                                info['cdhash'], info['page'], info['cdhash'][:40], 'entries=' if Path(path).is_dir() else 'not bound',
                                'version=2 rules=13 files=%d' % len(list(Path(path).iterdir())) if Path(path).is_dir() else 'none'))
        else:
            die('unsupported codesign invocation: %s' % ' '.join(argv))


def extract_entitlements(target):
    """The entitlements XML blob (0xfade7171) of the embedded signature, as `codesign -d --entitlements :-` prints it."""
    info = parse_signature(target)
    blob = info and info['blobs'].get(0xfade7171)
    return blob[8:] if blob else b''


# ---- metal ---------------------------------------------------------------------------------------------------------------------------
def metal_key(source, flags):
    return hashlib.sha256(Path(source).read_bytes() + b'\0' + ' '.join(sorted(flags)).encode()).hexdigest()


WINE_METAL_BIN = 'C:\\metal\\ios\\bin\\'


def wine_metal_env():
    """The Wine environment for the Metal Developer Tools for Windows, or None when Wine or the installed tools are missing."""
    prefix = os.environ.get('GPU_LOCAL_METAL_WINEPREFIX', '')
    if not prefix or not (Path(prefix) / 'drive_c/metal/ios/bin/metal.exe').is_file() or not shutil.which('wine'):
        return None
    return dict(os.environ, WINEPREFIX=prefix, WINEDEBUG='-all')


def wine_metallib(source, flags, out):
    """Compile source to a metallib at out with metal.exe + metallib.exe under Wine (paths as Z: drive paths: the tools mishandle a bare
    Unix output path)."""
    env = wine_metal_env()
    if env is None:
        return False
    air = Path(str(out) + '.air')
    z = lambda path: 'Z:' + str(Path(path).resolve()).replace('/', '\\')
    for cmd in (['wine', WINE_METAL_BIN + 'metal.exe', *flags, '-c', z(source), '-o', z(air)],
                ['wine', WINE_METAL_BIN + 'metallib.exe', z(air), '-o', z(out)]):
        if subprocess.run(cmd, env=env).returncode:
            die('%s failed under Wine' % Path(cmd[1].replace('\\', '/')).name)
    air.unlink()
    return True


def run_metal(argv):
    out = argv[argv.index('-o') + 1]
    sources = [a for a in argv if a.endswith('.metal')]
    flags = [a for a in argv if a.startswith(('-std=', '-mios-version-min=', '-O'))]
    if len(sources) != 1:
        die('expected one .metal source')
    key = metal_key(sources[0], flags)
    # A CI (Xcode) metallib for this source wins; otherwise build one with the Windows Metal tools, kept by key under <cache>/wine.
    cache = Path(os.environ.get('GPU_LOCAL_METALLIB_CACHE', ''))
    wine_lib = cache / 'wine' / (key + '.metallib')
    if not (cache / (key + '.metallib')).is_file() and not wine_lib.is_file():
        wine_lib.parent.mkdir(parents=True, exist_ok=True)
        tmp = wine_lib.with_suffix('.tmp')
        if wine_metallib(sources[0], flags, tmp):
            tmp.rename(wine_lib)
            sys.stderr.write('gpu-local metal: %s compiled with the Metal Developer Tools for Windows (no CI metallib for key %s)\n'
                             % (Path(sources[0]).name, key[:16]))
    Path(out).write_text('GPU-LOCAL-AIR %s\n' % key)


def run_metallib(argv):
    out = argv[argv.index('-o') + 1]
    air = [a for a in argv if a.endswith('.air')][0]
    key = Path(air).read_text().split()[1]
    cache = Path(os.environ.get('GPU_LOCAL_METALLIB_CACHE', ''))
    for lib in (cache / (key + '.metallib'), cache / 'wine' / (key + '.metallib')):
        if lib.is_file():
            shutil.copyfile(lib, out)
            return
    die('no metallib for this .metal source (key %s) and no Metal Developer Tools for Windows under Wine: run build-gpu-plugin-local.sh '
        '--setup-metal-win, or the "GPU plugin (iOS 16)" CI for the changed shader and then --seed-metallib RUN_ID' % key[:16])


# ---- xcrun ---------------------------------------------------------------------------------------------------------------------------
TOOLS = {'clang': 'clang', 'clang++': 'clang++', 'cc': 'clang', 'c++': 'clang++', 'ar': 'ar', 'strip': 'strip', 'nm': 'nm',
         'codesign': 'codesign', 'metal': 'metal', 'metallib': 'metallib', 'ranlib': 'ranlib', 'ld': 'ld', 'lipo': 'lipo'}


def run_xcrun(argv):
    args = list(argv)
    while args and args[0] in ('--sdk', '-sdk', '-find', '--find', '--show-sdk-path', '--show-sdk-version', '-show-sdk-path', '--toolchain'):
        a = args.pop(0)
        if a in ('--sdk', '-sdk', '--toolchain'):
            args.pop(0)
        elif a in ('--show-sdk-path', '-show-sdk-path'):
            print(SDK)
            return
        elif a == '--show-sdk-version':
            print(sdk_version())
            return
        elif a in ('-find', '--find'):
            tool = args.pop(0)
            if tool not in TOOLS:
                die('no stand-in for %s' % tool)
            print(BIN / TOOLS[tool])
            return
    if not args:
        die('nothing to run')
    tool = args.pop(0)
    if tool not in TOOLS:
        die('no stand-in for %s' % tool)
    os.execv(str(BIN / TOOLS[tool]), [str(BIN / TOOLS[tool]), *args])


def main():
    name = Path(sys.argv[0]).name
    argv = sys.argv[1:]
    if name == 'xcrun':
        run_xcrun(argv)
    elif name in ('clang', 'cc'):
        run_clang(argv, False)
    elif name in ('clang++', 'c++'):
        run_clang(argv, True)
    elif name == 'codesign':
        run_codesign(argv)
    elif name == 'strip':
        os.execv(llvm('llvm-strip'), [llvm('llvm-strip'), *argv])
    elif name == 'nm':
        os.execv(llvm('llvm-nm'), [llvm('llvm-nm'), *argv])
    elif name in ('ar', 'ranlib'):
        tool = llvm('llvm-' + name)
        os.execv(tool, [tool, *argv])
    elif name == 'lipo':
        os.execv(llvm('llvm-lipo'), [llvm('llvm-lipo'), *argv])
    elif name == 'ld':
        os.execv(llvm('ld64.lld'), [llvm('ld64.lld'), *argv])
    elif name == 'metal':
        run_metal(argv)
    elif name == 'metallib':
        run_metallib(argv)
    else:
        die('unknown tool name')


if __name__ == '__main__':
    main()
