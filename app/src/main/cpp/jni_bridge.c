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

    /* Build TLVs for QMI NAS SET_SYSTEM_SELECTION_PREFERENCE (0x0033).
     *
     * Per libqmi nas.json:
     *   0x11 Mode Preference            u16   (2 bytes)
     *   0x12 Band Preference            u64   (8 bytes, legacy GSM/UMTS)
     *   0x15 LTE Band Preference        u64   (8 bytes, deprecated)
     *   0x1C LTE Band Preference Ext    u64+u64 (16 bytes, bands 1..128)
     *   0x24 NR5G SA Band Preference    u64+u64 (16 bytes)
     *   0x25 NR5G NSA Band Preference   u64+u64 (16 bytes)
     *
     * Previous build had 0x11 and 0x12 swapped (sending 2 bytes to 0x12
     * which expects 8 -> MALFORMED_MSG).  This version fixes that and
     * also picks Mode Preference based on what the user actually
     * selected, and only emits NR5G TLVs when NR bands were chosen. */
    uint8_t tlvs[128];
    size_t  to = 0;

    int have_lte = (lteLow != 0) || (lteHigh != 0);
    int have_nr  = (nrLow  != 0) || (nrHigh  != 0);
    uint16_t mode_pref = 0;
    if (have_lte) mode_pref |= 0x10;  /* b4 LTE */
    if (have_nr)  mode_pref |= 0x40;  /* b6 5GNR */
    if (!mode_pref) mode_pref = 0x10; /* fallback LTE */

    /* TLV 0x11 Mode Preference (u16 LE) */
    tlvs[to++] = 0x11;
    tlvs[to++] = 0x02; tlvs[to++] = 0x00;
    tlvs[to++] = (uint8_t)(mode_pref & 0xFF);
    tlvs[to++] = (uint8_t)((mode_pref >> 8) & 0xFF);

    /* TLV 0x1C LTE Band Preference Ext (16 bytes, bands 1..128) */
    tlvs[to++] = 0x1C;
    tlvs[to++] = 0x10; tlvs[to++] = 0x00;
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((lteLow  >> (8*i)) & 0xFF);
    for (int i = 0; i < 8; i++) tlvs[to++] = (uint8_t)((lteHigh >> (8*i)) & 0xFF);

    if (have_nr) {
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
    if (result != 0) return (jint)(0x10000 | err);
    return 0;
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
