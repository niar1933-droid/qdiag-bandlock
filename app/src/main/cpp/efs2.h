#ifndef QDIAG_EFS2_H
#define QDIAG_EFS2_H

#include <stddef.h>
#include <stdint.h>

/*
 * EFS2 over DIAG — the actual mechanism Qualcomm tools (QXDM, Network Signal
 * Guru) use to apply PCI / cell / band lock. Confirmed by static analysis of
 * NSG 4.7.14's libqtrun_arch_jni.so which contains strings like:
 *   "EFS2/DIAG Put Item File Request" / "…Response"
 *   "/nv/item_files/modem/nr5g/RRC/pci_lock_info"
 *   "/nv/item_files/modem/lte/rrc/efs/cell_restrict_opt_params"
 *   "/nv/item_files/wcdma/rrc/wcdma_rrc_freq_lock_item"
 *   etc.
 *
 * Wire layout of an EFS2/DIAG Put Item File request:
 *
 *   offset  size  field
 *   ------  ----  -----------------------------------------------------
 *     0     1     DIAG_SUBSYS_CMD               (0x4B)
 *     1     1     DIAG_SUBSYS_FS                (0x13)
 *     2     2     EFS2 op id  (little-endian)   (0x0011 = PUT_ITEM_FILE)
 *     4     2     sequence number (LE)
 *     6     4     data length (LE)              = len(value)
 *    10     4     attribute / permissions (LE)  = 0x00000000 for typical item
 *    14     ..    NUL-terminated utf-8 path     ("/nv/item_files/…\0")
 *    ..     ..    value bytes                   (len(value) bytes)
 *
 * Response has the same op id (0x0011) and contains a 4-byte diag_errno
 * (LE, 0 == success) plus the sequence number for matching.
 */

#define EFS2_OP_HELLO           0x0000
#define EFS2_OP_QUERY           0x0001
#define EFS2_OP_OPEN            0x0002
#define EFS2_OP_CLOSE           0x0003
#define EFS2_OP_READ            0x0004
#define EFS2_OP_WRITE           0x0005
#define EFS2_OP_MKDIR           0x000E
#define EFS2_OP_RMDIR           0x000F
#define EFS2_OP_UNLINK          0x0010
#define EFS2_OP_PUT_ITEM_FILE   0x0011
#define EFS2_OP_GET_ITEM_FILE   0x0012

/* Build an EFS2 Put Item File request frame (pre-HDLC).
 *
 * out       - destination buffer (>= 64 + path_len + val_len)
 * out_cap   - capacity of 'out'
 * path      - NUL-terminated EFS item path (e.g. "/nv/item_files/…/pci_lock_info")
 * value     - raw bytes to store into the NV item
 * value_len - size of 'value'
 *
 * Returns frame length on success, 0 on buffer-too-small.
 */
size_t efs2_build_put_item_file(uint8_t *out, size_t out_cap,
                                const char *path,
                                const uint8_t *value, size_t value_len);

/* Build an EFS2 Get Item File request (read an NV item).
 * Response will contain the item's bytes after the standard header.
 * Returns frame length on success, 0 on error. */
size_t efs2_build_get_item_file(uint8_t *out, size_t out_cap,
                                const char *path);

/* Build an EFS2 Unlink (delete) request. Used to clear a lock by
 * removing the NV item entirely. Returns frame length. */
size_t efs2_build_unlink(uint8_t *out, size_t out_cap,
                         const char *path);

/* Build an EFS2 Hello request (handshake, often required before first
 * data op). Returns frame length. */
size_t efs2_build_hello(uint8_t *out, size_t out_cap);

/* Parse an EFS2 response payload (after HDLC decode + leading DIAG bytes).
 * Extracts op id and the 4-byte diag_errno. Returns 0 on success,
 * -1 on malformed frame. diag_errno == 0 means the modem accepted the op. */
int efs2_parse_response(const uint8_t *frame, size_t len,
                        uint16_t *out_op, int32_t *out_errno);

/* Parse an EFS2 Get Item File response. Copies the item data into
 * out_data (up to out_cap bytes). Returns the number of data bytes
 * on success, or -1 on error / non-zero diag_errno. */
int efs2_parse_get_item_response(const uint8_t *frame, size_t len,
                                 uint8_t *out_data, size_t out_cap);

/* --- High-level NV-item paths observed in NSG 4.7.14 --- */
#define EFS_NV_NR5G_PCI_LOCK        "/nv/item_files/modem/nr5g/RRC/pci_lock_info"
#define EFS_NV_NR5G_EARFCN_LOCK     "/nv/item_files/modem/nr5g/RRC/earfcn_lock"
#define EFS_NV_LTE_CELL_RESTRICT    "/nv/item_files/modem/lte/rrc/efs/cell_restrict_opt_params"
#define EFS_NV_LTE_CAMP_BAND_EARFCN "/nv/item_files/modem/lte/ML1/camp_band_earfcn"
#define EFS_NV_LTE_CSP              "/nv/item_files/modem/lte/rrc/csp"
#define EFS_NV_WCDMA_FREQ_LOCK      "/nv/item_files/wcdma/rrc/wcdma_rrc_freq_lock_item"
#define EFS_NV_WCDMA_PSC_LOCK       "/nv/item_files/wcdma/rrc/wcdma_rrc_enable_psc_lock"
#define EFS_NV_LTE_BANDPREF         "/nv/item_files/modem/mmode/lte_bandpref"
#define EFS_NV_LTE_BANDPREF_EXT     "/nv/item_files/modem/mmode/lte_bandpref_extn_65_256"
#define EFS_NV_NR_BAND_PREF         "/nv/item_files/modem/mmode/nr_band_pref"
#define EFS_NV_NR_NSA_BAND_PREF     "/nv/item_files/modem/mmode/nr_nsa_band_pref"
#define EFS_NV_TDS_BANDPREF         "/nv/item_files/modem/mmode/tds_bandpref"

#endif
