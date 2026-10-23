#!/usr/bin/env python3
"""Host end of the fajita USB debug channel (fjdbg, drivers/NativeDwc3/UsbDbgProto.h).

The phone's NativeDwc3 gadget exposes one CDC-ACM port (Linux: /dev/ttyACM*). One byte
stream carries console text, fjdbg frames (log, command/reply, KDP) and, optionally,
XNU serial-KDP frames. This tool demultiplexes it and drives the command protocol:

  fajita-usbdbg.py [--port DEV | --exec CMD] COMMAND ...
    console                      print console text and the kernel log until ^C
    hello | state                link identity / driver counters
    read  {virt|phys} ADDR LEN   hexdump memory (fault-safe on the device)
    write {virt|phys} ADDR HEX   write bytes            (device needs usb-debug=2)
    read32 PA [COUNT]            read device registers (MMIO)
    write32 PA VALUE...          write device registers  (device needs usb-debug=2)
    log [--from N]               replay the kernel log from stream offset N, then follow
    kcmd TEXT                    run a named kernel debug command (ps, threads, ...)
    panic MSG | reset [MODE] | wdog ACTION       (device needs usb-debug=2)
    kdp-proxy [--listen H:P]     bridge lldb's `kdp-remote` (UDP 41139) to the port
    selftest                     run the protocol tests against tests/UsbDbgSim

Python standard library only. Nothing here talks to fastboot or resets the phone on its
own; `reset` and `panic` are explicit commands.
"""
import argparse
import os
import queue
import select
import socket
import struct
import subprocess
import sys
import termios
import threading
import time
import tty
import zlib
from pathlib import Path

# ---- Constants mirrored from UsbDbgProto.h / UsbDbgDevice.h (checked by selftest) ----
FRAME_START, FRAME_END, FRAME_ESC, KDP_START = 0xFC, 0xFB, 0xFE, 0xFA
ESCAPED = {KDP_START, FRAME_END, FRAME_START, FRAME_ESC, 0x0A}
PROTO_VERSION, MAX_DATA = 1, 1536
CH_CTRL, CH_LOG, CH_CMD, CH_RSP, CH_KDP = range(5)
OPS = dict(hello=0x0001, state=0x0002, read_virt=0x0010, write_virt=0x0011, read_phys=0x0012, write_phys=0x0013,
           read32=0x0014, write32=0x0015, log_from=0x0020, kcmd=0x0030, panic=0x0040, reset=0x0041, wdog=0x0042)
REPLY_BIT = 0x8000
STATUS = {0: 'ok', -1: 'unknown op', -2: 'bad arguments', -3: 'fault', -4: 'denied (boot with usb-debug=2 for writes)',
          -5: 'too big', -6: 'unsupported'}
FEATURES = ['log', 'memory', 'mmio', 'kcmd', 'writes', 'kdp']
KDP_PORT = 41139


def escape(data):
    out = bytearray()
    for b in data:
        if b in ESCAPED:
            out += bytes((FRAME_ESC, b ^ 0xFF))
        else:
            out.append(b)
    return out


def encode_frame(channel, seq, data):
    if len(data) > MAX_DATA:
        raise ValueError('frame data too long')
    payload = bytes((channel, 0)) + struct.pack('<H', seq & 0xFFFF) + bytes(data)
    return bytes((FRAME_START,)) + escape(payload + struct.pack('<I', zlib.crc32(payload))) + bytes((FRAME_END,))


def escape_skdp(data):
    out = bytearray()
    for b in data:
        out += bytes((FRAME_ESC, b ^ 0xFF)) if b in (KDP_START, FRAME_END, FRAME_ESC, 0x0A) else bytes((b,))
    return out


def encode_skdp(packet):
    """osfmk/kdp/kdp_serial.c kdp_serialize_packet: START crc32 START payload END (0xFC is not escaped)."""
    return (bytes((KDP_START,)) + escape_skdp(struct.pack('<I', zlib.crc32(packet))) + bytes((KDP_START,))
            + escape_skdp(packet) + bytes((FRAME_END,)))


