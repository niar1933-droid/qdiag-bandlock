#include "sniffer.h"

#include "diag.h"
#include "hdlc.h"

#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "qdiag-sniffer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* --- Ring buffer of HDLC-decoded frames ------------------------------- */

#define RING_CAP (1 << 20)   /* 1 MiB — frames are a few hundred bytes each. */

static uint8_t        g_ring[RING_CAP];
static size_t         g_ring_head = 0;   /* writer index */
static size_t         g_ring_tail = 0;   /* reader index */
static pthread_mutex_t g_ring_mtx = PTHREAD_MUTEX_INITIALIZER;

static _Atomic unsigned long g_total   = 0;
static _Atomic unsigned long g_dropped = 0;

static size_t ring_used_locked(void) {
    return (g_ring_head + RING_CAP - g_ring_tail) % RING_CAP;
}
static size_t ring_free_locked(void) {
    return RING_CAP - 1 - ring_used_locked();
}
static void ring_write_locked(const uint8_t *p, size_t n) {
    size_t first = (g_ring_head + n <= RING_CAP) ? n : (RING_CAP - g_ring_head);
    memcpy(g_ring + g_ring_head, p, first);
    if (n > first) memcpy(g_ring, p + first, n - first);
    g_ring_head = (g_ring_head + n) % RING_CAP;
}
static void ring_read_locked(uint8_t *p, size_t n) {
    size_t first = (g_ring_tail + n <= RING_CAP) ? n : (RING_CAP - g_ring_tail);
    memcpy(p, g_ring + g_ring_tail, first);
    if (n > first) memcpy(p + first, g_ring, n - first);
    g_ring_tail = (g_ring_tail + n) % RING_CAP;
}

/* Drop the oldest frame(s) until there's room for need bytes. */
static void ring_drop_oldest_locked(size_t need) {
    while (ring_free_locked() < need && ring_used_locked() >= 4) {
        uint32_t hdr;
        uint8_t hb[4];
        /* peek header */
        size_t tmp = g_ring_tail;
        for (int i = 0; i < 4; i++) {
            hb[i] = g_ring[tmp];
            tmp = (tmp + 1) % RING_CAP;
        }
        hdr = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8)
            | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
        size_t adv = 4 + hdr;
        if (adv > ring_used_locked()) adv = ring_used_locked();
        g_ring_tail = (g_ring_tail + adv) % RING_CAP;
        atomic_fetch_add(&g_dropped, 1);
    }
}

static void ring_push_frame(const uint8_t *frame, size_t len) {
    if (len == 0 || len > 0xFFFF) return;
    size_t need = 4 + len;

    pthread_mutex_lock(&g_ring_mtx);
    if (ring_free_locked() < need) ring_drop_oldest_locked(need);
    uint8_t hb[4] = {
        (uint8_t)(len & 0xFF),
        (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF),
        (uint8_t)((len >> 24) & 0xFF),
    };
    ring_write_locked(hb, 4);
    ring_write_locked(frame, len);
    pthread_mutex_unlock(&g_ring_mtx);

    atomic_fetch_add(&g_total, 1);
}

/* --- Worker thread ----------------------------------------------------- */

static _Atomic int g_running = 0;
static pthread_t   g_thread;

static void *sniffer_loop(void *arg) {
    (void)arg;
    LOGI("sniffer thread started");

    /* Byte accumulator: HDLC frames are terminated by 0x7E, so we scan
     * each read chunk for terminators and decode complete frames. */
    static uint8_t acc[16384];
    size_t acc_n = 0;

    while (atomic_load(&g_running)) {
        if (!diag_is_open()) { usleep(100 * 1000); continue; }

        uint8_t chunk[4096];
        ssize_t n = diag_read(chunk, sizeof(chunk));
        if (n < 0) { usleep(100 * 1000); continue; }
        if (n == 0) continue;

        /* Append to accumulator (dropping oldest bytes if overflow). */
        if (acc_n + (size_t)n > sizeof(acc)) {
            size_t shift = (acc_n + (size_t)n) - sizeof(acc);
            if (shift > acc_n) shift = acc_n;
            memmove(acc, acc + shift, acc_n - shift);
            acc_n -= shift;
        }
        memcpy(acc + acc_n, chunk, (size_t)n);
        acc_n += (size_t)n;

        /* Extract complete HDLC frames. */
        size_t start = 0;
        for (size_t i = 0; i < acc_n; i++) {
            if (acc[i] == 0x7E) {
                size_t raw_len = i + 1 - start;
                if (raw_len > 3) {
                    uint8_t decoded[4096];
                    size_t dec = hdlc_decode(acc + start, raw_len,
                                             decoded, sizeof(decoded));
                    if (dec > 0) ring_push_frame(decoded, dec);
                }
                start = i + 1;
            }
        }
        if (start > 0) {
            memmove(acc, acc + start, acc_n - start);
            acc_n -= start;
        }
    }

    LOGI("sniffer thread exiting");
    return NULL;
}

int sniffer_start(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_running, &expected, 1)) {
        return 0; /* already running */
    }
    pthread_mutex_lock(&g_ring_mtx);
    g_ring_head = g_ring_tail = 0;
    atomic_store(&g_total, 0);
    atomic_store(&g_dropped, 0);
    pthread_mutex_unlock(&g_ring_mtx);

    if (pthread_create(&g_thread, NULL, sniffer_loop, NULL) != 0) {
        LOGE("pthread_create failed: %s", strerror(errno));
        atomic_store(&g_running, 0);
        return -errno;
    }
    return 0;
}

void sniffer_stop(void) {
    int was = atomic_exchange(&g_running, 0);
    if (!was) return;
    pthread_join(g_thread, NULL);
}

int sniffer_is_running(void) { return atomic_load(&g_running); }

size_t sniffer_drain(uint8_t *buf, size_t buf_cap, int *out_n_frames) {
    size_t written = 0;
    int frames = 0;
    pthread_mutex_lock(&g_ring_mtx);
    while (ring_used_locked() >= 4) {
        uint8_t hb[4];
        size_t save_tail = g_ring_tail;
        ring_read_locked(hb, 4);
        uint32_t len = (uint32_t)hb[0] | ((uint32_t)hb[1] << 8)
                     | ((uint32_t)hb[2] << 16) | ((uint32_t)hb[3] << 24);
        if (len > ring_used_locked()) { g_ring_tail = save_tail; break; }
        if (written + 4 + len > buf_cap) { g_ring_tail = save_tail; break; }
        memcpy(buf + written, hb, 4);
        written += 4;
        ring_read_locked(buf + written, len);
        written += len;
        frames++;
    }
    pthread_mutex_unlock(&g_ring_mtx);
    if (out_n_frames) *out_n_frames = frames;
    return written;
}

int sniffer_save_to_fd(int fd) {
    uint8_t buf[4096];
    int total_frames = 0;
    for (;;) {
        int n = 0;
        size_t got = sniffer_drain(buf, sizeof(buf), &n);
        if (got == 0) break;
        ssize_t w = write(fd, buf, got);
        if (w < 0) return -errno;
        total_frames += n;
    }
    return total_frames;
}

unsigned long sniffer_total_frames(void)   { return atomic_load(&g_total); }
unsigned long sniffer_dropped_frames(void) { return atomic_load(&g_dropped); }
