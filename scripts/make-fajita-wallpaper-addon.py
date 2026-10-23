#!/usr/bin/env python3
"""Build a wallpaper-only GPU addon for fajita: the default iPadOS 17 wallpaper with PNG images in place of its HEIC ones.

  make-fajita-wallpaper-addon.py [--edge PIXELS] [--release 20H392] OS_DMG OUTPUT

ImageIO cannot decode the default wallpaper's HEIC images without a hardware HEVC decoder, so PosterBoard's wallpaper stays black.
UniXNU's scripts/make-wallpaper-overlay.py (macOS: a mounted OS volume and sips) builds the PNG overlay, and make-gpu-addon.py --wallpaper
packs it, but only together with the Adreno Metal plugin, which fajita does not have. This script does both on Linux, without the plugin:

- reads the animation straight from the OS disk image of the IPSW (OS_DMG, 044-76040-038.dmg of 21H461, in the ipsw/21H461 directory
  of the unixnu output root) with 7-Zip (7z 21 or later reads DMG and APFS);
- decodes the two HEIC layers with libheif's heif-dec and scales them with ImageMagick to EDGE pixels square (default 2340, the long
  edge of the 6T panel; the layers stretch their contents to their bounds, sheng uses 2732 for its 12.9-inch class panel);
- rewrites main.caml to point at the PNG images, as make-wallpaper-overlay.py does;
- writes the NGPUADD2 file of make-gpu-addon.py with only wallpaper/* entries.

--release 20H392 (the iOS 16 board) takes the default iOS 16 iPhone wallpaper instead (7400, DefaultWallpapers~iphone.plist): three
layers (Background, Floating, Foreground .ca) whose five HEIC images are decoded at their own size, as wallpaper/<layer>.ca/...; the
xnu-8796 form of the patch (ios16-stack patches/xnu-8796) mounts them over the whole .wallpaper directory. OS_DMG: 087-86698-044.dmg.

Boot with it: "unixnu build payload --gpu-addon OUTPUT" and the boot argument sheng-gpu-addon=1. The kernel
(patches/xnu/zzy-native-gpu-addon-mount.patch) writes the files to /private/var/ngpu/wallpaper on the Data volume and mounts them over
the sealed animation; its other two mounts (plugin and launchd job) find no source directory and only record that.
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ANIMATION = ('Library/Wallpaper/Collections/iPadOS_17~ipad.wallpaperCollection/Wallpapers/7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper/'
             '7570.iPadOS_17_Flat_Background-1024w-1366h@2x~ipad.ca')
IMAGES = ('J120', 'J120-Dark')   # the iPad7,1 (J120) variants, the ones PosterBoard picks for fajita's iPad identity
# iOS 16 (20H392): the default iPhone wallpaper, every layer of it (patches/xnu-8796/zzy-native-gpu-addon-mount.patch of ios16-stack).
WALLPAPER_16 = 'Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers/7400.WWDC_2022-375w-812h@3x~iphone.wallpaper'


def layers_16(sevenzip, heif, magick, dmg, work):
    """[(path under wallpaper/, bytes)] of the iOS 16 wallpaper: each layer's documents, main.caml on PNG images."""
    import re
    subprocess.run([sevenzip, 'x', '-y', f'-o{work}', str(dmg), f'{WALLPAPER_16}/*', '-r'], check=True, stdout=subprocess.DEVNULL)
    source = work / WALLPAPER_16
    layers = sorted(path for path in source.glob('*.ca') if (path / 'main.caml').is_file())
    if len(layers) != 3:
        sys.exit(f'Expected the three layers of {WALLPAPER_16} in {dmg}, found {len(layers)}')
    files = []
    for layer in layers:
        for name in ('index.xml', 'assetManifest.caml'):
            files.append((f'{layer.name}/{name}', (layer / name).read_bytes()))
        main_caml = (layer / 'main.caml').read_text()
        for reference in sorted(set(re.findall(r'"(assets/[^"]+\.HEIC)"', main_caml))):
            image = reference[len('assets/'):-len('.HEIC')].replace('%20', ' ')
            png = image.replace(' ', '-') + '.png'
            decoded = work / f'{layer.name}-{png}'
            subprocess.run([heif, str(layer / 'assets' / f'{image}.HEIC'), str(decoded)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run([magick, str(decoded), '-strip', f'PNG32:{decoded}'], check=True)
            main_caml = main_caml.replace(f'"{reference}"', f'"{png}"')
            files.append((f'{layer.name}/{png}', decoded.read_bytes()))
        if re.search(r'src="[^"]*\.HEIC"', main_caml):
            sys.exit(f'{layer.name}/main.caml still points at a HEIC image')
        files.append((f'{layer.name}/main.caml', main_caml.encode()))
    return files


def pad(data):
    return data + bytes(-len(data) % 8)


def tool(name):
    path = shutil.which(name)
    if not path:
        sys.exit(f'{name} not found (7z: 7-Zip 21+, heif-dec: libheif, magick: ImageMagick 7)')
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--edge', type=int, default=2340, help='edge of the square PNG images in pixels (default 2340)')
    parser.add_argument('--release', choices=('21H461', '20H392'), default='21H461',
                        help='21H461: the iPadOS 17 wallpaper (default); 20H392: the iOS 16 iPhone wallpaper')
    parser.add_argument('dmg', type=Path, help='the OS disk image of the IPSW (044-76040-038.dmg; 20H392: 087-86698-044.dmg)')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        sys.exit(f'Refuse to overwrite {args.output}')
    if not args.dmg.is_file():
        sys.exit(f'{args.dmg} not found')
    if not 256 <= args.edge <= 3416:
        sys.exit('--edge takes 256 to 3416 (the HEIC images are 3416 pixels square)')
    sevenzip, heif, magick = tool('7z'), tool('heif-dec'), tool('magick')
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        if args.release == '20H392':
            files = layers_16(sevenzip, heif, magick, args.dmg, work)
        else:
            files = layers_17(sevenzip, heif, magick, args.dmg, args.edge, work)
    write_addon(files, args.output)


def layers_17(sevenzip, heif, magick, dmg, edge, work):
    """[(path under wallpaper/, bytes)] of the iPadOS 17 wallpaper's Background layer on PNG images."""
    subprocess.run([sevenzip, 'x', '-y', f'-o{work}', str(dmg), f'{ANIMATION}/*', '-r'], check=True, stdout=subprocess.DEVNULL)
    source = work / ANIMATION
    if not (source / 'main.caml').is_file():
        sys.exit(f'No wallpaper animation at {ANIMATION} in {dmg}')
    files = []
    for name in ('index.xml', 'assetManifest.caml'):
        files.append((name, (source / name).read_bytes()))
    main_caml = (source / 'main.caml').read_text()
    for image in IMAGES:
        main_caml = main_caml.replace(f'"/assets/{image}.HEIC"', f'"/{image}.png"').replace(f'"assets/{image}.HEIC"', f'"{image}.png"')
        decoded, scaled = work / f'{image}-full.png', work / f'{image}.png'
        subprocess.run([heif, str(source / 'assets' / f'{image}.HEIC'), str(decoded)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([magick, str(decoded), '-resize', f'{edge}x{edge}', '-strip', f'PNG32:{scaled}'], check=True)
        files.append((f'{image}.png', scaled.read_bytes()))
    if '.HEIC"/>' in main_caml:
        sys.exit('main.caml still points at a HEIC image')
    files.append(('main.caml', main_caml.encode()))
    return files


def write_addon(files, output):
    # The addon format of UniXNU scripts/make-gpu-addon.py: "NGPUADD2", u32 count, u32 0, u64 total bytes; per file u32 path bytes
    # (with its NUL), u32 mode, u64 size, u64 compressed size, the path padded to 8 bytes, the zlib stream padded to 8 bytes.
    body = b''
    for name, data in sorted(files):
        encoded = f'wallpaper/{name}'.encode() + b'\0'
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), 0o644, len(data), len(packed)) + pad(encoded) + pad(packed)
    blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    output.write_bytes(blob)
    for name, data in sorted(files):
        print(f'644 {len(data):>10} wallpaper/{name}')
    print(f'{output}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
