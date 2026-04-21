package com.qdiag.bandlock.root;

interface IDiagRoot {
    /** Open /dev/diag and switch logging to user-space / memory-device mode. */
    boolean openDiag();

    /** Close /dev/diag. */
    void closeDiag();

    /** True if /dev/diag is currently open in the root service. */
    boolean isDiagOpen();

    /**
     * Send a raw DIAG request (already HDLC-wrapped internally if needed) and return the response
     * bytes. Caller provides the raw DIAG payload (e.g. a QMI-over-DIAG SUBSYS_CMD).
     */
    byte[] sendDiagRaw(in byte[] request);

    /**
     * Apply an LTE/NR band preference via QMI_NAS_SET_SYSTEM_SELECTION_PREFERENCE.
     * lteBandMaskLow/High and nrBandMask are 64-bit bitmasks (bit N-1 == band N).
     * Returns the QMI result (0 on success) or negative on transport failure.
     */
    int setBandPreference(long lteBandMaskLow, long lteBandMaskHigh, long nrBandMaskLow, long nrBandMaskHigh);

    /**
     * Lock the device to a specific LTE cell by PCI + EARFCN.
     * Sends a Qualcomm-specific NAS cell-lock command over DIAG.
     * Returns the QMI result (0 on success) or negative on transport failure.
     */
    int setLteCellLock(int earfcn, int pci);

    /** Clear any cell lock applied by setLteCellLock. */
    int clearLteCellLock();

    /** Reset band preference to "all bands". */
    int resetBandPreference();

    /** Drain pending DIAG log messages accumulated since last drain. Returns concatenated hex lines. */
    String drainDiagLog();

    /* ---------- DIAG sniffer (raw HDLC-decoded frame capture) ---------- */

    /** Start the background sniffer thread. /dev/diag must already be open. */
    boolean startSniffer();

    /** Stop the background sniffer thread and clear its buffer on next start. */
    void stopSniffer();

    boolean isSnifferRunning();

    /**
     * Drain pending captured frames as a packed record stream:
     *   [u32 len_le][len bytes of decoded DIAG payload] ...
     * Caller is expected to parse repeatedly on a background thread.
     */
    byte[] drainSniffer();

    /** Append pending frames to a file path (must be world-writable or owned by root). */
    int saveSniffer(String path);

    /** Total frames captured since start (may exceed drained if overflow dropped some). */
    long snifferTotal();
    long snifferDropped();
}
