#!/usr/bin/env python3
"""fajita USB debug hub: one process owns the fjdbg tty for a whole run and shares it.

  fajita-usbdbg-hub.py serve --log FILE [--sock PATH]
      waits for the gadget (1209:4d69) to appear, opens its tty, appends the kernel log stream to FILE, and serves
      requests on a Unix socket. Reopens the tty when the phone resets and comes back. Ends on SIGTERM.
  fajita-usbdbg-hub.py call [--sock PATH] OP [ARGS...]
      one request through the hub:  read32 ADDR [COUNT] | write32 ADDR VALUE... | kcmd TEXT... | run CMD... |
      readp ADDR LEN | readv ADDR LEN | hello | state | wait-up [SECONDS] | screenshot FILE.png [--scale N]
      read32 prints `ADDR: VALUE` lines; kcmd prints the kernel's text; run prints the command's output.

The wire protocol is scripts/fajita-usbdbg.py's (imported from beside this file), so the framing and op codes stay in
one place. Python stdlib only."""
import argparse
import importlib.util
import json
import os
import re
import signal
import socket
import struct
import sys
import threading
import zlib
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('fjdbg', HERE / 'fajita-usbdbg.py')
fj = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fj)

DEFAULT_SOCK = os.environ.get('FAJITA_USBDBG_HUB', os.path.join(os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'fjdbg-hub.sock'))


def find_tty():
    base = Path('/sys/bus/usb/devices')
    for d in base.glob('*/idVendor'):
        try:
            if d.read_text().strip() != '1209':
                continue
        except OSError:
            continue
        for t in d.parent.glob('*:1.*/tty/*'):
            return '/dev/' + t.name
    return None


