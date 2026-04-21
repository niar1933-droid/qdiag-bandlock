package com.qdiag.bandlock.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

/* ------------------------------------------------------------------
 * NSG-style palette — near-black background, red accents for RAT
 * titles, muted grey labels, bright values. Signal bars use a fixed
 * red→amber→green ramp (see [signalColor]).
 * ------------------------------------------------------------------ */

object NsgColors {
    val Background       = Color(0xFF0A0A0A)
    val Surface          = Color(0xFF101114)
    val SurfaceElevated  = Color(0xFF16181C)
    val Divider          = Color(0xFF24262B)

    val Accent           = Color(0xFFE53935) // red — RAT titles, "Testing (Available)"
    val AccentMuted      = Color(0xFFB71C1C)

    val TextPrimary      = Color(0xFFECEDEE)
    val TextLabel        = Color(0xFF8B9098)
    val TextDim          = Color(0xFF5A5F66)

    /* signal-bar gradient */
    val SignalGreen      = Color(0xFF2E7D32)
    val SignalLime       = Color(0xFF7CB342)
    val SignalAmber      = Color(0xFFEF6C00)
    val SignalRed        = Color(0xFFC62828)

    val ChannelHi        = Color(0xFF64B5F6) // DL channel value
}

/** Map a signal level in dBm (RSRP/RSCP/RSSI-ish) to a bar colour. */
fun signalColor(dbm: Double?): Color = when {
    dbm == null   -> NsgColors.TextDim
    dbm >= -70    -> NsgColors.SignalGreen
    dbm >= -85    -> NsgColors.SignalLime
    dbm >= -100   -> NsgColors.SignalAmber
    else          -> NsgColors.SignalRed
}

/**
 * Map a normalised "quality" fraction (1.0 = best, 0.0 = worst) to a
 * smooth NSG-style green→lime→amber→red gauge colour.
 */
fun gaugeColor(fraction: Float): Color = when {
    fraction >= 0.75f -> NsgColors.SignalGreen
    fraction >= 0.50f -> NsgColors.SignalLime
    fraction >= 0.25f -> NsgColors.SignalAmber
    else              -> NsgColors.SignalRed
}

private val nsgColorScheme = darkColorScheme(
    primary            = NsgColors.Accent,
    onPrimary          = Color.White,
    secondary          = NsgColors.AccentMuted,
    background         = NsgColors.Background,
    onBackground       = NsgColors.TextPrimary,
    surface            = NsgColors.Surface,
    onSurface          = NsgColors.TextPrimary,
    surfaceVariant     = NsgColors.SurfaceElevated,
    onSurfaceVariant   = NsgColors.TextLabel,
    outline            = NsgColors.Divider,
    error              = NsgColors.SignalRed,
)

private val monoHeadlineSmall = TextStyle(
    fontFamily = FontFamily.Monospace,
    fontWeight = FontWeight.Medium,
    fontSize   = 22.sp,
)

private val nsgTypography = Typography(
    titleLarge  = TextStyle(fontWeight = FontWeight.Bold, fontSize = 28.sp),
    titleMedium = TextStyle(fontWeight = FontWeight.Medium, fontSize = 18.sp),
    headlineSmall = monoHeadlineSmall,
    bodyLarge   = TextStyle(fontSize = 16.sp),
    bodyMedium  = TextStyle(fontSize = 14.sp),
    labelMedium = TextStyle(fontSize = 13.sp, color = NsgColors.TextLabel),
)

@Composable
fun QDiagTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = nsgColorScheme,
        typography  = nsgTypography,
        content     = content,
    )
}