class Demux:
    """Splits the stream into ('text', bytes), ('frame', channel, seq, data), ('skdp', packet), ('bad', why)."""

    def __init__(self):
        self.kind = None      # None (text), 'fj' or 'skdp'
        self.esc = False
        self.buf = bytearray()
        self.crc = None
        self.bad = 0

    def feed(self, chunk):
        events, text = [], bytearray()
        for b in chunk:
            if self.kind is None:
                if b in (FRAME_START, KDP_START):
                    if text:
                        events.append(('text', bytes(text)))
                        text.clear()
                    self.kind = 'fj' if b == FRAME_START else 'skdp'
                    self.buf.clear()
                    self.esc, self.crc = False, None
                else:
                    text.append(b)
                continue
            if self.esc:
                self.esc = False
                if b in ((FRAME_START, FRAME_END, KDP_START) if self.kind == 'fj' else (FRAME_END, KDP_START)):
                    events.append(self._drop('escape'))
                    if b != FRAME_END:
                        self.kind = 'fj' if b == FRAME_START else 'skdp'
                    continue
                self.buf.append(b ^ 0xFF)
            elif b == FRAME_ESC:
                self.esc = True
            elif b == KDP_START and self.kind == 'skdp':
                if len(self.buf) >= 4:          # the CRC sits between two starts
                    self.crc = struct.unpack_from('<I', self.buf)[0]
                self.buf.clear()
            elif b == FRAME_START and self.kind == 'fj':   # kdp_serial.c does not escape 0xFC: data inside SKDP
                events.append(self._drop('cut'))
                self.kind = 'fj'
            elif b == FRAME_END:
                events.append(self._finish())
            else:
                self.buf.append(b)
            if len(self.buf) > MAX_DATA + 16:
                events.append(self._drop('overrun'))
        if text:
            events.append(('text', bytes(text)))
        return events

    def _drop(self, why):
        self.bad += 1
        self.kind = None
        self.buf.clear()
        return ('bad', why)

    def _finish(self):
        kind, buf = self.kind, bytes(self.buf)
        self.kind = None
        if kind == 'skdp':
            if self.crc is not None and zlib.crc32(buf) != self.crc:
                self.bad += 1
                return ('bad', 'skdp crc')
            return ('skdp', buf)
        if len(buf) < 8 or zlib.crc32(buf[:-4]) != struct.unpack_from('<I', buf, len(buf) - 4)[0] or buf[1]:
            self.bad += 1
            return ('bad', 'crc')
        return ('frame', buf[0], struct.unpack_from('<H', buf, 2)[0], buf[4:-4])


# ---- Transports -----------------------------------------------------------------
class TtyTransport:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        self.saved = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)           # no echo, no CR/LF mapping; DTR goes up on open (gadget starts the log)

    def read(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 65536) if r else b''

    def write(self, data):
        os.write(self.fd, data)

    def close(self):
        termios.tcsetattr(self.fd, termios.TCSANOW, self.saved)
        os.close(self.fd)


class ExecTransport:
    def __init__(self, argv):
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)

    def read(self, timeout):
        fd = self.proc.stdout.fileno()
        r, _, _ = select.select([fd], [], [], timeout)
        return os.read(fd, 65536) if r else b''

    def write(self, data):
        self.proc.stdin.write(data)

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=5)


