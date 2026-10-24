#!/usr/bin/env python3
"""bright-probe.py STAGE LEVEL: DSI brightness prototype over the hub. STAGE a = read-only, b = one DCS 0x51 write, c = 0x52 readback."""
import os, subprocess, sys, re, time
HUB = ['python3','-I',os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'fajita-usbdbg-hub.py'),'call']
DSI = 0xae94000
BUF = 0x9D400000 + 0x1800000
def call(*a):
    r = subprocess.run(HUB+[str(x) for x in a], capture_output=True, text=True, timeout=60)
    return r.stdout + r.stderr
def rd(addr, n=1):
    out = call('read32', hex(addr), n)
    return [int(m.group(1),16) for m in re.finditer(r':\s*(?:0x)?([0-9a-fA-F]{8})', out)] or out
def wr(addr, *v): print('W', hex(addr), [hex(x) for x in v]); print(call('write32', hex(addr), *[hex(x) for x in v]).strip())
def dump():
    regs = {'CTRL':0,'STATUS0':4,'FIFO':8,'CMD_DMA_CTRL':0x38,'CMD_CFG0':0x3c,'CMD_CFG1':0x40,'DMA_BASE':0x44,'DMA_LEN':0x48,'ACK_ERR':0x64,'TRIG_CTRL':0x80,'LANE_STATUS':0xa4,'LANE_CTRL':0xa8,'ERR_INT':0x108,'INTR':0x10c,'CLK_CTRL':0x118,'EOT':0xc8,'VERSION':0x1f0}
    for k,o in regs.items(): print('%-12s %s' % (k, [hex(x) for x in rd(DSI+o)] if isinstance(rd(DSI+o), list) else rd(DSI+o)))
st = sys.argv[1]
if st == 'a':
    dump()
    print('SMMU SMR0..3', [hex(x) for x in rd(0x15000800, 4)])
    print('SMMU S2CR0..3', [hex(x) for x in rd(0x15000c00, 4)])
    print('CB0 SCTLR', [hex(x) for x in rd(0x15040000)])
    print('BUF region', [hex(x) for x in rd(BUF, 2)])
    print(call('state'))
elif st in 'bc':
    lvl = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    ctrl = rd(DSI)[0]; dma = rd(DSI+0x38)[0]; intr = rd(DSI+0x10c)[0]
    print('before CTRL %x DMA_CTRL %x INTR %x' % (ctrl, dma, intr))
    if st == 'b':
        pkt = bytes([3,0,0x39,0xc0, 0x51, lvl>>8, lvl&0xff, 0xff])
    else:
        # max return packet size 1 (short 0x37, wc 1) is a separate packet; do it first
        pkt = None
    def send(pkt, rx=False):
        print(call('write', 'phys', hex(BUF), pkt.hex()).strip())
        wr(DSI, ctrl | 5)                   # ENABLE | CMD_MODE_EN
        wr(DSI+0x38, dma & ~(1<<26))        # high-speed, as Linux does for brightness
        wr(DSI+0x10c, 0x1|0x2)              # clear + unmask CMD_DMA_DONE
        wr(DSI+0x44, BUF); wr(DSI+0x48, len(pkt)); wr(DSI+0x8c, 1)
        for i in range(50):
            v = rd(DSI+0x10c)[0]
            if v & 1: break
            time.sleep(0.01)
        print('INTR after %x (done=%d) ACK_ERR %x STATUS0 %x' % (v, v&1, rd(DSI+0x64)[0], rd(DSI+4)[0]))
        wr(DSI+0x10c, 0x1)                  # clear done, mask off
        wr(DSI+0x10c, 0)
        return v & 1
    if st == 'b':
        send(pkt)
    else:
        # DCS 0x52 read: MAX_RETURN 2, then DCS short read
        send(bytes([2,0,0x37,0x80]))   # set max return packet size = 2
        send(bytes([0x52,0x00,0x06,0xa0]))  # DCS read no param: short, ack rx bit
        print('RDBK', [hex(x) for x in rd(DSI+0x68, 4)])
    wr(DSI, ctrl); wr(DSI+0x38, dma)
    print('restored CTRL %x DMA_CTRL %x' % (rd(DSI)[0], rd(DSI+0x38)[0]))
