/* SPDX-License-Identifier: Apache-2.0
 * QMUX transport over AF_UNIX /dev/socket/qmux_radio.
 *
 * qmuxd framing (wire-compatible with libqmi / uqmi):
 *
 *   +--------+--------+--------+--------+--------+--------+---------//
 *   |  IFC   |    total_len    | flags  |  svc   |   cid  |  QMI SDU ...
 *   | (0x01) |      LE16       | (0x00) |        |        |
 *   +--------+--------+--------+--------+--------+--------+---------//
 *
 *  total_len = bytes after the IFC byte, including total_len itself
 *            = 1 + 2(len) + 1(flags) + 1(svc) + 1(cid) + sdu_len - 1
 *            = 5 + sdu_len   (total_len EXCLUDES the 0x01 byte)
 *
 * QMI SDU (CTL variant — 1-byte txn, used only on QMUX_SVC_CTL):
 *   [ctl_flags][txn_id:1][msg_id:LE16][tlv_len:LE16][TLVs]
 *
 * QMI SDU (service variant — 2-byte txn, everyone else):
 *   [ctl_flags][txn_id:LE16][msg_id:LE16][tlv_len:LE16][TLVs]
 */

#include "qmux.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <android/log.h>

#define LOG_TAG "qmux"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

/* ---------- module-local state ---------- */
static int  g_sock = -1;
static int  g_last_errno = 0;
static char g_sockpath[128] = {0};
static uint8_t  g_ctl_txn = 1;   /* CTL uses 1-byte txn ids */
static uint16_t g_svc_txn = 1;   /* service SDUs use 2-byte txn ids */

static void save_errno(void) { g_last_errno = errno; }

int qmux_last_errno(void) { return g_last_errno; }
int qmux_is_open(void)    { return g_sock >= 0; }
const char *qmux_last_sockpath(void) { return g_sockpath; }

/* ---------- socket open/close ---------- */

/* Android AIDs used by the Qualcomm modem stack. qmuxd sets SO_PEERCRED on
 * the listening socket and only accepts clients whose effective uid matches
 * radio or system. Magisk root (uid=0) is rejected with a silent close(),
 * which surfaces to us as EPIPE on the first write(). Drop euid/egid to
 * AID_RADIO around connect() so the peer-cred check passes, then restore. */
#define AID_SYSTEM 1000
#define AID_RADIO  1001

/* Best-effort drop: runs if we are root. If already non-root (e.g. running
 * inside com.qdiag.bandlock itself) we just leave credentials alone; the
 * caller will see the real error from connect/write. */
static void qmux_drop_to_radio(uid_t *saved_euid, gid_t *saved_egid, int *dropped) {
    *saved_euid = geteuid();
    *saved_egid = getegid();
    *dropped = 0;
    if (*saved_euid != 0) return;  /* not root; nothing to do */

    /* Keep CAP_* across euid transition so we can seteuid(0) back. */
    prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);

    gid_t groups[] = { AID_RADIO, AID_SYSTEM };
    if (setgroups(2, groups) < 0) {
        LOGE("setgroups failed: %d (%s)", errno, strerror(errno));
    }
    if (setegid(AID_RADIO) < 0) {
        LOGE("setegid(radio) failed: %d (%s)", errno, strerror(errno));
        prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
        return;
    }
    if (seteuid(AID_RADIO) < 0) {
        LOGE("seteuid(radio) failed: %d (%s)", errno, strerror(errno));
        setegid(*saved_egid);
        prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
        return;
    }
    *dropped = 1;
    LOGI("qmux: euid/egid dropped to radio (uid=%d gid=%d)", geteuid(), getegid());
}

static void qmux_restore_euid(uid_t saved_euid, gid_t saved_egid, int dropped) {
    if (!dropped) return;
    if (seteuid(saved_euid) < 0) LOGE("seteuid restore failed: %d (%s)", errno, strerror(errno));
    if (setegid(saved_egid) < 0) LOGE("setegid restore failed: %d (%s)", errno, strerror(errno));
    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
}