# ---- Link: reader thread + request/reply ------------------------------------------
class Link:
    def __init__(self, transport, on_text=None, on_log=None, on_kdp=None):
        self.t = transport
        self.demux = Demux()
        self.replies = queue.Queue()
        self.on_text = on_text or (lambda b: None)
        self.on_log = on_log or (lambda off, b: None)
        self.on_kdp = on_kdp or (lambda kind, pkt: None)
        self.seq = 0
        self.lock = threading.Lock()
        self.stop = False
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while not self.stop:
            chunk = self.t.read(0.2)
            for ev in self.demux.feed(chunk):
                if ev[0] == 'text':
                    self.on_text(ev[1])
                elif ev[0] == 'skdp':
                    self.on_kdp('skdp', ev[1])
                elif ev[0] == 'frame':
                    _, ch, seq, data = ev
                    if ch == CH_RSP:
                        self.replies.put((seq, data))
                    elif ch == CH_LOG and len(data) >= 8:
                        self.on_log(struct.unpack_from('<Q', data)[0], data[8:])
                    elif ch == CH_KDP:
                        self.on_kdp('frame', data)

    def send_frame(self, channel, data):
        with self.lock:
            self.seq = (self.seq + 1) & 0xFFFF
            seq = self.seq
            self.t.write(encode_frame(channel, seq, data))
        return seq

    def request(self, op, args=b'', timeout=5.0):
        seq = self.send_frame(CH_CMD, struct.pack('<HH', OPS[op], 0) + args)
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f'no reply to {op} (seq {seq})')
            try:
                rseq, data = self.replies.get(timeout=left)
            except queue.Empty:
                continue
            if rseq != seq or len(data) < 8:
                continue                      # a late reply to an earlier request
            rop, _, status = struct.unpack_from('<HHi', data)
            if rop != OPS[op] | REPLY_BIT:
                raise RuntimeError(f'reply op {rop:#x} does not match {op}')
            return status, data[8:]

    def close(self):
        self.stop = True
        self.thread.join(timeout=1)
        self.t.close()


def check(status, what):
    if status:
        raise SystemExit(f'{what}: {STATUS.get(status, status)}')


def hexdump(addr, data):
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        print(f'{addr + i:016x}  {row.hex(" "):<47}  {"".join(chr(c) if 32 <= c < 127 else "." for c in row)}')


# ---- KDP bridge: lldb `kdp-remote` speaks UDP; the kernel's KDP link wants Ethernet frames ----
def ip_checksum(header):
    s = sum(struct.unpack(f'!{len(header) // 2}H', header))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def kdp_wrap(payload, src_ip, dst_ip, src_port, dst_port=KDP_PORT, src_mac=b'\x02\0\0\0\0\x01', dst_mac=b'\xff' * 6):
    udp = struct.pack('!HHHH', src_port, dst_port, 8 + len(payload), 0) + payload
    ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, socket.inet_aton(src_ip), socket.inet_aton(dst_ip))
    ip = ip[:10] + struct.pack('!H', ip_checksum(ip)) + ip[12:]
    return dst_mac + src_mac + b'\x08\x00' + ip + udp


def kdp_unwrap(frame):
    """Ethernet/IPv4/UDP -> (src_ip, src_port, dst_port, payload), or None."""
    if len(frame) < 42 or frame[12:14] != b'\x08\x00' or frame[14] >> 4 != 4 or frame[23] != 17:
        return None
    ihl = (frame[14] & 15) * 4
    total = struct.unpack_from('!H', frame, 16)[0]
    udp = 14 + ihl
    sport, dport, ulen = struct.unpack_from('!HHH', frame, udp)
    payload = frame[udp + 8:min(udp + ulen, 14 + total)]
    return socket.inet_ntoa(frame[26:30]), sport, dport, payload


def kdp_proxy(link_factory, listen, target_ip, mode):
    host, port = listen.rsplit(':', 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, int(port)))
    peer = {}

    def on_kdp(kind, frame):
        got = kdp_unwrap(frame)
        if got and 'addr' in peer:
            sock.sendto(got[3], peer['addr'])

    link = link_factory(on_kdp=on_kdp, on_text=lambda b: sys.stdout.buffer.write(b) or sys.stdout.flush())
    print(f'kdp-proxy: lldb> kdp-remote {host}:{port}   (target {target_ip}, {mode})', file=sys.stderr)
    try:
        while True:
            data, addr = sock.recvfrom(65536)
            peer['addr'] = addr
            frame = kdp_wrap(data, '10.0.0.1', target_ip, addr[1])
            if mode == 'serial':
                link.t.write(encode_skdp(frame))
            else:
                link.send_frame(CH_KDP, frame)
    except KeyboardInterrupt:
        pass
    finally:
        link.close()