class Hub:
    def __init__(self, log_path):
        self.log = open(log_path, 'ab', buffering=0)
        self.link = None
        self.lock = threading.Lock()
        self.shell = bytearray()
        self.shell_event = threading.Condition()
        self.up_since = None
        self.shots = fj.ShotCollector()

    def note(self, text):
        self.log.write(('\n[hub %s] %s\n' % (time.strftime('%H:%M:%S'), text)).encode())

    def on_log(self, off, data):
        self.log.write(data)

    def on_text(self, data):
        self.log.write(data)

    def on_shell(self, data):
        with self.shell_event:
            self.shell.extend(data)
            del self.shell[:-(32 << 20)]   # room for a pull of up to ~16 MiB (hex on the wire)
            self.shell_event.notify_all()

    def connect_loop(self, exec_cmd=None):
        if exec_cmd:   # tests: a program over stdin/stdout (tests/UsbDbgSim) instead of the gadget's tty
            self.link = fj.Link(fj.ExecTransport(exec_cmd.split()), on_text=self.on_text, on_log=self.on_log,
                                on_shell=self.on_shell, on_file=self.shots.on_file)
            self.up_since = time.time()
            return
        while True:
            if self.link is None:
                tty = find_tty()
                if tty and os.path.exists(tty):
                    try:
                        link = fj.Link(fj.TtyTransport(tty), on_text=self.on_text, on_log=self.on_log, on_shell=self.on_shell,
                                       on_file=self.shots.on_file)
                        self.link = link
                        self.up_since = time.time()
                        self.note('connected ' + tty)
                    except OSError as e:
                        self.note('open %s failed: %s' % (tty, e))
            else:
                if not find_tty():
                    self.note('gadget gone')
                    try:
                        self.link.close()
                    except Exception:
                        pass
                    self.link = None
                    self.up_since = None
            time.sleep(0.5)

    def request(self, op, args=b'', timeout=8.0):
        with self.lock:
            if not self.link:
                raise RuntimeError('no gadget')
            return self.link.request(op, args, timeout=timeout)

    def handle(self, req):
        op = req['op']
        a = req.get('args', [])
        num = lambda s: int(s, 0)
        if op == 'wait-up':
            limit = time.time() + float(a[0] if a else 300)
            while time.time() < limit:
                if self.link:
                    return {'ok': True, 'out': 'up %.0f s\n' % (time.time() - self.up_since)}
                time.sleep(0.5)
            return {'ok': False, 'out': 'timeout\n'}
        if op == 'hello':
            st, data = self.request('hello')
            return {'ok': st == 0, 'out': 'status %d features %#x %s\n' % (st, struct.unpack_from('<I', data, 4)[0] if len(data) >= 8 else 0, data[8:].decode(errors='replace'))}
        if op == 'state':
            st, data = self.request('state')
            return {'ok': st == 0, 'out': data.decode(errors='replace')}
        if op == 'reset':
            # the phone resets at once (device needs usb-debug=2), so no reply is expected
            try:
                st, _ = self.request('reset', struct.pack('<I', num(a[0]) if a else 0), timeout=2)
            except Exception:
                st = 0
            return {'ok': st == 0, 'out': 'reset sent (status %d)\n' % st}
        if op == 'read32':
            addr, count = num(a[0]), num(a[1]) if len(a) > 1 else 1
            out = []
            while count > 0:
                n = min(count, 300)
                st, data = self.request('read32', struct.pack('<QI', addr, n))
                if st:
                    return {'ok': False, 'out': ''.join(out) + 'read32 %#x: status %d\n' % (addr, st)}
                for i in range(n):
                    out.append('%08x: %08x\n' % (addr + 4 * i, struct.unpack_from('<I', data, 4 * i)[0]))
                addr += 4 * n
                count -= n
            return {'ok': True, 'out': ''.join(out)}
        if op == 'write32':
            addr = num(a[0])
            vals = [num(v) for v in a[1:]]
            st, _ = self.request('write32', struct.pack('<Q', addr) + b''.join(struct.pack('<I', v & 0xffffffff) for v in vals))
            return {'ok': st == 0, 'out': 'status %d\n' % st}
        if op in ('readp', 'readv'):
            addr, length = num(a[0]), num(a[1])
            st, data = self.request('read_phys' if op == 'readp' else 'read_virt', struct.pack('<QI', addr, length))
            return {'ok': st == 0, 'out': data.hex() + '\n' if st == 0 else 'status %d\n' % st}
        if op == 'kcmd':
            st, data = self.request('kcmd', ' '.join(a).encode(), timeout=float(req.get('timeout', 30)))
            return {'ok': st == 0, 'out': data.decode(errors='replace')}
        if op == 'screenshot':
            # screenshot FILE [--scale N]: the scanout as an RGB PNG (half resolution by default, 1 = full).
            if not a:
                return {'ok': False, 'out': 'screenshot: give a FILE\n'}
            path, scale = a[0], 2
            if '--scale' in a[1:]:
                scale = num(a[a.index('--scale') + 1])
            t0 = time.time()
            with self.lock:
                if not self.link:
                    raise RuntimeError('no gadget')
                w, h, bgr = self.shots.take(self.link, scale, timeout=float(req.get('timeout', 60)))
            fj.write_png(path, w, h, bgr)
            self.note('screenshot %s %dx%d %.1f s' % (path, w, h, time.time() - t0))
            return {'ok': True, 'out': '%s: %dx%d, %.1f s\n' % (path, w, h, time.time() - t0)}
        timeout = float(req.get('timeout') or 60)
        if op == 'run':
            body = self.shell_exchange([' '.join(a).encode()], timeout)
            if body is None:
                return {'ok': False, 'out': 'run: timeout\n'}
            return {'ok': True, 'out': body.decode(errors='replace')}
        if op == 'push':
            # push LOCAL REMOTE [MODE]: the agent writes REMOTE.fjtmp, checks size and CRC32, chmods, renames
            data = open(a[0], 'rb').read()
            marker = self.shell_send([b'@put %s %s %d' % (a[1].encode(), (a[2] if len(a) > 2 else '755').encode(), len(data))])
            limit = time.time() + timeout
            for i in range(0, len(data), 512):
                self.shell_send([b'@d ' + data[i:i + 512].hex().encode()], marker)
                if (i // 512) % 4 == 3 and not self.shell_wait(marker, rb'fjdbg: k %d\n' % min(len(data), i + 512), limit):
                    return {'ok': False, 'out': 'push: no ack at %d bytes\n' % (i + 512)}
            self.shell_send([b'@end %08x' % zlib.crc32(data)], marker)
            body = self.shell_collect(marker, limit)
            if body is None:
                return {'ok': False, 'out': 'push: timeout\n'}
            return {'ok': b'fjdbg: put ok' in body, 'out': body.decode(errors='replace')}
        if op == 'pull':
            # pull REMOTE LOCAL
            body = self.shell_exchange([b'@get ' + a[0].encode()], timeout)
            if body is None:
                return {'ok': False, 'out': 'pull: timeout\n'}
            m = re.search(rb'fjdbg: get (\d+)\n', body)
            c = re.search(rb'fjdbg: get crc ([0-9a-f]+)\n', body)
            if not m or not c:
                return {'ok': False, 'out': body.decode(errors='replace')}
            data = bytes.fromhex(b''.join(re.findall(rb'@d ([0-9a-f]*)\n', body)).decode())
            if len(data) != int(m.group(1)) or zlib.crc32(data) != int(c.group(1), 16):
                return {'ok': False, 'out': 'pull: size/crc mismatch (%d bytes)\n' % len(data)}
            open(a[1], 'wb').write(data)
            return {'ok': True, 'out': 'pulled %s -> %s, %d bytes, crc %08x\n' % (a[0], a[1], len(data), zlib.crc32(data))}
        return {'ok': False, 'out': 'unknown op %s\n' % op}

    def shell_send(self, lines, marker=None):
        """Send lines to the agent; a new exchange starts with a unique marker line (returned) that output is found after."""
        with self.lock:
            if not self.link:
                raise RuntimeError('no gadget')
            if marker is None:
                marker = ('fjdbg-hub-%d-%d' % (os.getpid(), int(time.time() * 1000))).encode()
                self.link.send_frame(fj.CH_SHELL, marker + b'\n')
            for line in lines:
                self.link.send_frame(fj.CH_SHELL, line + b'\n')
        return marker

    def shell_wait(self, marker, pattern, limit):
        """Wait until the agent's output after marker matches pattern; return the match or None at limit."""
        with self.shell_event:
            while time.time() < limit:
                text = bytes(self.shell)
                at = text.find(marker)
                if at >= 0:
                    m = re.search(pattern, text[at:])
                    if m:
                        return (text[at:], m)
                self.shell_event.wait(0.2)
        return None

    def shell_collect(self, marker, limit):
        """The agent's output after marker up to and including 'fjdbg: exit N' / 'fjdbg: eof', or None at limit."""
        r = self.shell_wait(marker, rb'fjdbg: (?:exit (-?[0-9]+)|eof)\n', limit)
        if r is None:
            return None
        body = r[0][:r[1].end()]
        return body[body.find(b'\n') + 1:]

    def shell_exchange(self, lines, timeout):
        return self.shell_collect(self.shell_send(lines), time.time() + timeout)


def serve(args):
    hub = Hub(args.log)
    threading.Thread(target=hub.connect_loop, args=(args.exec_cmd,), daemon=True).start()
    try:
        os.unlink(args.sock)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(args.sock)
    srv.listen(8)
    signal.signal(signal.SIGTERM, lambda *_: (srv.close(), os._exit(0)))
    hub.note('serving ' + args.sock)

    def client(c):
        try:
            f = c.makefile('rwb')
            req = json.loads(f.readline())
            try:
                rep = hub.handle(req)
            except Exception as e:  # noqa: BLE001 - report every failure to the caller
                rep = {'ok': False, 'out': 'error: %s\n' % e}
            f.write((json.dumps(rep) + '\n').encode())
            f.flush()
        finally:
            c.close()

    while True:
        c, _ = srv.accept()
        threading.Thread(target=client, args=(c,), daemon=True).start()


def call(args):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(args.sock)
    f = s.makefile('rwb')
    req = {'op': args.op, 'args': args.rest}
    if args.timeout:
        req['timeout'] = args.timeout
    f.write((json.dumps(req) + '\n').encode())
    f.flush()
    rep = json.loads(f.readline())
    sys.stdout.write(rep['out'])
    return 0 if rep['ok'] else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sock', default=DEFAULT_SOCK)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('serve'); p.add_argument('--log', required=True)
    p.add_argument('--exec', dest='exec_cmd', help='tests: talk to a program (the simulator) instead of the tty')
    p = sub.add_parser('call'); p.add_argument('--timeout', type=float); p.add_argument('op'); p.add_argument('rest', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    return serve(a) if a.cmd == 'serve' else call(a)


if __name__ == '__main__':
    sys.exit(main())
