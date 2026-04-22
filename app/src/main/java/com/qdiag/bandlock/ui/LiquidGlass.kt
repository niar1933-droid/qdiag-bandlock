package com.qdiag.bandlock.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp

/**
 * ------------------------------------------------------------------
 *  iOS-26-style "Liquid Glass" design variant.
 *
 *  Frosted translucent panels, thin specular-highlight borders, vivid
 *  pastel gradient backdrop with soft coloured blobs, pill-shaped
 *  controls, iOS-blue primary accent.
 * ------------------------------------------------------------------
 */
enum class DesignVariant { Nsg, Glass }

object GlassColors {
    val BgTop = Color(0xFFDCE8FB)
    val BgMid = Color(0xFFEADFFB)
    val BgBot = Color(0xFFFBE3E8)

    val BlobBlue   = Color(0xFF6EA8FF)
    val BlobPurple = Color(0xFFB27CFF)
    val BlobPink   = Color(0xFFFF8EB0)

    val PanelFill       = Color(0xFFFFFFFF).copy(alpha = 0.55f)
    val PanelFillStrong = Color(0xFFFFFFFF).copy(alpha = 0.72f)
    val PanelBorder     = Color(0xFFFFFFFF).copy(alpha = 0.65f)
    val PanelBorderSoft = Color(0xFFFFFFFF).copy(alpha = 0.35f)

    val TextPrimary   = Color(0xFF1C1C1E)
    val TextSecondary = Color(0xFF3A3A3C).copy(alpha = 0.72f)
    val TextTertiary  = Color(0xFF3A3A3C).copy(alpha = 0.50f)

    val DividerSoft = Color(0xFF1C1C1E).copy(alpha = 0.08f)

    val Accent      = Color(0xFF007AFF)
    val AccentLight = Color(0xFF4DA3FF)
    val AccentDark  = Color(0xFF0051D5)
    val AccentRed   = Color(0xFFFF3B30)
}

/** Full-screen pastel gradient + soft coloured blobs. */
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
                .padding(start = 40.dp, top = 20.dp)
                .size(260.dp)
                .clip(CircleShape)
                .background(GlassColors.BlobBlue.copy(alpha = 0.45f)),
        )
        Box(
            modifier = Modifier
                .padding(start = 180.dp, top = 320.dp)
                .size(320.dp)
                .clip(CircleShape)
                .background(GlassColors.BlobPurple.copy(alpha = 0.40f)),
        )
        Box(
            modifier = Modifier
                .padding(start = 20.dp, top = 560.dp)
                .size(280.dp)
                .clip(CircleShape)
                .background(GlassColors.BlobPink.copy(alpha = 0.45f)),
        )
        content()
    }
}

/** Frosted-glass card surface. Translucent white fill + thin
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
            0.0f to Color.White.copy(alpha = 0.55f),
            1.0f to Color.White.copy(alpha = 0.10f),
        ),
        shape = RoundedCornerShape(100),
    )