static int try_connect(const char *path, int abstract) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { save_errno(); return -1; }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    socklen_t sl;
    if (abstract) {
        sa.sun_path[0] = 0;
        strncpy(sa.sun_path + 1, path, sizeof(sa.sun_path) - 2);
        sl = offsetof(struct sockaddr_un, sun_path) + 1 + (socklen_t)strlen(path);
    } else {
        strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
        sl = sizeof(sa);
    }

    uid_t saved_euid = 0; gid_t saved_egid = 0; int dropped = 0;
    qmux_drop_to_radio(&saved_euid, &saved_egid, &dropped);

    int crc = connect(fd, (struct sockaddr *)&sa, sl);
    int cerr = errno;

    qmux_restore_euid(saved_euid, saved_egid, dropped);

    if (crc < 0) {
        errno = cerr;
        save_errno();
        LOGE("qmux connect('%s' abstract=%d) failed: %d (%s)",
             path, abstract, cerr, strerror(cerr));
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

int qmux_open(const char *sock_path) {
    if (g_sock >= 0) return QMUX_RC_OK;

    /* HyperOS lays out qmuxd sockets as a directory of per-purpose endpoints:
     *   /dev/socket/qmux_radio/       <-- directory
     *       ril_ipc                   <-- qmuxd QMI multiplexer (what we want)
     * Older ROMs put the socket directly at /dev/socket/qmux_radio. Try the
     * directory-first variant, then the legacy path. */
    const char *candidates[] = {
        sock_path ? sock_path : "/dev/socket/qmux_radio/ril_ipc",
        "/dev/socket/qmux_radio",
        "/dev/socket/qmuxd",
        NULL
    };

    int fd = -1;
    const char *chosen = NULL;
    for (int i = 0; candidates[i]; i++) {
        fd = try_connect(candidates[i], 0);
        if (fd >= 0) { chosen = candidates[i]; break; }
        fd = try_connect(candidates[i], 1);
        if (fd >= 0) { chosen = candidates[i]; break; }
    }
    if (fd < 0) return QMUX_RC_CONNECT_FAIL;

    g_sock = fd;
    strncpy(g_sockpath, chosen ? chosen : "", sizeof(g_sockpath) - 1);
    g_ctl_txn = 1;
    g_svc_txn = 1;
    LOGI("qmux_open %s fd=%d", chosen ? chosen : "?", fd);
    return QMUX_RC_OK;
}

void qmux_close(void) {
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
}

/* ---------- low-level frame I/O ---------- */

/* Write exactly `len` bytes, handling EINTR/short writes. */
static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            save_errno();
            LOGE("qmux write_all: poll failed errno=%d (%s) after %zu/%zu",
                 errno, strerror(errno), sent, len);
            return -1;
        }
        if (pr == 0) {
            g_last_errno = ETIMEDOUT;
            LOGE("qmux write_all: poll timeout after %zu/%zu (revents=0x%x)",
                 sent, len, pfd.revents);
            return -1;
        }
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            save_errno();
            LOGE("qmux write_all: write failed errno=%d (%s) after %zu/%zu",
                 errno, strerror(errno), sent, len);
            return -1;
        }
        if (n == 0) {
            g_last_errno = EPIPE;
            LOGE("qmux write_all: EOF after %zu/%zu (peer closed; likely qmuxd SO_PEERCRED rejected us)",
                 sent, len);
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* Read exactly `len` bytes, up to deadline. */
static int read_exact(int fd, void *buf, size_t len, int timeout_ms) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    int elapsed = 0;
    while (got < len) {
        int slice = timeout_ms - elapsed;
        if (slice <= 0) { g_last_errno = ETIMEDOUT; return -1; }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, slice);
        if (pr < 0) { if (errno == EINTR) continue; save_errno(); return -1; }
        if (pr == 0) { g_last_errno = ETIMEDOUT; return -1; }
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            save_errno(); return -1;
        }
        if (n == 0) { g_last_errno = EPIPE; return -1; }
        got += (size_t)n;
        elapsed += 10;  /* conservative */
    }
    return 0;
}

