#include "qrtr.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------- AF / struct fallbacks (NDK may not expose qrtr headers) ---------- */

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

struct sockaddr_qrtr_compat {
    unsigned short sq_family;
    uint32_t       sq_node;
    uint32_t       sq_port;
};

#define QRTR_PORT_CTRL 0xFFFFFFFEu
#define QRTR_NODE_BCAST 0xFFFFFFFFu

/* Control-message command codes (from linux/qrtr.h). */
enum {
    QRTR_TYPE_DATA          = 1,
    QRTR_TYPE_HELLO         = 2,
    QRTR_TYPE_BYE           = 3,
    QRTR_TYPE_NEW_SERVER    = 4,
    QRTR_TYPE_DEL_SERVER    = 5,
    QRTR_TYPE_DEL_CLIENT    = 6,
    QRTR_TYPE_RESUME_TX     = 7,
    QRTR_TYPE_EXIT          = 8,
    QRTR_TYPE_PING          = 9,
    QRTR_TYPE_NEW_LOOKUP    = 10,
    QRTR_TYPE_DEL_LOOKUP    = 11,
};

/* Control packet layout (packed LE). */
struct qrtr_ctrl_pkt {
    uint32_t cmd;
    uint32_t service;
    uint32_t instance;
    uint32_t node;
    uint32_t port;
} __attribute__((packed));

/* ---------- module state ---------- */

static int      g_sock = -1;
static uint32_t g_my_node = 0;
static uint32_t g_my_port = 0;
static int      g_last_errno = 0;
static uint16_t g_txn = 1;

static inline void save_errno(void) { g_last_errno = errno; }

int qrtr_last_errno(void) { return g_last_errno; }
int qrtr_is_open(void)    { return g_sock >= 0; }

/* ---------- open / close ---------- */

int qrtr_open(void) {
    if (g_sock >= 0) return QRTR_RC_OK;

    g_sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    if (g_sock < 0) { save_errno(); return QRTR_RC_SOCKET_FAIL; }

    /* Bind to a kernel-assigned port. The kernel fills in node/port. */
    struct sockaddr_qrtr_compat sa = { 0 };
    sa.sq_family = AF_QIPCRTR;
    sa.sq_node   = 0;      /* 0 == "don't care, kernel assigns" */
    sa.sq_port   = 0;

    if (bind(g_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        save_errno();
        close(g_sock);
        g_sock = -1;
        return QRTR_RC_BIND_FAIL;
    }

    socklen_t sl = sizeof(sa);
    if (getsockname(g_sock, (struct sockaddr *)&sa, &sl) == 0) {
        g_my_node = sa.sq_node;
        g_my_port = sa.sq_port;
    }

    /* Politeness: tell the router we're alive. Some kernels require this. */
    struct qrtr_ctrl_pkt hello = { 0 };
    hello.cmd = QRTR_TYPE_HELLO;

    struct sockaddr_qrtr_compat ctrl = { 0 };
    ctrl.sq_family = AF_QIPCRTR;
    ctrl.sq_node   = 1;                 /* local-node control */
    ctrl.sq_port   = QRTR_PORT_CTRL;

    /* HELLO is optional; ignore errors. */
    (void)sendto(g_sock, &hello, sizeof(hello), 0,
                 (struct sockaddr *)&ctrl, sizeof(ctrl));

    return QRTR_RC_OK;
}

void qrtr_close(void) {
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}

/* ---------- lookup / enumerate ---------- */

static int send_new_lookup(uint32_t service, uint32_t instance) {
    struct qrtr_ctrl_pkt pkt = { 0 };
    pkt.cmd      = QRTR_TYPE_NEW_LOOKUP;
    pkt.service  = service;
    pkt.instance = instance;

    struct sockaddr_qrtr_compat ctrl = { 0 };
    ctrl.sq_family = AF_QIPCRTR;
    ctrl.sq_node   = 1;
    ctrl.sq_port   = QRTR_PORT_CTRL;

    ssize_t n = sendto(g_sock, &pkt, sizeof(pkt), 0,
                       (struct sockaddr *)&ctrl, sizeof(ctrl));
    if (n < 0) { save_errno(); return QRTR_RC_SEND_FAIL; }
    return QRTR_RC_OK;
}

int qrtr_lookup(uint32_t service, uint32_t instance, int timeout_ms,
                uint32_t *out_node, uint32_t *out_port) {
    if (g_sock < 0) return QRTR_RC_NOT_OPEN;

    int rc = send_new_lookup(service, instance);
    if (rc < 0) return rc;

    /* Listen for NEW_SERVER broadcasts. */
    struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int poll_ms = timeout_ms - elapsed;
        if (poll_ms > 500) poll_ms = 500;
        int pr = poll(&pfd, 1, poll_ms);
        if (pr < 0) { save_errno(); return QRTR_RC_RECV_FAIL; }
        elapsed += poll_ms;
        if (pr == 0) continue;

        uint8_t buf[128];
        struct sockaddr_qrtr_compat src = { 0 };
        socklen_t sl = sizeof(src);
        ssize_t n = recvfrom(g_sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &sl);
        if (n < 0) { save_errno(); return QRTR_RC_RECV_FAIL; }
        if ((size_t)n < sizeof(struct qrtr_ctrl_pkt)) continue;
        if (src.sq_port != QRTR_PORT_CTRL) continue;

        const struct qrtr_ctrl_pkt *p = (const struct qrtr_ctrl_pkt *)buf;
        if (p->cmd != QRTR_TYPE_NEW_SERVER) continue;
        if (p->service != service) continue;
        if (instance != 0 && p->instance != instance) continue;

        if (out_node) *out_node = p->node;
        if (out_port) *out_port = p->port;
        return QRTR_RC_OK;
    }
    return QRTR_RC_NO_SERVICE;
}

