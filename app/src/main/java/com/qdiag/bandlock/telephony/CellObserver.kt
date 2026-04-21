package com.qdiag.bandlock.telephony

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.telephony.CellInfo
import android.telephony.CellInfoLte
import android.telephony.CellInfoNr
import android.telephony.CellSignalStrengthLte
import android.telephony.CellSignalStrengthNr
import android.telephony.TelephonyManager
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

data class CellSnapshot(
    val rat: String,
    val pci: Int?,
    val earfcnOrArfcn: Int?,
    val band: Int?,
    val cellId: Long?,
    val tac: Int?,
    val mcc: String?,
    val mnc: String?,
    val rsrpDbm: Int?,
    val rsrq: Int?,
    val registered: Boolean,
)

class CellObserver(private val context: Context) {
    private val tm = context.getSystemService(Context.TELEPHONY_SERVICE) as TelephonyManager

    private val _cells = MutableStateFlow<List<CellSnapshot>>(emptyList())
    val cells: StateFlow<List<CellSnapshot>> = _cells.asStateFlow()

    fun hasLocationPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    @SuppressLint("MissingPermission")
    fun refresh() {
        if (!hasLocationPermission()) {
            _cells.value = emptyList()
            return
        }
        val all: List<CellInfo> = tm.allCellInfo ?: emptyList()
        _cells.value = all.mapNotNull { it.toSnapshot() }
    }

    private fun CellInfo.toSnapshot(): CellSnapshot? = when (this) {
        is CellInfoLte -> {
            val id = cellIdentity
            val ss: CellSignalStrengthLte = cellSignalStrength
            CellSnapshot(
                rat = "LTE",
                pci = id.pci.takeIf { it != Int.MAX_VALUE },
                earfcnOrArfcn = id.earfcn.takeIf { it != Int.MAX_VALUE },
                band = id.earfcn.takeIf { it != Int.MAX_VALUE }?.let(::lteEarfcnToBand),
                cellId = id.ci.toLong().takeIf { it != Int.MAX_VALUE.toLong() },
                tac = id.tac.takeIf { it != Int.MAX_VALUE },
                mcc = id.mccString,
                mnc = id.mncString,
                rsrpDbm = ss.rsrp.takeIf { it != Int.MAX_VALUE },
                rsrq = ss.rsrq.takeIf { it != Int.MAX_VALUE },
                registered = isRegistered,
            )
        }
        is CellInfoNr -> {
            val id = cellIdentity as android.telephony.CellIdentityNr
            val ss = cellSignalStrength as CellSignalStrengthNr
            CellSnapshot(
                rat = "NR5G",
                pci = id.pci.takeIf { it != Int.MAX_VALUE },
                earfcnOrArfcn = id.nrarfcn.takeIf { it != Int.MAX_VALUE },
                band = null,
                cellId = id.nci.takeIf { it != Long.MAX_VALUE },
                tac = id.tac.takeIf { it != Int.MAX_VALUE },
                mcc = id.mccString,
                mnc = id.mncString,
                rsrpDbm = ss.ssRsrp.takeIf { it != Int.MAX_VALUE },
                rsrq = ss.ssRsrq.takeIf { it != Int.MAX_VALUE },
                registered = isRegistered,
            )
        }
        else -> null
    }
}

/** Map an LTE EARFCN (downlink) to its 3GPP band number, based on TS 36.101 v17. */
fun lteEarfcnToBand(earfcn: Int): Int? {
    val ranges = listOf(
        1 to (0..599), 2 to (600..1199), 3 to (1200..1949), 4 to (1950..2399),
        5 to (2400..2649), 6 to (2650..2749), 7 to (2750..3449), 8 to (3450..3799),
        9 to (3800..4149), 10 to (4150..4749), 11 to (4750..4949), 12 to (5010..5179),
        13 to (5180..5279), 14 to (5280..5379), 17 to (5730..5849), 18 to (5850..5999),
        19 to (6000..6149), 20 to (6150..6449), 21 to (6450..6599), 22 to (6600..7399),
        23 to (7500..7699), 24 to (7700..8039), 25 to (8040..8689), 26 to (8690..9039),
        27 to (9040..9209), 28 to (9210..9659), 29 to (9660..9769), 30 to (9770..9869),
        31 to (9870..9919), 32 to (9920..10359),
        33 to (36000..36199), 34 to (36200..36349), 35 to (36350..36949),
        36 to (36950..37549), 37 to (37550..37749), 38 to (37750..38249),
        39 to (38250..38649), 40 to (38650..39649), 41 to (39650..41589),
        42 to (41590..43589), 43 to (43590..45589), 44 to (45590..46589),
        45 to (46590..46789), 46 to (46790..54539), 47 to (54540..55239),
        48 to (55240..56739), 49 to (56740..58239), 50 to (58240..59089),
        51 to (59090..59139), 52 to (59140..60139), 65 to (65536..66435),
        66 to (66436..67335), 67 to (67336..67535), 68 to (67536..67835),
        69 to (67836..68335), 70 to (68336..68585), 71 to (68586..68935),
        72 to (68936..68985), 73 to (68986..69035), 74 to (69036..69465),
        75 to (69466..70315), 76 to (70316..70365), 85 to (70366..70545),
        87 to (70546..70595), 88 to (70596..70645),
    )
    return ranges.firstOrNull { earfcn in it.second }?.first
}
