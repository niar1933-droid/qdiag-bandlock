package com.qdiag.bandlock.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.qdiag.bandlock.telephony.CellRow
import com.qdiag.bandlock.telephony.Rat
import com.qdiag.bandlock.telephony.RatSnapshot

/* ------------------------------------------------------------------ */
/*  RAT header — red title like NSG ("WCDMA • Testing (Available)").  */
/* ------------------------------------------------------------------ */

@Composable
fun RatHeader(rat: Rat, available: Boolean) {
    val label = when (rat) {
        Rat.GSM   -> "GSM"
        Rat.WCDMA -> "WCDMA"
        Rat.LTE   -> "LTE"
        Rat.NR    -> "NR5G"
    }
    val status = if (available) "Testing (Available)" else "Not Available"
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = "$label  •  $status",
            color = NsgColors.Accent,
            fontWeight = FontWeight.Bold,
            fontSize = 18.sp,
        )
    }
    HorizontalDivider(color = NsgColors.Divider, thickness = 1.dp)
}

/* ------------------------------------------------------------------ */
/*  Metric row — label on the left, value right-aligned.              */
/* ------------------------------------------------------------------ */

private val MonoValue = TextStyle(
    fontFamily = FontFamily.Monospace,
    fontWeight = FontWeight.Medium,
    fontSize = 15.sp,
)

@Composable
fun MetricRow(
    label: String,
    value: String?,
    valueColor: Color = NsgColors.TextPrimary,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            color = NsgColors.TextLabel,
            fontSize = 13.sp,
            modifier = Modifier.weight(1f),
        )
        Text(
            text = value ?: "-",
            style = MonoValue,
            color = if (value == null) NsgColors.TextDim else valueColor,
        )
    }
}

/* ------------------------------------------------------------------ */
/*  Gauge row — NSG-style: label | coloured bar with value inside.    */
/* ------------------------------------------------------------------ */

/**
 * A single bar with the numeric value overlaid inside it, like NSG
 * shows for Carrier RSSI / TxPower / BLER / TA.
 *
 * [quality] is a 0..1 "how good" fraction that drives both the bar
 * width and the colour ramp — callers translate whatever metric they
 * have (RSRP dBm, BLER %, dBm TxPower, etc.) into that scale.
 */
@Composable
fun GaugeBar(
    value: String,
    quality: Float?,
    modifier: Modifier = Modifier,
    barHeight: androidx.compose.ui.unit.Dp = 22.dp,
) {
    val clamped = quality?.coerceIn(0f, 1f)
    val color = clamped?.let(::gaugeColor) ?: NsgColors.TextDim
    Box(
        modifier = modifier
            .fillMaxWidth()
            .height(barHeight)
            .clip(RoundedCornerShape(3.dp))
            .background(NsgColors.SurfaceElevated),
        contentAlignment = Alignment.CenterStart,
    ) {
        if (clamped != null) {
            Box(
                modifier = Modifier
                    .fillMaxWidth(clamped)
                    .height(barHeight)
                    .background(color),
            )
        }
        Box(
            modifier = Modifier.fillMaxWidth(),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                value,
                style = MonoValue,
                color = Color.White,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

/**
 * Row: label on left, full-width gauge bar on right.
 */
@Composable
fun GaugeRow(
    label: String,
    value: String,
    quality: Float?,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            color = NsgColors.TextLabel,
            fontSize = 13.sp,
            modifier = Modifier.weight(1f),
        )
        GaugeBar(
            value = value,
            quality = quality,
            modifier = Modifier.weight(1.2f),
        )
    }
}

/** Two narrow gauges side by side (e.g. PUSCH + PUCCH TxPower). */
@Composable
fun DualGaugeRow(
    label: String,
    leftValue: String,
    leftQuality: Float?,
    rightValue: String,
    rightQuality: Float?,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            color = NsgColors.TextLabel,
            fontSize = 13.sp,
            modifier = Modifier.weight(1f),
        )
        GaugeBar(leftValue, leftQuality, modifier = Modifier.weight(0.6f))
        Spacer(Modifier.width(6.dp))
        GaugeBar(rightValue, rightQuality, modifier = Modifier.weight(0.6f))
    }
}

