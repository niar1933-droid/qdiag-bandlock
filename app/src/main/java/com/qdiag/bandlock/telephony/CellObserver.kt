package com.qdiag.bandlock.telephony

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.telephony.CellInfo
import android.telephony.CellInfoGsm
import android.telephony.CellInfoLte
import android.telephony.CellInfoNr
import android.telephony.CellInfoWcdma
import android.telephony.CellSignalStrengthLte
import android.telephony.CellSignalStrengthNr
import android.telephony.TelephonyManager
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

enum class Rat { GSM, WCDMA, LTE, NR }

/**
 * Row in a "Cell Table" (like NSG's WCDMA/LTE cell table).
 * label = "A"/"M" for active/monitored in WCDMA, "S" for serving / "N" for neighbor elsewhere.
 */
data class CellRow(
    val label: String,
    val channel: Int?,         // UARFCN / EARFCN / ARFCN / NRARFCN
    val identity: Int?,        // PSC / PCI
    val metricA: Int?,         // EcNo / RSRQ (dB)
    val metricB: Int?,         // RSCP / RSRP (dBm)
)

/** Per-RAT snapshot used by the NSG-style page UI. */
data class RatSnapshot(
    val rat: Rat,
    val available: Boolean,
    val band: String? = null,         // e.g. "B1-IMT 2100" / "B3-1800" / "n78-TDD 3500"
    val rrcState: String? = null,     // connected / idle / null
    val channelDl: Int? = null,       // UARFCN / EARFCN / NRARFCN
    val channelUl: Int? = null,       // UL pair for WCDMA/LTE; null for NR
    val rssiDbm: Double? = null,      // Carrier RSSI / RSSI-like metric
    val ueTxPower: Int? = null,
    val ulInterference: Int? = null,
    val uuSir: Double? = null,
    val trchBlerDl: Double? = null,
    val mcc: String? = null,
    val mnc: String? = null,
    val tac: Int? = null,
    val cellId: Long? = null,
    val rows: List<CellRow> = emptyList(),
)

class CellObserver(private val context: Context) {
    private val tm = context.getSystemService(Context.TELEPHONY_SERVICE) as TelephonyManager

    private val _snapshots = MutableStateFlow<Map<Rat, RatSnapshot>>(emptyMap())
    val snapshots: StateFlow<Map<Rat, RatSnapshot>> = _snapshots.asStateFlow()

    fun hasLocationPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    @SuppressLint("MissingPermission")
    fun refresh() {
        if (!hasLocationPermission()) {
            _snapshots.value = emptyMap()
            return
        }
        val all: List<CellInfo> = tm.allCellInfo ?: emptyList()

        val lteCells = all.filterIsInstance<CellInfoLte>()
        val nrCells  = all.filterIsInstance<CellInfoNr>()
        val wcdmaCells = all.filterIsInstance<CellInfoWcdma>()
        val gsmCells = all.filterIsInstance<CellInfoGsm>()

        val map = mutableMapOf<Rat, RatSnapshot>()
        map[Rat.LTE]   = buildLte(lteCells)
        map[Rat.NR]    = buildNr(nrCells)
        map[Rat.WCDMA] = buildWcdma(wcdmaCells)
        map[Rat.GSM]   = buildGsm(gsmCells)
        _snapshots.value = map
    }

