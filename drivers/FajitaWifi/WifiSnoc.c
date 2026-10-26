// SPDX-License-Identifier: BSD-2-Clause-Patent
// WCN3990 SNOC CE tables, from ath10k snoc.c (Linux v6.16). See WifiSnoc.h.
#include "WifiSnoc.h"

#define F WSNOC_CE_ATTR_FLAGS
#define DIS WSNOC_CE_ATTR_DIS_INTR
#define IN WSNOC_PIPEDIR_IN
#define OUT WSNOC_PIPEDIR_OUT

const wsnoc_host_ce wsnoc_host_ce_config[WSNOC_CE_COUNT] = {
    {F, 16, 2048, 0},          // CE0: host->target HTC control
    {F, 0, 2048, 512},         // CE1: target->host HTT + HTC control
    {F, 0, 2048, 64},          // CE2: target->host WMI
    {F, 32, 2048, 0},          // CE3: host->target WMI
    {F | DIS, 2048, 256, 0},   // CE4: host->target HTT
    {F, 0, 512, 512},          // CE5: target->host HTT (ipa_uc->target)
    {F, 0, 0, 0},              // CE6: target autonomous hif_memcpy
    {F, 2, 2048, 2},           // CE7: ce_diag
    {F, 0, 2048, 128},         // CE8: target to uMC
    {F, 0, 2048, 512},         // CE9: target->host HTT
    {F, 0, 2048, 512},         // CE10: target->host HTT
    {F, 0, 2048, 512},         // CE11: target->host PKTLOG
};

const wlfw_ce_tgt wsnoc_target_ce_config[WSNOC_CE_COUNT] = {
    {0, OUT, 32, 2048, F},
    {1, IN, 32, 2048, F},
    {2, IN, 64, 2048, F},
    {3, OUT, 32, 2048, F},
    {4, OUT, 256, 256, F | DIS},
    {5, OUT, 1024, 64, F | DIS},
    {6, WSNOC_PIPEDIR_INOUT, 32, 16384, F},
    {7, 4, 0, 0, F | DIS},     // CE7 used only by the host (pipedir 4, as in snoc.c)
    {8, IN, 32, 2048, 0},
    {9, IN, 32, 2048, F},
    {10, IN, 32, 2048, F},
    {11, IN, 32, 2048, F},
};

const wlfw_ce_svc wsnoc_service_map[21] = {
    {WHTC_SVC_WMI_DATA_VO, OUT, 3}, {WHTC_SVC_WMI_DATA_VO, IN, 2},
    {WHTC_SVC_WMI_DATA_BK, OUT, 3}, {WHTC_SVC_WMI_DATA_BK, IN, 2},
    {WHTC_SVC_WMI_DATA_BE, OUT, 3}, {WHTC_SVC_WMI_DATA_BE, IN, 2},
    {WHTC_SVC_WMI_DATA_VI, OUT, 3}, {WHTC_SVC_WMI_DATA_VI, IN, 2},
    {WHTC_SVC_WMI_CONTROL, OUT, 3}, {WHTC_SVC_WMI_CONTROL, IN, 2},
    {WHTC_SVC_RSVD_CTRL, OUT, 0}, {WHTC_SVC_RSVD_CTRL, IN, 2},
    {WHTC_SVC_TEST_RAW_STREAMS, OUT, 0}, {WHTC_SVC_TEST_RAW_STREAMS, IN, 2},
    {WHTC_SVC_HTT_DATA_MSG, OUT, 4}, {WHTC_SVC_HTT_DATA_MSG, IN, 1},
    {WHTC_SVC_TEST_RAW_STREAMS, OUT, 5},
    {WHTC_SVC_HTT_DATA2_MSG, IN, 9},
    {WHTC_SVC_HTT_DATA3_MSG, IN, 10},
    {WHTC_SVC_HTT_LOG_MSG, IN, 11},
    {0, 0, 0},                 // must be last (and is sent)
};

const wlfw_shadow wsnoc_shadow_map[12] = {
    {0, WSNOC_SRC_WR_IDX_OFFSET}, {3, WSNOC_SRC_WR_IDX_OFFSET}, {4, WSNOC_SRC_WR_IDX_OFFSET},
    {5, WSNOC_SRC_WR_IDX_OFFSET}, {7, WSNOC_SRC_WR_IDX_OFFSET},
    {1, WSNOC_DST_WR_IDX_OFFSET}, {2, WSNOC_DST_WR_IDX_OFFSET}, {7, WSNOC_DST_WR_IDX_OFFSET},
    {8, WSNOC_DST_WR_IDX_OFFSET}, {9, WSNOC_DST_WR_IDX_OFFSET}, {10, WSNOC_DST_WR_IDX_OFFSET},
    {11, WSNOC_DST_WR_IDX_OFFSET},
};

int wsnoc_service_pipes(uint32_t service_id, uint8_t *ul, uint8_t *dl)
{
    int ul_set = 0, dl_set = 0;
    for (unsigned i = 0; i < WSNOC_N_SERVICE_MAP; ++i) {
        const wlfw_ce_svc *e = &wsnoc_service_map[i];
        if (e->service_id != service_id) continue;
        if (e->pipe_dir == OUT) {if (ul) *ul = (uint8_t)e->pipe_num; ul_set = 1;}
        if (e->pipe_dir == IN) {if (dl) *dl = (uint8_t)e->pipe_num; dl_set = 1;}
    }
    return ul_set && dl_set ? 0 : -1;
}
