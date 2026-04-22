#include "qrtr.h"

#include <android/log.h>
#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef LOG_TAG
#define LOG_TAG "qrtr"
#endif
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Android system UIDs. qrtr-ns / kernel qrtr control-plane may gate
 * NEW_SERVER visibility on the caller's effective UID; qcrild runs as
 * AID_RADIO(1001). Drop to radio around socket()+bind() so the kernel
 * sees us as the radio daemon. */
#define AID_SYSTEM 1000
#define AID_RADIO  1001

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

/* Drop effective uid/gid to AID_RADIO while keeping CAP_* (via
 * PR_SET_KEEPCAPS). Restored by qrtr_restore_euid(). No-op when we are
 * not running as root. */
static void qrtr_drop_to_radio(uid_t *saved_euid, gid_t *saved_egid, int *dropped) {
    *saved_euid = geteuid();
    *saved_egid = getegid();
    *dropped = 0;
    if (*saved_euid != 0) return;
    prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
    gid_t groups[] = { AID_RADIO, AID_SYSTEM };
    if (setgroups(2, groups) < 0) {
        LOGE("qrtr setgroups failed: %d (%s)", errno, strerror(errno));
    }
    if (setegid(AID_RADIO) < 0) {
        LOGE("qrtr setegid(radio) failed: %d (%s)", errno, strerror(errno));
        prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
        return;
    }
    if (seteuid(AID_RADIO) < 0) {
        LOGE("qrtr seteuid(radio) failed: %d (%s)", errno, strerror(errno));
        setegid(*saved_egid);
        prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
        return;
    }
    *dropped = 1;
    LOGI("qrtr: dropped to radio (euid=%d egid=%d)", geteuid(), getegid());
}

static void qrtr_restore_euid(uid_t saved_euid, gid_t saved_egid, int dropped) {
    if (!dropped) return;
    if (seteuid(saved_euid) < 0) LOGE("qrtr seteuid restore failed: %d (%s)", errno, strerror(errno));
    if (setegid(saved_egid) < 0) LOGE("qrtr setegid restore failed: %d (%s)", errno, strerror(errno));
    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
}

int qrtr_open(void) {
    if (g_sock >= 0) return QRTR_RC_OK;

    /* Drop to AID_RADIO around socket()+bind() — kernel qrtr ns may gate
     * NEW_SERVER announcements (modem QMI services) by peer UID, filtering
     * them out for non-radio callers. */
    uid_t seuid = 0; gid_t segid = 0; int dropped = 0;
    qrtr_drop_to_radio(&seuid, &segid, &dropped);

    g_sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        save_errno();
        qrtr_restore_euid(seuid, segid, dropped);
        return QRTR_RC_SOCKET_FAIL;
    }

    /* Some kernels (HyperOS on Poco F6) reject bind(sq_node=0) with EINVAL
     * because the socket is already pre-bound to ipc->us.sq_node != 0.
     * Try 3 strategies in order:
     *   1. No bind at all — kernel auto-assigns a port on first sendto.
     *   2. bind with sq_node set to our current node (from getsockname).
     *   3. bind with sq_node=0 (older kernels).
     * We always skip bind() errors — the socket itself is usable either way.
     */
    struct sockaddr_qrtr_compat sa = { 0 };
    socklen_t sl = sizeof(sa);

    /* Learn our node id first. */
    if (getsockname(g_sock, (struct sockaddr *)&sa, &sl) == 0) {
        g_my_node = sa.sq_node;
    }

    /* Try bind with our own node id (strategy 2). If that fails, drop bind. */
    memset(&sa, 0, sizeof(sa));
    sa.sq_family = AF_QIPCRTR;
    sa.sq_node   = g_my_node;
    sa.sq_port   = 0;
    /* bind() is best-effort: some kernels already pre-bind the socket and
     * reject an explicit re-bind with EINVAL. The socket is still fully
     * usable — the first sendto will lazily allocate the port. */
    (void)bind(g_sock, (struct sockaddr *)&sa, sizeof(sa));

    /* Re-read node/port after the kernel has had a chance to assign them. */
    sl = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (getsockname(g_sock, (struct sockaddr *)&sa, &sl) == 0) {
        g_my_node = sa.sq_node;
        g_my_port = sa.sq_port;
    }

    /* Politeness: tell the router we're alive. HELLO is entirely optional;
     * errors are ignored. CTRL target is our own node, not hardcoded 1. */
    struct qrtr_ctrl_pkt hello = { 0 };
    hello.cmd = QRTR_TYPE_HELLO;

    struct sockaddr_qrtr_compat ctrl = { 0 };
    ctrl.sq_family = AF_QIPCRTR;
    ctrl.sq_node   = g_my_node;
    ctrl.sq_port   = QRTR_PORT_CTRL;
    (void)sendto(g_sock, &hello, sizeof(hello), 0,
                 (struct sockaddr *)&ctrl, sizeof(ctrl));

    /* Keep the socket open under the original euid; the kernel remembers
     * the creating credentials on the struct sock, so qrtr-ns treats us
     * as the radio daemon for the lifetime of this fd. Restore process
     * euid so subsequent non-qrtr syscalls (file I/O, other sockets) run
     * with full root privileges. */
    qrtr_restore_euid(seuid, segid, dropped);
    LOGI("qrtr: open ok my_node=%u my_port=%u", g_my_node, g_my_port);

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
    ctrl.sq_node   = g_my_node;      /* own local node id */
    ctrl.sq_port   = QRTR_PORT_CTRL;

    ssize_t n = sendto(g_sock, &pkt, sizeof(pkt), 0,
                       (struct sockaddr *)&ctrl, sizeof(ctrl));
    if (n < 0) { save_errno(); return QRTR_RC_SEND_FAIL; }
    return QRTR_RC_OK;
}

