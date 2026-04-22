#ifndef QDIAG_HDLC_H
#define QDIAG_HDLC_H

#include <stddef.h>
#include <stdint.h>

/*
 * DIAG frames travel over HDLC-like framing: trailing CRC-16/X-25 and byte
 * stuffing of 0x7D / 0x7E, terminated by 0x7E.
 */

/* Encode a DIAG payload into an HDLC frame.
 * out must have room for 2*len + 4 bytes. Returns encoded length. */
size_t hdlc_encode(const uint8_t *in, size_t len, uint8_t *out);

/* Decode one HDLC frame from 'in' (of length len) into 'out'.
 * Returns decoded payload length (excluding CRC), or 0 if CRC/framing invalid. */
size_t hdlc_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

#endif
