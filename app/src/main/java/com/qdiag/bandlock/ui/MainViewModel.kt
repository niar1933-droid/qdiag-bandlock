package com.qdiag.bandlock.ui

import android.app.Application
import android.os.Environment
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.qdiag.bandlock.qmi.BandCatalog
import com.qdiag.bandlock.qmi.BandMask
import com.qdiag.bandlock.root.IDiagRoot
import com.qdiag.bandlock.root.RootClient
import com.qdiag.bandlock.telephony.CellObserver
import com.qdiag.bandlock.telephony.Rat
import com.qdiag.bandlock.telephony.RatSnapshot
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/* ----------------- sniffer frame types ----------------- */

data class SniffFrame(
    val seq: Long,
    val tsMs: Long,
    val bytes: ByteArray,
) {
    val subsysLabel: String
        get() {
            if (bytes.isEmpty()) return "empty"
            val code = bytes[0].toInt() and 0xFF
            return when (code) {
                0x4B -> {
                    val subsys = if (bytes.size >= 2) bytes[1].toInt() and 0xFF else 0
                    "SUBSYS_CMD/0x%02X".format(subsys)
                }
                0x10 -> "LOG"
                0x79 -> "EVENT"
                0x92 -> "F3_MSG"
                else -> "CMD_0x%02X".format(code)
            }
        }

    fun hexPreview(max: Int = 24): String {
        val n = minOf(bytes.size, max)
        val sb = StringBuilder(n * 3)
        for (i in 0 until n) {
            sb.append("%02X".format(bytes[i].toInt() and 0xFF))
            if (i != n - 1) sb.append(' ')
        }
        if (bytes.size > max) sb.append(" …")
        return sb.toString()
    }
}

private const val MAX_SNIFFER_UI_FRAMES = 500

private fun defaultCatalogLteMask(): BandMask =
    BandCatalog.LTE.fold(BandMask.NONE) { acc, e -> acc.with(e.number, true) }

private fun defaultCatalogNrMask(): BandMask =
    BandCatalog.NR.fold(BandMask.NONE) { acc, e -> acc.with(e.number, true) }

data class UiState(
    val rootStatus: RootClient.Status = RootClient.Status.Unknown,
    val apiBound: Boolean = false,
    val diagOpen: Boolean = false,
    val lteMask: BandMask = defaultCatalogLteMask(),
    val nrMask: BandMask = defaultCatalogNrMask(),
    val snapshots: Map<Rat, RatSnapshot> = emptyMap(),
    val lockEarfcn: String = "",
    val lockPci: String = "",
    val log: String = "",
    val busy: Boolean = false,

    /* sniffer */
    val snifferRunning: Boolean = false,
    val snifferFrames: List<SniffFrame> = emptyList(),
    val snifferTotal: Long = 0,
    val snifferDropped: Long = 0,
    val snifferSavePath: String? = null,

    /* NV inspector */
    val nvPath: String = "/nv/item_files/modem/mmode/lte_bandpref",
    val nvHex: String = "",
    val nvWriteHex: String = "",
)

/** Well-known NV item paths exposed in the inspector UI. */
object NvPresets {
    val ALL: List<Pair<String, String>> = listOf(
        "LTE band pref (1..64)"         to "/nv/item_files/modem/mmode/lte_bandpref",
        "LTE band pref (65..256)"       to "/nv/item_files/modem/mmode/lte_bandpref_extn_65_256",
        "NR SA band pref"               to "/nv/item_files/modem/mmode/nr_band_pref",
        "NR NSA band pref"              to "/nv/item_files/modem/mmode/nr_nsa_band_pref",
        "TDSCDMA band pref"             to "/nv/item_files/modem/mmode/tds_bandpref",
        "LTE cell_restrict_opt_params"  to "/nv/item_files/modem/lte/rrc/efs/cell_restrict_opt_params",
        "LTE camp_band_earfcn"          to "/nv/item_files/modem/lte/ML1/camp_band_earfcn",
        "LTE CSP"                       to "/nv/item_files/modem/lte/rrc/csp",
        "NR5G pci_lock_info"            to "/nv/item_files/modem/nr5g/RRC/pci_lock_info",
        "NR5G earfcn_lock"              to "/nv/item_files/modem/nr5g/RRC/earfcn_lock",
        "WCDMA freq lock"               to "/nv/item_files/wcdma/rrc/wcdma_rrc_freq_lock_item",
        "WCDMA PSC lock"                to "/nv/item_files/wcdma/rrc/wcdma_rrc_enable_psc_lock",
    )
}

