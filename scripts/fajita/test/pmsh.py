"""Run one command in the postmarketOS initramfs telnet debug shell and print its output."""
import socket, sys, time
cmd = sys.argv[1]
s = socket.create_connection(('172.16.42.1', 23), timeout=5)
def drain(t):
    end, out = time.time() + t, b''
    s.settimeout(0.3)
    while time.time() < end:
        try:
            d = s.recv(65536)
            if not d: break
            # refuse telnet option negotiation
            while b'\xff' in d:
                i = d.index(b'\xff'); opt = d[i:i+3]
                if len(opt) == 3 and opt[1] in (251, 253):
                    s.send(bytes([255, 254 if opt[1] == 253 else 252, opt[2]]))
                d = d[:i] + d[i+3:]
            out += d
        except socket.timeout:
            pass
    return out
drain(1.0)
s.send(cmd.encode() + b'; echo __END__\n')
out = drain(float(sys.argv[2]) if len(sys.argv) > 2 else 3.0)
print(out.decode(errors='replace'))
