#!/usr/bin/env python3
"""usbnet-host.py: the laptop end of the fajita USB network transport (fjdbg NET channel 7; the phone needs usb-net=1).

  usbnet-host.py [--sock HUB] bridge [--seconds S] [--pcap FILE] [--mtu 1500] [--passt ARGS...]
      bridge the phone's Ethernet frames to passt (user-mode networking, no root: DHCP, DNS forwarding, NAT through
      this laptop's sockets). passt is started here on its own socket (its qemu stream format: u32 big-endian length,
      then the frame, which is also what the hub's `net` op speaks), and the frames are relayed both ways until
      SIGTERM/SIGINT, S seconds, or either side closing. --pcap makes passt record every frame.
  usbnet-host.py [--sock HUB] echo [--count N] [--window W] [--timeout S]
      the transport test without passt: switch the phone's NET channel to echo (kcmd usbnet echo on), send N frames of
      14..1536 bytes (every byte value, the fjdbg escape bytes included), check each comes back intact and in order,
      then switch echo off. Prints `usbnet echo PASS ...` or `usbnet echo FAIL ...` (exit status 0 / 1).

The hub (scripts/fajita-usbdbg-hub.py serve, started by session.sh/cycle.sh) owns the tty; this tool only talks to the
hub's socket (FAJITA_USBDBG_HUB, default $XDG_RUNTIME_DIR/fjdbg-hub.sock). Python stdlib only."""
import argparse
import json
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

MAX_FRAME = 1536   # the fjdbg kMaxData
DEFAULT_SOCK = os.environ.get('FAJITA_USBDBG_HUB', os.path.join(os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'fjdbg-hub.sock'))


def hub_call(sock, op, args, timeout=20):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock)
    f = s.makefile('rwb')
    f.write((json.dumps({'op': op, 'args': args, 'timeout': timeout}) + '\n').encode())
    f.flush()
    rep = json.loads(f.readline())
    s.close()
    return rep['ok'], rep['out']