    private fun buildLte(list: List<CellInfoLte>): RatSnapshot {
        if (list.isEmpty()) return RatSnapshot(Rat.LTE, available = false)
        val serving = list.firstOrNull { it.isRegistered } ?: list.first()
        val id = serving.cellIdentity
        val ss: CellSignalStrengthLte = serving.cellSignalStrength
        val earfcn = id.earfcn.takeIf { it != Int.MAX_VALUE }
        val bandNum = earfcn?.let(::lteEarfcnToBand)
        val rows = list.mapIndexed { i, c ->
            val cid = c.cellIdentity
            val cs = c.cellSignalStrength
            CellRow(
                label = if (c.isRegistered) "S" else "N",
                channel = cid.earfcn.takeIf { it != Int.MAX_VALUE },
                identity = cid.pci.takeIf { it != Int.MAX_VALUE },
                metricA = cs.rsrq.takeIf { it != Int.MAX_VALUE },
                metricB = cs.rsrp.takeIf { it != Int.MAX_VALUE },
            )
        }
        return RatSnapshot(
            rat = Rat.LTE,
            available = true,
            band = bandNum?.let { "B$it  ${lteBandName(it)}" },
            rrcState = if (serving.isRegistered) "Connected" else "Idle",
            channelDl = earfcn,
            channelUl = earfcn?.let(::lteEarfcnDlToUl),
            rssiDbm = ss.rssi.takeIf { Build.VERSION.SDK_INT >= 29 && it != Int.MAX_VALUE }?.toDouble(),
            mcc = id.mccString,
            mnc = id.mncString,
            tac = id.tac.takeIf { it != Int.MAX_VALUE },
            cellId = id.ci.toLong().takeIf { it != Int.MAX_VALUE.toLong() },
            rows = rows,
        )
    }

    private fun buildNr(list: List<CellInfoNr>): RatSnapshot {
        if (list.isEmpty()) return RatSnapshot(Rat.NR, available = false)
        val serving = list.firstOrNull { it.isRegistered } ?: list.first()
        val id = serving.cellIdentity as android.telephony.CellIdentityNr
        val ss = serving.cellSignalStrength as CellSignalStrengthNr
        val nrarfcn = id.nrarfcn.takeIf { it != Int.MAX_VALUE }
        val bandNum = nrarfcn?.let(::nrArfcnToBand)
        val rows = list.map { c ->
            val cid = c.cellIdentity as android.telephony.CellIdentityNr
            val cs = c.cellSignalStrength as CellSignalStrengthNr
            CellRow(
                label = if (c.isRegistered) "S" else "N",
                channel = cid.nrarfcn.takeIf { it != Int.MAX_VALUE },
                identity = cid.pci.takeIf { it != Int.MAX_VALUE },
                metricA = cs.ssRsrq.takeIf { it != Int.MAX_VALUE },
                metricB = cs.ssRsrp.takeIf { it != Int.MAX_VALUE },
            )
        }
        return RatSnapshot(
            rat = Rat.NR,
            available = true,
            band = bandNum?.let { "n$it  ${nrBandName(it)}" },
            rrcState = if (serving.isRegistered) "Connected" else "Idle",
            channelDl = nrarfcn,
            channelUl = null,
            rssiDbm = ss.ssRsrp.takeIf { it != Int.MAX_VALUE }?.toDouble(),
            mcc = id.mccString,
            mnc = id.mncString,
            tac = id.tac.takeIf { it != Int.MAX_VALUE },
            cellId = id.nci.takeIf { it != Long.MAX_VALUE },
            rows = rows,
        )
    }

    private fun buildWcdma(list: List<CellInfoWcdma>): RatSnapshot {
        if (list.isEmpty()) return RatSnapshot(Rat.WCDMA, available = false)
        val serving = list.firstOrNull { it.isRegistered } ?: list.first()
        val id = serving.cellIdentity
        val ss = serving.cellSignalStrength
        val uarfcn = id.uarfcn.takeIf { it != Int.MAX_VALUE }
        val bandNum = uarfcn?.let(::uarfcnToBand)
        val rows = list.mapIndexed { _, c ->
            val cid = c.cellIdentity
            val cs = c.cellSignalStrength
            CellRow(
                label = if (c.isRegistered) "A" else "M",
                channel = cid.uarfcn.takeIf { it != Int.MAX_VALUE },
                identity = cid.psc.takeIf { it != Int.MAX_VALUE },
                metricA = if (Build.VERSION.SDK_INT >= 30) cs.ecNo.takeIf { it != Int.MAX_VALUE } else null,
                metricB = cs.dbm.takeIf { it != Int.MAX_VALUE },
            )
        }
        return RatSnapshot(
            rat = Rat.WCDMA,
            available = true,
            band = bandNum?.let { "B$it-${wcdmaBandName(it)}" },
            rrcState = null,
            channelDl = uarfcn,
            channelUl = uarfcn?.let(::wcdmaDlUarfcnToUl),
            rssiDbm = ss.dbm.toDouble(),
            mcc = id.mccString,
            mnc = id.mncString,
            tac = id.lac.takeIf { it != Int.MAX_VALUE },
            cellId = id.cid.toLong().takeIf { it != Int.MAX_VALUE.toLong() },
            rows = rows,
        )
    }

