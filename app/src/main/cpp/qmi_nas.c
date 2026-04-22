#include "qmi_nas.h"

#include <string.h>

#include "diag.h"

/*
 * Wire layout for a QMI-over-DIAG request:
 *
 *   offset  size  field
 *   ------  ----  ---------------------------------------------------
 *     0     1     DIAG_SUBSYS_CMD (0x4B)
 *     1     1     subsystem id (DIAG_SUBSYS_QMI, 0x1D)
 *     2     2     subsystem cmd code (0x0000 for plain QMI request)
 *     4     1     service id (QMI_SVC_NAS = 0x03)
 *     5     1     client id (we use 0)
 *     6     1     control flag (0 = request)
 *     7     2     transaction id
 *     9     2     QMI message id (little-endian)
 *    11     2     TLV payload length
 *    13     ..    TLV payload
 *
 * The above matches the framing observed in QCSuper / diag_revealer and is
 * the same layout QXDM uses when issuing NAS requests over the modem's
 * diagnostic channel.
 */

static uint16_t g_txid = 1;

static size_t put_u8 (uint8_t *p, size_t off, uint8_t v)  { p[off] = v; return off + 1; }
static size_t put_u16(uint8_t *p, size_t off, uint16_t v) { p[off] = v & 0xFF; p[off+1] = (v >> 8) & 0xFF; return off + 2; }
static size_t put_u32(uint8_t *p, size_t off, uint32_t v) {
    p[off]   = v & 0xFF;
    p[off+1] = (v >> 8) & 0xFF;
    p[off+2] = (v >> 16) & 0xFF;
    p[off+3] = (v >> 24) & 0xFF;
    return off + 4;
}
/* put_u64 currently unused but kept as a helper for future TLVs that carry
 * 64-bit values (e.g. NR5G band mask low/high split). */
static size_t __attribute__((unused))
put_u64(uint8_t *p, size_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) p[off + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    return off + 8;
}

size_t qmi_nas_build_req(uint8_t *out,
                         uint16_t msg_id,
                         const uint8_t *tlvs,
                         size_t tlvs_len) {
    if (tlvs_len + 32 > QMI_MAX_REQUEST) return 0;

    size_t o = 0;
    o = put_u8 (out, o, DIAG_SUBSYS_CMD);
    o = put_u8 (out, o, DIAG_SUBSYS_QMI);
    o = put_u16(out, o, 0x0000);          /* subsys command code: plain QMI tunnel */
    o = put_u8 (out, o, QMI_SVC_NAS);
    o = put_u8 (out, o, 0x00);            /* client id */
    o = put_u8 (out, o, 0x00);            /* control flag: request */
    o = put_u16(out, o, g_txid++);
    o = put_u16(out, o, msg_id);
    o = put_u16(out, o, (uint16_t)tlvs_len);
    if (tlvs_len) {
        memcpy(out + o, tlvs, tlvs_len);
        o += tlvs_len;
    }
    return o;
}

/* TLV helper. */
static size_t put_tlv(uint8_t *tlvs, size_t off,
                      uint8_t type, const uint8_t *val, size_t val_len) {
    tlvs[off++] = type;
    tlvs[off++] = val_len & 0xFF;
    tlvs[off++] = (val_len >> 8) & 0xFF;
    memcpy(tlvs + off, val, val_len);
    return off + val_len;
}

/*
 * QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE_REQ TLVs we use:
 *   0x11  Band preference (uint64, bitmask of legacy bands 1..64)
 *   0x12  Mode preference (uint16, bitmask of modes)
 *   0x1C  LTE band preference extended (128-bit, bands 1..128)  -- optional
 *   0x24  NR5G SA band preference (uint64x2 = 128-bit, n1..n128)
 *   0x25  NR5G NSA band preference (uint64x2)
 *
 * Values of bit N-1 == band N. See modem-public docs (libqmi /
 * libqrtr-glib) for the exact layout — we match libqmi's encoding.
 */

#define TLV_LEGACY_BAND_PREF   0x11
#define TLV_MODE_PREF          0x12
#define TLV_LTE_BAND_PREF_EXT  0x1C
#define TLV_NR5G_SA_BAND_PREF  0x24
#define TLV_NR5G_NSA_BAND_PREF 0x25

#define MODE_PREF_ALL 0x00FF   /* CDMA | HDR | GSM | WCDMA | LTE | TDSCDMA | NR5G | ... */