int qrtr_enumerate(qrtr_service_t *out, int cap, int timeout_ms) {
    if (g_sock < 0) return QRTR_RC_NOT_OPEN;

    /* Request a global lookup (service==0 means "everything"). */
    int rc = send_new_lookup(0, 0);
    if (rc < 0) return rc;

    int count = 0;
    struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int poll_ms = timeout_ms - elapsed;
        if (poll_ms > 300) poll_ms = 300;
        int pr = poll(&pfd, 1, poll_ms);
        if (pr < 0) { save_errno(); return QRTR_RC_RECV_FAIL; }
        elapsed += poll_ms;
        if (pr == 0) continue;

        uint8_t buf[128];
        struct sockaddr_qrtr_compat src = { 0 };
        socklen_t sl = sizeof(src);
        ssize_t n = recvfrom(g_sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &sl);
        if (n < 0) { save_errno(); return QRTR_RC_RECV_FAIL; }
        if ((size_t)n < sizeof(struct qrtr_ctrl_pkt)) continue;
        if (src.sq_port != QRTR_PORT_CTRL) continue;

        const struct qrtr_ctrl_pkt *p = (const struct qrtr_ctrl_pkt *)buf;
        if (p->cmd != QRTR_TYPE_NEW_SERVER) continue;
        if (p->service == 0 && p->node == 0 && p->port == 0) {
            /* Sentinel "end of list". */
            break;
        }
        if (count < cap) {
            out[count].service  = p->service;
            out[count].instance = p->instance;
            out[count].node     = p->node;
            out[count].port     = p->port;
        }
        count++;
    }
    return count;
}

/* ---------- QMI request builder ---------- */

