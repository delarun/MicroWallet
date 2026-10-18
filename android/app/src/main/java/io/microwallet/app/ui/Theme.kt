package io.microwallet.app.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

/** Палитра — та же, что в приложении для ПК (host/app.py). */
object C {
    val bg = Color(0xFF0E0F13)
    val side = Color(0xFF121318)
    val card = Color(0xFF16171E)
    val card2 = Color(0xFF1B1C25)
    val line = Color(0xFF242530)
    val text = Color(0xFFECEDF2)
    val muted = Color(0xFF8B8D98)
    val faint = Color(0xFF5D5F6B)
    val accent = Color(0xFF7C6CFF)
    val accent2 = Color(0xFF5B8CFF)
    val ok = Color(0xFF3DD68C)
    val warn = Color(0xFFF5B544)
    val bad = Color(0xFFFF5C6C)
    val field = Color(0xFF101117)
    val fieldLine = Color(0xFF272833)
    val button = Color(0xFF1E1F29)
    val buttonLine = Color(0xFF2A2B36)
    val gradient = Brush.horizontalGradient(listOf(accent, accent2))
}

val COIN_COLORS = mapOf(
    "btc" to Color(0xFFF7931A),
    "tron" to Color(0xFFEF0027),
    "bsc" to Color(0xFFF3BA2F),
    "prl" to Color(0xFFB9C7DA),
    "usdt" to Color(0xFF26A17B),
    "usdc" to Color(0xFF2775CA),
    "eth" to Color(0xFF627EEA),
    "ton" to Color(0xFF0098EA),
    "dai" to Color(0xFFF5AC37),
    "usde" to Color(0xFF3A3A44),
    "pyusd" to Color(0xFF0070E0),
    "shib" to Color(0xFFE42D04),
    "pepe" to Color(0xFF4C9540),
)

val Mono = FontFamily.Monospace

private val type = Typography(
    headlineMedium = TextStyle(fontSize = 26.sp, fontWeight = FontWeight.Bold, color = C.text),
    titleLarge = TextStyle(fontSize = 20.sp, fontWeight = FontWeight.Bold, color = C.text),
    titleMedium = TextStyle(fontSize = 17.sp, fontWeight = FontWeight.SemiBold, color = C.text),
    bodyLarge = TextStyle(fontSize = 16.sp, color = C.text),
    bodyMedium = TextStyle(fontSize = 14.sp, color = C.text),
    bodySmall = TextStyle(fontSize = 12.sp, color = C.muted),
    labelLarge = TextStyle(fontSize = 15.sp, fontWeight = FontWeight.SemiBold),
)

@Composable
fun MicroTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = C.accent,
            onPrimary = Color.White,
            secondary = C.accent2,
            background = C.bg,
            onBackground = C.text,
            surface = C.card,
            onSurface = C.text,
            surfaceVariant = C.card2,
            onSurfaceVariant = C.muted,
            surfaceContainer = C.card,
            surfaceContainerHigh = C.card2,
            outline = C.line,
            error = C.bad,
        ),
        typography = type,
        content = content,
    )
}
