package com.qdiag.bandlock.root;

interface IDiagRoot {
    /** Open /dev/diag and switch logging to user-space / memory-device mode. */
    boolean openDiag();

    /** Same as openDiag(), but returns 0 on success or -errno on failure. */
    int openDiagEx();

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

    /**
     * Raw EFS2 Put Item File — write `value` into the NV item at `path`
     * (e.g. "/nv/item_files/modem/nr5g/RRC/pci_lock_info"). This is the
     * mechanism used by Network Signal Guru / QXDM for PCI / cell / band
     * lock. Exact byte layout of `value` is per-NV-item; capture one with
     * the sniffer to learn the layout for your modem.
     *
     * Returns 0 on success, negative on transport failure, or
     * (0x20000 | diag_errno) if the modem rejected the write.
     */
    int efsPutItemFile(String path, in byte[] value);

    /** EFS2 Get Item File — read current NV item bytes. Null on error. */
    @nullable byte[] efsGetItemFile(String path);

    /** EFS2 Unlink — delete the NV item (equivalent to clearing a lock).
     * 0 on success, negative on transport failure, (0x20000|err) on modem reject. */
    int efsUnlink(String path);

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

    /* ---------- QRTR transport (AF_QIPCRTR socket, no /dev/diag) ---------- */

    /** Open AF_QIPCRTR socket. 0 on success, -(2000+errno) on failure. */
    int qrtrOpen();
    void qrtrClose();
    boolean qrtrIsOpen();

    /** Multi-line dump of all QMI services currently advertised on QRTR. */
    String qrtrEnumerate();

    /** Apply LTE+NR band preference via QMI NAS over QRTR (no /dev/diag). */
    int qrtrSetBandPref(long lteLow, long lteHigh, long nrLow, long nrHigh);
}
