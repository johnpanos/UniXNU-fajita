#!/bin/bash
# pull-log.sh OUTDIR: in the pmOS debug shell (telnet 172.16.42.1), read the UniXNU log window (the last 8192 4 KiB blocks of
# userdata, package/BootLog.c), gzip+base64 it over telnet, decode it with read-sheng-log.py --decode, then reboot to fastboot.
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W
OUT=${1:-$S/logs/$(date +%H%M%S)}; mkdir -p "$OUT"
timeout 240 python3 -I $TD/pmsh.py 'P=/dev/sda17; B=$(( $(cat /sys/class/block/sda17/size) / 8 )); dd if=$P bs=4096 skip=$((B-8192)) count=8192 2>/dev/null | gzip -1 | base64; echo ==LOG''END==' 60 > "$OUT/raw.txt" 2>&1
python3 -I - "$OUT" <<'PY'
import base64, gzip, re, sys
out = sys.argv[1]
text = open(out + '/raw.txt', errors='replace').read()
body = text.rsplit('==LOGEND==', 1)[0].split('__END__', 1)[-1]
lines = [l.strip() for l in body.splitlines() if re.fullmatch(r'[A-Za-z0-9+/=]+', l.strip() or '-')]
data = gzip.decompress(base64.b64decode(''.join(lines)))
open(out + '/log.bin', 'wb').write(data)
print('log.bin', len(data))
PY
python3 -I $TD/read-sheng-log.py --decode "$OUT/log.bin" --out "$OUT" 2>&1 | tail -3
# The diagnostic export (BootDiagExport.c: screen.bgrx, unified log, crash reports), when the header names one: it lies in the export
# region that ends export_below blocks below the window; unpack it into $OUT/diag and render the screenshot as screen.png.
H=$(ls $OUT/*/header.txt 2>/dev/null | head -1)
DB=$(grep '^diag_bytes=' "$H" 2>/dev/null | cut -d= -f2); DF=$(grep '^diag_first=' "$H" | cut -d= -f2); EB=$(grep '^export_below=' "$H" | cut -d= -f2)
if [ -n "$DB" ] && [ "$DB" -gt 0 ]; then
  N=$(( (DB + 4095) / 4096 ))
  timeout 400 python3 -I $TD/pmsh.py "P=/dev/sda17; B=\$(( \$(cat /sys/class/block/sda17/size) / 8 )); dd if=\$P bs=4096 skip=\$((B-8192-$EB+$DF)) count=$N 2>/dev/null | gzip -1 | base64; echo ==LOG''END==" 300 > "$OUT/diag-raw.txt" 2>&1
  python3 -I - "$OUT" "$DB" <<'PY'
import base64, gzip, re, sys, struct, zlib, importlib.util
out, size = sys.argv[1], int(sys.argv[2])
text = open(out + '/diag-raw.txt', errors='replace').read()
body = text.rsplit('==LOGEND==', 1)[0].split('__END__', 1)[-1]
lines = [l.strip() for l in body.splitlines() if re.fullmatch(r'[A-Za-z0-9+/=]+', l.strip() or '-')]
blob = gzip.decompress(base64.b64decode(''.join(lines)))[:size]
spec = importlib.util.spec_from_file_location('rsl', '$TD/read-sheng-log.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
import pathlib; m.unpack_export(blob, pathlib.Path(out), 'diag')
shot = open(out + '/diag/files/screen.bgrx', 'rb').read()
magic, w, h, stride = struct.unpack_from('<4sIII', shot)
raw = b''.join(b'\0' + bytes(x for i in range(w) for x in (shot[16 + y * stride + 4 * i + 2], shot[16 + y * stride + 4 * i + 1], shot[16 + y * stride + 4 * i])) for y in range(h))
chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(out + '/screen.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
print('diag', len(blob), 'screen', w, h)
PY
fi
