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
    /* Deep navy backdrop sampled from the FoxikNetwork premium mockup. */
    val BgTop = Color(0xFF0A0E1F)
    val BgMid = Color(0xFF141832)
    val BgBot = Color(0xFF05080F)

    /* Soft aurora-like glow blobs behind the cards — desaturated navy/purple. */
    val BlobBlue   = Color(0xFF2D4A7E)
    val BlobPurple = Color(0xFF3C2F5E)
    val BlobTeal   = Color(0xFF1F3E58)

    /* Frosted glass panel fill + highlight border (on dark backdrop). */
    val PanelFill       = Color(0xFFFFFFFF).copy(alpha = 0.07f)
    val PanelFillStrong = Color(0xFFFFFFFF).copy(alpha = 0.12f)
    val PanelBorder     = Color(0xFFFFFFFF).copy(alpha = 0.32f)
    val PanelBorderSoft = Color(0xFFFFFFFF).copy(alpha = 0.06f)

    /* Typography — light text on dark glass. */
    val TextPrimary   = Color(0xFFF5F7FA)
    val TextSecondary = Color(0xFFAAB1C3)
    val TextTertiary  = Color(0xFF6A6F80)

    /* Dividers. */
    val DividerSoft = Color(0xFFFFFFFF).copy(alpha = 0.07f)

    /* Accent blue sampled from the brand mark + Apply pill + checkbox fill. */
    val Accent       = Color(0xFF3C8EF5)
    val AccentLight  = Color(0xFF7AB5FF)
    val AccentDark   = Color(0xFF1E5FB8)
    val AccentDeep   = Color(0xFF0F3F8A)

    /* Gradient stops for the big Apply-pill. */
    val PillGradTop    = Color(0xFF89C1FF)   // top specular sheen
    val PillGradCenter = Color(0xFF3C8EF5)
    val PillGradEdge   = Color(0xFF1E5FB8)

    /* Coral-red used for RAT titles ("LTE · Testing (Available)"). */
    val AccentRed = Color(0xFFFF5E55)

    /* Channel-number (DL/UL) blue — sampled from 1425 / 19425. */
    val ChannelBlue = Color(0xFF5BA8FF)

    /* Signal-strength ramp for RSRP/RSRQ gauge bars (mockup-matched). */
    val BarGreen = Color(0xFF6DD97A)
    val BarAmber = Color(0xFFF2B24C)
    val BarRed   = Color(0xFFE26A34)
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

/** iOS-blue primary pill — horizontal "dark-edge → bright-center → dark-edge"
 *  fill with a subtle top specular sheen, matching the premium mockup. */
fun Modifier.glassPrimaryPill(): Modifier = this
    .clip(RoundedCornerShape(100))
    .background(
        Brush.horizontalGradient(
            0.00f to GlassColors.PillGradEdge,
            0.50f to GlassColors.PillGradCenter,
            1.00f to GlassColors.PillGradEdge,
        ),
    )
    .background(
        Brush.verticalGradient(
            0.00f to GlassColors.PillGradTop.copy(alpha = 0.55f),
            0.55f to Color.Transparent,
            1.00f to Color.Black.copy(alpha = 0.18f),
        ),
    )
    .border(
        width = 1.dp,
        brush = Brush.verticalGradient(
            0.0f to Color.White.copy(alpha = 0.55f),
            1.0f to Color.White.copy(alpha = 0.10f),
        ),
        shape = RoundedCornerShape(100),
    )
