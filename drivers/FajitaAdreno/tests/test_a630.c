/* SPDX-License-Identifier: MIT */
/* Host tests of the A630 power-up against a register model of GCC, GPUCC, the GMU, its RSC and PDC, and a simulated GMU
 * firmware that answers HFI v1 commands the way the a630 firmware does (an ACK whose return header echoes the command).
 * cc -std=c11 -Wall -Wextra -Werror -I.. test_a630.c ../A630Gmu.c ../A630Gpu.c && ./a.out [cmd-db dump] */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "A630Gmu.h"
#include "A630Gpu.h"
#include "A630Regs.h"

enum { kBlockBytes = 0x100000 };
static uint32_t *mem[A630BlockCount];
static unsigned writes[A630BlockCount];
static uint32_t hfi[0x4000 / 4];
static uint8_t firmware[0x8000];
static unsigned hfi_commands, hfi_ids[32];
static int drop_reply_id = -1;

#define R(b, off) mem[b][(off) / 4]
#define GMU(reg) R(A630BlockGmu, (reg) * 4)

static void gmuFirmware(void)
{
    /* One pass over the command queue: every command gets an ACK in the response queue. */
    uint32_t *table = hfi, *cmdh = table + 6, *rsph = table + 6 + 12;
    uint32_t *cmdq = hfi + 0x1000 / 4, *rspq = hfi + 0x2000 / 4;
    assert(cmdh[3] == 1024 && rsph[3] == 1024 && (rsph[2] & 0xff) == 4);
    while (cmdh[10] != cmdh[11]) {
        uint32_t r = cmdh[10];
        uint32_t hdr = cmdq[r], size = (hdr >> 8) & 0xff, msg[256];
        for (uint32_t i = 0; i < size; ++i) msg[i] = cmdq[(r + i) % 1024];
        cmdh[10] = (r + size) % 1024;
        hfi_ids[hfi_commands++ % 32] = hdr & 0xff;
        if ((int)(hdr & 0xff) == drop_reply_id) continue;
        uint32_t ack[19] = {0};
        ack[0] = (2u << 16) | (19u << 8) | 126;   /* HFI_MSG_ACK_V1, HFI_F2H_MSG_ACK */
        ack[1] = hdr;
        ack[2] = 0;
        if ((hdr & 0xff) == 1) ack[3] = 0x10f0000;   /* FW_VERSION reply */
        if ((hdr & 0xff) == 4) {assert(msg[1] == 8 && msg[2] == 3);}   /* perf table v1: 8 GPU levels, 3 GMU levels */
        if ((hdr & 0xff) == 3) {assert(size == 160 && msg[1] == 1 && msg[24] == 0x50000);}
        uint32_t w = rsph[11];
        for (unsigned i = 0; i < 19; ++i) rspq[(w + i) % 1024] = ack[i];
        rsph[11] = (w + 19) % 1024;
        GMU(A630_GMU_GMU2HOST_INTR_INFO) |= 1;
    }
}