/* Build the 6-byte qmux header into `hdr`. sdu_len is the QMI payload length. */
static void qmux_build_hdr(uint8_t hdr[6], uint8_t svc, uint8_t cid, size_t sdu_len) {
    uint16_t total_len = (uint16_t)(5 + sdu_len);  /* total_len excludes IFC */
    hdr[0] = 0x01;                                  /* IFC */
    hdr[1] = (uint8_t)(total_len & 0xFF);
    hdr[2] = (uint8_t)((total_len >> 8) & 0xFF);
    hdr[3] = 0x00;                                  /* flags */
    hdr[4] = svc;
    hdr[5] = cid;
}

/* Send a full qmux frame (header + sdu). */
static int qmux_send_frame(uint8_t svc, uint8_t cid,
                           const uint8_t *sdu, size_t sdu_len) {
    if (g_sock < 0) return QMUX_RC_NOT_OPEN;
    uint8_t hdr[6];
    qmux_build_hdr(hdr, svc, cid, sdu_len);
    if (write_all(g_sock, hdr, sizeof(hdr)) < 0) return QMUX_RC_SEND_FAIL;
    if (sdu_len && write_all(g_sock, sdu, sdu_len) < 0) return QMUX_RC_SEND_FAIL;
    return QMUX_RC_OK;
}

/* Dump up to 64 bytes as hex to logcat under the `qmux` tag. */
static void hexdump(const char *prefix, const uint8_t *buf, size_t len) {
    char line[3 * 64 + 1];
    size_t n = len > 64 ? 64 : len;
    for (size_t i = 0; i < n; i++) {
        snprintf(line + i * 3, 4, "%02X ", buf[i]);
    }
    line[n * 3] = '\0';
    LOGI("%s [%zu bytes] %s%s", prefix, len, line, len > 64 ? "..." : "");
}

/* Receive one qmux frame. On success, copies SDU into `out` and returns
 * (svc, cid, sdu_len) via out-params. On protocol error we hexdump what we
 * got so the user can decode qmuxd's actual wire format. */
static int qmux_recv_frame(uint8_t *out_svc, uint8_t *out_cid,
                           uint8_t *out, size_t out_cap, size_t *out_len,
                           int timeout_ms) {
    if (g_sock < 0) return QMUX_RC_NOT_OPEN;
    uint8_t hdr[6];
    if (read_exact(g_sock, hdr, 6, timeout_ms) < 0) return QMUX_RC_RECV_FAIL;
    if (hdr[0] != 0x01) {
        LOGE("qmux bad IFC 0x%02X (expected 0x01)", hdr[0]);
        hexdump("qmux bad-ifc hdr", hdr, 6);
        /* Drain whatever else is pending so we can hexdump it and then
         * close the socket (the peer is speaking a framing we don't
         * recognise — subsequent requests on this fd are doomed). */
        uint8_t drain[128];
        struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
        if (poll(&pfd, 1, 50) > 0) {
            ssize_t n = read(g_sock, drain, sizeof(drain));
            if (n > 0) hexdump("qmux bad-ifc drain", drain, (size_t)n);
        }
        return QMUX_RC_PROTO;
    }
    uint16_t total = (uint16_t)(hdr[1] | (hdr[2] << 8));
    if (total < 5) { LOGE("qmux total_len=%u < 5", total); return QMUX_RC_PROTO; }
    size_t sdu_len = (size_t)total - 5;
    if (sdu_len > out_cap) return QMUX_RC_BUF_OVERFLOW;
    if (sdu_len && read_exact(g_sock, out, sdu_len, timeout_ms) < 0)
        return QMUX_RC_RECV_FAIL;
    hexdump("qmux rx hdr", hdr, 6);
    if (sdu_len) hexdump("qmux rx sdu", out, sdu_len);
    *out_svc = hdr[4];
    *out_cid = hdr[5];
    *out_len = sdu_len;
    return QMUX_RC_OK;
}

/* ---------- QMI SDU builders ---------- */

static size_t put_u16_le(uint8_t *p, size_t o, uint16_t v) {
    p[o] = (uint8_t)(v & 0xFF); p[o + 1] = (uint8_t)((v >> 8) & 0xFF); return o + 2;
}

