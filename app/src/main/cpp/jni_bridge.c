#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diag.h"
#include "efs2.h"
#include "hdlc.h"
#include "qmi_nas.h"
#include "qmux.h"
#include "qrtr.h"
#include "sniffer.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define LOG_TAG "qdiag-jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * Non-QMI return codes surfaced to Kotlin. Kept well clear of 0x0000..0xFFFF
 * (QMI errors, |= 0x10000) and of -errno values.
 */
#define QDIAG_RC_NOT_OPEN       -1001  /* /dev/diag is not open at all */
#define QDIAG_RC_WRITE_FAIL     -1002  /* write() failed; see errno field */
#define QDIAG_RC_READ_FAIL      -1003  /* read() failed; see errno field */
#define QDIAG_RC_TIMEOUT        -1004  /* modem did not answer in time */
#define QDIAG_RC_BAD_FRAME      -1005  /* response arrived but couldn't be decoded */
#define QDIAG_RC_BUILD_FAIL     -1006  /* qmi_nas_build_* returned 0 */

/*
 * Helper: HDLC-encode 'req' (length reqLen), write to /dev/diag, then read
 * up to 'timeout_ms' milliseconds for a response. HDLC-decode the first
 * complete frame and return a fresh jbyteArray with the decoded payload,
 * or NULL on error. '*err' gets one of QDIAG_RC_* on failure, or the
 * errno value (positive) when write/read failed.
 */
