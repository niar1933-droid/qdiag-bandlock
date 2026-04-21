package com.qdiag.bandlock.ui

import android.app.Application
import android.os.Environment
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.qdiag.bandlock.qmi.BandMask
import com.qdiag.bandlock.root.IDiagRoot
import com.qdiag.bandlock.root.RootClient
import com.qdiag.bandlock.telephony.CellObserver
import com.qdiag.bandlock.telephony.Rat
import com.qdiag.bandlock.telephony.RatSnapshot
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
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

data class UiState(
    val rootStatus: RootClient.Status = RootClient.Status.Unknown,
    val diagOpen: Boolean = false,
    val lteMask: BandMask = BandMask.ALL,
    val nrMask: BandMask = BandMask.ALL,
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
)

class MainViewModel(app: Application) : AndroidViewModel(app) {
    private val observer = CellObserver(app)
    private val _ui = MutableStateFlow(UiState())
    val ui: StateFlow<UiState> = _ui.asStateFlow()

    private var snifferSeq: Long = 0

    init {
        RootClient.checkRoot()
        viewModelScope.launch {
            RootClient.status.collect { s -> _ui.value = _ui.value.copy(rootStatus = s) }
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
    }

    fun bindRootService() = RootClient.bind(getApplication())
    fun refreshCells() = observer.refresh()

    fun toggleLte(band: Int, enabled: Boolean) {
        _ui.value = _ui.value.copy(lteMask = _ui.value.lteMask.with(band, enabled))
    }
    fun toggleNr(band: Int, enabled: Boolean) {
        _ui.value = _ui.value.copy(nrMask = _ui.value.nrMask.with(band, enabled))
    }
    fun setAllLte(on: Boolean) {
        _ui.value = _ui.value.copy(lteMask = if (on) BandMask.ALL else BandMask.NONE)
    }
    fun setAllNr(on: Boolean) {
        _ui.value = _ui.value.copy(nrMask = if (on) BandMask.ALL else BandMask.NONE)
    }

    fun setLockInput(earfcn: String, pci: String) {
        _ui.value = _ui.value.copy(lockEarfcn = earfcn, lockPci = pci)
    }

    private fun withApi(block: suspend (IDiagRoot) -> Unit) {
        val api = RootClient.api.value
        if (api == null) {
            appendLog("Root service not bound. Tap 'Connect root service' first.")
            return
        }
        viewModelScope.launch {
            _ui.value = _ui.value.copy(busy = true)
            try {
                withContext(Dispatchers.IO) { block(api) }
            } catch (t: Throwable) {
                Log.e("MainVm", "root call failed", t)
                appendLog("Error: ${t.message}")
            } finally {
                _ui.value = _ui.value.copy(busy = false, diagOpen = api.isDiagOpen)
            }
        }
    }

    fun openDiag()  = withApi { appendLog(if (it.openDiag()) "DIAG opened" else "DIAG open FAILED (check /dev/diag permissions)") }
    fun closeDiag() = withApi { it.closeDiag(); appendLog("DIAG closed") }

    fun applyBandPreference() = withApi { api ->
        if (!api.isDiagOpen) api.openDiag()
        val s = _ui.value
        val rc = api.setBandPreference(s.lteMask.low, s.lteMask.high, s.nrMask.low, s.nrMask.high)
        appendLog("setBandPreference(LTE=${s.lteMask.enabledBands()}, NR=${s.nrMask.enabledBands()}) -> rc=0x${rc.toString(16)}")
    }

    fun resetBandPreference() = withApi { api ->
        if (!api.isDiagOpen) api.openDiag()
        val rc = api.resetBandPreference()
        _ui.value = _ui.value.copy(lteMask = BandMask.ALL, nrMask = BandMask.ALL)
        appendLog("resetBandPreference -> rc=0x${rc.toString(16)}")
    }

    fun applyCellLock() = withApi { api ->
        if (!api.isDiagOpen) api.openDiag()
        val s = _ui.value
        val earfcn = s.lockEarfcn.toIntOrNull()
        val pci = s.lockPci.toIntOrNull()
        if (earfcn == null || pci == null) { appendLog("EARFCN and PCI must be integers"); return@withApi }
        val rc = api.setLteCellLock(earfcn, pci)
        appendLog("setLteCellLock(EARFCN=$earfcn, PCI=$pci) -> rc=0x${rc.toString(16)}")
    }

    fun clearCellLock() = withApi { api ->
        if (!api.isDiagOpen) api.openDiag()
        val rc = api.clearLteCellLock()
        appendLog("clearLteCellLock -> rc=0x${rc.toString(16)}")
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
        appendLog(if (ok) "Sniffer started" else "Sniffer start FAILED")
    }

    fun stopSniffer() = withApi { api ->
        api.stopSniffer()
        _ui.value = _ui.value.copy(snifferRunning = api.isSnifferRunning)
        appendLog("Sniffer stopped")
    }

    fun clearSnifferUi() {
        _ui.value = _ui.value.copy(snifferFrames = emptyList())
    }

    fun saveSnifferToFile() = withApi { api ->
        val dir = File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
            "QDiag",
        ).apply { mkdirs() }
        val ts = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
            .format(java.util.Date())
        val path = File(dir, "qdiag_capture_$ts.bin").absolutePath
        val n = api.saveSniffer(path)
        if (n < 0) {
            appendLog("saveSniffer($path) -> errno=${-n}")
        } else {
            _ui.value = _ui.value.copy(snifferSavePath = path)
            appendLog("Saved $n frames → $path")
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