/* CTL SDU: 1-byte txn. */
static size_t build_ctl_sdu(uint8_t *out, size_t cap,
                            uint8_t txn, uint16_t msg_id,
                            const uint8_t *tlvs, size_t tlvs_len) {
    size_t need = 1 + 1 + 2 + 2 + tlvs_len;
    if (need > cap) return 0;
    size_t o = 0;
    out[o++] = 0x00;          /* ctl flags: request */
    out[o++] = txn;
    o = put_u16_le(out, o, msg_id);
    o = put_u16_le(out, o, (uint16_t)tlvs_len);
    if (tlvs_len) { memcpy(out + o, tlvs, tlvs_len); o += tlvs_len; }
    return o;
}

/* Service SDU: 2-byte txn. Exposed for callers. */
size_t qmux_build_qmi_sdu(uint8_t *out, size_t cap,
                          uint16_t msg_id,
                          const uint8_t *tlvs, size_t tlvs_len) {
    size_t need = 1 + 2 + 2 + 2 + tlvs_len;
    if (need > cap) return 0;
    size_t o = 0;
    out[o++] = 0x00;          /* ctl flags: request */
    uint16_t txn = g_svc_txn++;
    o = put_u16_le(out, o, txn);
    o = put_u16_le(out, o, msg_id);
    o = put_u16_le(out, o, (uint16_t)tlvs_len);
    if (tlvs_len) { memcpy(out + o, tlvs, tlvs_len); o += tlvs_len; }
    return o;
}

int qmux_parse_qmi_response(const uint8_t *buf, size_t len,
                            uint16_t *out_msg_id,
                            uint16_t *out_result,
                            uint16_t *out_error) {
    if (len < 7) return QMUX_RC_PROTO;
    uint16_t msg_id  = (uint16_t)(buf[3] | (buf[4] << 8));
    uint16_t tlv_len = (uint16_t)(buf[5] | (buf[6] << 8));
    if (7 + tlv_len > len) return QMUX_RC_PROTO;
    if (out_msg_id) *out_msg_id = msg_id;
    /* Walk TLVs, find 0x02 (result). */
    size_t off = 7;
    size_t end = 7 + tlv_len;
    while (off + 3 <= end) {
        uint8_t  t = buf[off];
        uint16_t l = (uint16_t)(buf[off + 1] | (buf[off + 2] << 8));
        if (off + 3 + l > end) break;
        if (t == 0x02 && l >= 4) {
            if (out_result) *out_result = (uint16_t)(buf[off + 3] | (buf[off + 4] << 8));
            if (out_error)  *out_error  = (uint16_t)(buf[off + 5] | (buf[off + 6] << 8));
            return QMUX_RC_OK;
        }
        off += 3 + l;
    }
    return QMUX_RC_BAD_RESPONSE;
}

/* ---------- CTL: allocate / release client id ---------- */

/* GetClientId: msg 0x0022, request TLV 0x01 = [service_id:1].
 * Response TLV 0x01 = [service_id:1][client_id:1]. */