def hub_net(sock):
    """Attach to the hub as its NET client: returns (socket, buffered reader) carrying the qemu-format frame stream."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock)
    f = s.makefile('rb')
    s.sendall((json.dumps({'op': 'net'}) + '\n').encode())
    rep = json.loads(f.readline())
    if not rep['ok']:
        raise SystemExit('hub: ' + rep['out'].strip())
    return s, f


def read_frame(f):
    hdr = f.read(4)
    if len(hdr) < 4:
        return None
    n = struct.unpack('>I', hdr)[0]
    data = f.read(n)
    return data if len(data) == n else None


def send_frame(s, frame):
    s.sendall(struct.pack('>I', len(frame)) + frame)


def describe(frame):
    """A short label for the log: ARP / DHCP type / ICMP / TCP / UDP port."""
    if len(frame) < 14:
        return 'runt'
    et = struct.unpack_from('>H', frame, 12)[0]
    if et == 0x0806:
        return 'arp'
    if et == 0x86dd:
        return 'ipv6'
    if et != 0x0800 or len(frame) < 34:
        return 'eth %04x' % et
    proto, ihl = frame[23], (frame[14] & 15) * 4
    if proto == 1:
        return 'icmp'
    if proto == 6:
        return 'tcp'
    if proto == 17 and len(frame) >= 14 + ihl + 4:
        sp, dp = struct.unpack_from('>HH', frame, 14 + ihl)
        if {sp, dp} == {67, 68}:
            opts = frame[14 + ihl + 8 + 240:]
            i = 0
            while i + 2 < len(opts) and opts[i] != 255:
                if opts[i] == 0:
                    i += 1
                    continue
                if opts[i] == 53:
                    return 'dhcp ' + {1: 'discover', 2: 'offer', 3: 'request', 5: 'ack', 6: 'nak'}.get(opts[i + 2], str(opts[i + 2]))
                i += 2 + opts[i + 1]
            return 'dhcp'
        return 'udp %d->%d' % (sp, dp)
    return 'ip %d' % proto


def bridge(a):
    run_dir = os.environ.get('XDG_RUNTIME_DIR', '/tmp')
    psock = os.path.join(run_dir, 'fjdbg-passt-%d.sock' % os.getpid())
    cmd = ['passt', '-f', '-1', '-s', psock, '--mtu', str(a.mtu)] + (['-p', a.pcap] if a.pcap else []) + a.passt
    passt = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        limit = time.time() + 10
        while not os.path.exists(psock):
            if passt.poll() is not None or time.time() > limit:
                raise SystemExit('passt did not start: %s' % passt.stderr.read().decode(errors='replace').strip())
            time.sleep(0.05)
        ps = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        ps.connect(psock)
        pf = ps.makefile('rb')
        hs, hf = hub_net(a.sock)
        print('usbnet bridge: phone <-> hub %s <-> passt %s (mtu %d%s)' % (a.sock, psock, a.mtu, ', pcap ' + a.pcap if a.pcap else ''),
              flush=True)
        counts = {'phone->passt': 0, 'passt->phone': 0}
        done = threading.Event()

        def pump(src, dst, key):
            try:
                while True:
                    fr = read_frame(src)
                    if fr is None:
                        break
                    if len(fr) > MAX_FRAME and key == 'passt->phone':
                        print('usbnet bridge: drop %d-byte frame from passt (above %d)' % (len(fr), MAX_FRAME), flush=True)
                        continue
                    send_frame(dst, fr)
                    counts[key] += 1
                    if a.verbose:
                        print('  %s %4d %s' % (key, len(fr), describe(fr)), flush=True)
            except OSError:
                pass
            done.set()
        threading.Thread(target=pump, args=(hf, ps, 'phone->passt'), daemon=True).start()
        threading.Thread(target=pump, args=(pf, hs, 'passt->phone'), daemon=True).start()
        signal.signal(signal.SIGTERM, lambda *_: done.set())
        try:
            done.wait(a.seconds if a.seconds else None)
        except KeyboardInterrupt:
            pass
        print('usbnet bridge: end, phone->passt %d, passt->phone %d frames' % (counts['phone->passt'], counts['passt->phone']), flush=True)
        hs.close()
        ps.close()
    finally:
        passt.terminate()
        try:
            passt.wait(timeout=3)
        except subprocess.TimeoutExpired:
            passt.kill()
        try:
            os.unlink(psock)
        except OSError:
            pass
    return 0


def echo(a):
    ok, out = hub_call(a.sock, 'kcmd', ['usbnet', 'echo', 'on'])
    if not ok or 'echo on' not in out:
        print('usbnet echo FAIL: could not switch echo on: %s' % out.strip())
        return 1
    rng = random.Random(0x6a17)
    sizes = [14, 15, 60, 64, 511, 512, 513, 1500, 1514, 1535, MAX_FRAME]
    sizes += [rng.randint(14, MAX_FRAME) for _ in range(max(0, a.count - len(sizes)))]
    sizes = sizes[:a.count]
    frames = []
    for i, n in enumerate(sizes):
        body = bytes(rng.getrandbits(8) for _ in range(n - 2)) if i % 2 else bytes((i + k) & 255 for k in range(n - 2))
        frames.append(struct.pack('>H', i & 0xffff) + body)   # the index first, so an out-of-order echo is named
    s, f = hub_net(a.sock)
    s.settimeout(a.timeout)
    got, bad, t0 = 0, [], time.time()
    sent = 0
    try:
        while got < len(frames):
            while sent < len(frames) and sent - got < a.window:
                send_frame(s, frames[sent])
                sent += 1
            fr = read_frame(f)
            if fr is None:
                bad.append('hub closed the stream after %d frames' % got)
                break
            if fr != frames[got]:
                idx = struct.unpack_from('>H', fr)[0] if len(fr) >= 2 else -1
                bad.append('frame %d (%d bytes): got %d bytes, index %d' % (got, len(frames[got]), len(fr), idx))
                if len(bad) > 5:
                    break
            got += 1
    except socket.timeout:
        bad.append('no echo for frame %d within %.0f s' % (got, a.timeout))
    dt = time.time() - t0
    s.close()
    hub_call(a.sock, 'kcmd', ['usbnet', 'echo', 'off'])
    total = sum(len(x) for x in frames[:got])
    if bad or got != len(frames):
        print('usbnet echo FAIL: %d of %d frames back; %s' % (got, len(frames), '; '.join(bad)))
        return 1
    print('usbnet echo PASS: %d frames (%d..%d bytes, %d bytes) echoed intact and in order in %.2f s (%.0f KiB/s each way, window %d)'
          % (got, min(sizes), max(sizes), total, dt, total / 1024 / dt if dt else 0, a.window))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sock', default=DEFAULT_SOCK)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('bridge')
    p.add_argument('--seconds', type=float, default=0, help='stop after S seconds (default: until signalled)')
    p.add_argument('--pcap', help='passt records the traffic here')
    p.add_argument('--mtu', type=int, default=1500, help='MTU passt offers over DHCP (frames must fit 1536 bytes)')
    p.add_argument('-v', '--verbose', action='store_true', help='one line per frame')
    p.add_argument('--passt', nargs=argparse.REMAINDER, default=[], help='extra passt arguments')
    p = sub.add_parser('echo')
    p.add_argument('--count', type=int, default=200)
    p.add_argument('--window', type=int, default=8, help='frames in flight')
    p.add_argument('--timeout', type=float, default=10)
    a = ap.parse_args()
    return bridge(a) if a.cmd == 'bridge' else echo(a)


if __name__ == '__main__':
    sys.exit(main())
