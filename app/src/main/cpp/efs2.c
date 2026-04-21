#include "efs2.h"

#include <string.h>

#include "diag.h"

static uint16_t g_efs2_seq = 1;

static size_t put_u8 (uint8_t *p, size_t o, uint8_t v) {
    p[o] = v; return o + 1;
}
static size_t put_u16(uint8_t *p, size_t o, uint16_t v) {
    p[o]   = v & 0xFF;
    p[o+1] = (v >> 8) & 0xFF;
    return o + 2;
}
static size_t put_u32(uint8_t *p, size_t o, uint32_t v) {
    p[o]   = v & 0xFF;
    p[o+1] = (v >> 8) & 0xFF;
    p[o+2] = (v >> 16) & 0xFF;
    p[o+3] = (v >> 24) & 0xFF;
    return o + 4;
}

size_t efs2_build_put_item_file(uint8_t *out, size_t out_cap,
                                const char *path,
                                const uint8_t *value, size_t value_len) {
    if (!out || !path || !value) return 0;

    size_t path_len = strlen(path);
    /* DIAG header (4) + seq (2) + data_len (4) + attrs (4) + path + NUL + value */
    size_t total = 4 + 2 + 4 + 4 + path_len + 1 + value_len;
    if (total > out_cap) return 0;

    size_t o = 0;
    o = put_u8 (out, o, DIAG_SUBSYS_CMD);       /* 0x4B */
    o = put_u8 (out, o, DIAG_SUBSYS_FS);        /* 0x13 */
    o = put_u16(out, o, EFS2_OP_PUT_ITEM_FILE); /* 0x0011 */
    o = put_u16(out, o, g_efs2_seq++);
    o = put_u32(out, o, (uint32_t)value_len);
    o = put_u32(out, o, 0x00000000);            /* default attrs/perms */
    memcpy(out + o, path, path_len);
    o += path_len;
    out[o++] = 0x00;                            /* NUL-terminate path */
    if (value_len) {
        memcpy(out + o, value, value_len);
        o += value_len;
    }
    return o;
}

size_t efs2_build_hello(uint8_t *out, size_t out_cap) {
    if (out_cap < 8) return 0;
    size_t o = 0;
    o = put_u8 (out, o, DIAG_SUBSYS_CMD);
    o = put_u8 (out, o, DIAG_SUBSYS_FS);
    o = put_u16(out, o, EFS2_OP_HELLO);
    o = put_u16(out, o, g_efs2_seq++);
    /* Hello payload: target_version(4) + minimum_version(4) +
     * version(4) + max_read(4) + max_write(4) + max_filename(4) +
     * max_pathname(4) + max_iter(4) = 32 bytes. We request modem defaults. */
    if (o + 32 > out_cap) return 0;
    memset(out + o, 0, 32);
    o += 32;
    return o;
}

size_t efs2_build_get_item_file(uint8_t *out, size_t out_cap,
                                const char *path) {
    if (!out || !path) return 0;
    size_t path_len = strlen(path);
    /* DIAG header (4) + seq (2) + path + NUL */
    size_t total = 4 + 2 + path_len + 1;
    if (total > out_cap) return 0;

    size_t o = 0;
    o = put_u8 (out, o, DIAG_SUBSYS_CMD);
    o = put_u8 (out, o, DIAG_SUBSYS_FS);
    o = put_u16(out, o, EFS2_OP_GET_ITEM_FILE);
    o = put_u16(out, o, g_efs2_seq++);
    memcpy(out + o, path, path_len);
    o += path_len;
    out[o++] = 0x00;
    return o;
}

size_t efs2_build_unlink(uint8_t *out, size_t out_cap,
                         const char *path) {
    if (!out || !path) return 0;
    size_t path_len = strlen(path);
    size_t total = 4 + 2 + path_len + 1;
    if (total > out_cap) return 0;

    size_t o = 0;
    o = put_u8 (out, o, DIAG_SUBSYS_CMD);
    o = put_u8 (out, o, DIAG_SUBSYS_FS);
    o = put_u16(out, o, EFS2_OP_UNLINK);
    o = put_u16(out, o, g_efs2_seq++);
    memcpy(out + o, path, path_len);
    o += path_len;
    out[o++] = 0x00;
    return o;
}

int efs2_parse_get_item_response(const uint8_t *frame, size_t len,
                                 uint8_t *out_data, size_t out_cap) {
    if (!frame || len < 10) return -1;
    if (frame[0] != DIAG_SUBSYS_CMD) return -1;
    if (frame[1] != DIAG_SUBSYS_FS)  return -1;
    uint16_t op = (uint16_t)(frame[2] | (frame[3] << 8));
    if (op != EFS2_OP_GET_ITEM_FILE) return -1;
    /* [4..6] = seq, [6..10] = diag_errno */
    int32_t er = (int32_t)((uint32_t)frame[6]       |
                           ((uint32_t)frame[7]  << 8)  |
                           ((uint32_t)frame[8]  << 16) |
                           ((uint32_t)frame[9]  << 24));
    if (er != 0) return -1;
    /* Data starts at offset 10 */
    size_t data_len = len - 10;
    if (data_len > out_cap) data_len = out_cap;
    if (data_len && out_data) memcpy(out_data, frame + 10, data_len);
    return (int)data_len;
}

int efs2_parse_response(const uint8_t *frame, size_t len,
                        uint16_t *out_op, int32_t *out_errno) {
    if (!frame || len < 12) return -1;
    if (frame[0] != DIAG_SUBSYS_CMD) return -1;
    if (frame[1] != DIAG_SUBSYS_FS)  return -1;
    uint16_t op = (uint16_t)(frame[2] | (frame[3] << 8));
    /* sequence is at [4..6]; we don't use it yet */
    int32_t er = (int32_t)((uint32_t)frame[6]       |
                           ((uint32_t)frame[7]  << 8)  |
                           ((uint32_t)frame[8]  << 16) |
                           ((uint32_t)frame[9]  << 24));
    if (out_op)    *out_op = op;
    if (out_errno) *out_errno = er;
    return 0;
}
