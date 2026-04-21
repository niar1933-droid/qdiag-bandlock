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
    @JvmStatic external fun drainLog(): String

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