    private fun buildGsm(list: List<CellInfoGsm>): RatSnapshot {
        if (list.isEmpty()) return RatSnapshot(Rat.GSM, available = false)
        val serving = list.firstOrNull { it.isRegistered } ?: list.first()
        val id = serving.cellIdentity
        val ss = serving.cellSignalStrength
        val rows = list.map { c ->
            val cid = c.cellIdentity
            val cs = c.cellSignalStrength
            CellRow(
                label = if (c.isRegistered) "S" else "N",
                channel = cid.arfcn.takeIf { it != Int.MAX_VALUE },
                identity = cid.bsic.takeIf { it != Int.MAX_VALUE },
                metricA = null,
                metricB = cs.dbm.takeIf { it != Int.MAX_VALUE },
            )
        }
        return RatSnapshot(
            rat = Rat.GSM,
            available = true,
            band = id.arfcn.takeIf { it != Int.MAX_VALUE }?.let { "GSM ${gsmArfcnBand(it)}" },
            channelDl = id.arfcn.takeIf { it != Int.MAX_VALUE },
            channelUl = id.arfcn.takeIf { it != Int.MAX_VALUE },
            rssiDbm = ss.dbm.toDouble(),
            mcc = id.mccString,
            mnc = id.mncString,
            tac = id.lac.takeIf { it != Int.MAX_VALUE },
            cellId = id.cid.toLong().takeIf { it != Int.MAX_VALUE.toLong() },
            rows = rows,
        )
    }
}

/* ---------- 3GPP channel-number helpers ---------- */

/** TS 36.101 v17: LTE E-UTRA EARFCN DL -> band. */
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
        42 to (41590..43589), 43 to (43590..45589), 46 to (46790..54539),
        48 to (55240..56739), 66 to (66436..67335), 71 to (68586..68935),
    )
    return ranges.firstOrNull { earfcn in it.second }?.first
}

/** DL EARFCN -> UL EARFCN (offsets from TS 36.101). */
fun lteEarfcnDlToUl(dl: Int): Int? {
    val band = lteEarfcnToBand(dl) ?: return null
    return when (band) {
        1 -> dl + 18000
        2 -> dl + 18000
        3 -> dl + 18000
        4 -> dl + 18000
        5 -> dl + 18000
        7 -> dl + 18000
        8 -> dl + 18000
        12 -> dl + 18000
        13 -> dl + 18000
        14 -> dl + 18000
        17 -> dl + 18000
        18 -> dl + 18000
        19 -> dl + 18000
        20 -> dl + 18000
        25 -> dl + 18000
        26 -> dl + 18000
        28 -> dl + 18000
        29 -> null /* supplementary DL only */
        66 -> dl + 65536
        71 -> dl + 65536
        else -> if (band >= 33) dl else null /* TDD: same channel */
    }
}

fun lteBandName(b: Int): String = when (b) {
    1 -> "IMT 2100"; 2 -> "PCS 1900"; 3 -> "DCS 1800"; 4 -> "AWS-1"; 5 -> "CLR 850"
    7 -> "IMT-E 2600"; 8 -> "GSM 900"; 12 -> "700 a"; 13 -> "700 c"; 17 -> "700 b"
    18 -> "LLU 800"; 19 -> "SLU 800"; 20 -> "DD 800"; 25 -> "EPCS 1900"; 26 -> "ELB 850"
    28 -> "APT 700"; 38 -> "TDD 2600"; 39 -> "TDD 1900"; 40 -> "TDD 2300"
    41 -> "TDD 2500"; 42 -> "TDD 3500"; 46 -> "TDD 5200 LAA"; 48 -> "TDD 3600 CBRS"
    66 -> "AWS-3"; 71 -> "600"
    else -> ""
}

