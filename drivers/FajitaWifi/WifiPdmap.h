// SPDX-License-Identifier: BSD-2-Clause-Patent
// pd-mapper: the servreg locator QMI server (service 0x40, version 0x101) the modem asks for its protection-domain
// lists, after linux-msm/pd-mapper (pd-mapper.c, servreg_loc.qmi). The domain table is loaded from the .jsn files of the
// phone's own modem partition (image/modemr.jsn, image/modemuw.jsn), read through the FAT reader. Freestanding,
// host-tested against the real files and hand-built requests.
#ifndef FAJITA_WIFI_PDMAP_H
#define FAJITA_WIFI_PDMAP_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WPDM_SERVICE 0x40u
#define WPDM_VERSION 0x101u
#define WPDM_INSTANCE 0u
#define WPDM_GET_DOMAIN_LIST 0x21u   // SERVREG_LOC_GET_DOMAIN_LIST (servreg_loc.h 33, pdr_internal.h)
#define WPDM_PFR 0x24u
#define WPDM_MAX_MAPS 24
#define WPDM_NAME 65

typedef struct {
    char service[WPDM_NAME];   // "provider/service", e.g. "wlan/fw"
    char domain[WPDM_NAME];    // "soc/domain/subdomain", e.g. "msm/modem/wlan_pd"
    uint32_t instance;         // qmi_instance_id
} wpdm_map;

typedef struct {
    wpdm_map map[WPDM_MAX_MAPS];
    unsigned count;
    uint32_t requests, pfr;
    char names[6][48]; unsigned nnames;   // the service names asked for (first 6), with the offset and number of domains returned: "name@off=n"
} wpdm;

void wpdm_init(wpdm *p);
// Add the services of one .jsn file (pd-mapper.c pd_load_map). Returns how many maps were added, or -1 if malformed.
int wpdm_add_jsn(wpdm *p, const char *json, size_t len);
// Answer one request (GET_DOMAIN_LIST or PFR). Returns the response length, or -1 when it is not a request to answer.
long wpdm_handle(wpdm *p, const uint8_t *req, size_t len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