# ---- Self test against the host simulator --------------------------------------------
def selftest(sim):
    # Framing: every byte value, CRC, and the SKDP framing the kernel's kdp_serial.c expects.
    data = bytes(range(256))
    d = Demux()
    kpkt = b'\x0a\xfa\xfb\xfc' * 50          # 0xFC travels unescaped inside an SKDP frame
    evs = d.feed(b'pre\n' + encode_frame(CH_LOG, 0xFA0A, data) + b'post' + encode_skdp(kpkt))
    assert [e[0] for e in evs] == ['text', 'frame', 'text', 'skdp'], evs
    assert evs[1][1:] == (CH_LOG, 0xFA0A, data) and evs[3][1] == kpkt
    bad = bytearray(encode_frame(CH_CMD, 1, b'abc'))
    bad[3] ^= 0x40
    assert [e[0] for e in Demux().feed(bytes(bad) + encode_frame(CH_CMD, 2, b'x'))] == ['bad', 'frame']
    frame = kdp_wrap(b'kdp-request', '10.0.0.1', '10.0.0.2', 50000)
    assert kdp_unwrap(frame) == ('10.0.0.1', 50000, KDP_PORT, b'kdp-request')
    assert ip_checksum(frame[14:34]) == 0
    print('selftest: framing, SKDP and KDP wrap OK')

    texts, logs = [], []
    link = Link(ExecTransport([sim]), on_text=texts.append, on_log=lambda off, b: logs.append((off, b)))
    st, out = link.request('hello')
    check(st, 'hello')
    assert out[0] == PROTO_VERSION and struct.unpack_from('<H', out, 2)[0] == MAX_DATA and out[8:] == b'UsbDbgSim'
    st, out = link.request('read_virt', struct.pack('<QI', 0xfffffff007004010, 32))
    assert st == 0 and out == bytes(range(16, 48))
    st, out = link.request('read_phys', struct.pack('<QI', 0x80000000, 4))
    assert st == 0 and out == bytes(range(4))
    st, out = link.request('read_virt', struct.pack('<QI', 0x1000, 4))
    assert st == -3
    st, out = link.request('read32', struct.pack('<QI', 0x0a60c120, 2))
    assert st == 0 and struct.unpack('<II', out) == (0x5533330a, 0)
    st, out = link.request('write_virt', struct.pack('<Q', 0xfffffff007004000) + b'\xee')
    assert st == -4
    st, out = link.request('state')
    assert st == 0 and b'link=sim' in out
    st, out = link.request('kcmd', b'ps')
    assert st == 0 and out == b"kcmd 'ps': ok\n"
    st, out = link.request('log_from', struct.pack('<Q', 23))
    assert st == 0 and struct.unpack('<Q', out)[0] == 23
    time.sleep(0.3)
    link.close()
    assert b''.join(texts) == b'UsbDbgSim console up\n', texts
    assert logs[0][0] == 0 and logs[0][1].startswith(b'sim: kernel log line 1') and logs[-1] == (23, b'sim: kernel log line 2\n')

    link = Link(ExecTransport([sim, '--writes']))
    st, _ = link.request('write_virt', struct.pack('<Q', 0xfffffff007004000) + b'\xee\xff')
    assert st == 0
    st, out = link.request('read_virt', struct.pack('<QI', 0xfffffff007004000, 3))
    assert out == b'\xee\xff\x02'
    st, _ = link.request('reset', struct.pack('<I', 0))
    st, out = link.request('state')
    assert b'resets=1' in out
    link.close()
    print('selftest: request/reply against UsbDbgSim OK')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default=os.environ.get('FAJITA_USBDBG_PORT', '/dev/ttyACM0'))
    ap.add_argument('--exec', dest='exec_cmd', help='talk to a program over stdin/stdout instead (the simulator)')
    ap.add_argument('--log-file', help='also append the kernel log stream here')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('console')
    sub.add_parser('hello')
    sub.add_parser('state')
    p = sub.add_parser('read'); p.add_argument('space', choices=['virt', 'phys']); p.add_argument('addr'); p.add_argument('length')
    p = sub.add_parser('write'); p.add_argument('space', choices=['virt', 'phys']); p.add_argument('addr'); p.add_argument('hex')
    p = sub.add_parser('read32'); p.add_argument('addr'); p.add_argument('count', nargs='?', default='1')
    p = sub.add_parser('write32'); p.add_argument('addr'); p.add_argument('values', nargs='+')
    p = sub.add_parser('log'); p.add_argument('--from', dest='offset', default='0')
    p = sub.add_parser('kcmd'); p.add_argument('text', nargs='+')
    p = sub.add_parser('panic'); p.add_argument('message', nargs='+')
    p = sub.add_parser('reset'); p.add_argument('mode', nargs='?', default='0')
    p = sub.add_parser('wdog'); p.add_argument('action', help='0 = stop petting (the watchdog bites)')
    p = sub.add_parser('kdp-proxy'); p.add_argument('--listen', default=f'127.0.0.1:{KDP_PORT}')
    p.add_argument('--target-ip', default='10.0.0.2'); p.add_argument('--mode', choices=['frame', 'serial'], default='frame',
                                                                       help='frame: fjdbg KDP channel; serial: kdp_match_name=serial')
    p = sub.add_parser('selftest'); p.add_argument('--sim', required=True)
    a = ap.parse_args()
    num = lambda s: int(s, 0)

    if a.cmd == 'selftest':
        return selftest(a.sim)

    log_file = open(a.log_file, 'ab') if a.log_file else None

    def on_log(off, data):
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
        if log_file:
            log_file.write(data)
            log_file.flush()

    def on_text(data):
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

    def make(**kw):
        transport = ExecTransport(a.exec_cmd.split()) if a.exec_cmd else TtyTransport(a.port)
        return Link(transport, **kw)

    if a.cmd == 'kdp-proxy':
        return kdp_proxy(make, a.listen, a.target_ip, a.mode)
    streaming = a.cmd in ('console', 'log')
    link = make(on_text=on_text if streaming else None, on_log=on_log if streaming else None)
    try:
        if a.cmd == 'console':
            while True:
                time.sleep(1)
        elif a.cmd == 'log':
            st, out = link.request('log_from', struct.pack('<Q', num(a.offset)))
            check(st, 'log')
            while True:
                time.sleep(1)
        elif a.cmd == 'hello':
            st, out = link.request('hello')
            check(st, 'hello')
            feats = struct.unpack_from('<I', out, 4)[0]
            print(f'fjdbg v{out[0]} max {struct.unpack_from("<H", out, 2)[0]} B, {out[8:].decode(errors="replace")}, '
                  f'features: {" ".join(f for i, f in enumerate(FEATURES) if feats >> i & 1)}')
        elif a.cmd == 'state':
            st, out = link.request('state')
            check(st, 'state')
            sys.stdout.write(out.decode(errors='replace'))
        elif a.cmd == 'read':
            st, out = link.request('read_' + a.space, struct.pack('<QI', num(a.addr), num(a.length)))
            check(st, 'read')
            hexdump(num(a.addr), out)
        elif a.cmd == 'write':
            st, _ = link.request('write_' + a.space, struct.pack('<Q', num(a.addr)) + bytes.fromhex(a.hex))
            check(st, 'write')
        elif a.cmd == 'read32':
            st, out = link.request('read32', struct.pack('<QI', num(a.addr), num(a.count)))
            check(st, 'read32')
            for i, (v,) in enumerate(struct.iter_unpack('<I', out)):
                print(f'{num(a.addr) + 4 * i:#010x}: {v:#010x}')
        elif a.cmd == 'write32':
            st, _ = link.request('write32', struct.pack('<Q', num(a.addr)) + b''.join(struct.pack('<I', num(v)) for v in a.values))
            check(st, 'write32')
        elif a.cmd == 'kcmd':
            st, out = link.request('kcmd', ' '.join(a.text).encode(), timeout=30)
            check(st, 'kcmd')
            sys.stdout.write(out.decode(errors='replace'))
        elif a.cmd == 'panic':
            link.send_frame(CH_CMD, struct.pack('<HH', OPS['panic'], 0) + ' '.join(a.message).encode())
            time.sleep(1)
        elif a.cmd == 'reset':
            st, _ = link.request('reset', struct.pack('<I', num(a.mode)), timeout=2)
            check(st, 'reset')
        elif a.cmd == 'wdog':
            st, _ = link.request('wdog', struct.pack('<I', num(a.action)))
            check(st, 'wdog')
    except KeyboardInterrupt:
        pass
    finally:
        link.close()


if __name__ == '__main__':
    sys.exit(main())