class MainViewModel(app: Application) : AndroidViewModel(app) {
    private val observer = CellObserver(app)
    private val _ui = MutableStateFlow(UiState())
    val ui: StateFlow<UiState> = _ui.asStateFlow()

    /** One-shot snackbar events. */
    private val _toasts = MutableSharedFlow<String>(
        extraBufferCapacity = 8,
        onBufferOverflow = BufferOverflow.DROP_OLDEST,
    )
    val toasts: SharedFlow<String> = _toasts.asSharedFlow()

    private fun toast(msg: String) {
        _toasts.tryEmit(msg)
        appendLog(msg)
    }

    private var snifferSeq: Long = 0

    /**
     * Decode a return code from the native layer into a short human-readable
     * string. Matches the QDIAG_RC_* sentinels in jni_bridge.c.
     */
    private fun decodeRc(rc: Int): String = when {
        rc == 0 -> "ok (rc=0)"
        rc == -1001 -> "DIAG not open"
        rc == -1002 -> "write failed (EBADF/EACCES?)"
        rc == -1003 -> "read failed"
        rc == -1004 -> "timeout (no modem response in 2s — wrong opcode or channel?)"
        rc == -1005 -> "decoder failed"
        rc == -1006 -> "request builder failed"
        rc in -1102..-1002 -> "write failed errno=${-1002 - rc} (${errnoName(-1002 - rc)})"
        rc in -1103..-1003 -> "read failed errno=${-1003 - rc} (${errnoName(-1003 - rc)})"
        rc and 0xFFFF0000.toInt() == 0x10000 -> {
            val qerr = rc and 0xFFFF
            val name = qmiErrName(qerr)
            "QMI error 0x${qerr.toString(16).padStart(4, '0')} ($name)"
        }
        rc in -2010..-2001 -> "QMI parse error (rc=${rc + 2000})"
        else -> "rc=0x${rc.toString(16)}"
    }

    private fun qmiErrName(e: Int): String = when (e) {
        0x0000 -> "NONE"
        0x0001 -> "MALFORMED_MSG"
        0x0002 -> "NO_MEMORY"
        0x0003 -> "INTERNAL — band not supported by modem?"
        0x0004 -> "ABORTED"
        0x0005 -> "CLIENT_IDS_EXHAUSTED"
        0x0006 -> "UNABORTABLE_TRANSACTION"
        0x0007 -> "INVALID_CLIENT_ID"
        0x0008 -> "NO_THRESHOLDS"
        0x0009 -> "INVALID_HANDLE"
        0x000A -> "INVALID_PROFILE"
        0x000B -> "INVALID_PINID"
        0x000C -> "INCORRECT_PIN"
        0x000D -> "NO_NETWORK_FOUND"
        0x000E -> "CALL_FAILED"
        0x000F -> "OUT_OF_CALL"
        0x0010 -> "NOT_PROVISIONED"
        0x0011 -> "MISSING_ARG"
        0x0013 -> "ARG_TOO_LONG"
        0x0016 -> "INVALID_TX_ID"
        0x0017 -> "DEVICE_IN_USE"
        0x0019 -> "OP_NETWORK_UNSUPPORTED"
        0x001A -> "OP_DEVICE_UNSUPPORTED"
        0x001B -> "NO_EFFECT"
        0x001D -> "NO_FREE_PROFILE"
        0x001E -> "INVALID_PDP_TYPE"
        0x001F -> "INVALID_TECHNOLOGY_PREFERENCE"
        0x0025 -> "INVALID_ARG"
        0x0026 -> "INVALID_INDEX"
        0x0027 -> "NO_ENTRY"
        0x0033 -> "NOT_SUPPORTED"
        0x0034 -> "NO_SUBSCRIPTION"
        0x005A -> "UNSUPPORTED_BAND_LTE_NR"
        else -> "UNKNOWN"
    }