/* Milliseconds since CLOCK_MONOTONIC epoch. */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int qrtr_lookup(uint32_t service, uint32_t instance, int timeout_ms,
                uint32_t *out_node, uint32_t *out_port) {
    if (g_sock < 0) return QRTR_RC_NOT_OPEN;

    LOGI("qrtr_lookup svc=0x%02X inst=%u timeout=%d", service, instance, timeout_ms);
    int rc = send_new_lookup(service, instance);
    if (rc < 0) { LOGE("qrtr_lookup send_new_lookup rc=%d errno=%d", rc, errno); return rc; }

    /* Listen for NEW_SERVER broadcasts. HyperOS qrtr-ns does NOT honour
     * NEW_LOOKUP's service filter — it floods all servers. So we drain every
     * pending packet on each poll wakeup (MSG_DONTWAIT loop) and match
     * client-side. Real-time elapsed accounting so a fast flood doesn't
     * burn the budget on phantom poll_ms decrements. */
    struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
    long long t0 = now_ms();
    int got_packets = 0, non_match = 0;
    for (;;) {
        long long elapsed = now_ms() - t0;
        if (elapsed >= timeout_ms) break;
        int poll_ms = (int)(timeout_ms - elapsed);
        if (poll_ms > 500) poll_ms = 500;
        int pr = poll(&pfd, 1, poll_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            save_errno();
            return QRTR_RC_RECV_FAIL;
        }
        if (pr == 0) continue;

        /* Drain every packet the kernel has buffered for us in this wakeup. */
        for (;;) {
            uint8_t buf[128];
            struct sockaddr_qrtr_compat src = { 0 };
            socklen_t sl = sizeof(src);
            ssize_t n = recvfrom(g_sock, buf, sizeof(buf), MSG_DONTWAIT,
                                 (struct sockaddr *)&src, &sl);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                save_errno();
                return QRTR_RC_RECV_FAIL;
            }
            got_packets++;
            if ((size_t)n < sizeof(struct qrtr_ctrl_pkt)) continue;
            const struct qrtr_ctrl_pkt *p = (const struct qrtr_ctrl_pkt *)buf;
            if (src.sq_port != QRTR_PORT_CTRL) continue;
            if (p->cmd != QRTR_TYPE_NEW_SERVER) continue;
            if (p->service != service) { non_match++; continue; }
            if (instance != 0 && p->instance != instance) { non_match++; continue; }

            if (out_node) *out_node = p->node;
            if (out_port) *out_port = p->port;
            LOGI("qrtr_lookup HIT svc=0x%X inst=%u node=%u port=0x%X "
                 "(after %d pkts, %d non-match)",
                 p->service, p->instance, p->node, p->port,
                 got_packets, non_match);
            return QRTR_RC_OK;
        }
    }
    LOGE("qrtr_lookup timeout svc=0x%02X inst=%u (%d pkts drained, %d non-match)",
         service, instance, got_packets, non_match);
    return QRTR_RC_NO_SERVICE;
}

