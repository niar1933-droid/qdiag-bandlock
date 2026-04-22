/*
 * QRTR (Qualcomm IPC Router) transport — direct QMI messaging bypassing
 * /dev/diag. Works on any Android device whose kernel has CONFIG_QRTR=y,
 * which is every modern Snapdragon device (including Poco F6 / HyperOS).
 *
 * The modem, modem-dsp, etc. register QMI services on QRTR and can be
 * reached from user-space without qcrild / diagchar as long as the caller
 * has CAP_NET_BIND / root and SELinux allows qipcrtr_socket.
 *
 * Wire format:
 *   - socket(AF_QIPCRTR, SOCK_DGRAM, 0)
 *   - sockaddr_qrtr { sq_family=42, sq_node, sq_port }
 *   - "CTRL" port = 0xFFFFFFFE, "BCAST" node = 0xFFFFFFFF
 *   - control packets announce {service, instance, node, port} tuples; we
 *     use NEW_LOOKUP to ask the router to push current server list.
 *   - each datagram body is the raw QMI service-level header:
 *       [type:u8][txn:u16 LE][msg_id:u16 LE][tlv_length:u16 LE][TLVs]
 *     (no QMUX header — unlike /dev/cdc-wdm or /dev/diag).
 */

#ifndef QDIAG_QRTR_H
#define QDIAG_QRTR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QMI service IDs (match libqmi / QMI-IDL). */
#define QMI_SVC_CTL    0x00
#define QMI_SVC_WDS    0x01
#define QMI_SVC_DMS    0x02
#define QMI_SVC_NAS    0x03
#define QMI_SVC_WMS    0x05
#define QMI_SVC_PDS    0x06
#define QMI_SVC_VOICE  0x09
#define QMI_SVC_PBM    0x0B
#define QMI_SVC_UIM    0x0B
#define QMI_SVC_DSD    0x22
#define QMI_SVC_PDC    0x24   /* Persistent Device Config */
#define QMI_SVC_RFSA   0xE1   /* Remote File System Access (vendor-assigned) */

/* Return codes. */
#define QRTR_RC_OK              0
#define QRTR_RC_SOCKET_FAIL   -1
#define QRTR_RC_BIND_FAIL     -2
#define QRTR_RC_SEND_FAIL     -3
#define QRTR_RC_RECV_FAIL     -4
#define QRTR_RC_TIMEOUT       -5
#define QRTR_RC_NO_SERVICE    -6
#define QRTR_RC_PARSE_FAIL    -7
#define QRTR_RC_BUF_OVERFLOW  -8
#define QRTR_RC_NOT_OPEN      -9

/** Open an AF_QIPCRTR socket, bind a local port, send HELLO.
 *  Returns 0 on success, QRTR_RC_* on error (see errno for detail). */
int qrtr_open(void);

/** Close the QRTR socket. */
void qrtr_close(void);

/** True if the socket is open. */
int qrtr_is_open(void);

/** Last errno from the most recent syscall (for diagnostic display). */
int qrtr_last_errno(void);

/**
 * Look up a QMI service on QRTR. Sends NEW_LOOKUP to CTRL port, then
 * reads NEW_SERVER announcements until one matches (service==`service`,
 * instance==`instance` or any) or timeout_ms elapses.
 *
 * @param service     QMI service id (e.g. QMI_SVC_NAS).
 * @param instance    Instance id (0 for default modem; >0 for dual-SIM
 *                    secondary, vendor-specific mapping).
 * @param timeout_ms  Max wait. 3000 is a sane default.
 * @param out_node    Destination node id (set on success).
 * @param out_port    Destination port (set on success).
 * @return 0 on success, QRTR_RC_NO_SERVICE on miss.
 */
int qrtr_lookup(uint32_t service, uint32_t instance, int timeout_ms,
                uint32_t *out_node, uint32_t *out_port);

/**
 * Collect all currently-advertised QMI services. Returns count written
 * to `out` (each entry = service, instance, node, port).
 */
typedef struct {
    uint32_t service;
    uint32_t instance;
    uint32_t node;
    uint32_t port;
} qrtr_service_t;

int qrtr_enumerate(qrtr_service_t *out, int cap, int timeout_ms);

/**
 * Build the QMI service-level header + TLVs into `out`.
 *   [type=0 request][txn LE][msg_id LE][tlv_len LE][TLVs]
 * Returns total bytes written or 0 on overflow.
 */
size_t qrtr_build_qmi_request(uint8_t *out, size_t out_cap,
                              uint16_t msg_id,
                              const uint8_t *tlvs, size_t tlvs_len);

/**
 * Send a datagram to (node, port) and wait for one response on our bound
 * port (up to `timeout_ms`). Response bytes are copied to `rx` (at most
 * `rx_cap`). Returns response length on success or negative QRTR_RC_*.
 */
int qrtr_transact(uint32_t node, uint32_t port,
                  const uint8_t *tx, size_t tx_len,
                  uint8_t *rx, size_t rx_cap, int timeout_ms);

/**
 * Parse a QMI response. Fills *out_result (0=success, 1=failure) and
 * *out_error (QMI error code if result==1). Returns 0 if parsed OK,
 * negative otherwise. If result==0 (success), TLVs follow at offset 7
 * and can be parsed by the caller.
 */
int qrtr_parse_qmi_response(const uint8_t *buf, size_t len,
                            uint16_t *out_msg_id,
                            uint16_t *out_result,
                            uint16_t *out_error);

#ifdef __cplusplus
}
#endif

#endif /* QDIAG_QRTR_H */