static uint32_t rd(void *c, unsigned b, uint32_t off)
{
    (void)c;
    assert(b < A630BlockCount && off < kBlockBytes && !(off & 3));
    return R(b, off);
}
static void wr(void *c, unsigned b, uint32_t off, uint32_t v)
{
    (void)c;
    assert(b < A630BlockCount && off < kBlockBytes && !(off & 3));
    writes[b]++;
    if (b == A630BlockGmu && off / 4 == A630_GMU_GMU2HOST_INTR_CLR) {GMU(A630_GMU_GMU2HOST_INTR_INFO) &= ~v; return;}
    if (b == A630BlockGmu && off / 4 == A630_GMU_HOST2GMU_INTR_SET) {
        if (v & 1) gmuFirmware();
        static const unsigned set[4] = {16, 17, 22, 23}, ack[4] = {24, 25, 30, 31};
        for (unsigned i = 0; i < 4; ++i) if (v == (1u << set[i])) GMU(A630_GMU_GMU2HOST_INTR_INFO) |= 1u << ack[i];
        return;
    }
    R(b, off) = v;
    if (b == A630BlockGcc || b == A630BlockGpuCc) {
        if (off == 0x106c) {   /* GDSC; an enabled branch of the domain starts running once it is powered */
            R(b, off) = (v & 1) ? (v & ~0x80000000u) : (v | 0x80000000u);
            if (!(v & 1) && (R(b, 0x109c) & 1)) R(b, 0x109c) &= ~0x80000000u;
        }
        else if (off == 0x1120) R(b, off) = (v & 2) ? (v & ~0x80000001u) : v;               /* RCG: ROOT_EN, UPDATE done */
        else if (off != 0x1124 && off != 0x52004 && off != 0x71028)                          /* branches */
            R(b, off) = (v & 1) ? (v & ~0x80000000u) : (v | 0x80000000u);
    }
    if (b == A630BlockGmu) {
        uint32_t reg = off / 4;
        if (reg == A630_GMU_RSCC_CONTROL_REQ) {
            GMU(A630_GMU_RSCC_CONTROL_ACK) = v & 2;
            R(A630BlockRscc, A630_RSCC_GPU_RSCC_RSC_STATUS0_DRV0 * 4) |= (v & 1) ? (1u << 16) : 0;
        }
        if (reg == A630_GMU_CM3_SYSRESET && v == 0) {
            for (unsigned i = 0; i < sizeof firmware / 4; ++i) {
                uint32_t want;
                memcpy(&want, firmware + 4 * i, 4);
                assert(GMU(A630_GMU_CM3_ITCM_START + i) == want);
            }
            GMU(A630_GMU_CM3_FW_INIT_RESULT) = 0xbabeface;
        }
        if (reg == A630_GMU_HFI_CTRL_INIT && v == 1) GMU(A630_GMU_HFI_CTRL_STATUS) = 1;
        if (reg == A630_GMU_GX_SPTPRAC_POWER_CONTROL && v == 0x778000) GMU(A630_GMU_SPTPRAC_PWR_CLK_STATUS) = 0x28;
    }
}
static void barrierNop(void *c) {(void)c;}
static unsigned delays;
static void delayUs(void *c, unsigned us) {(void)c; delays += us;}