    private fun errnoName(e: Int): String = when (e) {
        1 -> "EPERM"; 2 -> "ENOENT"; 5 -> "EIO"; 9 -> "EBADF"; 11 -> "EAGAIN"
        12 -> "ENOMEM"; 13 -> "EACCES"; 14 -> "EFAULT"; 16 -> "EBUSY"; 19 -> "ENODEV"
        22 -> "EINVAL"; 25 -> "ENOTTY"; 32 -> "EPIPE"; 100 -> "ENETDOWN"; 110 -> "ETIMEDOUT"
        else -> "errno $e"
    }

    init {
        RootClient.checkRoot()
        viewModelScope.launch {
            RootClient.status.collect { s ->
                _ui.value = _ui.value.copy(rootStatus = s)
                if (s is RootClient.Status.HasRoot && RootClient.api.value == null) {
                    appendLog("Root granted, binding root service…")
                    RootClient.bind(getApplication())
                }
            }
        }
        viewModelScope.launch {
            RootClient.api.collect { api ->
                _ui.value = _ui.value.copy(apiBound = api != null)
                if (api != null) appendLog("Root service connected.")
            }
        }
        viewModelScope.launch {
            observer.snapshots.collect { c -> _ui.value = _ui.value.copy(snapshots = c) }
        }
        viewModelScope.launch {
            while (isActive) {
                delay(1500)
                if (_ui.value.snifferRunning) pollSniffer()
            }
        }
        /*
         * Real-time telemetry: subscribe to push updates from
         * TelephonyRegistry (modem-driven, ~100-500ms cadence), plus a
         * fast fallback poll for phones where the push channel goes
         * quiet or the OS throttles callbacks.
         */
        observer.startPushUpdates()
        viewModelScope.launch {
            while (isActive) {
                observer.refresh()
                delay(400)
            }
        }
    }

    override fun onCleared() {
        observer.stopPushUpdates()
        super.onCleared()
    }

    fun bindRootService() {
        appendLog("Binding root service…")
        RootClient.checkRoot()
        RootClient.bind(getApplication())
    }
    fun refreshCells() = observer.refresh()

    fun toggleLte(band: Int, enabled: Boolean) {
        _ui.value = _ui.value.copy(lteMask = _ui.value.lteMask.with(band, enabled))
    }
    fun toggleNr(band: Int, enabled: Boolean) {
        _ui.value = _ui.value.copy(nrMask = _ui.value.nrMask.with(band, enabled))
    }
    fun setAllLte(on: Boolean) {
        _ui.value = _ui.value.copy(lteMask = if (on) defaultCatalogLteMask() else BandMask.NONE)
    }
    fun setAllNr(on: Boolean) {
        _ui.value = _ui.value.copy(nrMask = if (on) defaultCatalogNrMask() else BandMask.NONE)
    }

    fun setLockInput(earfcn: String, pci: String) {
        _ui.value = _ui.value.copy(lockEarfcn = earfcn, lockPci = pci)
    }

    private fun withApi(block: suspend (IDiagRoot) -> Unit) {
        val api = RootClient.api.value
        if (api == null) {
            toast("Root service not bound yet. Binding now — retry in a second.")
            RootClient.bind(getApplication())
            return
        }
        viewModelScope.launch {
            _ui.value = _ui.value.copy(busy = true)
            try {
                withContext(Dispatchers.IO) {
                    block(api)
                    val open = try { api.isDiagOpen } catch (_: Throwable) { false }
                    _ui.value = _ui.value.copy(diagOpen = open)
                }
            } catch (t: Throwable) {
                Log.e("MainVm", "root call failed", t)
                toast("Error: ${t.message}")
            } finally {
                _ui.value = _ui.value.copy(busy = false)
            }
        }
    }