static jbyteArray send_and_recv(JNIEnv *env, const uint8_t *req, size_t reqLen,
                                int timeout_ms, int *err, int *err_errno) {
    *err = 0; *err_errno = 0;
    if (!diag_is_open()) { *err = QDIAG_RC_NOT_OPEN; return NULL; }

    uint8_t encoded[2048];
    size_t  encLen = hdlc_encode(req, reqLen, encoded);

    ssize_t w = diag_write_raw(encoded, encLen);
    if (w < 0) {
        LOGE("diag write failed (%zd), errno=%d (%s)", w, errno, strerror(errno));
        *err = QDIAG_RC_WRITE_FAIL;
        *err_errno = (int)(-w); /* diag_write_raw returns -errno */
        return NULL;
    }

    uint8_t rx[8192];
    size_t  rxHave = 0;
    int elapsed = 0;
    while (elapsed < timeout_ms && rxHave < sizeof(rx)) {
        ssize_t n = diag_read(rx + rxHave, sizeof(rx) - rxHave);
        if (n < 0) {
            LOGE("diag read failed: errno=%d (%s)", errno, strerror(errno));
            *err = QDIAG_RC_READ_FAIL;
            *err_errno = (int)(-n);
            return NULL;
        }
        if (n == 0) { elapsed += 200; continue; }
        rxHave += (size_t)n;
        for (size_t i = rxHave - (size_t)n; i < rxHave; i++) {
            if (rx[i] == 0x7E) {
                uint8_t decoded[2048];
                size_t dec = hdlc_decode(rx, i + 1, decoded, sizeof(decoded));
                if (dec == 0) continue;
                jbyteArray out = (*env)->NewByteArray(env, (jsize)dec);
                if (!out) { *err = QDIAG_RC_BAD_FRAME; return NULL; }
                (*env)->SetByteArrayRegion(env, out, 0, (jsize)dec, (const jbyte *)decoded);
                return out;
            }
        }
    }
    LOGE("send_and_recv: timeout after %d ms", timeout_ms);
    *err = QDIAG_RC_TIMEOUT;
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_openDiag(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return (diag_open() == 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_openDiagEx(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    /* Returns 0 on success or -errno on failure. */
    return (jint)diag_open();
}

JNIEXPORT void JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_closeDiag(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    diag_close();
}

JNIEXPORT jboolean JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_isDiagOpen(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return diag_is_open() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_sendRaw(JNIEnv *env, jclass clz, jbyteArray req) {
    (void)clz;
    jsize len = (*env)->GetArrayLength(env, req);
    jbyte *buf = (*env)->GetByteArrayElements(env, req, NULL);
    if (!buf) return NULL;
    int e1 = 0, e2 = 0;
    jbyteArray out = send_and_recv(env, (const uint8_t *)buf, (size_t)len, 2000, &e1, &e2);
    (*env)->ReleaseByteArrayElements(env, req, buf, JNI_ABORT);
    return out;
}

static jint run_qmi(JNIEnv *env, const uint8_t *req, size_t reqLen) {
    int e1 = 0, e2 = 0;
    jbyteArray ja = send_and_recv(env, req, reqLen, 2000, &e1, &e2);
    if (!ja) {
        if (e1 == QDIAG_RC_WRITE_FAIL || e1 == QDIAG_RC_READ_FAIL) {
            /* Fold errno into low byte so Kotlin can decode it. */
            return (jint)(e1 - e2);
        }
        return (jint)e1;
    }
    jsize jl = (*env)->GetArrayLength(env, ja);
    jbyte *p = (*env)->GetByteArrayElements(env, ja, NULL);
    uint16_t err = 0;
    int rc = qmi_parse_response((const uint8_t *)p, (size_t)jl, &err);
    (*env)->ReleaseByteArrayElements(env, ja, p, JNI_ABORT);
    if (rc < 0) return (jint)(-2000 + rc); /* parse error */
    if (rc != 0) {
        LOGE("QMI result=%d error=0x%04x", rc, err);
        return (jint)(0x10000 | err);
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_setBandPref(JNIEnv *env, jclass clz,
                                                    jlong lteLow, jlong lteHigh,
                                                    jlong nrLow,  jlong nrHigh) {
    (void)clz;
    uint8_t req[QMI_MAX_REQUEST];
    size_t  len = qmi_nas_build_set_band_pref(req,
        (uint64_t)lteLow, (uint64_t)lteHigh,
        (uint64_t)nrLow,  (uint64_t)nrHigh);
    if (!len) return QDIAG_RC_BUILD_FAIL;
    return run_qmi(env, req, len);
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_resetBandPref(JNIEnv *env, jclass clz) {
    (void)clz;
    uint8_t req[QMI_MAX_REQUEST];
    size_t  len = qmi_nas_build_reset_band_pref(req);
    if (!len) return QDIAG_RC_BUILD_FAIL;
    return run_qmi(env, req, len);
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_setLteCellLock(JNIEnv *env, jclass clz,
                                                       jint earfcn, jint pci) {
    (void)clz;
    uint8_t req[QMI_MAX_REQUEST];
    size_t  len = qmi_nas_build_lte_cell_lock(req, (uint32_t)earfcn, (uint32_t)pci);
    if (!len) return QDIAG_RC_BUILD_FAIL;
    return run_qmi(env, req, len);
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_clearLteCellLock(JNIEnv *env, jclass clz) {
    (void)clz;
    uint8_t req[QMI_MAX_REQUEST];
    size_t  len = qmi_nas_build_lte_cell_unlock(req);
    if (!len) return QDIAG_RC_BUILD_FAIL;
    return run_qmi(env, req, len);
}

/* ---------------- EFS2 NV writes (NSG-style lock) ---------------------- */

/**
 * Write a raw value into an EFS NV item via DIAG EFS2 Put Item File.
 * Returns 0 on success, -errno or QDIAG_RC_* on transport failure, or
 * diag_errno encoded as 0x20000|errno when the modem rejects the write.
 */
JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_efsPutItemFile(JNIEnv *env, jclass clz,
                                                       jstring jpath, jbyteArray jval) {
    (void)clz;
    if (!diag_is_open()) return QDIAG_RC_NOT_OPEN;
    if (!jpath || !jval) return QDIAG_RC_BUILD_FAIL;

    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return QDIAG_RC_BUILD_FAIL;
    jsize vlen = (*env)->GetArrayLength(env, jval);
    jbyte *vbuf = (*env)->GetByteArrayElements(env, jval, NULL);
    if (!vbuf) {
        (*env)->ReleaseStringUTFChars(env, jpath, path);
        return QDIAG_RC_BUILD_FAIL;
    }

    uint8_t req[4096];
    size_t  reqLen = efs2_build_put_item_file(req, sizeof(req), path,
                                              (const uint8_t *)vbuf, (size_t)vlen);
    (*env)->ReleaseByteArrayElements(env, jval, vbuf, JNI_ABORT);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (!reqLen) return QDIAG_RC_BUILD_FAIL;

    int e1 = 0, e2 = 0;
    jbyteArray ja = send_and_recv(env, req, reqLen, 2000, &e1, &e2);
    if (!ja) {
        if (e1 == QDIAG_RC_WRITE_FAIL || e1 == QDIAG_RC_READ_FAIL) return (jint)(e1 - e2);
        return (jint)e1;
    }
    jsize jl = (*env)->GetArrayLength(env, ja);
    jbyte *p = (*env)->GetByteArrayElements(env, ja, NULL);
    uint16_t op = 0; int32_t derr = 0;
    int prc = efs2_parse_response((const uint8_t *)p, (size_t)jl, &op, &derr);
    (*env)->ReleaseByteArrayElements(env, ja, p, JNI_ABORT);
    if (prc < 0)     return QDIAG_RC_BAD_FRAME;
    if (derr != 0) {
        LOGE("EFS2 Put Item File diag_errno=%d op=0x%04x", derr, op);
        return (jint)(0x20000 | (derr & 0xFFFF));
    }
    return 0;
}

/**
 * EFS2 Get Item File — read NV item content.
 * Returns null on error, otherwise a byte[] with the item contents.
 */
JNIEXPORT jbyteArray JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_efsGetItemFile(JNIEnv *env, jclass clz,
                                                       jstring jpath) {
    (void)clz;
    if (!diag_is_open() || !jpath) return NULL;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return NULL;

    uint8_t req[512];
    size_t  reqLen = efs2_build_get_item_file(req, sizeof(req), path);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (!reqLen) return NULL;

    int e1 = 0, e2 = 0;
    jbyteArray ja = send_and_recv(env, req, reqLen, 2000, &e1, &e2);
    if (!ja) return NULL;
    jsize jl = (*env)->GetArrayLength(env, ja);
    jbyte *p = (*env)->GetByteArrayElements(env, ja, NULL);

    uint8_t data[4096];
    int n = efs2_parse_get_item_response((const uint8_t *)p, (size_t)jl, data, sizeof(data));
    (*env)->ReleaseByteArrayElements(env, ja, p, JNI_ABORT);
    if (n < 0) return NULL;
    jbyteArray out = (*env)->NewByteArray(env, (jsize)n);
    if (!out) return NULL;
    if (n) (*env)->SetByteArrayRegion(env, out, 0, (jsize)n, (const jbyte *)data);
    return out;
}

/**
 * EFS2 Unlink — delete an NV item. Used to clear a lock.
 * Returns 0 on success, negative on transport failure, or
 * (0x20000 | diag_errno) if the modem rejected.
 */
JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_efsUnlink(JNIEnv *env, jclass clz,
                                                  jstring jpath) {
    (void)clz;
    if (!diag_is_open()) return QDIAG_RC_NOT_OPEN;
    if (!jpath) return QDIAG_RC_BUILD_FAIL;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return QDIAG_RC_BUILD_FAIL;

    uint8_t req[512];
    size_t  reqLen = efs2_build_unlink(req, sizeof(req), path);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (!reqLen) return QDIAG_RC_BUILD_FAIL;

    int e1 = 0, e2 = 0;
    jbyteArray ja = send_and_recv(env, req, reqLen, 2000, &e1, &e2);
    if (!ja) {
        if (e1 == QDIAG_RC_WRITE_FAIL || e1 == QDIAG_RC_READ_FAIL) return (jint)(e1 - e2);
        return (jint)e1;
    }
    jsize jl = (*env)->GetArrayLength(env, ja);
    jbyte *p = (*env)->GetByteArrayElements(env, ja, NULL);
    uint16_t op = 0; int32_t derr = 0;
    int prc = efs2_parse_response((const uint8_t *)p, (size_t)jl, &op, &derr);
    (*env)->ReleaseByteArrayElements(env, ja, p, JNI_ABORT);
    if (prc < 0)    return QDIAG_RC_BAD_FRAME;
    if (derr != 0)  return (jint)(0x20000 | (derr & 0xFFFF));
    return 0;
}

JNIEXPORT jstring JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_drainLog(JNIEnv *env, jclass clz) {
    (void)clz;
    uint8_t buf[4096];
    char    hex[9000];
    size_t  hoff = 0;
    ssize_t n = diag_read(buf, sizeof(buf));
    if (n <= 0) return (*env)->NewStringUTF(env, "");
    for (ssize_t i = 0; i < n && hoff + 3 < sizeof(hex); i++) {
        static const char digits[] = "0123456789abcdef";
        hex[hoff++] = digits[(buf[i] >> 4) & 0xF];
        hex[hoff++] = digits[buf[i] & 0xF];
        hex[hoff++] = (i && (i % 32 == 31)) ? '\n' : ' ';
    }
    hex[hoff] = 0;
    return (*env)->NewStringUTF(env, hex);
}

/* ------------------------- DIAG sniffer ------------------------------- */

JNIEXPORT jboolean JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_snifferStart(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return sniffer_start() == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_snifferStop(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    sniffer_stop();
}

JNIEXPORT jboolean JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_snifferIsRunning(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return sniffer_is_running() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Returns a byte array containing one or more records of
 *   [u32 len_le][len bytes of HDLC-decoded frame]
 * concatenated together, up to ~64 KiB per drain. Returns an empty array
 * if nothing is pending.
 */
JNIEXPORT jbyteArray JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_snifferDrain(JNIEnv *env, jclass clz) {
    (void)clz;
    static uint8_t staging[65536];
    int nframes = 0;
    size_t got = sniffer_drain(staging, sizeof(staging), &nframes);
    jbyteArray out = (*env)->NewByteArray(env, (jsize)got);
    if (!out) return NULL;
    if (got) (*env)->SetByteArrayRegion(env, out, 0, (jsize)got, (const jbyte *)staging);
    return out;
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_snifferSaveTo(JNIEnv *env, jclass clz, jstring jpath) {
    (void)clz;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    int saved_errno = errno;
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    if (fd < 0) { LOGE("open %s failed: %d", path, saved_errno); return -saved_errno; }
    int n = sniffer_save_to_fd(fd);
    close(fd);
    return n;
}

JNIEXPORT jlong JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_snifferTotal(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return (jlong)sniffer_total_frames();
}

JNIEXPORT jlong JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_snifferDropped(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return (jlong)sniffer_dropped_frames();
}

/* ========================================================================== */
/* QRTR transport — AF_QIPCRTR socket; works without /dev/diag                */
/* ========================================================================== */

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qrtrOpen(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    int rc = qrtr_open();
    if (rc != 0) {
        LOGE("qrtr_open rc=%d errno=%d (%s)", rc, qrtr_last_errno(), strerror(qrtr_last_errno()));
        /* Encode errno into the rc so Kotlin can decode. */
        int e = qrtr_last_errno();
        if (e > 0) return -(2000 + e);
        return rc;
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qrtrClose(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    qrtr_close();
}

JNIEXPORT jboolean JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qrtrIsOpen(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return qrtr_is_open() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Return a newline-separated string of advertised QRTR services, e.g.:
 *   "svc=0x03 inst=0 node=0x05 port=0x1234"
 *   ...
 * Empty string on failure.
 */
JNIEXPORT jstring JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qrtrEnumerate(JNIEnv *env, jclass clz) {
    (void)clz;
    if (!qrtr_is_open()) return (*env)->NewStringUTF(env, "qrtr not open");

    qrtr_service_t svcs[128];
    int n = qrtr_enumerate(svcs, (int)(sizeof(svcs)/sizeof(svcs[0])), 4500);
    if (n < 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "enumerate rc=%d errno=%d", n, qrtr_last_errno());
        return (*env)->NewStringUTF(env, msg);
    }
    char buf[8192];
    size_t o = 0;
    o += snprintf(buf + o, sizeof(buf) - o, "%d services:\n", n);
    for (int i = 0; i < n && o < sizeof(buf) - 64; i++) {
        o += snprintf(buf + o, sizeof(buf) - o,
                      "svc=0x%02X inst=%u node=0x%02X port=0x%X\n",
                      svcs[i].service, svcs[i].instance,
                      svcs[i].node, svcs[i].port);
    }
    return (*env)->NewStringUTF(env, buf);
}

/**
 * High-level helper: set LTE+NR band preference via QRTR (QMI NAS
 * SET_SYSTEM_SELECTION_PREFERENCE = 0x0033). Returns 0 on success,
 * (0x10000 | qmi_err) on modem rejection, or negative QRTR_RC_*.
 */
JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qrtrSetBandPref(
        JNIEnv *env, jclass clz,
        jlong lteLow, jlong lteHigh, jlong nrLow, jlong nrHigh) {
    (void)env; (void)clz;
    /* Always cycle the socket before a new transaction: (1) re-applies the
     * radio-UID drop so the fresh struct sock gets the right creator creds,
     * (2) flushes any stale NEW_SERVER CTRL broadcasts left over from a
     * previous Enumerate that would otherwise be consumed in place of our
     * NEW_LOOKUP reply. */
    qrtr_close();
    int orc = qrtr_open();
    if (orc < 0) return orc;

    uint32_t node = 0, port = 0;
    /* Try instance=1 first (enumerate shows NAS at inst=1 on HyperOS), then
     * fall back to instance=0 (wildcard) if 1 misses. */
    int rc = qrtr_lookup(QMI_SVC_NAS, 1, 2000, &node, &port);
    if (rc == QRTR_RC_NO_SERVICE) {
        rc = qrtr_lookup(QMI_SVC_NAS, 0, 2000, &node, &port);
    }
    if (rc < 0) return rc;

    /* Empirically verified on Poco F6 / HyperOS / X70 modem:
     *   TLV 0x11  Mode Preference        u16 LE      (bits: LTE=0x10, 5GNR=0x40)
     *   TLV 0x15  LTE Band Preference    u64 LE      (legacy, bands 1..64)
     * Result SUCCESS/0 from the modem for this exact combination.
     * TLV 0x1C ("LTE ext", bands 1..128) was rejected MALFORMED by
     * this firmware, so we stick to 0x15 for LTE.
     *
     * NR5G TLV IDs for this firmware are still unknown — we probe a
     * handful of candidates below when the user also selected NR
     * bands, and log each reply so we can lock them down next. */

    int have_lte = (lteLow != 0);      /* 0x15 only covers low 64 bits */
    int have_nr  = (nrLow != 0) || (nrHigh != 0);

    uint16_t mode_pref = 0;
    if (have_lte) mode_pref |= 0x0010;  /* LTE  */
    if (have_nr)  mode_pref |= 0x0040;  /* 5GNR */
    if (!mode_pref) mode_pref = 0x0010; /* fallback */

    /* ---- Primary LTE request (known-good on X70 HyperOS) ---- */
    uint8_t tlvs[64];
    size_t to = 0;
    tlvs[to++] = 0x11; tlvs[to++] = 0x02; tlvs[to++] = 0x00;
    tlvs[to++] = (uint8_t)(mode_pref & 0xFF);
    tlvs[to++] = (uint8_t)((mode_pref >> 8) & 0xFF);
    if (have_lte) {
        tlvs[to++] = 0x15; tlvs[to++] = 0x08; tlvs[to++] = 0x00;
        for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((lteLow >> (8*i)) & 0xFF);
    }

    uint8_t req[256];
    size_t reqLen = qrtr_build_qmi_request(req, sizeof(req), 0x0033, tlvs, to);
    if (!reqLen) return QDIAG_RC_BUILD_FAIL;

    uint8_t rx[2048];
    int n = qrtr_transact(node, port, req, reqLen, rx, sizeof(rx), 5000);
    if (n < 0) return n;

    uint16_t mid = 0, result = 0xFFFF, err = 0xFFFF;
    int prc = qrtr_parse_qmi_response(rx, (size_t)n, &mid, &result, &err);
    if (prc < 0) return QDIAG_RC_BAD_FRAME;
    LOGI("LTE apply result=%u err=0x%04X", result, err);
    int primary_rc = (result == 0) ? 0 : (int)(0x10000 | err);

    /* ---- NR5G band preference (empirically verified on X70 HyperOS):
     *   TLV 0x40  NR5G SA Band Pref    u128 LE
     *   TLV 0x41  NR5G NSA Band Pref   u128 LE
     * Sent as a separate transaction so an NR-side rejection does not
     * undo the LTE change above. */
    if (have_nr) {
        uint8_t t[96]; size_t to2 = 0;
        t[to2++] = 0x11; t[to2++] = 0x02; t[to2++] = 0x00;
        t[to2++] = (uint8_t)(mode_pref & 0xFF);
        t[to2++] = (uint8_t)((mode_pref >> 8) & 0xFF);
        for (uint8_t id = 0x40; id <= 0x41; id++) {
            t[to2++] = id; t[to2++] = 0x10; t[to2++] = 0x00;
            for (int k = 0; k < 8; k++) t[to2++] = (uint8_t)((nrLow  >> (8*k)) & 0xFF);
            for (int k = 0; k < 8; k++) t[to2++] = (uint8_t)((nrHigh >> (8*k)) & 0xFF);
        }
        uint8_t rq[256];
        size_t rl = qrtr_build_qmi_request(rq, sizeof(rq), 0x0033, t, to2);
        uint8_t rxb[2048];
        int m = qrtr_transact(node, port, rq, rl, rxb, sizeof(rxb), 5000);
        if (m >= 0) {
            uint16_t nmid, nresult, nerr;
            if (qrtr_parse_qmi_response(rxb, (size_t)m, &nmid, &nresult, &nerr) >= 0) {
                LOGI("NR5G apply result=%u err=0x%04X", nresult, nerr);
                if (primary_rc == 0 && nresult != 0) primary_rc = (int)(0x10000 | nerr);
            }
        } else {
            LOGE("NR5G apply transact rc=%d", m);
        }
    }

    return primary_rc;
}

/* ========================================================================== */
/* QMUX transport — AF_UNIX /dev/socket/qmux_radio/ril_ipc                    */
/* Works on devices without /dev/diag AND with kernel-ns hiding modem QMI    */
/* services from untrusted sockets. qmuxd is the trusted path that qcrild   */
/* itself uses.                                                             */
/* ========================================================================== */

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qmuxOpen(JNIEnv *env, jclass clz, jstring path) {
    (void)clz;
    const char *cpath = NULL;
    if (path) cpath = (*env)->GetStringUTFChars(env, path, NULL);
    int rc = qmux_open(cpath);
    if (path && cpath) (*env)->ReleaseStringUTFChars(env, path, cpath);
    if (rc != QMUX_RC_OK) {
        LOGE("qmux_open rc=%d errno=%d (%s)", rc, qmux_last_errno(),
             strerror(qmux_last_errno()));
        int e = qmux_last_errno();
        if (e > 0) return -(3000 + e);
        return rc;
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qmuxClose(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    qmux_close();
}

JNIEXPORT jboolean JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qmuxIsOpen(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return qmux_is_open() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qmuxSockPath(JNIEnv *env, jclass clz) {
    (void)clz;
    return (*env)->NewStringUTF(env, qmux_last_sockpath());
}

/*
 * Allocate a QMI client ID on a given service. Returns the assigned client id
 * (0..255) on success, or a negative error code.
 */
JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qmuxAllocClient(JNIEnv *env, jclass clz,
                                                       jint service) {
    (void)env; (void)clz;
    uint8_t cid = 0;
    int rc = qmux_alloc_client((uint8_t)service, &cid, 3000);
    if (rc < 0) return rc;
    return (jint)cid;
}

/*
 * Apply LTE+NR band preference via QMI_NAS over qmuxd.
 * Returns 0 on success, (0x10000 | qmi_err) on modem rejection, or negative
 * QMUX_RC_*.
 */
JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qmuxSetBandPref(
        JNIEnv *env, jclass clz,
        jlong lteLow, jlong lteHigh, jlong nrLow, jlong nrHigh) {
    (void)env; (void)clz;
    if (!qmux_is_open()) return QMUX_RC_NOT_OPEN;

    uint8_t cid = 0;
    int rc = qmux_alloc_client(QMUX_SVC_NAS, &cid, 3000);
    if (rc < 0) { LOGE("qmux alloc NAS client rc=%d", rc); return rc; }

    /* Build TLVs — identical layout to DIAG/QRTR paths; the QMI SDU is
     * transport-agnostic. */
    uint8_t tlvs[128];
    size_t  to = 0;

    /* TLV 0x11 legacy band pref (8 bytes, bands 1..64) */
    tlvs[to++] = 0x11;
    tlvs[to++] = 0x08; tlvs[to++] = 0x00;
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((lteLow >> (8*i)) & 0xFF);

    /* TLV 0x12 mode pref (u16 LE = 0x00FF) */
    tlvs[to++] = 0x12;
    tlvs[to++] = 0x02; tlvs[to++] = 0x00;
    tlvs[to++] = 0xFF; tlvs[to++] = 0x00;

    /* TLV 0x1C LTE band pref ext (16 bytes, bands 1..128) */
    tlvs[to++] = 0x1C;
    tlvs[to++] = 0x10; tlvs[to++] = 0x00;
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((lteLow  >> (8*i)) & 0xFF);
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((lteHigh >> (8*i)) & 0xFF);

    /* TLV 0x24 NR5G SA band pref (16 bytes) */
    tlvs[to++] = 0x24;
    tlvs[to++] = 0x10; tlvs[to++] = 0x00;
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((nrLow  >> (8*i)) & 0xFF);
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((nrHigh >> (8*i)) & 0xFF);

    /* TLV 0x25 NR5G NSA band pref (16 bytes) */
    tlvs[to++] = 0x25;
    tlvs[to++] = 0x10; tlvs[to++] = 0x00;
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((nrLow  >> (8*i)) & 0xFF);
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((nrHigh >> (8*i)) & 0xFF);

    uint8_t sdu[256];
    size_t  sdu_len = qmux_build_qmi_sdu(sdu, sizeof(sdu),
                                         0x0033, /* SET_SYSTEM_SELECTION_PREFERENCE */
                                         tlvs, to);
    if (!sdu_len) { qmux_release_client(QMUX_SVC_NAS, cid); return QDIAG_RC_BUILD_FAIL; }

    uint8_t rx[2048];
    int n = qmux_transact(QMUX_SVC_NAS, cid, sdu, sdu_len, rx, sizeof(rx), 5000);
    qmux_release_client(QMUX_SVC_NAS, cid);
    if (n < 0) return n;

    uint16_t mid = 0, result = 0xFFFF, err = 0xFFFF;
    int prc = qmux_parse_qmi_response(rx, (size_t)n, &mid, &result, &err);
    if (prc < 0) return QDIAG_RC_BAD_FRAME;
    if (result != 0) return (jint)(0x10000 | err);
    return 0;
}

/* ========================================================================== */
/* QMI DMS WRITE_NV_ITEM (msg 0x003D) over QRTR — Cell Lock probe.           */
/*                                                                            */
/* The HyperOS kernel on Poco F6 has /dev/diag disabled (diagchar not         */
/* compiled in), so the legacy DIAG EFS path used by NSG to write NV item     */
/* 6828 (LTE cell lock) is unreachable. DMS (svc=0x02) is visible on QRTR,    */
/* though, and QMI_DMS_WRITE_NV_ITEM takes:                                   */
/*   TLV 0x01 mandatory: u16 item_id + variable bytes (item payload)          */
/*                                                                            */
/* The exact NV item ID for LTE cell lock on X70 is not documented. We probe */
/* a shortlist of known-to-work-on-older-Qualcomm IDs with several payload   */
/* sizes each, log every response, and stop at the first SUCCESS (result=0). */
/* The Kotlin side shows aggregate rc + logs hint the working combo.         */
/* ========================================================================== */

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qrtrProbeCellLock(
        JNIEnv *env, jclass clz,
        jint earfcn, jint pci) {
    (void)env; (void)clz;

    qrtr_close();
    int orc = qrtr_open();
    if (orc < 0) return orc;

    /* DMS service lookup. On HyperOS QRTR typically advertises DMS at inst=1
     * on the modem remote node; fall back to wildcard. */
    uint32_t node = 0, port = 0;
    int rc = qrtr_lookup(QMI_SVC_DMS, 1, 2000, &node, &port);
    if (rc == QRTR_RC_NO_SERVICE) {
        rc = qrtr_lookup(QMI_SVC_DMS, 0, 2000, &node, &port);
    }
    if (rc < 0) {
        LOGE("cell-probe DMS lookup rc=%d", rc);
        return rc;
    }
    LOGI("cell-probe DMS node=0x%X port=0x%X", node, port);

    uint16_t u16_earfcn = (uint16_t)(earfcn & 0xFFFF);
    uint16_t u16_pci    = (uint16_t)(pci    & 0xFFFF);

    /* Candidate {NV item id, payload layout} pairs. Payload layouts that
     * Qualcomm's legacy NV dictionary hinted at:
     *   6828 "lte_cell_lock_info" — historically {flag, earfcn, pci, pad}
     *   6829 "lte_cell_lock"      — {flag, earfcn, pci}
     *   466  "lte_nas_rel9_plus"  — sometimes carries cell-lock flags
     *   4964 "lte_nas_rrc_cell"   — per-Qualcomm internal layout
     *
     * For each ID we probe a few candidate layouts; total probes = ~12,
     * one QMI transaction each, ~5s timeout worst case. */
    struct probe_entry {
        uint16_t item_id;
        const char *name;
        /* layout kind: 0={flag u8, earfcn u16, pci u16, pad u8} (6B)
         *              1={flag u8, earfcn u16, pci u16}          (5B)
         *              2={earfcn u16, pci u16, flag u8}          (5B)
         *              3={earfcn u32, pci u32, flag u8}          (9B) */
        uint8_t kind;
    };
    static const struct probe_entry probes[] = {
        { 6828,  "6828-kind0",  0 },
        { 6828,  "6828-kind1",  1 },
        { 6828,  "6828-kind2",  2 },
        { 6828,  "6828-kind3",  3 },
        { 6829,  "6829-kind0",  0 },
        { 6829,  "6829-kind1",  1 },
        { 6829,  "6829-kind2",  2 },
        { 466,   "466-kind0",   0 },
        { 466,   "466-kind1",   1 },
        { 4964,  "4964-kind0",  0 },
        { 4964,  "4964-kind1",  1 },
        { 4964,  "4964-kind2",  2 },
    };
    const size_t nprobes = sizeof(probes) / sizeof(probes[0]);

    int first_success_rc = -1;
    for (size_t i = 0; i < nprobes; i++) {
        uint16_t item = probes[i].item_id;
        uint8_t  kind = probes[i].kind;

        /* Build TLV 0x01 body: [u16 item_id][item_payload...] */
        uint8_t body[32]; size_t bl = 0;
        body[bl++] = (uint8_t)(item & 0xFF);
        body[bl++] = (uint8_t)((item >> 8) & 0xFF);

        switch (kind) {
            case 0:
                body[bl++] = 0x01; /* flag = enable */
                body[bl++] = (uint8_t)(u16_earfcn & 0xFF);
                body[bl++] = (uint8_t)((u16_earfcn >> 8) & 0xFF);
                body[bl++] = (uint8_t)(u16_pci & 0xFF);
                body[bl++] = (uint8_t)((u16_pci >> 8) & 0xFF);
                body[bl++] = 0x00; /* pad */
                break;
            case 1:
                body[bl++] = 0x01;
                body[bl++] = (uint8_t)(u16_earfcn & 0xFF);
                body[bl++] = (uint8_t)((u16_earfcn >> 8) & 0xFF);
                body[bl++] = (uint8_t)(u16_pci & 0xFF);
                body[bl++] = (uint8_t)((u16_pci >> 8) & 0xFF);
                break;
            case 2:
                body[bl++] = (uint8_t)(u16_earfcn & 0xFF);
                body[bl++] = (uint8_t)((u16_earfcn >> 8) & 0xFF);
                body[bl++] = (uint8_t)(u16_pci & 0xFF);
                body[bl++] = (uint8_t)((u16_pci >> 8) & 0xFF);
                body[bl++] = 0x01;
                break;
            case 3:
                body[bl++] = (uint8_t)(earfcn & 0xFF);
                body[bl++] = (uint8_t)((earfcn >> 8) & 0xFF);
                body[bl++] = (uint8_t)((earfcn >> 16) & 0xFF);
                body[bl++] = (uint8_t)((earfcn >> 24) & 0xFF);
                body[bl++] = (uint8_t)(pci & 0xFF);
                body[bl++] = (uint8_t)((pci >> 8) & 0xFF);
                body[bl++] = (uint8_t)((pci >> 16) & 0xFF);
                body[bl++] = (uint8_t)((pci >> 24) & 0xFF);
                body[bl++] = 0x01;
                break;
        }

        /* Wrap as TLV 0x01 = mandatory NV item + data. */
        uint8_t tlvs[48]; size_t to = 0;
        tlvs[to++] = 0x01;
        tlvs[to++] = (uint8_t)(bl & 0xFF);
        tlvs[to++] = (uint8_t)((bl >> 8) & 0xFF);
        for (size_t k = 0; k < bl; k++) tlvs[to++] = body[k];

        uint8_t req[128];
        size_t reqLen = qrtr_build_qmi_request(req, sizeof(req),
                                               0x003D, /* WRITE_NV_ITEM */
                                               tlvs, to);
        if (!reqLen) continue;

        uint8_t rx[1024];
        int n = qrtr_transact(node, port, req, reqLen, rx, sizeof(rx), 3000);
        if (n < 0) {
            LOGE("cell-probe[%zu] %s transact rc=%d", i, probes[i].name, n);
            continue;
        }
        uint16_t mid = 0, result = 0xFFFF, err = 0xFFFF;
        int prc = qrtr_parse_qmi_response(rx, (size_t)n, &mid, &result, &err);
        if (prc < 0) {
            LOGE("cell-probe[%zu] %s bad-frame n=%d", i, probes[i].name, n);
            continue;
        }
        LOGI("cell-probe[%zu] %s result=%u err=0x%04X (mid=0x%04X)",
             i, probes[i].name, result, err, mid);
        if (result == 0 && first_success_rc < 0) {
            first_success_rc = 0;
            /* Keep probing so logs show which IDs the modem accepts. */
        }
    }

    if (first_success_rc == 0) return 0;
    /* All probes failed — return a synthetic "not supported" code that the
     * Kotlin decode maps to a friendly message. */
    return (jint)(0x10000 | 0x003E); /* QMI_ERR_NOT_SUPPORTED */
}

/* ========================================================================== */
/* QMUX reachability probe — NSG uses /dev/socket/qmux_radio for cell lock.   */
/* Tries every candidate path (with peer-cred drop to AID_RADIO), logs per-   */
/* path open/errno; on first successful open does CTL GET_VERSION (0x0021)    */
/* and NAS client alloc (0x0022 svc=0x03) to verify the socket is actually    */
/* multiplexable. Returns 0 if any candidate produced a usable qmux channel,  */
/* negative otherwise.                                                         */
/* ========================================================================== */
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <grp.h>
#include <poll.h>

#define AID_SYSTEM 1000
#define AID_RADIO  1001

static int probe_connect(const char *path, int abstract, int *out_errno) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { *out_errno = errno; return -1; }

    struct sockaddr_un sa; memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    socklen_t sl;
    if (abstract) {
        sa.sun_path[0] = 0;
        strncpy(sa.sun_path + 1, path, sizeof(sa.sun_path) - 2);
        sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(path));
    } else {
        strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
        sl = sizeof(sa);
    }

    /* Drop to radio uid/gid: qmuxd SO_PEERCRED-checks callers. */
    uid_t saved_euid = geteuid(); gid_t saved_egid = getegid();
    int dropped = 0;
    if (saved_euid == 0) {
        prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
        gid_t groups[] = { AID_RADIO, AID_SYSTEM };
        setgroups(2, groups);
        if (setegid(AID_RADIO) == 0 && seteuid(AID_RADIO) == 0) dropped = 1;
    }
    int crc = connect(fd, (struct sockaddr *)&sa, sl);
    int cerr = errno;
    if (dropped) {
        seteuid(saved_euid); setegid(saved_egid);
        prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
    }
    if (crc < 0) { *out_errno = cerr; close(fd); return -1; }
    *out_errno = 0;
    return fd;
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qmuxProbeCellLock(
        JNIEnv *env, jclass clz, jint earfcn, jint pci) {
    (void)env; (void)clz; (void)earfcn; (void)pci;

    static const struct { const char *path; int abstract; } cands[] = {
        { "/dev/socket/qmux_radio/ril_ipc", 0 },
        { "/dev/socket/qmux_radio",         0 },
        { "/dev/socket/qmuxd",              0 },
        { "qmux_radio",                     1 },   /* abstract namespace */
        { "qmuxd",                          1 },
        { NULL, 0 }
    };

    int best_fd = -1;
    const char *best_path = NULL;
    for (int i = 0; cands[i].path; i++) {
        int e = 0;
        int fd = probe_connect(cands[i].path, cands[i].abstract, &e);
        LOGI("qmux-probe[%d] path=%s%s fd=%d errno=%d (%s)",
             i, cands[i].abstract ? "@" : "", cands[i].path,
             fd, e, e ? strerror(e) : "ok");
        if (fd >= 0 && best_fd < 0) {
            best_fd = fd;
            best_path = cands[i].path;
        } else if (fd >= 0) {
            close(fd);
        }
    }

    if (best_fd < 0) {
        LOGE("qmux-probe: no qmuxd socket reachable");
        return -1;
    }
    LOGI("qmux-probe: first reachable = %s (fd=%d)", best_path, best_fd);

    /* 2) Send CTL GET_VERSION (msg 0x0021, no TLVs) and read reply. */
    /* qmux frame for CTL request:
     *   01 | total_len:LE16 | flags=00 | svc=00 | cid=00 | SDU
     * CTL SDU: flags=0x00 txn=01 msg=0x0021 tlv_len=0x0000
     */
    uint8_t frame[] = {
        0x01,                   /* IFC */
        0x0C, 0x00,             /* total_len = 12 (hdr 5 + SDU 7) */
        0x00,                   /* flags */
        0x00,                   /* svc CTL */
        0x00,                   /* cid=0 (broadcast) */
        /* SDU: */
        0x00,                   /* ctl flags: request */
        0x01,                   /* txn */
        0x21, 0x00,             /* msg_id = 0x0021 GET_VERSION_INFO */
        0x00, 0x00              /* tlv_len */
    };
    ssize_t w = write(best_fd, frame, sizeof(frame));
    LOGI("qmux-probe CTL GET_VERSION write=%zd errno=%d", w, w < 0 ? errno : 0);

    uint8_t rx[512];
    struct pollfd pfd = { .fd = best_fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 2000);
    if (pr <= 0) {
        LOGE("qmux-probe CTL GET_VERSION: poll rc=%d errno=%d (no reply from qmuxd)",
             pr, pr < 0 ? errno : 0);
        close(best_fd);
        return -2;
    }
    ssize_t r = read(best_fd, rx, sizeof(rx));
    if (r <= 0) {
        LOGE("qmux-probe CTL GET_VERSION: read=%zd errno=%d (peer rejected us — SO_PEERCRED mismatch?)",
             r, r < 0 ? errno : 0);
        close(best_fd);
        return -3;
    }
    /* Hexdump first 32 bytes of reply. */
    {
        char line[3 * 32 + 1]; size_t n = r > 32 ? 32 : (size_t)r;
        for (size_t i = 0; i < n; i++) snprintf(line + i * 3, 4, "%02X ", rx[i]);
        line[n * 3] = 0;
        LOGI("qmux-probe CTL GET_VERSION: read=%zd hex=%s%s",
             r, line, r > 32 ? "..." : "");
    }

    /* 3) Send CTL GET_CLIENT_ID (msg 0x0022) for NAS (svc=0x03) — validates
     * the multiplex layer works for a real service. */
    uint8_t frame2[] = {
        0x01,
        0x10, 0x00,             /* total_len = 16 */
        0x00,
        0x00,
        0x00,
        /* SDU: */
        0x00,                   /* ctl flags */
        0x02,                   /* txn */
        0x22, 0x00,             /* msg_id = 0x0022 GET_CLIENT_ID */
        0x04, 0x00,             /* tlv_len = 4 */
        0x01,                   /* TLV 0x01 */
        0x01, 0x00,             /* TLV len = 1 */
        0x03                    /* service = NAS */
    };
    w = write(best_fd, frame2, sizeof(frame2));
    LOGI("qmux-probe CTL GET_CLIENT_ID(NAS) write=%zd errno=%d", w, w < 0 ? errno : 0);
    pr = poll(&pfd, 1, 2000);
    if (pr > 0) {
        r = read(best_fd, rx, sizeof(rx));
        if (r > 0) {
            char line[3 * 32 + 1]; size_t n = r > 32 ? 32 : (size_t)r;
            for (size_t i = 0; i < n; i++) snprintf(line + i * 3, 4, "%02X ", rx[i]);
            line[n * 3] = 0;
            LOGI("qmux-probe CTL GET_CLIENT_ID reply=%zd hex=%s", r, line);
            /* Extract assigned CID if present: TLV 0x01 payload = [svc, cid]. */
            if (r >= 14 && rx[0] == 0x01 && rx[7] == 0x01 /* resp flag */) {
                /* SDU starts at rx[6]; walk TLVs starting at rx[6+7]. */
                LOGI("qmux-probe OK — qmuxd is reachable and multiplexes NAS");
                close(best_fd);
                return 0;
            }
        } else {
            LOGE("qmux-probe CTL GET_CLIENT_ID: read=%zd errno=%d", r, r < 0 ? errno : 0);
        }
    } else {
        LOGE("qmux-probe CTL GET_CLIENT_ID: poll rc=%d", pr);
    }
    close(best_fd);
    /* Got version reply but not client id: qmuxd alive but may not expose NAS. */
    return 1;
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qrtrClearCellLock(
        JNIEnv *env, jclass clz) {
    (void)env; (void)clz;

    qrtr_close();
    int orc = qrtr_open();
    if (orc < 0) return orc;

    uint32_t node = 0, port = 0;
    int rc = qrtr_lookup(QMI_SVC_DMS, 1, 2000, &node, &port);
    if (rc == QRTR_RC_NO_SERVICE) rc = qrtr_lookup(QMI_SVC_DMS, 0, 2000, &node, &port);
    if (rc < 0) return rc;

    /* Clear = write lock=0 across same candidate set. */
    static const uint16_t ids[] = { 6828, 6829, 466, 4964 };
    int ok = -1;
    for (size_t i = 0; i < sizeof(ids)/sizeof(ids[0]); i++) {
        uint8_t body[8]; size_t bl = 0;
        body[bl++] = (uint8_t)(ids[i] & 0xFF);
        body[bl++] = (uint8_t)((ids[i] >> 8) & 0xFF);
        body[bl++] = 0x00; /* flag=disable */
        body[bl++] = 0x00; body[bl++] = 0x00; /* earfcn=0 */
        body[bl++] = 0x00; body[bl++] = 0x00; /* pci=0 */

        uint8_t tlvs[16]; size_t to = 0;
        tlvs[to++] = 0x01;
        tlvs[to++] = (uint8_t)(bl & 0xFF);
        tlvs[to++] = (uint8_t)((bl >> 8) & 0xFF);
        for (size_t k = 0; k < bl; k++) tlvs[to++] = body[k];

        uint8_t req[64];
        size_t reqLen = qrtr_build_qmi_request(req, sizeof(req), 0x003D, tlvs, to);
        uint8_t rx[512];
        int n = qrtr_transact(node, port, req, reqLen, rx, sizeof(rx), 2000);
        if (n < 0) continue;
        uint16_t mid, result = 0xFFFF, err = 0xFFFF;
        if (qrtr_parse_qmi_response(rx, (size_t)n, &mid, &result, &err) < 0) continue;
        LOGI("cell-clear[%u] result=%u err=0x%04X", ids[i], result, err);
        if (result == 0) ok = 0;
    }
    return (ok == 0) ? 0 : (jint)(0x10000 | 0x003E);
}

/* ========================================================================== */
/* Vendor QMI probe — NSG path: dlopen /vendor/lib64/libqmi_client_qmux.so    */
/* and resolve its Linux-userspace helpers + classic qmi_client_* entrypoints.*/
/* Returns bitmask of features found; 0 means library not loadable at all.    */
/* ========================================================================== */
#include <dlfcn.h>
#include <android/dlext.h>

/* Android linker-namespace bypass helpers (runtime lookup so we don't have
 * to link against libdl_android.so which is API-level gated). */
struct android_namespace_t;
typedef struct android_namespace_t* (*p_android_get_exported_namespace_t)(const char *name);
typedef void* (*p_android_dlopen_ext_t)(const char *filename, int flag,
                                        const android_dlextinfo *extinfo);

static p_android_get_exported_namespace_t p_get_ns = NULL;
static p_android_dlopen_ext_t             p_dlopen_ext = NULL;

static void init_ns_api(void) {
    static int tried = 0;
    if (tried) return;
    tried = 1;
    /* Both symbols are exported by the dynamic linker; RTLD_DEFAULT works. */
    p_get_ns = (p_android_get_exported_namespace_t)
               dlsym(RTLD_DEFAULT, "android_get_exported_namespace");
    if (!p_get_ns) {
        /* Some Androids hide it behind libdl_android.so. */
        void *libdl = dlopen("libdl_android.so", RTLD_NOW | RTLD_GLOBAL);
        if (libdl) {
            p_get_ns = (p_android_get_exported_namespace_t)
                       dlsym(libdl, "android_get_exported_namespace");
        }
    }
    p_dlopen_ext = (p_android_dlopen_ext_t)dlsym(RTLD_DEFAULT, "android_dlopen_ext");
    LOGI("vendor-qmi: ns api get_exported_namespace=%p android_dlopen_ext=%p",
         p_get_ns, p_dlopen_ext);
}

static void *try_dlopen_ns(const char *path, const char *ns_name) {
    init_ns_api();
    if (!p_get_ns || !p_dlopen_ext) return NULL;
    struct android_namespace_t *ns = p_get_ns(ns_name);
    if (!ns) {
        LOGI("vendor-qmi: ns[%s] not exported", ns_name);
        return NULL;
    }
    android_dlextinfo info = { 0 };
    info.flags = ANDROID_DLEXT_USE_NAMESPACE;
    info.library_namespace = ns;
    void *h = p_dlopen_ext(path, RTLD_NOW | RTLD_GLOBAL, &info);
    if (h) {
        LOGI("vendor-qmi: dlopen_ext[ns=%s] OK %s -> %p", ns_name, path, h);
    } else {
        LOGI("vendor-qmi: dlopen_ext[ns=%s] FAIL %s: %s", ns_name, path, dlerror());
    }
    return h;
}

static void *try_dlopen(const char *path) {
    /* Try plain dlopen first (works inside our own classloader namespace
     * only for whitelisted public vendor libs). */
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (h) {
        LOGI("vendor-qmi: dlopen OK %s -> %p", path, h);
        return h;
    }
    LOGI("vendor-qmi: dlopen plain FAIL %s: %s", path, dlerror());

    /* Fall back to the exported namespaces that grant access to /vendor. */
    static const char *ns_names[] = {
        "sphal", "vndk", "vndk_in_system", "default", "system",
        "rs", "product", NULL,
    };
    for (int i = 0; ns_names[i]; i++) {
        h = try_dlopen_ns(path, ns_names[i]);
        if (h) return h;
    }
    LOGE("vendor-qmi: dlopen FAIL %s (all namespaces refused)", path);
    return NULL;
}

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

/* Root-service copy of a vendor library into a path the app linker namespace
 * allows (our app's files dir). Returns 0 on success. */
static int copy_file(const char *src, const char *dst) {
    int sfd = open(src, O_RDONLY | O_CLOEXEC);
    if (sfd < 0) { LOGE("vendor-qmi: cp: open src FAIL %s: %s", src, strerror(errno)); return -1; }
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (dfd < 0) { LOGE("vendor-qmi: cp: open dst FAIL %s: %s", dst, strerror(errno)); close(sfd); return -2; }
    char buf[64 * 1024];
    ssize_t n; off_t total = 0;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t ww = write(dfd, buf + w, n - w);
            if (ww < 0) { close(sfd); close(dfd); return -3; }
            w += ww;
        }
        total += n;
    }
    fchmod(dfd, 0644);
    close(sfd);
    close(dfd);
    LOGI("vendor-qmi: cp %s -> %s (%lld B)", src, dst, (long long)total);
    return 0;
}

/* Helper shipped as libqdiag_helper_exec.so in jniLibs. Copy it out of
 * nativeLibraryDir into /data/local/tmp (which is exec-friendly and
 * owned by shell), chmod +x, run from there. The copy runs as root so
 * writes to /data/local/tmp are always allowed.
 *
 * The helper runs OUTSIDE our classloader linker namespace (it's a fresh
 * exec'd process), so its dlopen() is not constrained by clns-1.
 */
#define HELPER_SRC_NAME  "libqdiag_helper_exec.so"
#define HELPER_DST_PATH  "/data/local/tmp/qdiag_helper_exec"

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_qmiVendorProbe(
        JNIEnv *env, jclass clz, jstring jNativeLibDir) {
    (void)clz;

    const char *nativeLibDir = (*env)->GetStringUTFChars(env, jNativeLibDir, NULL);
    if (!nativeLibDir) return 0;

    char src[512];
    snprintf(src, sizeof(src), "%s/%s", nativeLibDir, HELPER_SRC_NAME);
    LOGI("vendor-qmi: helper src = %s", src);

    int rc_cp = copy_file(src, HELPER_DST_PATH);
    (*env)->ReleaseStringUTFChars(env, jNativeLibDir, nativeLibDir);
    if (rc_cp != 0) {
        LOGE("vendor-qmi: helper copy FAIL rc=%d (not extracted? extractNativeLibs=true?)", rc_cp);
        return 0;
    }
    if (chmod(HELPER_DST_PATH, 0755) != 0) {
        LOGE("vendor-qmi: chmod 0755 %s FAIL: %s", HELPER_DST_PATH, strerror(errno));
    }

    /* Exec helper. We're already root (DiagRootService runs as UID=0), so
     * `sh -c <helper> probe` inherits default linker namespace. */
    FILE *pp = popen(HELPER_DST_PATH " probe 2>&1", "r");
    if (!pp) {
        LOGE("vendor-qmi: popen FAIL: %s", strerror(errno));
        return 0;
    }

    int mask = 0;
    int saw_main_ok = 0;
    char line[1024];
    while (fgets(line, sizeof(line), pp)) {
        /* Strip trailing newline for logging. */
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        LOGI("vendor-qmi[helper]: %s", line);
        if (strncmp(line, "MAIN OK", 7) == 0) saw_main_ok = 1;
        if (strncmp(line, "MASK ", 5) == 0) {
            /* "MASK 0xXXXX N/M" */
            unsigned m = 0;
            if (sscanf(line + 5, "0x%x", &m) == 1) mask = (int)m;
        }
    }
    int wstat = pclose(pp);
    LOGI("vendor-qmi: helper exit status=0x%X main_ok=%d mask=0x%04X",
         wstat, saw_main_ok, mask);

    if (!saw_main_ok) return 0;
    return (jint)(0x10000 | (mask & 0xFFFF));
}
