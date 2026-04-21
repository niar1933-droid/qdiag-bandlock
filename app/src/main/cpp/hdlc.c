#include "hdlc.h"

/* CRC-16/X-25 (poly 0x1021, init 0xFFFF, reflected, xor-out 0xFFFF). */
static uint16_t crc16_x25(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0x8408;
            else         crc >>= 1;
        }
    }
    return ~crc;
}

size_t hdlc_encode(const uint8_t *in, size_t len, uint8_t *out) {
    uint16_t crc = crc16_x25(in, len);
    size_t o = 0;
    for (size_t i = 0; i < len + 2; i++) {
        uint8_t b = (i < len) ? in[i]
                              : (i == len ? (uint8_t)(crc & 0xFF)
                                          : (uint8_t)((crc >> 8) & 0xFF));
        if (b == 0x7E || b == 0x7D) {
            out[o++] = 0x7D;
            out[o++] = b ^ 0x20;
        } else {
            out[o++] = b;
        }
    }
    out[o++] = 0x7E;
    return o;
}

size_t hdlc_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap) {
    size_t o = 0;
    size_t i = 0;
    /* Trim trailing 0x7E. */
    if (len && in[len - 1] == 0x7E) len--;

    while (i < len && o < out_cap) {
        uint8_t b = in[i++];
        if (b == 0x7D && i < len) {
            out[o++] = in[i++] ^ 0x20;
        } else {
            out[o++] = b;
        }
    }
    if (o < 3) return 0;
    uint16_t want = (uint16_t)out[o - 2] | ((uint16_t)out[o - 1] << 8);
    uint16_t got  = crc16_x25(out, o - 2);
    if (want != got) return 0;
    return o - 2;
}
