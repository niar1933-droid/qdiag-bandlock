package com.qdiag.bandlock.ui

import android.app.Application
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.qdiag.bandlock.qmi.BandMask
import com.qdiag.bandlock.root.IDiagRoot
import com.qdiag.bandlock.root.RootClient
import com.qdiag.bandlock.telephony.CellObserver
import com.qdiag.bandlock.telephony.CellSnapshot
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

sealed interface UiEvent {
    data class Toast(val message: String) : UiEvent
}

data class UiState(
    val rootStatus: RootClient.Status = RootClient.Status.Unknown,
    val diagOpen: Boolean = false,
    val lteMask: BandMask = BandMask.ALL,
    val nrMask: BandMask = BandMask.ALL,
    val cells: List<CellSnapshot> = emptyList(),
    val lockEarfcn: String = "",
    val lockPci: String = "",
    val log: String = "",
    val busy: Boolean = false,
)

class MainViewModel(app: Application) : AndroidViewModel(app) {
    private val observer = CellObserver(app)
    private val _ui = MutableStateFlow(UiState())
    val ui: StateFlow<UiState> = _ui.asStateFlow()

    init {
        RootClient.checkRoot()
        viewModelScope.launch {
            RootClient.status.collect { s -> _ui.value = _ui.value.copy(rootStatus = s) }
        }
        viewModelScope.launch {
            observer.cells.collect { c -> _ui.value = _ui.value.copy(cells = c) }
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

    private fun appendLog(line: String) {
        val ts = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
            .format(java.util.Date())
        _ui.value = _ui.value.copy(log = "[$ts] $line\n" + _ui.value.log.take(8192))
    }
}
