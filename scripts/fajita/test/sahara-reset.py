#!/usr/bin/env python3
"""Leave Qualcomm CrashDump Mode from the host: Sahara RESET over USB (05c6:900e), no libusb.

The phone sends HELLO (mode 2, memory debug); we answer with HELLO_RESP (mode 2), wait for its next packet (MEMORY_DEBUG), send RESET
(07000000 08000000) and read RESET_RESP (08000000 08000000); the phone then resets. Packet bytes per fajita-soc-reset-freeze-research.md
(Linux qdl sahara.c). Exit 0 = reset acknowledged, 2 = no crashdump device, 3 = no permission (needs a udev uaccess rule for 05c6)."""
import fcntl
import glob
import os
import struct
import sys
import time

USBDEVFS_CLAIMINTERFACE = 0x8004550F
USBDEVFS_RELEASEINTERFACE = 0x80045510
USBDEVFS_BULK = 0xC0185502


def find():
    for dev in glob.glob('/sys/bus/usb/devices/*'):
        try:
            vid = open(dev + '/idVendor').read().strip()
            pid = open(dev + '/idProduct').read().strip()
        except OSError:
            continue
        if vid == '05c6' and pid in ('900e', '9008'):
            bus, num = int(open(dev + '/busnum').read()), int(open(dev + '/devnum').read())
            eps = []
            for ep in glob.glob(dev + '/' + os.path.basename(dev) + ':1.0/ep_*'):
                eps.append((int(open(ep + '/bEndpointAddress').read(), 16), open(ep + '/type').read().strip()))
            return f'/dev/bus/usb/{bus:03d}/{num:03d}', pid, eps
    return None


def bulk(fd, ep, data=None, length=0, timeout=2000):
    import ctypes
    buf = ctypes.create_string_buffer(data if data is not None else length, len(data) if data is not None else length)
    xfer = struct.pack('IIIxxxxQ', ep, len(buf), timeout, ctypes.addressof(buf))
    n = fcntl.ioctl(fd, USBDEVFS_BULK, bytearray(xfer), True) if False else None
    arg = bytearray(xfer)
    n = fcntl.ioctl(fd, USBDEVFS_BULK, arg)
    return buf.raw[:n] if data is None else n


def main():
    found = find()
    if not found:
        print('no Qualcomm 05c6:900e/9008 device'); return 2
    path, pid, eps = found
    ins = [a for a, t in eps if t == 'Bulk' and a & 0x80]
    outs = [a for a, t in eps if t == 'Bulk' and not a & 0x80]
    print(f'{path} 05c6:{pid} bulk in {[hex(a) for a in ins]} out {[hex(a) for a in outs]}')
    if not ins or not outs:
        print('no bulk endpoints'); return 4
    try:
        fd = os.open(path, os.O_RDWR)
    except PermissionError:
        print('permission denied: add SUBSYSTEM=="usb", ATTRS{idVendor}=="05c6", TAG+="uaccess" to a udev rule'); return 3
    fcntl.ioctl(fd, USBDEVFS_CLAIMINTERFACE, struct.pack('I', 0))
    try:
        hello = bulk(fd, ins[0], length=1024)
        cmd, size = struct.unpack_from('<II', hello)
        mode = struct.unpack_from('<I', hello, 20)[0] if len(hello) >= 24 else -1
        print(f'HELLO cmd={cmd} len={size} mode={mode}')
        resp = bytes.fromhex('02000000' '30000000' '02000000' '01000000' '00000000' '02000000') + bytes(24)
        bulk(fd, outs[0], data=resp)
        try:
            nxt = bulk(fd, ins[0], length=1024, timeout=2000)
            print(f'next cmd={struct.unpack_from("<I", nxt)[0]} len={len(nxt)}')
        except OSError as error:
            print('no packet after HELLO_RESP:', error)
        bulk(fd, outs[0], data=bytes.fromhex('07000000' '08000000'))
        try:
            ack = bulk(fd, ins[0], length=64, timeout=3000)
            print('RESET_RESP', ack.hex())
        except OSError as error:
            print('no RESET_RESP (the phone may already be resetting):', error)
        return 0
    finally:
        try:
            fcntl.ioctl(fd, USBDEVFS_RELEASEINTERFACE, struct.pack('I', 0))
        except OSError:
            pass
        os.close(fd)


if __name__ == '__main__':
    sys.exit(main())