    private fun openDiagReason(api: IDiagRoot): String {
        val rc = api.openDiagEx()
        if (rc == 0) return "DIAG opened"
        val e = -rc
        return "DIAG open FAILED: ${errnoName(e)} (errno=$e) — ${openDiagHint(e)}"
    }

    private fun openDiagHint(e: Int): String = when (e) {
        2  -> "нет ноды /dev/diag (ядро без diagchar)"
        13 -> "SELinux/DAC; нужен magiskpolicy + chmod 666 /dev/diag"
        16 -> "устройство занято другим процессом (NSG, qcrild?)"
        19 -> "нет модуля diagchar / устройство не зарегистрировано"
        else -> "см. README раздел SELinux"
    }

    fun openDiag() = withApi { toast(openDiagReason(it)) }
    fun closeDiag() = withApi { it.closeDiag(); toast("DIAG closed") }

    /** Ensure DIAG is open; returns false with a descriptive toast otherwise. */
    private fun ensureDiagOpen(api: IDiagRoot): Boolean {
        if (api.isDiagOpen) return true
        val reason = openDiagReason(api)
        if (reason == "DIAG opened") return true
        toast(reason)
        return false
    }

    fun applyBandPreference() = withApi { api ->
        if (!ensureDiagOpen(api)) return@withApi
        val s = _ui.value
        val rc = api.setBandPreference(s.lteMask.low, s.lteMask.high, s.nrMask.low, s.nrMask.high)
        toast("Apply bands → ${decodeRc(rc)}")
    }

    fun resetBandPreference() = withApi { api ->
        if (!ensureDiagOpen(api)) return@withApi
        val rc = api.resetBandPreference()
        _ui.value = _ui.value.copy(lteMask = defaultCatalogLteMask(), nrMask = defaultCatalogNrMask())
        toast("Reset bands → ${decodeRc(rc)}")
    }

    /** LTE cell lock — NSG-path probe.
     *  DIAG is unavailable on HyperOS (no /dev/diag) and DMS WRITE_NV_ITEM over
     *  QRTR returns ACCESS_DENIED on X70. NSG uses /dev/socket/qmux_radio
     *  (classic qmuxd) to reach a vendor "lockextn" command. This probe tests
     *  whether qmuxd is reachable from our root context; see logcat tag
     *  `qdiag-jni:V` for per-candidate open/errno + CTL GET_VERSION reply. */
    fun applyCellLock() = withApi { api ->
        val s = _ui.value
        val earfcn = s.lockEarfcn.toIntOrNull()
        val pci = s.lockPci.toIntOrNull()
        if (earfcn == null || pci == null) { toast("EARFCN and PCI must be integers"); return@withApi }
        val v = api.qmiVendorProbe()
        val vHint = if (v == 0) {
            "libqmi_client_qmux.so: NOT LOADABLE (dlopen отвергнут — namespace/SELinux)"
        } else {
            val m = v and 0xFFFF
            val names = listOf(
                "init_instance", "init", "send_sync", "send_async",
                "release", "get_port", "get_conn_id",
            )
            val found = names.mapIndexedNotNull { i, n -> if ((m shr i) and 1 == 1) n else null }
            "libqmi_client_qmux.so: LOADED, mask=0x${"%04X".format(m)} [${found.joinToString(",")}]"
        }
        toast("vendor-qmi → $vHint (см. logcat qdiag-jni)")
    }

