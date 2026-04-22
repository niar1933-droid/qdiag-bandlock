package com.qdiag.bandlock.root

import android.content.Intent
import android.os.IBinder
import android.util.Log
import com.qdiag.bandlock.diag.DiagNative
import com.topjohnwu.superuser.ipc.RootService

/**
 * libsu-backed root service. Runs in a separate UID=0 process and exposes the
 * [IDiagRoot] AIDL that the main app calls. All /dev/diag traffic happens here.
 */
class DiagRootService : RootService() {
    override fun onBind(intent: Intent): IBinder = Impl()

    private class Impl : IDiagRoot.Stub() {
        override fun openDiag(): Boolean = DiagNative.openDiag()
        override fun openDiagEx(): Int = DiagNative.openDiagEx()
        override fun closeDiag() = DiagNative.closeDiag()
        override fun isDiagOpen(): Boolean = DiagNative.isDiagOpen()

        override fun sendDiagRaw(request: ByteArray): ByteArray? = try {
            DiagNative.sendRaw(request)
        } catch (t: Throwable) {
            Log.e(TAG, "sendDiagRaw failed", t); null
        }

        override fun setBandPreference(
            lteBandMaskLow: Long, lteBandMaskHigh: Long,
            nrBandMaskLow: Long,  nrBandMaskHigh: Long,
        ): Int = DiagNative.setBandPref(lteBandMaskLow, lteBandMaskHigh, nrBandMaskLow, nrBandMaskHigh)

        override fun setLteCellLock(earfcn: Int, pci: Int): Int = DiagNative.setLteCellLock(earfcn, pci)
        override fun clearLteCellLock(): Int = DiagNative.clearLteCellLock()
        override fun resetBandPreference(): Int = DiagNative.resetBandPref()
        override fun efsPutItemFile(path: String, value: ByteArray): Int =
            DiagNative.efsPutItemFile(path, value)
        override fun efsGetItemFile(path: String): ByteArray? =
            DiagNative.efsGetItemFile(path)
        override fun efsUnlink(path: String): Int = DiagNative.efsUnlink(path)
        override fun drainDiagLog(): String = DiagNative.drainLog()

        override fun startSniffer(): Boolean = DiagNative.snifferStart()
        override fun stopSniffer() = DiagNative.snifferStop()
        override fun isSnifferRunning(): Boolean = DiagNative.snifferIsRunning()
        override fun drainSniffer(): ByteArray = DiagNative.snifferDrain()
        override fun saveSniffer(path: String): Int = DiagNative.snifferSaveTo(path)
        override fun snifferTotal(): Long = DiagNative.snifferTotal()
        override fun snifferDropped(): Long = DiagNative.snifferDropped()

        override fun qrtrOpen(): Int = DiagNative.qrtrOpen()
        override fun qrtrClose() = DiagNative.qrtrClose()
        override fun qrtrIsOpen(): Boolean = DiagNative.qrtrIsOpen()
        override fun qrtrEnumerate(): String = DiagNative.qrtrEnumerate()
        override fun qrtrSetBandPref(lteLow: Long, lteHigh: Long, nrLow: Long, nrHigh: Long): Int =
            DiagNative.qrtrSetBandPref(lteLow, lteHigh, nrLow, nrHigh)

        override fun qmuxOpen(path: String?): Int = DiagNative.qmuxOpen(path)
        override fun qmuxClose() = DiagNative.qmuxClose()
        override fun qmuxIsOpen(): Boolean = DiagNative.qmuxIsOpen()
        override fun qmuxSockPath(): String = DiagNative.qmuxSockPath()
        override fun qmuxAllocClient(service: Int): Int = DiagNative.qmuxAllocClient(service)
        override fun qmuxSetBandPref(lteLow: Long, lteHigh: Long, nrLow: Long, nrHigh: Long): Int =
            DiagNative.qmuxSetBandPref(lteLow, lteHigh, nrLow, nrHigh)

        override fun qrtrProbeCellLock(earfcn: Int, pci: Int): Int =
            DiagNative.qrtrProbeCellLock(earfcn, pci)
        override fun qrtrClearCellLock(): Int = DiagNative.qrtrClearCellLock()
        override fun qmuxProbeCellLock(earfcn: Int, pci: Int): Int =
            DiagNative.qmuxProbeCellLock(earfcn, pci)
    }

    companion object { private const val TAG = "DiagRootService" }
}