/** TS 38.104 v17: NR NR-ARFCN -> band number (subset of most common bands). */
fun nrArfcnToBand(nrarfcn: Int): Int? = when (nrarfcn) {
    in 422000..434000 -> 1
    in 386000..398000 -> 2
    in 361000..376000 -> 3
    in 173800..178800 -> 5
    in 524000..538000 -> 7
    in 185000..192000 -> 8
    in 158200..164200 -> 20
    in 386000..399000 -> 25
    in 151600..160600 -> 28
    in 499200..537999 -> 41
    in 422000..440000 -> 66
    in 123400..130400 -> 71
    in 620000..680000 -> 77
    in 620000..653333 -> 78
    in 693334..733333 -> 79
    in 2054166..2104165 -> 257
    in 2016667..2070832 -> 258
    in 2070833..2084999 -> 260
    in 2229166..2279165 -> 261
    else -> null
}

fun nrBandName(b: Int): String = when (b) {
    1 -> "2100 FDD"; 2 -> "1900 FDD"; 3 -> "1800 FDD"; 5 -> "850 FDD"; 7 -> "2600 FDD"
    8 -> "900 FDD"; 20 -> "800 FDD"; 25 -> "1900+"; 28 -> "700 FDD"
    41 -> "TDD 2500"; 66 -> "AWS-3"; 71 -> "600 FDD"
    77 -> "TDD 3700"; 78 -> "TDD 3500"; 79 -> "TDD 4700"
    257 -> "28 GHz"; 258 -> "26 GHz"; 260 -> "39 GHz"; 261 -> "28 GHz"
    else -> ""
}

/** UTRA UARFCN -> band (rough). */
fun uarfcnToBand(uarfcn: Int): Int? = when (uarfcn) {
    in 10562..10838 -> 1
    in 9662..9938 -> 2
    in 1162..1513 -> 3
    in 4357..4458 -> 4
    in 4132..4233 -> 5
    in 2237..2563 -> 7
    in 2937..3088 -> 8
    in 3712..3787 -> 10
    in 3937..3988 -> 13
    in 4037..4088 -> 14
    in 712..763 -> 19
    in 4512..4638 -> 20
    in 862..912 -> 21
    else -> null
}

fun wcdmaBandName(b: Int): String = when (b) {
    1 -> "IMT 2100"; 2 -> "PCS 1900"; 3 -> "DCS 1800"; 4 -> "AWS-1"
    5 -> "CLR 850"; 7 -> "IMT-E 2600"; 8 -> "GSM 900"; 10 -> "AWS ext"
    13 -> "700 c"; 14 -> "700 PS"; 19 -> "SLU 800"; 20 -> "DD 800"; 21 -> "PCS upper"
    else -> ""
}

fun wcdmaDlUarfcnToUl(dl: Int): Int? = uarfcnToBand(dl)?.let { band ->
    when (band) {
        1 -> dl - 950
        2 -> dl - 950
        3 -> dl - 225
        4 -> dl - 400
        5 -> dl - 225
        7 -> dl - 225
        8 -> dl - 134
        10 -> dl - 400
        13 -> dl + 180
        14 -> dl + 198
        19 -> dl - 400
        20 -> dl - 491
        21 -> dl - 950
        else -> null
    }
}

/** GSM ARFCN -> rough band label. */
fun gsmArfcnBand(arfcn: Int): String = when (arfcn) {
    in 1..124 -> "GSM 900"
    in 128..251 -> "GSM 850"
    in 259..293 -> "GSM 450"
    in 306..340 -> "GSM 480"
    in 438..511 -> "GSM 750"
    in 512..885 -> "GSM 1800"
    in 955..1023 -> "GSM 900 (E)"
    else -> ""
}
