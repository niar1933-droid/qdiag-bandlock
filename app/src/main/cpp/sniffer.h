#ifndef QDIAG_SNIFFER_H
#define QDIAG_SNIFFER_H

#include <stddef.h>
#include <stdint.h>

/*
 * A background thread reads /dev/diag continuously, splits the byte stream
 * into HDLC frames, HDLC-decodes each, and enqueues it in a ring buffer.
 * The Kotlin/Java side drains the buffer periodically with sniffer_drain().
 *
 * sniffer_start()  -> returns 0 on success; /dev/diag must already be open.
 * sniffer_stop()   -> joins the worker thread.
 * sniffer_is_running() -> thread state.
 *
 * sniffer_drain(buf, buf_cap, out_n_frames) -> writes frames into buf as
 *   [ u32 len_le ][ len bytes of decoded frame ] records. Returns number
 *   of bytes written. Frames that don't fit are left in the ring for the
 *   next call. Also returns the number of frames written via out_n_frames.
 *
 * sniffer_save_to_fd(fd) -> flush the ring to an open file-descriptor as
 *   raw decoded frames (same [len][frame] format). Useful for .qmdl dumps.
 */

int  sniffer_start(void);
void sniffer_stop(void);
int  sniffer_is_running(void);

size_t sniffer_drain(uint8_t *buf, size_t buf_cap, int *out_n_frames);
int    sniffer_save_to_fd(int fd);

/* Total frames seen since start (including those dropped by ring overflow). */
unsigned long sniffer_total_frames(void);
unsigned long sniffer_dropped_frames(void);

#endif