    fun clearCellLock() = withApi { api ->
        if (!api.qrtrIsOpen()) {
            val orc = api.qrtrOpen()
            if (orc != 0) { toast("QRTR open FAILED rc=$orc"); return@withApi }
        }
        val rc = api.qrtrClearCellLock()
        toast("Clear cell lock → ${decodeRc(rc)}")
    }

    /* --------------------- EFS2-based band preference --------------------- */

    /**
     * Apply LTE+NR band preference by writing the standard Qualcomm NV items
     * via EFS2 Put Item File. This is the same mechanism Network Signal Guru
     * and Qct Modem Capabilities use; it works on all Qualcomm modems from
     * SDX5x through X75 without per-modem opcode tuning.
     *
     * Layouts (documented in Qualcomm 80-V0345 / libqmi / QXDM specs):
     *   lte_bandpref            : 8-byte little-endian bitmask (bit N-1 = band N, bands 1..64)
     *   lte_bandpref_extn_65_256: 24-byte little-endian bitmask (bit N-65 = band N, bands 65..256)
     *   nr_band_pref            : 32-byte little-endian bitmask (bit N-1 = band n(N+1), n1..n256)
     *   nr_nsa_band_pref        : 32-byte bitmask, same layout as nr_band_pref
     */
    /**
     * Apply band preference via QRTR (AF_QIPCRTR socket) — works on kernels
     * WITHOUT `/dev/diag` (e.g. Poco F6 / HyperOS). Opens the QRTR socket on
     * demand, looks up QMI NAS service, sends SET_SYSTEM_SELECTION_PREFERENCE.
     */
    fun applyBandPreferenceQrtr() = withApi { api ->
        if (!api.qrtrIsOpen()) {
            val rc = api.qrtrOpen()
            if (rc != 0) {
                val hint = when (rc) {
                    -2013 -> "EACCES (SELinux запрещает AF_QIPCRTR; см. magiskpolicy ниже)"
                    -2001 -> "EPERM (нет root)"
                    -2097 -> "EAFNOSUPPORT (ядро без CONFIG_QRTR)"
                    else  -> "rc=$rc"
                }
                toast("QRTR open FAILED: $hint")
                return@withApi
            }
            toast("QRTR socket opened")
        }
        val s = _ui.value
        val rc = api.qrtrSetBandPref(s.lteMask.low, s.lteMask.high, s.nrMask.low, s.nrMask.high)
        toast("QRTR band pref: LTE=${s.lteMask.enabledBands().size} " +
              "NR=${s.nrMask.enabledBands().size} → ${decodeRc(rc)}")
    }

    /** Enumerate QMI services on QRTR (diagnostic). Logs to command log. */
    fun enumerateQrtr() = withApi { api ->
        if (!api.qrtrIsOpen()) {
            val rc = api.qrtrOpen()
            if (rc != 0) { toast("QRTR open FAILED rc=$rc"); return@withApi }
        }
        val dump = api.qrtrEnumerate()
        appendLog("--- QRTR services ---\n$dump")
        toast("Enumerated QMI services (см. Command log)")
    }

    /**
     * Apply band preference via QMUX (AF_UNIX `/dev/socket/qmux_radio/ril_ipc`).
     * This path bypasses BOTH `/dev/diag` (absent on HyperOS) AND the QRTR ns
     * daemon (which hides modem QMI services from untrusted UIDs). qmuxd is
     * the standard Qualcomm QMI multiplexer that qcrild itself speaks to.
     */
    fun applyBandPreferenceQmux() = withApi { api ->
        if (!api.qmuxIsOpen()) {
            val rc = api.qmuxOpen(null)
            if (rc != 0) {
                val hint = when (rc) {
                    -(3000 + 2)   -> "ENOENT (сокет не найден — qmuxd не запущен?)"
                    -(3000 + 13)  -> "EACCES (SELinux режет; нужен magiskpolicy)"
                    -(3000 + 111) -> "ECONNREFUSED (qmuxd не слушает; пробуем другой путь)"
                    else          -> "rc=$rc"
                }
                toast("QMUX open FAILED: $hint")
                appendLog("qmux_open rc=$rc")
                return@withApi
            }
            appendLog("qmux opened at ${api.qmuxSockPath()}")
        }
        val s = _ui.value
        val rc = api.qmuxSetBandPref(s.lteMask.low, s.lteMask.high, s.nrMask.low, s.nrMask.high)
        appendLog("qmux_set_band_pref rc=$rc (LTE=${s.lteMask.enabledBands()} NR=${s.nrMask.enabledBands()})")
        toast("QMUX band pref: LTE=${s.lteMask.enabledBands().size} " +
              "NR=${s.nrMask.enabledBands().size} → ${decodeRc(rc)}")
    }