/* A command DB with gfx.lvl, cx.lvl, mx.lvl on slave 3 (ARC) laid out as cmd-db.c reads it. */
static size_t makeCmdDb(uint8_t *db)
{
    memset(db, 0, 0x20000);
    db[0] = 1; db[4] = 0xdb; db[5] = 0x30; db[6] = 0x03; db[7] = 0x0c;
    /* rsc_hdr[0]: slv_id 3, header_offset 0, data_offset 0x100, cnt 3 */
    uint8_t *h = db + 8;
    h[0] = 3; h[2] = 0; h[4] = 0x00; h[5] = 0x01; h[6] = 3;
    static const char *ids[3] = {"gfx.lvl", "cx.lvl", "mx.lvl"};
    static const uint16_t gfx[] = {0, 16, 48, 64, 128, 192, 256, 320, 384, 416}, cx[] = {0, 16, 48, 64, 128, 192, 256, 384},
                          mx[] = {0, 16, 48, 64, 128, 256, 384};
    const uint16_t *aux[3] = {gfx, cx, mx};
    const unsigned counts[3] = {10, 8, 7};
    unsigned data = 0;
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t *e = db + 144 + 24 * i;
        memcpy(e, ids[i], strlen(ids[i]));
        uint32_t addr = 0x30000 + 0x10 * i;
        memcpy(e + 16, &addr, 4);
        uint16_t len = (uint16_t)(2 * counts[i]), offset = (uint16_t)data;
        memcpy(e + 20, &len, 2);
        memcpy(e + 22, &offset, 2);
        memcpy(db + 144 + 0x100 + data, aux[i], len);
        data += len;
    }
    return 0x20000;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned b = 0; b < A630BlockCount; ++b) mem[b] = calloc(kBlockBytes / 4, 4);
    for (unsigned i = 0; i < sizeof firmware; ++i) firmware[i] = (uint8_t)(i * 7 + 3);
    memset(firmware + 0x7fe0, 0, 32);   /* a630_gmu.bin: DTCM + 0xff8 (the version a6xx_gmu_start checks) is 0 */
    /* Reset values seen on fajita (gpu-peek1): GDSCs collapsed, branches off, cfg AHB on. */
    R(A630BlockGcc, 0x71004) = 0x00000001;
    R(A630BlockGcc, 0x7100c) = 0x80004220;
    R(A630BlockGcc, 0x44038) = 0x80000000;
    R(A630BlockGcc, 0x52004) = 0x01601d7b;
    R(A630BlockGpuCc, 0x106c) = 0x00222001;
    R(A630BlockGpuCc, 0x100c) = 0x00222001;
    R(A630BlockGpuCc, 0x1120) = 0x80000000;
    R(A630BlockGpuCc, 0x1098) = 0x80004220;
    R(A630BlockGpuCc, 0x109c) = 0x80000001;
    GMU(A630_GMU_CM3_SYSRESET) = 1;
    GMU(A630_GMU_SPTPRAC_PWR_CLK_STATUS) = 0xd4;
    A630Io io = {0, rd, wr, barrierNop, delayUs};

    A630ClockReport cr = {0};
    assert(A630PowerCx(&io, &cr) == A630Ok);
    assert(cr.cx_after & 0x80000000u);
    assert((R(A630BlockGpuCc, 0x106c) & 0x00fff006) == 0x00288000);   /* REST 2, FEW 8, CLK_DIS 8, HW ctl off */
    assert(R(A630BlockGcc, 0x52004) == (0x01601d7bu | (3u << 15)));
    assert((R(A630BlockGcc, 0x71028) & 3) == 3);
    assert(A630GmuClocks(&io, &cr) == A630Ok);
    assert((R(A630BlockGpuCc, 0x1124) & 0x71f) == 0x602);
    assert(R(A630BlockGpuCc, 0x1098) == 0x00004ff1);
    assert(!(R(A630BlockGpuCc, 0x1120) & 2));   /* force enable dropped */

    /* Command DB votes */
    static uint8_t db[0x20000];
    size_t dbBytes = makeCmdDb(db);
    if (argc > 1) {   /* a real dump of aop-cmd-db-mem (fjdbg readp 0x85fe0000) */
        FILE *f = fopen(argv[1], "rb");
        assert(f);
        dbBytes = fread(db, 1, sizeof db, f);
        fclose(f);
    }
    A630PerfTable perf;
    assert(A630BuildPerfTable(db, dbBytes, &perf) == A630Ok);
    printf("gx votes:");
    for (unsigned i = 0; i < perf.gpu_count; ++i) printf(" %u:%08x", perf.gpu_khz[i] / 1000, perf.gx_votes[i]);
    printf("\ncx votes:");
    for (unsigned i = 0; i < perf.gmu_count; ++i) printf(" %u:%08x", perf.gmu_khz[i] / 1000, perf.cx_votes[i]);
    printf("\n");
    if (argc <= 1) {
        /* 710 MHz TURBO_L1 (416): gfx index 9 (416), mx: nothing >= 416, highest nonzero = index 6 */
        assert(perf.gx_votes[7] == ((416u << 16) | (6u << 8) | 9u));
        /* off: level 0 -> index 0 both */
        assert(perf.gx_votes[0] == 0);
        /* 257 MHz LOW_SVS (64): gfx index 3, mx index 3 */
        assert(perf.gx_votes[1] == ((64u << 16) | (3u << 8) | 3u));
        /* GMU 200 MHz MIN_SVS (48): cx index 2, mx index 2 */
        assert(perf.cx_votes[1] == ((48u << 16) | (2u << 8) | 2u));
    }

    /* RSC / PDC */
    A630GmuReport gr = {0};
    assert(A630RpmhInit(&io, &gr) == A630Ok);
    assert(R(A630BlockRscc, A630_RSCC_RSCC_SEQ_MEM_0_DRV0 * 4) == 0xa7a506a0);
    assert(R(A630BlockRscc, (A630_RSCC_RSCC_HIDDEN_TCS_CMD0_DATA + 4) * 4) == 0x80000000);
    assert(R(A630BlockPdcSeq, 4 * 4) == 0x002081fc);
    assert(R(A630BlockPdc, A630_PDC_GPU_ENABLE_PDC * 4) == 0x80000001);
    assert(R(A630BlockPdc, (A630_PDC_GPU_TCS3_CMD0_DATA + 8) * 4) == 0x3);
    assert(R(A630BlockPdc, (A630_PDC_GPU_TCS1_CMD0_ADDR + 8) * 4) == 0x30080);
    assert(GMU(A630_GMU_RSCC_CONTROL_REQ) == 0);
    assert(A630_PDC_GPU_ENABLE_PDC == 0x1140 && A630_PDC_GPU_TCS1_CMD0_MSGID == 0x1575);

    /* Cold start */
    A630GmuMemory m = {hfi, 0x60009000, 0x60001000, 0x4000, 0x60005000, 0x4000};
    A630Hfi h;
    A630HfiInit(&m, &h);
    assert(hfi[1] == 30 && hfi[2] == 6 && hfi[4] == 2 && hfi[6 + 1] == 0x6000a000 && hfi[6 + 12 + 1] == 0x6000b000);
    A630GmuConfig c = {0x06030001, firmware, sizeof firmware, &perf, perf.gpu_count - 1};
    A630Status s = A630GmuColdStart(&io, &m, &h, &c, &gr);
    printf("cold start: status %d step %u fw %08x replies %u commands %u init %08x polls %u\n", s, gr.step, h.fw_version, h.replies, hfi_commands, gr.fw_init_result, gr.polls);
    assert(s == A630Ok && gr.step == A630StepDone);
    assert(hfi_commands == 5 && hfi_ids[0] == 0 && hfi_ids[1] == 1 && hfi_ids[2] == 4 && hfi_ids[3] == 3 && hfi_ids[4] == 5);
    assert(h.fw_version == 0x10f0000);
    assert(GMU(A630_GMU_HFI_QTBL_ADDR) == 0x60009000 && GMU(A630_GMU_GENERAL_7) == 1);
    assert(GMU(A630_GMU_HFI_SFR_ADDR) == 0x06030100);   /* 6.3.0.1: minor 0 in bits 15:12, patch 1 in bits 11:8 */
    assert(GMU(A630_GPU_GMU_CX_GMU_PWR_COL_CP_MSG) == (0x60005000u | 3));
    assert(GMU(A630_GMU_GX_VOTE_IDX) == (perf.gx_votes[7] & 0xff));
    assert(GMU(A630_GMU_DCVS_PERF_SETTING) == ((3u << 28) | 7));
    assert(GMU(A630_GMU_GMU2HOST_INTR_MASK) == ~(1u << 23));
    assert((GMU(A630_GMU_RPMH_CTRL) & 0xf11) == 0xf11);

    /* A failing reply times out instead of hanging */
    drop_reply_id = 5;
    uint32_t msg[2];
    A630Hfi h2 = h;
    unsigned before = delays;
    assert(A630HfiSend(&io, &m, &h2, 5, msg, 1, 0, 0) == A630ErrHfiTimeout);
    assert(delays - before >= 1000000);
    drop_reply_id = -1;

    /* PM4 headers (values seen in Linux a6xx ring dumps) */
    assert(A630Pkt7(A630CpMeInit, 8) == 0x70c80008);
    assert(A630Pkt7(A630CpEventWrite, 4) == 0x70460004);
    assert(A630Pkt4(A630_CP_SCRATCH + 2, 1) == 0x48088501);   /* parity(0x885) = 1 -> bit 27 */
    uint32_t words[16];
    assert(A630CpInitWords(words) == 9 && words[1] == 0x2f && words[3] == 0x20000000);
    assert(A630FenceWords(5, 0x1200004, words) == 7 && words[3] == 0x80000004 && words[4] == 0x1200004 && words[6] == 5);

    /* hw_init */
    A630HwInitConfig hc = {0x1000000, 0x1100000, 0x1200000, 0x8000, 0x1fffffffff000ull, 0x100000, 0x100000};
    A630HwInitReport hr = {0};
    A630HwInit(&io, &hc, &hr);
    assert(hr.step == 8);
    assert(R(A630BlockGpu, A630_CP_RB_CNTL * 4) == 0x0800020c);
    assert(R(A630BlockGpu, A630_RBBM_CLOCK_CNTL * 4) == 0x8aa8aa02);
    assert(R(A630BlockGpu, A630_UCHE_GMEM_RANGE_MAX * 4) == 0x1fffff);
    assert(R(A630BlockGpu, A630_UCHE_TRAP_BASE * 4 + 4) == 0x1ffff && R(A630BlockGpu, A630_UCHE_TRAP_BASE * 4) == 0xfffff000);
    assert(R(A630BlockGpu, (A630_CP_PROTECT + 31) * 4) == 0x80011c00);
    assert(R(A630BlockGpu, A630_RBBM_INT_0_MASK * 4) == 0x3d0e242);
    assert(R(A630BlockGpu, A630_RB_NC_MODE_CNTL * 4) == 4 && R(A630BlockGpu, A630_UCHE_MODE_CNTL * 4) == 0x400000);
    assert(R(A630BlockGpu, A630_CP_SQE_CNTL * 4) == 1);
    assert(GMU(A630_GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL) & 1);
    printf("A630 host tests: PASS (%u GMU writes, %u RSCC, %u PDC, %u GPU)\n", writes[A630BlockGmu], writes[A630BlockRscc],
           writes[A630BlockPdc], writes[A630BlockGpu]);
    return 0;
}
