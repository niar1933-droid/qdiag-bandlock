/* SPDX-License-Identifier: Apache-2.0
 * QMUX transport over /dev/socket/qmux_radio
 *
 * Implements the Qualcomm QMI multiplexer (qmuxd) framing used on Android
 * devices without /dev/diag. Same wire format libqmi/uqmi/qcrild speak:
 *
 *   [0x01][total_len:LE16][flags:1][service_id:1][client_id:1][SDU...]
 *
 * Where total_len EXCLUDES the leading 0x01 byte but INCLUDES itself. SDU
 * is a regular QMI message (CTL or service-specific), identical to what
 * we already build for the DIAG/QRTR paths.
 *
 * Flow (band pref over NAS):
 *   1. connect AF_UNIX /dev/socket/qmux_radio
 *   2. QMI_CTL.GetClientId(service=NAS) -> client_id
 *   3. Wrap QMI NAS request in qmux frame with allocated client_id
 *   4. Read qmux reply, unwrap, parse QMI response TLV 0x02 (result/error)
 *   5. QMI_CTL.ReleaseClientId(...)  [best-effort on close]
 *   6. close socket
 */

#ifndef QMUX_H
#define QMUX_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- return codes ---------- */
#define QMUX_RC_OK               0
#define QMUX_RC_SOCKET_FAIL     -1
#define QMUX_RC_CONNECT_FAIL    -2
#define QMUX_RC_SEND_FAIL       -3
#define QMUX_RC_RECV_FAIL       -4
#define QMUX_RC_TIMEOUT         -5
#define QMUX_RC_PROTO           -6   /* malformed frame from peer */
#define QMUX_RC_NOT_OPEN        -7
#define QMUX_RC_BUF_OVERFLOW    -8
#define QMUX_RC_CLIENT_ALLOC    -9   /* CTL rejected GetClientId */
#define QMUX_RC_BAD_RESPONSE   -10

/* QMI service IDs used by this module. */
#define QMUX_SVC_CTL   0x00
#define QMUX_SVC_WDS   0x01
#define QMUX_SVC_DMS   0x02
#define QMUX_SVC_NAS   0x03

int  qmux_open(const char *sock_path);   /* "/dev/socket/qmux_radio" */
void qmux_close(void);
int  qmux_is_open(void);
int  qmux_last_errno(void);
const char *qmux_last_sockpath(void);

/** Allocate a client ID on the given service. Blocking up to timeout_ms. */
int qmux_alloc_client(uint8_t service_id, uint8_t *out_client_id, int timeout_ms);

/** Release a previously allocated client ID. Best-effort; ignore errors. */
void qmux_release_client(uint8_t service_id, uint8_t client_id);

/** Send a service QMI request and wait for the matching response.
 *  `qmi_sdu` must be the raw QMI SDU (without qmux header).
 *  Returns bytes written into `rx` or negative QMUX_RC_* code. */
int qmux_transact(uint8_t service_id, uint8_t client_id,
                  const uint8_t *qmi_sdu, size_t sdu_len,
                  uint8_t *rx, size_t rx_cap, int timeout_ms);

/** Build a service-level QMI SDU (same format as QRTR path).
 *  Layout: [0x00][txn LE16][msg_id LE16][tlv_len LE16][TLVs...] */
size_t qmux_build_qmi_sdu(uint8_t *out, size_t out_cap,
                          uint16_t msg_id,
                          const uint8_t *tlvs, size_t tlvs_len);

/** Parse a service-level QMI response. Extracts result/error from TLV 0x02.
 *  Returns 0 on success, <0 on malformed input. */
int qmux_parse_qmi_response(const uint8_t *buf, size_t len,
                            uint16_t *msg_id,
                            uint16_t *result,
                            uint16_t *error);

#ifdef __cplusplus
}
#endif

#endif /* QMUX_H */