int qmux_alloc_client(uint8_t service_id, uint8_t *out_cid, int timeout_ms) {
    if (g_sock < 0) return QMUX_RC_NOT_OPEN;

    uint8_t tlv[4] = { 0x01, 0x01, 0x00, service_id };   /* T=01,L=0001,V=svc */
    uint8_t sdu[32];
    uint8_t txn = g_ctl_txn++;
    size_t sdu_len = build_ctl_sdu(sdu, sizeof(sdu), txn, 0x0022, tlv, 4);
    if (!sdu_len) return QMUX_RC_BUF_OVERFLOW;

    int rc = qmux_send_frame(QMUX_SVC_CTL, 0, sdu, sdu_len);
    if (rc < 0) { if (rc == QMUX_RC_PROTO || rc == QMUX_RC_SEND_FAIL) qmux_close(); return rc; }

    /* Receive until we see a CTL reply matching our txn id. */
    uint8_t rx[256];
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        uint8_t rsvc = 0, rcid = 0;
        size_t rlen = 0;
        int rr = qmux_recv_frame(&rsvc, &rcid, rx, sizeof(rx), &rlen,
                                 timeout_ms - elapsed);
        if (rr < 0) {
            if (rr == QMUX_RC_PROTO || rr == QMUX_RC_RECV_FAIL) qmux_close();
            return rr;
        }
        elapsed += 50;
        if (rsvc != QMUX_SVC_CTL || rlen < 7) continue;
        /* CTL SDU: [flags][txn:1][msg:LE16][tlv_len:LE16][TLVs] */
        uint8_t rtxn = rx[1];
        if (rtxn != txn) continue;
        uint16_t rmsg = (uint16_t)(rx[2] | (rx[3] << 8));
        if (rmsg != 0x0022) continue;
        uint16_t tlv_len = (uint16_t)(rx[4] | (rx[5] << 8));
        if (6 + tlv_len > rlen) return QMUX_RC_PROTO;
        /* Check result TLV 0x02 first. */
        size_t off = 6;
        uint16_t result = 0xFFFF, err = 0xFFFF;
        uint8_t  assigned_svc = 0, assigned_cid = 0;
        int have_id = 0;
        while (off + 3 <= 6 + tlv_len) {
            uint8_t t = rx[off];
            uint16_t l = (uint16_t)(rx[off + 1] | (rx[off + 2] << 8));
            if (off + 3 + l > 6 + tlv_len) break;
            if (t == 0x02 && l >= 4) {
                result = (uint16_t)(rx[off + 3] | (rx[off + 4] << 8));
                err    = (uint16_t)(rx[off + 5] | (rx[off + 6] << 8));
            } else if (t == 0x01 && l >= 2) {
                assigned_svc = rx[off + 3];
                assigned_cid = rx[off + 4];
                have_id = 1;
            }
            off += 3 + l;
        }
        if (result != 0 || !have_id) {
            LOGE("CTL GetClientId result=0x%04X err=0x%04X svc=%02X", result, err, service_id);
            return QMUX_RC_CLIENT_ALLOC;
        }
        (void)assigned_svc;
        if (out_cid) *out_cid = assigned_cid;
        return QMUX_RC_OK;
    }
    return QMUX_RC_TIMEOUT;
}

/* ReleaseClientId: msg 0x0023, TLV 0x01 = [svc:1][cid:1]. Fire and forget. */
void qmux_release_client(uint8_t service_id, uint8_t client_id) {
    if (g_sock < 0) return;
    uint8_t tlv[5] = { 0x01, 0x02, 0x00, service_id, client_id };
    uint8_t sdu[32];
    size_t sdu_len = build_ctl_sdu(sdu, sizeof(sdu), g_ctl_txn++, 0x0023, tlv, 5);
    if (!sdu_len) return;
    (void)qmux_send_frame(QMUX_SVC_CTL, 0, sdu, sdu_len);
    /* Don't wait for reply. */
}

/* ---------- service transaction ---------- */

int qmux_transact(uint8_t service_id, uint8_t client_id,
                  const uint8_t *sdu, size_t sdu_len,
                  uint8_t *rx, size_t rx_cap, int timeout_ms) {
    if (g_sock < 0) return QMUX_RC_NOT_OPEN;
    if (sdu_len < 3) return QMUX_RC_PROTO;

    /* Extract expected txn id from SDU (bytes 1..2 LE). */
    uint16_t expect_txn = (uint16_t)(sdu[1] | (sdu[2] << 8));

    int rc = qmux_send_frame(service_id, client_id, sdu, sdu_len);
    if (rc < 0) { if (rc == QMUX_RC_PROTO || rc == QMUX_RC_SEND_FAIL) qmux_close(); return rc; }

    int elapsed = 0;
    while (elapsed < timeout_ms) {
        uint8_t rsvc = 0, rcid = 0;
        size_t rlen = 0;
        int rr = qmux_recv_frame(&rsvc, &rcid, rx, rx_cap, &rlen,
                                 timeout_ms - elapsed);
        if (rr < 0) {
            if (rr == QMUX_RC_PROTO || rr == QMUX_RC_RECV_FAIL) qmux_close();
            return rr;
        }
        elapsed += 50;
        if (rsvc != service_id) continue;
        if (rcid && rcid != client_id) continue;    /* broadcasts have cid=0 */
        if (rlen < 5) continue;
        uint16_t rtxn = (uint16_t)(rx[1] | (rx[2] << 8));
        if (rtxn != expect_txn) continue;          /* async indications */
        return (int)rlen;
    }
    return QMUX_RC_TIMEOUT;
}
