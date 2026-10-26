#!/usr/bin/env python3
"""persist-wlan-mac.py [persist.img]: print the phone's Wi-Fi MAC from /wlan_mac.bin of a persist image (read-only, debugfs).

The OnePlus 6T keeps "Intf<N>MacAddress=<12 hex>" lines in persist:/wlan_mac.bin (on this phone only Intf2 is filled).
Prints the first non-empty, unicast, non-zero value as 12 hex digits (the fajita-wifi-mac= boot argument), or nothing.
Default image: .cache/efs-backup/persist.img (the verified EFS backup; the real partition is never written).
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    img = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / '.cache/efs-backup/persist.img'
    if not img.is_file():
        return 1
    r = subprocess.run(['debugfs', '-R', 'cat /wlan_mac.bin', str(img)], capture_output=True)
    for m in re.finditer(rb'Intf(\d)MacAddress=([0-9A-Fa-f]{12})', r.stdout):
        mac = bytes.fromhex(m.group(2).decode())
        if mac != bytes(6) and not mac[0] & 1:
            print(m.group(2).decode().lower())
            return 0
    return 1


if __name__ == '__main__':
    sys.exit(main())
