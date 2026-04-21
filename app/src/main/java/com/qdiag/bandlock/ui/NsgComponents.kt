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
/*  Signal bar — horizontal filled bar coloured by [signalColor].     */
/* ------------------------------------------------------------------ */

@Composable
fun SignalBar(
    label: String,
    dbm: Double?,
    unit: String = "dBm",
    minDbm: Double = -120.0,
    maxDbm: Double = -40.0,
) {
    val fraction = when {
        dbm == null -> 0f
        else -> ((dbm - minDbm) / (maxDbm - minDbm))
            .coerceIn(0.0, 1.0).toFloat()
    }
    val color = signalColor(dbm)
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 6.dp),
    ) {
        Row {
            Text(
                label,
                color = NsgColors.TextLabel,
                fontSize = 13.sp,
                modifier = Modifier.weight(1f),
            )
            Text(
                dbm?.let { "${"%.0f".format(it)} $unit" } ?: "-",
                style = MonoValue,
                color = color,
            )
        }
        Spacer(Modifier.height(4.dp))
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(6.dp)
                .clip(RoundedCornerShape(3.dp))
                .background(NsgColors.SurfaceElevated),
        ) {
            Box(
                modifier = Modifier
                    .fillMaxWidth(fraction)
                    .height(6.dp)
                    .background(color),
            )
        }
    }
}

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
            rows.forEach { r ->
                val metricB = r.metricB
                val rowColor = signalColor(metricB?.toDouble())
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 6.dp, vertical = 3.dp),
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
                    Text(
                        r.metricA?.toString() ?: "-",
                        color = NsgColors.TextPrimary,
                        style = MonoValue,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        metricB?.toString() ?: "-",
                        color = rowColor,
                        style = MonoValue,
                        modifier = Modifier.weight(1f),
                    )
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
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .background(NsgColors.Background),
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
        SignalBar("Carrier RSSI",     snap.rssiDbm)
        MetricRow("UE TxPower",       snap.ueTxPower?.let { "$it dBm" })
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
