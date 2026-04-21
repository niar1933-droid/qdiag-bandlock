#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diag.h"
#include "hdlc.h"
#include "qmi_nas.h"
#include "sniffer.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define LOG_TAG "qdiag-jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * Helper: HDLC-encode 'req' (length reqLen), write to /dev/diag, then read
 * up to 'timeout_ms' milliseconds for a response. HDLC-decode the first
 * complete frame and return a fresh jbyteArray with the decoded payload,
 * or NULL on timeout/error.
 */
static jbyteArray send_and_recv(JNIEnv *env, const uint8_t *req, size_t reqLen,
                                int timeout_ms) {
    uint8_t encoded[2048];
    size_t  encLen = hdlc_encode(req, reqLen, encoded);

    ssize_t w = diag_write_raw(encoded, encLen);
    if (w < 0) {
        LOGE("diag write failed (%zd)", w);
        return NULL;
    }

    uint8_t rx[8192];
    size_t  rxHave = 0;
    int elapsed = 0;
    while (elapsed < timeout_ms && rxHave < sizeof(rx)) {
        ssize_t n = diag_read(rx + rxHave, sizeof(rx) - rxHave);
        if (n < 0) return NULL;
        if (n == 0) { elapsed += 200; continue; }
        rxHave += (size_t)n;
        /* Scan for terminating 0x7E within the just-received chunk. */
        for (size_t i = rxHave - (size_t)n; i < rxHave; i++) {
            if (rx[i] == 0x7E) {
                uint8_t decoded[2048];
                size_t dec = hdlc_decode(rx, i + 1, decoded, sizeof(decoded));
                if (dec == 0) {
                    /* bad frame, keep reading */
                    continue;
                }
                jbyteArray out = (*env)->NewByteArray(env, (jsize)dec);
                if (!out) return NULL;
                (*env)->SetByteArrayRegion(env, out, 0, (jsize)dec, (const jbyte *)decoded);
                return out;
            }
        }
    }
    LOGE("send_and_recv: timeout after %d ms", timeout_ms);
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_openDiag(JNIEnv *env, jclass clz) {
    (void)env; (void)clz;
    return (diag_open() == 0) ? JNI_TRUE : JNI_FALSE;
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
    jbyteArray out = send_and_recv(env, (const uint8_t *)buf, (size_t)len, 2000);
    (*env)->ReleaseByteArrayElements(env, req, buf, JNI_ABORT);
    return out;
}

static jint run_qmi(JNIEnv *env, const uint8_t *req, size_t reqLen) {
    jbyteArray ja = send_and_recv(env, req, reqLen, 2000);
    if (!ja) return -100;
    jsize jl = (*env)->GetArrayLength(env, ja);
    jbyte *p = (*env)->GetByteArrayElements(env, ja, NULL);
    uint16_t err = 0;
    int rc = qmi_parse_response((const uint8_t *)p, (size_t)jl, &err);
    (*env)->ReleaseByteArrayElements(env, ja, p, JNI_ABORT);
    if (rc < 0) return -200 + rc;
    if (rc != 0) {
        LOGE("QMI result=%d error=0x%04x", rc, err);
        return (jint)(0x10000 | err); /* surface the QMI error code */
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
    if (!len) return -1;
    return run_qmi(env, req, len);
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_resetBandPref(JNIEnv *env, jclass clz) {
    (void)clz;
    uint8_t req[QMI_MAX_REQUEST];
    size_t  len = qmi_nas_build_reset_band_pref(req);
    if (!len) return -1;
    return run_qmi(env, req, len);
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_setLteCellLock(JNIEnv *env, jclass clz,
                                                       jint earfcn, jint pci) {
    (void)clz;
    uint8_t req[QMI_MAX_REQUEST];
    size_t  len = qmi_nas_build_lte_cell_lock(req, (uint32_t)earfcn, (uint32_t)pci);
    if (!len) return -1;
    return run_qmi(env, req, len);
}

JNIEXPORT jint JNICALL
Java_com_qdiag_bandlock_diag_DiagNative_clearLteCellLock(JNIEnv *env, jclass clz) {
    (void)clz;
    uint8_t req[QMI_MAX_REQUEST];
    size_t  len = qmi_nas_build_lte_cell_unlock(req);
    if (!len) return -1;
    return run_qmi(env, req, len);
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
