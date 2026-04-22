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

    /* ---------- QMUX transport (AF_UNIX /dev/socket/qmux_radio/ril_ipc) ---------- */

    /** Open qmuxd socket. path may be null for auto-discovery.
     *  Returns 0 on success; -(3000+errno) on failure. */
    int qmuxOpen(String path);
    void qmuxClose();
    boolean qmuxIsOpen();

    /** Socket path actually connected to (for diagnostic display). */
    String qmuxSockPath();

    /** Allocate a QMI client ID on the given service (e.g. 0x03 = NAS).
     *  Returns cid (0..255) on success, negative on error. */
    int qmuxAllocClient(int service);

    /** Apply LTE+NR band preference via QMI NAS over qmuxd.
     *  Bypasses /dev/diag AND kernel-ns filtering on HyperOS. */
    int qmuxSetBandPref(long lteLow, long lteHigh, long nrLow, long nrHigh);

    /* ---------- QRTR DMS WRITE_NV probe (Cell Lock without /dev/diag) ------ */

    /** Probe several candidate NV item IDs + payload layouts for LTE cell
     *  lock via QMI_DMS_WRITE_NV_ITEM (msg 0x003D) over QRTR. See logcat
     *  tag qdiag-jni for per-probe result. Returns 0 if at least one
     *  probe succeeded, (0x10000 | 0x003E) if all were rejected, or a
     *  negative QRTR transport error. */
    int qrtrProbeCellLock(int earfcn, int pci);

    /** Clear any NV-item cell lock set by qrtrProbeCellLock. */
    int qrtrClearCellLock();

    /** NSG-path probe: tests qmuxd reachability via /dev/socket/qmux_radio
     *  and variants. Returns 0 if qmuxd answers CTL GET_CLIENT_ID(NAS),
     *  1 if version reply but no client id, -1 if no socket opens at all,
     *  -2/-3 for handshake failures. See logcat tag qdiag-jni. */
    int qmuxProbeCellLock(int earfcn, int pci);

    /** Vendor QMI library probe (NSG path). dlopen /vendor/lib64/libqmi_client_qmux.so
     *  + deps, resolve classic Qualcomm QMI entrypoints. Returns 0 if library
     *  cannot be loaded at all; else 0x10000|symbolMask. See DiagNative kdoc. */
    int qmiVendorProbe();
}