/* ------------------------------------------------------------------ */
/*  Signal bar — kept for backwards compat (thin bar, label + value). */
/* ------------------------------------------------------------------ */

@Composable
fun SignalBar(
    label: String,
    dbm: Double?,
    unit: String = "dBm",
    minDbm: Double = -120.0,
    maxDbm: Double = -40.0,
) {
    val quality = dbm?.let { dbmQuality(it, worst = minDbm, best = maxDbm) }
    val value = dbm?.let { "${"%.1f".format(it)} $unit" } ?: "-"
    GaugeRow(label = label, value = value, quality = quality)
}

/**
 * Convert a dBm-scale reading to a 0..1 "quality" fraction using a
 * linear worst→best ramp. Works for both "higher is better" metrics
 * (RSRP, RSSI, SNR) and "lower absolute value is better" (TxPower
 * where a less-negative number still means the UE is pushing harder,
 * so callers can flip worst/best to invert the ramp).
 */
fun dbmQuality(value: Double, worst: Double, best: Double): Float =
    ((value - worst) / (best - worst)).coerceIn(0.0, 1.0).toFloat()

/* ------------------------------------------------------------------ */
/*  Cell table — header + rows of neighbours.                          */
/* ------------------------------------------------------------------ */

@Composable
fun CellTable(rat: Rat, rows: List<CellRow>) {
    val cols = when (rat) {
        Rat.WCDMA -> listOf("", "UARFCN", "PSC", "EcNo", "RSCP")
        Rat.LTE   -> listOf("", "EARFCN", "PCI", "RSRQ", "RSRP")
        Rat.NR    -> listOf("", "NRARFCN", "PCI", "RSRQ", "RSRP")
        Rat.GSM   -> listOf("", "ARFCN", "BSIC", "C1", "RxLev")
    }
    Column(Modifier.padding(horizontal = 12.dp, vertical = 6.dp)) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(NsgColors.SurfaceElevated)
                .padding(horizontal = 6.dp, vertical = 4.dp),
        ) {
            cols.forEachIndexed { i, h ->
                Text(
                    h,
                    color = NsgColors.TextLabel,
                    fontSize = 12.sp,
                    modifier = if (i == 0) Modifier.width(28.dp) else Modifier.weight(1f),
                )
            }
        }
        if (rows.isEmpty()) {
            Box(Modifier.fillMaxWidth().padding(vertical = 10.dp), contentAlignment = Alignment.Center) {
                Text("— no cells —", color = NsgColors.TextDim, fontSize = 12.sp)
            }
        } else {
            /* Per-RAT "worst..best" ramps for the inline gauge bars. */
            val (aWorst, aBest) = when (rat) {
                Rat.LTE, Rat.NR -> -20.0 to -5.0       // RSRQ
                Rat.WCDMA       -> -24.0 to -3.0       // EcNo / Ec/Io
                Rat.GSM         -> 0.0 to 60.0         // C1 (rough)
            }
            val (bWorst, bBest) = when (rat) {
                Rat.LTE, Rat.NR -> -120.0 to -70.0     // RSRP
                Rat.WCDMA       -> -120.0 to -60.0     // RSCP
                Rat.GSM         -> -110.0 to -60.0     // RxLev dBm
            }
            rows.forEach { r ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 6.dp, vertical = 3.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        r.label,
                        color = NsgColors.Accent,
                        fontWeight = FontWeight.Bold,
                        style = MonoValue,
                        modifier = Modifier.width(28.dp),
                    )
                    Text(
                        r.channel?.toString() ?: "-",
                        color = NsgColors.ChannelHi,
                        style = MonoValue,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        r.identity?.toString() ?: "-",
                        color = NsgColors.TextPrimary,
                        style = MonoValue,
                        modifier = Modifier.weight(1f),
                    )
                    Box(Modifier.weight(1f).padding(end = 2.dp)) {
                        GaugeBar(
                            value = r.metricA?.toString() ?: "-",
                            quality = r.metricA?.toDouble()?.let {
                                dbmQuality(it, worst = aWorst, best = aBest)
                            },
                            barHeight = 18.dp,
                        )
                    }
                    Box(Modifier.weight(1f)) {
                        GaugeBar(
                            value = r.metricB?.toString() ?: "-",
                            quality = r.metricB?.toDouble()?.let {
                                dbmQuality(it, worst = bWorst, best = bBest)
                            },
                            barHeight = 18.dp,
                        )
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Full RAT page.                                                     */
/* ------------------------------------------------------------------ */

@Composable
fun RatPage(snap: RatSnapshot) {
    val isGlass = LocalDesignVariant.current == DesignVariant.Glass
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .then(
                if (isGlass) Modifier
                else Modifier.background(NsgColors.Background),
            ),
    ) {
        RatHeader(snap.rat, snap.available)
        if (!snap.available) {
            Box(
                modifier = Modifier.fillMaxWidth().padding(32.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text("Not available", color = NsgColors.TextDim)
            }
            return@Column
        }
        MetricRow("3GPP Band",        snap.band)
        MetricRow("RRC State",        snap.rrcState)
        MetricRow("Channel DL",       snap.channelDl?.toString(), valueColor = NsgColors.ChannelHi)
        MetricRow("Channel UL",       snap.channelUl?.toString(), valueColor = NsgColors.ChannelHi)
        MetricRow("MCC / MNC",        if (snap.mcc != null) "${snap.mcc} / ${snap.mnc ?: "?"}" else null)
        MetricRow("TAC",              snap.tac?.toString())
        MetricRow("Cell ID",          snap.cellId?.toString())
        /*
         * Gauge bars, like NSG. Quality scale is chosen per-metric from
         * typical 3GPP operating ranges rather than from a fixed global
         * scale — this matches NSG's visible behaviour where a weak
         * RSRP of -95 shows yellow-orange, not ~50% green.
         */
        GaugeRow(
            label = "Carrier RSSI",
            value = snap.rssiDbm?.let { "${"%.1f".format(it)} dBm" } ?: "-",
            quality = snap.rssiDbm?.let { dbmQuality(it, worst = -110.0, best = -40.0) },
        )
        GaugeRow(
            label = "RSRP",
            value = snap.rsrpDbm?.let { "${"%.1f".format(it)} dBm" } ?: "-",
            quality = snap.rsrpDbm?.let { dbmQuality(it, worst = -120.0, best = -70.0) },
        )
        GaugeRow(
            label = "RSRQ",
            value = snap.rsrqDb?.let { "${"%.1f".format(it)} dB" } ?: "-",
            quality = snap.rsrqDb?.let { dbmQuality(it, worst = -20.0, best = -5.0) },
        )
        GaugeRow(
            label = if (snap.rat == Rat.NR) "SS-SINR" else "RS-SNR",
            value = snap.snrDb?.let { "${"%.1f".format(it)} dB" } ?: "-",
            quality = snap.snrDb?.let { dbmQuality(it, worst = -5.0, best = 20.0) },
        )
        snap.ueTxPower?.let {
            GaugeRow(
                label = "UE TxPower",
                value = "$it dBm",
                quality = dbmQuality(it.toDouble(), worst = 23.0, best = -30.0),
            )
        }
        snap.timingAdvance?.let {
            MetricRow("Timing Advance", it.toString())
        }
        HorizontalDivider(color = NsgColors.Divider, thickness = 1.dp, modifier = Modifier.padding(vertical = 6.dp))
        Text(
            "Cells",
            color = NsgColors.TextLabel,
            fontSize = 12.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 4.dp),
        )
        CellTable(snap.rat, snap.rows)
    }
}

/* ------------------------------------------------------------------ */
/*  Dot indicator for the RAT pager.                                   */
/* ------------------------------------------------------------------ */

@Composable
fun PagerDots(pageCount: Int, currentPage: Int) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp),
        horizontalArrangement = Arrangement.Center,
    ) {
        repeat(pageCount) { i ->
            val active = i == currentPage
            Box(
                modifier = Modifier
                    .padding(horizontal = 4.dp)
                    .size(if (active) 8.dp else 6.dp)
                    .clip(RoundedCornerShape(50))
                    .background(if (active) NsgColors.Accent else NsgColors.TextDim),
            )
        }
    }
}