    fun applyBandPreferenceEfs() = withApi { api ->
        if (!ensureDiagOpen(api)) return@withApi
        val s = _ui.value
        val ltePayload = longToLeBytes(s.lteMask.low)                     // 8 bytes, bands 1..64
        val rcLte = api.efsPutItemFile(EFS_LTE_BANDPREF, ltePayload)
        toast("EFS lte_bandpref → ${decodeRc(rcLte)}")

        /* NR band preference is 32 bytes (bits 0..255) */
        val nrPayload = ByteArray(32)
        for (b in 1..64) if (s.nrMask.contains(b)) setBit(nrPayload, b - 1)
        val rcNr = api.efsPutItemFile(EFS_NR_BAND_PREF, nrPayload)
        toast("EFS nr_band_pref → ${decodeRc(rcNr)}")
    }

    private fun longToLeBytes(v: Long): ByteArray {
        val out = ByteArray(8)
        for (i in 0 until 8) out[i] = ((v ushr (i * 8)) and 0xFF).toByte()
        return out
    }

    private fun setBit(buf: ByteArray, bit: Int) {
        if (bit < 0 || bit >= buf.size * 8) return
        buf[bit / 8] = (buf[bit / 8].toInt() or (1 shl (bit % 8))).toByte()
    }

    /* --------------------- NV inspector --------------------- */

    fun setNvPath(path: String) { _ui.value = _ui.value.copy(nvPath = path) }
    fun setNvWriteHex(hex: String) { _ui.value = _ui.value.copy(nvWriteHex = hex) }

    fun readNv() = withApi { api ->
        if (!ensureDiagOpen(api)) return@withApi
        val path = _ui.value.nvPath.trim()
        if (path.isEmpty()) { toast("NV path is empty"); return@withApi }
        val bytes = try { api.efsGetItemFile(path) } catch (t: Throwable) {
            toast("Read error: ${t.message}"); null
        }
        if (bytes == null) {
            toast("Read FAILED (item missing, modem rejected, or DIAG closed)")
            _ui.value = _ui.value.copy(nvHex = "")
        } else {
            val hex = bytes.joinToString(" ") { "%02X".format(it.toInt() and 0xFF) }
            _ui.value = _ui.value.copy(nvHex = "(${bytes.size}B) $hex")
            toast("Read ${bytes.size} B from $path")
        }
    }

    fun deleteNv() = withApi { api ->
        if (!ensureDiagOpen(api)) return@withApi
        val path = _ui.value.nvPath.trim()
        if (path.isEmpty()) { toast("NV path is empty"); return@withApi }
        val rc = api.efsUnlink(path)
        toast("Unlink $path → ${decodeRc(rc)}")
    }

    fun writeNv() = withApi { api ->
        if (!ensureDiagOpen(api)) return@withApi
        val path = _ui.value.nvPath.trim()
        val bytes = parseHex(_ui.value.nvWriteHex)
        if (path.isEmpty() || bytes == null) {
            toast("Invalid path or hex (use e.g. 01 02 FF)")
            return@withApi
        }
        val rc = api.efsPutItemFile(path, bytes)
        toast("Write ${bytes.size}B → $path : ${decodeRc(rc)}")
    }

