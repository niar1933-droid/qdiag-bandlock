#ifndef QDIAG_QMI_NAS_H
#define QDIAG_QMI_NAS_H

#include <stddef.h>
#include <stdint.h>

/*
 * QMI service IDs we speak to over DIAG. The NAS service handles
 * "system selection preference" (band/network type) and the
 * Qualcomm-internal cell-lock messages used by QXDM / NSG.
 */
#define QMI_SVC_NAS 0x03

/* QMI_NAS message IDs (public portion). */
#define QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE_REQ 0x0033
#define QMI_NAS_GET_SYSTEM_SELECTION_PREFERENCE_REQ 0x0034

/*
 * Known Qualcomm-internal / RPC opcodes observed in QXDM traces for
 * explicit PCI + EARFCN LTE cell lock. These numbers differ across modem
 * generations and are only correct for MDM9x07 / SDX5x / modern SDX
 * basebands; adjust for your target device if it does not accept them.
 */
#define QMI_NAS_LTE_CPHY_CA_IND      0x756F  /* CA/serving-cell info indication */
#define QMI_NAS_SET_LTE_PCI_LOCK     0x4567  /* device-specific; see README */
#define QMI_NAS_CLEAR_LTE_PCI_LOCK   0x4568

/* Sizes large enough for any request we construct here. */
#define QMI_MAX_REQUEST 512
#define QMI_MAX_RESPONSE 4096

/* Build a QMI-over-DIAG SUBSYS_CMD frame for the NAS service.
 *
 * out  - destination buffer (size >= QMI_MAX_REQUEST)
 * msg_id - QMI message id (little-endian 16-bit)
 * tlvs - already-encoded TLV payload bytes
 * tlvs_len - length of the TLV payload
 *
 * Returns total length of built frame (DIAG header + QMI header + TLVs),
 * or 0 on error.
 */
size_t qmi_nas_build_req(uint8_t *out,
                         uint16_t msg_id,
                         const uint8_t *tlvs,
                         size_t tlvs_len);

/* Build a QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE_REQ with LTE + NR band
 * bitmasks. lteMask is 64-bit (bands 1..64); nrMask is 64-bit (n1..n64).
 * out must be >= QMI_MAX_REQUEST. Returns frame length. */
size_t qmi_nas_build_set_band_pref(uint8_t *out,
                                   uint64_t lteMaskLow, uint64_t lteMaskHigh,
                                   uint64_t nrMaskLow,  uint64_t nrMaskHigh);

/* Reset band preference = "all bands allowed". */
size_t qmi_nas_build_reset_band_pref(uint8_t *out);

/* Build an LTE PCI + EARFCN lock frame (Qualcomm-specific).
 * See README for per-baseband caveats. Returns frame length. */
size_t qmi_nas_build_lte_cell_lock(uint8_t *out, uint32_t earfcn, uint32_t pci);

/* Build an LTE PCI-lock clear frame. */
size_t qmi_nas_build_lte_cell_unlock(uint8_t *out);

/* Parse a QMI response; returns the QMI result value (0 = success,
 * 1 = failure) or negative on malformed frame. Also fills out_error with
 * the QMI extended error code when result != 0. */
int qmi_parse_response(const uint8_t *frame, size_t len, uint16_t *out_error);

#endif