/* Drain any NEW_SERVER replies currently sitting on the socket into `out`.
 * Stops on timeout OR sentinel (service=0,node=0,port=0). Returns number of
 * entries seen (may exceed cap; cap only limits writes to `out`). */
static int drain_new_server(qrtr_service_t *out, int cap, int *count,
                            int timeout_ms) {
    struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int poll_ms = timeout_ms - elapsed;
        if (poll_ms > 150) poll_ms = 150;
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
        if (p->service == 0 && p->node == 0 && p->port == 0) break;

        /* De-dup against already-collected entries. */
        int dup = 0;
        for (int i = 0; i < *count && i < cap; i++) {
            if (out[i].service  == p->service  &&
                out[i].instance == p->instance &&
                out[i].node     == p->node     &&
                out[i].port     == p->port) { dup = 1; break; }
        }
        if (dup) continue;

        if (*count < cap) {
            out[*count].service  = p->service;
            out[*count].instance = p->instance;
            out[*count].node     = p->node;
            out[*count].port     = p->port;
        }
        (*count)++;
    }
    return QRTR_RC_OK;
}

/* Well-known QMI service IDs to probe when wildcard lookup under-reports
 * (e.g. on HyperOS where ns filters modem-side services from app UID). */
static const uint32_t PROBE_SERVICES[] = {
    0x01,  /* WDS  - wireless data */
    0x02,  /* DMS  - device management */
    0x03,  /* NAS  - network access (band pref lives here) */
    0x04,  /* QOS  */
    0x05,  /* WMS  - wireless messaging */
    0x06,  /* PDS  - position */
    0x07,  /* AUTH */
    0x09,  /* VOICE */
    0x0A,  /* CAT  - card app toolkit */
    0x0B,  /* UIM  */
    0x0C,  /* PBM  */
    0x10,  /* LOC  */
    0x11,  /* SAR  */
    0x1A,  /* IMSp */
    0x1E,  /* DSD  - data services dormant */
    0x24,  /* PDC  - persistent device configuration */
    0x42,  /* IMSP - IMS presence */
    0xE1,  /* RFSA - remote fs access (EFS over QRTR!) */
    0x190  /* seen in dmesg wakeup reason */
};
#define N_PROBE_SERVICES (int)(sizeof(PROBE_SERVICES)/sizeof(PROBE_SERVICES[0]))

int qrtr_enumerate(qrtr_service_t *out, int cap, int timeout_ms) {
    if (g_sock < 0) return QRTR_RC_NOT_OPEN;

    int count = 0;

    /* Pass 1: wildcard lookup (catches ns-forwarded services). */
    if (send_new_lookup(0, 0) == QRTR_RC_OK) {
        int r = drain_new_server(out, cap, &count, timeout_ms / 3);
        if (r < 0) return r;
    }

    /* Pass 2: probe specific well-known service IDs. Some kernels (HyperOS)
     * only answer pointed lookups, not wildcards, especially for modem
     * services. We share the remaining budget across all probes. */
    int per_probe = timeout_ms / (N_PROBE_SERVICES + 2);
    if (per_probe < 40) per_probe = 40;
    for (int i = 0; i < N_PROBE_SERVICES; i++) {
        if (send_new_lookup(PROBE_SERVICES[i], 0) != QRTR_RC_OK) continue;
        int r = drain_new_server(out, cap, &count, per_probe);
        if (r < 0) return r;
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