    private fun parseHex(s: String): ByteArray? {
        val cleaned = s.replace(Regex("[^0-9A-Fa-f]"), "")
        if (cleaned.isEmpty() || cleaned.length % 2 != 0) return null
        val out = ByteArray(cleaned.length / 2)
        for (i in out.indices) {
            out[i] = cleaned.substring(i * 2, i * 2 + 2).toInt(16).toByte()
        }
        return out
    }

    companion object {
        const val EFS_LTE_BANDPREF     = "/nv/item_files/modem/mmode/lte_bandpref"
        const val EFS_LTE_BANDPREF_EXT = "/nv/item_files/modem/mmode/lte_bandpref_extn_65_256"
        const val EFS_NR_BAND_PREF     = "/nv/item_files/modem/mmode/nr_band_pref"
        const val EFS_NR_NSA_BAND_PREF = "/nv/item_files/modem/mmode/nr_nsa_band_pref"
    }

    /* --------------------- DIAG sniffer --------------------- */

    fun startSniffer() = withApi { api ->
        if (!api.isDiagOpen) api.openDiag()
        val ok = api.startSniffer()
        snifferSeq = 0
        _ui.value = _ui.value.copy(
            snifferRunning = ok && api.isSnifferRunning,
            snifferFrames = emptyList(),
        )
        toast(if (ok) "Sniffer started" else "Sniffer start FAILED (DIAG not open?)")
    }

    fun stopSniffer() = withApi { api ->
        api.stopSniffer()
        _ui.value = _ui.value.copy(snifferRunning = api.isSnifferRunning)
        toast("Sniffer stopped")
    }

    fun clearSnifferUi() {
        _ui.value = _ui.value.copy(snifferFrames = emptyList())
    }

    fun saveSnifferToFile() = withApi { api ->
        val dir = File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
            "FoxikNetwork",
        ).apply { mkdirs() }
        val ts = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
            .format(java.util.Date())
        val path = File(dir, "qdiag_capture_$ts.bin").absolutePath
        val n = api.saveSniffer(path)
        if (n < 0) {
            toast("saveSniffer failed: errno=${-n}")
        } else {
            _ui.value = _ui.value.copy(snifferSavePath = path)
            toast("Saved $n bytes → $path")
        }
    }

    private suspend fun pollSniffer() {
        val api = RootClient.api.value ?: return
        val raw = try {
            withContext(Dispatchers.IO) { api.drainSniffer() }
        } catch (t: Throwable) {
            Log.w("MainVm", "drainSniffer failed", t); return
        }
        val total = try { api.snifferTotal() } catch (_: Throwable) { 0L }
        val dropped = try { api.snifferDropped() } catch (_: Throwable) { 0L }
        val running = try { api.isSnifferRunning } catch (_: Throwable) { false }

        val parsed = parseSnifferStream(raw)
        if (parsed.isNotEmpty() || total != _ui.value.snifferTotal) {
            val merged = (_ui.value.snifferFrames + parsed)
                .takeLast(MAX_SNIFFER_UI_FRAMES)
            _ui.value = _ui.value.copy(
                snifferFrames = merged,
                snifferTotal = total,
                snifferDropped = dropped,
                snifferRunning = running,
            )
        }
    }

    private fun parseSnifferStream(data: ByteArray): List<SniffFrame> {
        if (data.isEmpty()) return emptyList()
        val out = mutableListOf<SniffFrame>()
        val bb = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        val now = System.currentTimeMillis()
        while (bb.remaining() >= 4) {
            val len = bb.int
            if (len < 0 || len > bb.remaining()) break
            val frame = ByteArray(len)
            bb.get(frame)
            out += SniffFrame(seq = ++snifferSeq, tsMs = now, bytes = frame)
        }
        return out
    }

    private fun appendLog(line: String) {
        val ts = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
            .format(java.util.Date())
        _ui.value = _ui.value.copy(log = "[$ts] $line\n" + _ui.value.log.take(8192))
    }
}