size_t qmi_nas_build_set_band_pref(uint8_t *out,
                                   uint64_t lteMaskLow, uint64_t lteMaskHigh,
                                   uint64_t nrMaskLow,  uint64_t nrMaskHigh) {
    uint8_t tlvs[256];
    size_t  o = 0;

    /* Legacy band preference: keep the low 64 bits so "bands 1..64" users
     * on old modems still work. */
    uint8_t legacy[8];
    for (int i = 0; i < 8; i++) legacy[i] = (uint8_t)((lteMaskLow >> (8 * i)) & 0xFF);
    o = put_tlv(tlvs, o, TLV_LEGACY_BAND_PREF, legacy, sizeof(legacy));

    /* Mode preference: allow the modem to pick any RAT, band mask governs. */
    uint8_t mode[2] = { 0xFF, 0x00 };
    o = put_tlv(tlvs, o, TLV_MODE_PREF, mode, sizeof(mode));

    /* LTE band preference extended: 128-bit. */
    uint8_t lte_ext[16];
    for (int i = 0; i < 8;  i++) lte_ext[i]     = (uint8_t)((lteMaskLow  >> (8 * i)) & 0xFF);
    for (int i = 0; i < 8;  i++) lte_ext[8 + i] = (uint8_t)((lteMaskHigh >> (8 * i)) & 0xFF);
    o = put_tlv(tlvs, o, TLV_LTE_BAND_PREF_EXT, lte_ext, sizeof(lte_ext));

    /* NR5G SA + NSA band preferences: 128-bit. */
    uint8_t nr[16];
    for (int i = 0; i < 8;  i++) nr[i]     = (uint8_t)((nrMaskLow  >> (8 * i)) & 0xFF);
    for (int i = 0; i < 8;  i++) nr[8 + i] = (uint8_t)((nrMaskHigh >> (8 * i)) & 0xFF);
    o = put_tlv(tlvs, o, TLV_NR5G_SA_BAND_PREF,  nr, sizeof(nr));
    o = put_tlv(tlvs, o, TLV_NR5G_NSA_BAND_PREF, nr, sizeof(nr));

    return qmi_nas_build_req(out, QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE_REQ, tlvs, o);
}

size_t qmi_nas_build_reset_band_pref(uint8_t *out) {
    return qmi_nas_build_set_band_pref(out,
                                       0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
                                       0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
}

size_t qmi_nas_build_lte_cell_lock(uint8_t *out, uint32_t earfcn, uint32_t pci) {
    /*
     * Qualcomm's NAS PCI-lock request (vendor-specific) payload layout observed
     * in QXDM/NSG traces on SDX5x basebands:
     *   TLV 0x01:
     *     uint8   action       (1 = lock, 0 = unlock)
     *     uint32  earfcn
     *     uint16  pci
     *     uint8   pci_valid    (1)
     *     uint8   earfcn_valid (1)
     *     uint32  timeout_ms   (0 = permanent until cleared)
     *
     * Per-device baseband tweaks may be needed; see README.
     */
    uint8_t tlvs[32];
    size_t  o = 0;
    uint8_t payload[13];
    size_t  p = 0;
    payload[p++] = 0x01;                              /* action = lock */
    p = put_u32(payload, p, earfcn);
    p = put_u16(payload, p, (uint16_t)pci);
    payload[p++] = 0x01;                              /* pci_valid */
    payload[p++] = 0x01;                              /* earfcn_valid */
    p = put_u32(payload, p, 0x00000000);              /* no timeout */
    o = put_tlv(tlvs, o, 0x01, payload, p);

    return qmi_nas_build_req(out, QMI_NAS_SET_LTE_PCI_LOCK, tlvs, o);
}

size_t qmi_nas_build_lte_cell_unlock(uint8_t *out) {
    uint8_t tlvs[8];
    size_t  o = 0;
    uint8_t payload[1] = { 0x00 };                    /* action = unlock */
    o = put_tlv(tlvs, o, 0x01, payload, 1);
    return qmi_nas_build_req(out, QMI_NAS_CLEAR_LTE_PCI_LOCK, tlvs, o);
}

int qmi_parse_response(const uint8_t *frame, size_t len, uint16_t *out_error) {
    if (out_error) *out_error = 0;
    /* Minimum frame: DIAG header (4) + QMI header (9) + TLV 0x02 (7 bytes). */
    if (len < 4 + 9 + 7) return -1;

    /* Skip DIAG header (4 bytes) and QMI header (svc,client,flag,txid,msgid,len = 9 bytes). */
    size_t off = 4 + 9;
    /* Walk TLVs looking for type 0x02 (result). */
    while (off + 3 <= len) {
        uint8_t  type = frame[off];
        uint16_t vlen = (uint16_t)frame[off + 1] | ((uint16_t)frame[off + 2] << 8);
        off += 3;
        if (off + vlen > len) return -2;
        if (type == 0x02 && vlen >= 4) {
            uint16_t result = (uint16_t)frame[off] | ((uint16_t)frame[off + 1] << 8);
            uint16_t err    = (uint16_t)frame[off + 2] | ((uint16_t)frame[off + 3] << 8);
            if (out_error) *out_error = err;
            return result;
        }
        off += vlen;
    }
    return -3;
}
