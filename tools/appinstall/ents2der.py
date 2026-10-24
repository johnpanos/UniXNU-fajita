#!/usr/bin/env python3
"""ents2der.py IN.plist OUT.der: encode an entitlements plist as the DER blob iOS 15+ reads (CS_OPS_DER_ENTITLEMENTS_BLOB):
[APPLICATION 16] { INTEGER 1, [CONTEXT 16] { SEQUENCE { UTF8String key, value }... } }, keys sorted."""
import plistlib, sys

def tlv(tag, body):
    n = len(body)
    if n < 0x80:
        ln = bytes([n])
    else:
        nb = n.to_bytes((n.bit_length() + 7) // 8, 'big')
        ln = bytes([0x80 | len(nb)]) + nb
    return bytes([tag]) + ln + body

def enc(v):
    if isinstance(v, bool):
        return tlv(0x01, b'\xff' if v else b'\x00')
    if isinstance(v, int):
        return tlv(0x02, v.to_bytes(max(1, (v.bit_length() + 8) // 8), 'big', signed=True))
    if isinstance(v, str):
        return tlv(0x0c, v.encode())
    if isinstance(v, list):
        return tlv(0x30, b''.join(enc(x) for x in v))
    if isinstance(v, dict):
        return tlv(0xb0, b''.join(tlv(0x30, enc(k) + enc(v[k])) for k in sorted(v, key=lambda s: s.encode())))
    raise SystemExit(f'unsupported entitlement value {v!r}')

d = plistlib.load(open(sys.argv[1], 'rb'))
open(sys.argv[2], 'wb').write(tlv(0x70, tlv(0x02, b'\x01') + enc(d)))
