package com.qdiag.bandlock.diag

/**
 * JNI facade for the native DIAG / QMI_NAS module.
 *
 * These calls MUST be invoked from a process that has CAP_SYS_ADMIN and read/write
 * on /dev/diag — i.e. the libsu root service (see [com.qdiag.bandlock.root.DiagRootService]).
 * Calling from the main app process will fail with `open /dev/diag: permission denied`.
 */
object DiagNative {
    init {
        System.loadLibrary("qdiag_native")
    }

    @JvmStatic external fun openDiag(): Boolean
    /** 0 on success, or -errno on failure (ENOENT/EACCES/ENODEV/EBUSY/…). */
    @JvmStatic external fun openDiagEx(): Int
    @JvmStatic external fun closeDiag()
    @JvmStatic external fun isDiagOpen(): Boolean

    /** Send a pre-built DIAG/QMI payload and return the HDLC-decoded response bytes. */
    @JvmStatic external fun sendRaw(request: ByteArray): ByteArray?

    /** @return 0 on success, 0x1xxxx = QMI error code, negative = transport failure. */
    @JvmStatic external fun setBandPref(
        lteMaskLow: Long, lteMaskHigh: Long,
        nrMaskLow: Long,  nrMaskHigh: Long,
    ): Int

    @JvmStatic external fun resetBandPref(): Int
    @JvmStatic external fun setLteCellLock(earfcn: Int, pci: Int): Int
    @JvmStatic external fun clearLteCellLock(): Int

    /**
     * Raw EFS2 Put Item File. Writes `value` into the NV item at `path`
     * (e.g. "/nv/item_files/modem/nr5g/RRC/pci_lock_info"). Used to
     * implement NSG-style PCI / cell / band lock once the exact byte
     * layouts have been captured via the DIAG sniffer.
     *
     * @return 0 on success; negative on transport failure;
     *         (0x20000 | diag_errno) if the modem rejected the write.
     */
    @JvmStatic external fun efsPutItemFile(path: String, value: ByteArray): Int

    /**
     * Raw EFS2 Get Item File. Reads the current bytes stored at `path`.
     * Returns null on transport failure or non-zero diag_errno, otherwise
     * the raw NV-item bytes. Useful for inspecting current lock state
     * before overwriting.
     */
    @JvmStatic external fun efsGetItemFile(path: String): ByteArray?

    /**
     * Raw EFS2 Unlink — delete the NV item at `path`. Equivalent to
     * clearing a lock (modem falls back to default behaviour).
     *
     * @return 0 on success; negative on transport failure;
     *         (0x20000 | diag_errno) if the modem rejected.
     */
    @JvmStatic external fun efsUnlink(path: String): Int

    @JvmStatic external fun drainLog(): String

    /* ---------- QRTR transport (AF_QIPCRTR, no /dev/diag needed) ---------- */

    /** Open AF_QIPCRTR socket. Returns 0 on success; negative on error.
     *  `-(2000 + errno)` encodes the underlying errno (e.g. -2013 = EACCES). */
    @JvmStatic external fun qrtrOpen(): Int
    @JvmStatic external fun qrtrClose()
    @JvmStatic external fun qrtrIsOpen(): Boolean

    /** Returns a multi-line dump of all QMI services currently advertised on QRTR. */
    @JvmStatic external fun qrtrEnumerate(): String

    /** Apply LTE+NR band preference via QMI NAS over QRTR (bypasses /dev/diag).
     *  Same return convention as setBandPreference() over DIAG. */
    @JvmStatic external fun qrtrSetBandPref(
        lteLow: Long, lteHigh: Long, nrLow: Long, nrHigh: Long,
    ): Int

    /* ---------- QMUX transport (AF_UNIX /dev/socket/qmux_radio/ril_ipc) ---------- */

    /** Open AF_UNIX socket to qmuxd. `path` may be null for auto-discovery
     *  (tries /dev/socket/qmux_radio/ril_ipc → /dev/socket/qmux_radio → /dev/socket/qmuxd).
     *  Returns 0 on success; `-(3000+errno)` encodes the underlying errno. */
    @JvmStatic external fun qmuxOpen(path: String?): Int
    @JvmStatic external fun qmuxClose()
    @JvmStatic external fun qmuxIsOpen(): Boolean
    @JvmStatic external fun qmuxSockPath(): String

    /** Allocate a QMI client ID on the given service (e.g. 0x03 = NAS). */
    @JvmStatic external fun qmuxAllocClient(service: Int): Int

    /** Apply LTE+NR band preference via QMI NAS over qmuxd (bypasses /dev/diag
     *  and bypasses kernel-ns filtering on HyperOS). */
    @JvmStatic external fun qmuxSetBandPref(
        lteLow: Long, lteHigh: Long, nrLow: Long, nrHigh: Long,
    ): Int

    /** Probe several NV item IDs + layouts for LTE cell lock via
     *  QMI_DMS_WRITE_NV_ITEM over QRTR (bypasses /dev/diag). See logcat
     *  tag qdiag-jni for per-probe detail. */
    @JvmStatic external fun qrtrProbeCellLock(earfcn: Int, pci: Int): Int

    /** Clear any NV-item cell lock set by qrtrProbeCellLock. */
    @JvmStatic external fun qrtrClearCellLock(): Int

    /** NSG-path probe: try every /dev/socket/qmux_radio candidate, log
     *  per-path fd/errno, and if any opens do a CTL GET_VERSION +
     *  GET_CLIENT_ID(NAS) handshake to confirm qmuxd is alive.
     *  Return codes:
     *      0  — qmuxd reachable AND allocated a NAS client id
     *      1  — qmuxd reachable but did not answer GET_CLIENT_ID
     *     -1  — no candidate socket opened at all
     *     -2/-3 — socket opened but peer refused / read failed
     */
    @JvmStatic external fun qmuxProbeCellLock(earfcn: Int, pci: Int): Int

    /* ---------- DIAG sniffer (background HDLC-decoded frame capture) ---------- */

    @JvmStatic external fun snifferStart(): Boolean
    @JvmStatic external fun snifferStop()
    @JvmStatic external fun snifferIsRunning(): Boolean

    /** Drain up to ~64 KiB of pending frames, packed as [u32 len_le][frame bytes] records. */
    @JvmStatic external fun snifferDrain(): ByteArray

    /** Append pending frames to a file (same record format). Returns frames written, or -errno. */
    @JvmStatic external fun snifferSaveTo(path: String): Int

    @JvmStatic external fun snifferTotal(): Long
    @JvmStatic external fun snifferDropped(): Long
}