static size_t put_u16_le(uint8_t *p, size_t off, uint16_t v) {
    p[off]     = (uint8_t)(v & 0xFF);
    p[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    return off + 2;
}

size_t qrtr_build_qmi_request(uint8_t *out, size_t out_cap,
                              uint16_t msg_id,
                              const uint8_t *tlvs, size_t tlvs_len) {
    size_t need = 7 + tlvs_len;
    if (need > out_cap) return 0;

    size_t o = 0;
    out[o++] = 0x00;                       /* type = request */
    o = put_u16_le(out, o, g_txn++);       /* transaction id */
    o = put_u16_le(out, o, msg_id);        /* message id */
    o = put_u16_le(out, o, (uint16_t)tlvs_len);
    if (tlvs_len) {
        memcpy(out + o, tlvs, tlvs_len);
        o += tlvs_len;
    }
    return o;
}

/* ---------- transact ---------- */

int qrtr_transact(uint32_t node, uint32_t port,
                  const uint8_t *tx, size_t tx_len,
                  uint8_t *rx, size_t rx_cap, int timeout_ms) {
    if (g_sock < 0) return QRTR_RC_NOT_OPEN;

    struct sockaddr_qrtr_compat dst = { 0 };
    dst.sq_family = AF_QIPCRTR;
    dst.sq_node   = node;
    dst.sq_port   = port;

    ssize_t n = sendto(g_sock, tx, tx_len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    if (n < 0) { save_errno(); return QRTR_RC_SEND_FAIL; }

    struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int poll_ms = timeout_ms - elapsed;
        if (poll_ms > 1000) poll_ms = 1000;
        int pr = poll(&pfd, 1, poll_ms);
        if (pr < 0) { save_errno(); return QRTR_RC_RECV_FAIL; }
        elapsed += poll_ms;
        if (pr == 0) continue;

        struct sockaddr_qrtr_compat src = { 0 };
        socklen_t sl = sizeof(src);
        ssize_t m = recvfrom(g_sock, rx, rx_cap, 0,
                             (struct sockaddr *)&src, &sl);
        if (m < 0) { save_errno(); return QRTR_RC_RECV_FAIL; }
        if (src.sq_port == QRTR_PORT_CTRL) {
            /* Control advertisement; not our reply — keep waiting. */
            continue;
        }
        if (src.sq_node == node && src.sq_port == port) {
            return (int)m;
        }
    }
    return QRTR_RC_TIMEOUT;
}

/* ---------- parse QMI response ---------- */

int qrtr_parse_qmi_response(const uint8_t *buf, size_t len,
                            uint16_t *out_msg_id,
                            uint16_t *out_result,
                            uint16_t *out_error) {
    if (!buf || len < 7) return QRTR_RC_PARSE_FAIL;
    /* QMI service header:
     *   +0 type (should be 2 for response)
     *   +1 txn u16 LE
     *   +3 msg_id u16 LE
     *   +5 tlv_len u16 LE
     *   +7 TLVs...
     *
     * The "result" TLV (type 0x02) is always first and is 4 bytes:
     *   result (u16 LE)  0=SUCCESS, 1=FAILURE
     *   error  (u16 LE)  QMI_ERR_*
     */
    uint16_t msg_id  = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
    uint16_t tlv_len = (uint16_t)buf[5] | ((uint16_t)buf[6] << 8);
    if (out_msg_id) *out_msg_id = msg_id;
    if ((size_t)(7 + tlv_len) > len) return QRTR_RC_PARSE_FAIL;

    /* Walk TLVs looking for type 0x02 (result). */
    size_t o = 7;
    while (o + 3 <= 7 + tlv_len) {
        uint8_t  t   = buf[o];
        uint16_t tl  = (uint16_t)buf[o + 1] | ((uint16_t)buf[o + 2] << 8);
        if (o + 3 + tl > 7 + tlv_len) return QRTR_RC_PARSE_FAIL;
        if (t == 0x02 && tl >= 4) {
            uint16_t r = (uint16_t)buf[o + 3] | ((uint16_t)buf[o + 4] << 8);
            uint16_t e = (uint16_t)buf[o + 5] | ((uint16_t)buf[o + 6] << 8);
            if (out_result) *out_result = r;
            if (out_error)  *out_error  = e;
            return QRTR_RC_OK;
        }
        o += 3 + tl;
    }
    return QRTR_RC_PARSE_FAIL;
}
