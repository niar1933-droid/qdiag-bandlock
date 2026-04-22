#include "diag.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "qdiag-native"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static int g_diag_fd = -1;

/*
 * Layout of the SWITCH_LOGGING ioctl varies between kernels:
 *   - Legacy (pre-Android 7): ioctl(fd, DIAG_IOCTL_SWITCH_LOGGING, &mode) where mode is int
 *   - Mid era: struct { int req_mode; int peripheral_mask; }
 *   - Modern (MSM-diag, ~2018+): struct diag_logging_mode_param_t (see below)
 *
 * We try them in order most-likely-to-work-first.
 */
struct diag_logging_mode_param_t {
    uint32_t req_mode;
    uint32_t peripheral_mask;
    uint32_t pd_mask;
    uint8_t  mode_param;
    uint8_t  diag_id;
    uint8_t  pd_val;
    uint8_t  reserved;
    int      peripheral;
    int      device_mask;
} __attribute__((packed));

#define DIAG_CON_APSS    0x0001
#define DIAG_CON_MPSS    0x0002
#define DIAG_CON_LPASS   0x0004
#define DIAG_CON_WCNSS   0x0008
#define DIAG_CON_SENSORS 0x0010
#define DIAG_CON_ALL     (DIAG_CON_APSS | DIAG_CON_MPSS | DIAG_CON_LPASS | DIAG_CON_WCNSS | DIAG_CON_SENSORS)

static int try_switch_logging(int fd) {
    /* Modern struct form. */
    struct diag_logging_mode_param_t p = {0};
    p.req_mode        = MEMORY_DEVICE_MODE;
    p.peripheral_mask = DIAG_CON_ALL;
    p.pd_mask         = 0;
    p.mode_param      = 1;
    p.diag_id         = 0;
    p.pd_val          = 0;
    p.peripheral      = 0;
    p.device_mask     = 1; /* local device */

    if (ioctl(fd, DIAG_IOCTL_SWITCH_LOGGING, &p) == 0) {
        LOGI("switch_logging: modern struct OK");
        return 0;
    }
    LOGW("switch_logging modern-struct failed: %s", strerror(errno));

    /* Mid-era: { int req_mode; int peripheral_mask; } */
    int mid[2] = { MEMORY_DEVICE_MODE, DIAG_CON_ALL };
    if (ioctl(fd, DIAG_IOCTL_SWITCH_LOGGING, mid) == 0) {
        LOGI("switch_logging: mid-era struct OK");
        return 0;
    }
    LOGW("switch_logging mid-era failed: %s", strerror(errno));

    /* Legacy: plain int. */
    int legacy = MEMORY_DEVICE_MODE;
    if (ioctl(fd, DIAG_IOCTL_SWITCH_LOGGING, &legacy) == 0) {
        LOGI("switch_logging: legacy int OK");
        return 0;
    }
    LOGE("switch_logging legacy failed: %s", strerror(errno));
    return -errno;
}

int diag_open(void) {
    if (g_diag_fd >= 0) return 0;

    int fd = open("/dev/diag", O_RDWR | O_LARGEFILE | O_NONBLOCK);
    if (fd < 0) {
        LOGE("open /dev/diag failed: %s", strerror(errno));
        return -errno;
    }

    int rc = try_switch_logging(fd);
    if (rc != 0) {
        close(fd);
        return rc;
    }

    g_diag_fd = fd;
    LOGI("diag opened, fd=%d", fd);
    return 0;
}

void diag_close(void) {
    if (g_diag_fd >= 0) {
        close(g_diag_fd);
        g_diag_fd = -1;
    }
}

int diag_is_open(void) { return g_diag_fd >= 0; }

ssize_t diag_write_raw(const uint8_t *data, size_t len) {
    if (g_diag_fd < 0) return -EBADF;
    /*
     * The msm-diag driver expects writes prefixed with a 4-byte data_type marker
     * (USER_SPACE_DATA_TYPE). See diag_char.c in msm kernels.
     */
    uint8_t buf[4096];
    if (len + 4 > sizeof(buf)) return -E2BIG;
    uint32_t dtype = USER_SPACE_DATA_TYPE;
    memcpy(buf, &dtype, 4);
    memcpy(buf + 4, data, len);
    ssize_t n = write(g_diag_fd, buf, len + 4);
    if (n < 0) {
        LOGW("diag write failed: %s", strerror(errno));
        return -errno;
    }
    return n - 4;
}

ssize_t diag_read(uint8_t *buf, size_t buf_len) {
    if (g_diag_fd < 0) return -EBADF;

    struct pollfd pfd = { .fd = g_diag_fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 200 /* ms */);
    if (pr <= 0) return 0;

    ssize_t n = read(g_diag_fd, buf, buf_len);
    if (n < 0) {
        if (errno == EAGAIN) return 0;
        LOGW("diag read failed: %s", strerror(errno));
        return -errno;
    }
    return n;
}
