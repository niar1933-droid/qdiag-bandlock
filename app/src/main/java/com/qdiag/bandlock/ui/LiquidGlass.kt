package com.qdiag.bandlock.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/**
 * ------------------------------------------------------------------
 *  iOS-26-style "Liquid Glass" design — premium dark variant.
 *
 *  Calm graphite-navy backdrop, very subtle muted blobs, near-clear
 *  frosted panels with thin specular-highlight borders, iOS-blue
 *  primary accent. Reads as business / premium, not decorative.
 * ------------------------------------------------------------------
 */
enum class DesignVariant { Nsg, Glass }

/** CompositionLocal so shared components (RatPage, MetricRow, panels)
 *  can adapt their background/text colours without threading a
 *  `variant` parameter through every function signature. */
val LocalDesignVariant = staticCompositionLocalOf { DesignVariant.Nsg }

object GlassColors {
    /* Deep graphite / navy backdrop — barely gradient, almost flat. */
    val BgTop = Color(0xFF0E1218)
    val BgMid = Color(0xFF12172A)
    val BgBot = Color(0xFF0B0E18)

    /* Muted, desaturated glow blobs — barely perceptible. */
    val BlobBlue   = Color(0xFF3B5B88)
    val BlobPurple = Color(0xFF4A3E6B)
    val BlobTeal   = Color(0xFF2F5A64)

    /* Frosted glass panel fill + highlight border (on dark backdrop). */
    val PanelFill       = Color(0xFFFFFFFF).copy(alpha = 0.06f)
    val PanelFillStrong = Color(0xFFFFFFFF).copy(alpha = 0.10f)
    val PanelBorder     = Color(0xFFFFFFFF).copy(alpha = 0.22f)
    val PanelBorderSoft = Color(0xFFFFFFFF).copy(alpha = 0.05f)

    /* Typography — light text on dark glass. */
    val TextPrimary   = Color(0xFFF2F3F5)
    val TextSecondary = Color(0xFFB8BDC7)
    val TextTertiary  = Color(0xFF6E737D)

    /* Dividers. */
    val DividerSoft = Color(0xFFFFFFFF).copy(alpha = 0.06f)

    /* iOS dark-mode system blue. */
    val Accent      = Color(0xFF0A84FF)
    val AccentLight = Color(0xFF4DA3FF)
    val AccentDark  = Color(0xFF0051D5)
    val AccentRed   = Color(0xFFFF453A)
}

/** Full-screen graphite-navy gradient + very soft muted blobs.
 *  Calm, premium, business — no pastel / decorative colour. */
@Composable
fun GlassBackdrop(content: @Composable () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(
                Brush.verticalGradient(
                    0.0f to GlassColors.BgTop,
                    0.5f to GlassColors.BgMid,
                    1.0f to GlassColors.BgBot,
                ),
            ),
    ) {
        Box(
            modifier = Modifier
                .offset(x = (-80).dp, y = 60.dp)
                .size(320.dp)
                .clip(CircleShape)
                .background(GlassColors.BlobBlue.copy(alpha = 0.28f)),
        )
        Box(
            modifier = Modifier
                .offset(x = 200.dp, y = 380.dp)
                .size(280.dp)
                .clip(CircleShape)
                .background(GlassColors.BlobPurple.copy(alpha = 0.22f)),
        )
        Box(
            modifier = Modifier
                .offset(x = (-60).dp, y = 720.dp)
                .size(300.dp)
                .clip(CircleShape)
                .background(GlassColors.BlobTeal.copy(alpha = 0.20f)),
        )
        content()
    }
}

/** Frosted-glass surface. Near-clear translucent fill, thin
 *  top-specular / soft-bottom border, large corner radius. */
fun Modifier.glassPanel(
    cornerDp: Int = 22,
    strong: Boolean = false,
): Modifier = this
    .clip(RoundedCornerShape(cornerDp.dp))
    .background(if (strong) GlassColors.PanelFillStrong else GlassColors.PanelFill)
    .border(
        width = 1.dp,
        brush = Brush.verticalGradient(
            0.0f to GlassColors.PanelBorder,
            1.0f to GlassColors.PanelBorderSoft,
        ),
        shape = RoundedCornerShape(cornerDp.dp),
    )

/** Frosted-glass pill for outlined / secondary buttons. */
fun Modifier.glassPill(): Modifier = this
    .clip(RoundedCornerShape(100))
    .background(GlassColors.PanelFillStrong)
    .border(
        width = 1.dp,
        brush = Brush.verticalGradient(
            0.0f to GlassColors.PanelBorder,
            1.0f to GlassColors.PanelBorderSoft,
        ),
        shape = RoundedCornerShape(100),
    )

/** iOS-blue primary pill — vertical gradient fill + top highlight. */
fun Modifier.glassPrimaryPill(): Modifier = this
    .clip(RoundedCornerShape(100))
    .background(
        Brush.verticalGradient(
            0.0f to GlassColors.AccentLight,
            1.0f to GlassColors.AccentDark,
        ),
    )
    .border(
        width = 1.dp,
        brush = Brush.verticalGradient(
            0.0f to Color.White.copy(alpha = 0.45f),
            1.0f to Color.White.copy(alpha = 0.08f),
        ),
        shape = RoundedCornerShape(100),
    )
