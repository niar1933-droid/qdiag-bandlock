#ifndef QDIAG_DIAG_H
#define QDIAG_DIAG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * DIAG ioctl constants. These match the public msm-diag / upstream Qualcomm
 * kernel headers used by Network Signal Guru, QCSuper, diag_revealer, etc.
 */
#define DIAG_IOCTL_SWITCH_LOGGING        7
#define DIAG_IOCTL_REMOTE_DEV            32
#define DIAG_IOCTL_OPTIMIZED_LOGGING     35
#define DIAG_IOCTL_OPTIMIZED_LOGGING_FLUSH 36

#define USER_SPACE_DATA_TYPE             0x00000020
#define MEMORY_DEVICE_MODE               2
#define CALLBACK_MODE                    6
#define MEMORY_DEVICE_MODE_2             7 /* newer kernels */

/* DIAG command codes we care about. */
#define DIAG_SUBSYS_CMD                  0x4B
#define DIAG_SUBSYS_CMD_VER_2            0x80

/* Well-known subsystem IDs. */
#define DIAG_SUBSYS_WCDMA                0x04
#define DIAG_SUBSYS_LTE                  0x0B
#define DIAG_SUBSYS_NR5G                 0x47
#define DIAG_SUBSYS_FS                   0x13
#define DIAG_SUBSYS_QMI                  0x1D

/* Opens /dev/diag and switches it into USER_SPACE / memory-device mode.
 * Returns 0 on success, -errno on failure. */
int diag_open(void);

/* Close /dev/diag. */
void diag_close(void);

/* True if currently open. */
int diag_is_open(void);

/* Write a raw (already HDLC-wrapped) frame to /dev/diag. Returns bytes written. */
ssize_t diag_write_raw(const uint8_t *data, size_t len);

/* Read any available bytes from /dev/diag (non-blocking with a short timeout).
 * Returns bytes read (may be 0). Buffer must be >= 8192 bytes. */
ssize_t diag_read(uint8_t *buf, size_t buf_len);

#endif
