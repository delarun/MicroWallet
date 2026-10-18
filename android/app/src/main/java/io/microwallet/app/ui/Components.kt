package io.microwallet.app.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.google.zxing.BarcodeFormat
import com.google.zxing.EncodeHintType
import com.google.zxing.qrcode.QRCodeWriter
import com.google.zxing.qrcode.decoder.ErrorCorrectionLevel
import io.microwallet.app.core.COINS
import io.microwallet.app.core.groupAddr
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.Close

// ---------------- карточки и кнопки ----------------

@Composable
fun Card(
    modifier: Modifier = Modifier,
    padding: PaddingValues = PaddingValues(20.dp),
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(C.card)
            .border(1.dp, C.line, RoundedCornerShape(20.dp))
            .padding(padding),
        content = content,
    )
}

@Composable
fun PrimaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    icon: ImageVector? = null,
    loading: Boolean = false,
) {
    val shape = RoundedCornerShape(14.dp)
    Row(
        modifier
            .height(52.dp)
            .clip(shape)
            .background(if (enabled && !loading) C.gradient else SolidColor(Color(0xFF2A2B3A)))
            .clickable(enabled = enabled && !loading, onClick = onClick)
            .padding(horizontal = 20.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center,
    ) {
        val color = if (enabled || loading) Color.White else C.faint
        if (loading) {
            CircularProgressIndicator(Modifier.size(18.dp), color = Color.White, strokeWidth = 2.dp)
            Spacer(Modifier.width(10.dp))
        } else if (icon != null) {
            Icon(icon, null, tint = color, modifier = Modifier.size(20.dp))
            Spacer(Modifier.width(10.dp))
        }
        Text(text, color = color, style = MaterialTheme.typography.labelLarge)
    }
}

@Composable
fun SecondaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    icon: ImageVector? = null,
    danger: Boolean = false,
) {
    val shape = RoundedCornerShape(14.dp)
    val fg = when {
        !enabled -> C.faint
        danger -> C.bad
        else -> C.text
    }
    Row(
        modifier
            .height(48.dp)
            .clip(shape)
            .background(if (danger) Color(0xFF26151A) else C.button)
            .border(1.dp, if (danger) Color(0xFF45202A) else C.buttonLine, shape)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center,
    ) {
        if (icon != null) {
            Icon(icon, null, tint = fg, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
        }
        Text(text, color = fg, style = MaterialTheme.typography.labelLarge)
    }
}

@Composable
fun Badge(text: String, color: Color) {
    Text(
        text,
        color = color,
        fontSize = 11.sp,
        fontWeight = FontWeight.Bold,
        letterSpacing = 0.6.sp,
        modifier = Modifier
            .clip(RoundedCornerShape(8.dp))
            .background(color.copy(alpha = 0.14f))
            .padding(horizontal = 8.dp, vertical = 3.dp),
    )
}

@Composable
fun ScreenTitle(title: String, subtitle: String? = null, trailing: @Composable RowScope.() -> Unit = {}) {
    Row(Modifier.fillMaxWidth().padding(bottom = 16.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.headlineMedium)
            if (subtitle != null) Text(subtitle, color = C.muted, fontSize = 14.sp)
        }
        trailing()
    }
}

@Composable
fun FieldLabel(text: String) {
    Text(text, color = C.muted, fontSize = 13.sp, modifier = Modifier.padding(bottom = 6.dp, top = 4.dp))
}

/** Поле ввода в стиле ПК-приложения: тёмное, со скруглением, без плавающей подписи. */
@Composable
fun InputField(
    value: String,
    onChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    placeholder: String = "",
    mono: Boolean = false,
    big: Boolean = false,
    singleLine: Boolean = true,
    minLines: Int = 1,
    keyboard: KeyboardOptions = KeyboardOptions.Default,
    trailing: @Composable RowScope.() -> Unit = {},
) {
    val style = TextStyle(
        color = C.text,
        fontSize = if (big) 22.sp else 15.sp,
        fontWeight = if (big) FontWeight.Bold else FontWeight.Normal,
        fontFamily = if (mono) Mono else null,
    )
    BasicTextField(
        value = value,
        onValueChange = onChange,
        singleLine = singleLine,
        minLines = minLines,
        textStyle = style,
        keyboardOptions = keyboard,
        visualTransformation = VisualTransformation.None,
        cursorBrush = SolidColor(C.accent),
        modifier = modifier.fillMaxWidth(),
        decorationBox = { inner ->
            Row(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(12.dp))
                    .background(C.field)
                    .border(1.dp, C.fieldLine, RoundedCornerShape(12.dp))
                    .padding(start = 14.dp, end = 6.dp, top = 12.dp, bottom = 12.dp),
                verticalAlignment = if (singleLine) Alignment.CenterVertically else Alignment.Top,
            ) {
                Box(Modifier.weight(1f).padding(end = 8.dp)) {
                    if (value.isEmpty()) Text(placeholder, style = style.copy(color = C.faint, fontWeight = FontWeight.Normal))
                    inner()
                }
                trailing()
            }
        },
    )
}

@Composable
fun SmallIconButton(icon: ImageVector, description: String, onClick: () -> Unit, enabled: Boolean = true) {
    Box(
        Modifier
            .size(40.dp)
            .clip(CircleShape)
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, description, tint = if (enabled) C.accent2 else C.faint, modifier = Modifier.size(22.dp))
    }
}

// ---------------- выбор монеты, сегменты ----------------

@Composable
fun CoinPills(coins: List<String>, selected: String, onSelect: (String) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(bottom = 14.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        for (c in COINS.map { it.id }.filter { it in coins }) {
            val on = c == selected
            Row(
                Modifier
                    .clip(RoundedCornerShape(22.dp))
                    .background(if (on) Color(0xFF20213A) else C.card)
                    .border(1.dp, if (on) C.accent else C.line, RoundedCornerShape(22.dp))
                    .clickable { onSelect(c) }
                    .padding(start = 8.dp, end = 14.dp, top = 7.dp, bottom = 7.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CoinIcon(c, 22.dp)
                Spacer(Modifier.width(8.dp))
                Text(COINS.first { it.id == c }.name, color = if (on) C.text else C.muted, fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold)
            }
        }
    }
}

@Composable
fun Segmented(items: List<Pair<String, String>>, selected: String?, onSelect: (String) -> Unit, modifier: Modifier = Modifier) {
    Row(
        modifier
            .clip(RoundedCornerShape(12.dp))
            .background(C.field)
            .border(1.dp, C.fieldLine, RoundedCornerShape(12.dp))
            .padding(4.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        for ((key, text) in items) {
            val on = key == selected
            Box(
                Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(9.dp))
                    .background(if (on) Color(0xFF2A2B3C) else Color.Transparent)
                    .clickable { onSelect(key) }
                    .padding(vertical = 9.dp, horizontal = 6.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(text, color = if (on) C.text else C.muted, fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
                    textAlign = TextAlign.Center, lineHeight = 16.sp)
            }
        }
    }
}

@Composable
fun Status(kind: String, text: String, modifier: Modifier = Modifier) {
    if (text.isEmpty()) return
    val color = when (kind) {
        "ok" -> C.ok
        "bad" -> C.bad
        "wait" -> C.warn
        else -> C.muted
    }
    Text(text, color = color, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, modifier = modifier)
}

@Composable
fun AddressBox(addr: String, modifier: Modifier = Modifier) {
    Text(
        groupAddr(addr),
        fontFamily = Mono,
        fontSize = 16.sp,
        lineHeight = 24.sp,
        color = C.text,
        modifier = modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(C.card2)
            .padding(horizontal = 16.dp, vertical = 14.dp),
    )
}


// ---------------- значки монет (как coin_icon в host/app.py) ----------------

@Composable
fun CoinIcon(kind: String, size: Dp, modifier: Modifier = Modifier) {
    val measurer = rememberTextMeasurer()
    Canvas(modifier.size(size)) {
        val w = this.size.width
        val s = w / 40f
        val white = Color.White
        when (kind) {
            "prl" -> {  // жемчужина: радиальный градиент и блик
                drawCircle(
                    Brush.radialGradient(
                        0f to Color.White, 0.45f to Color(0xFFDCE4F0), 1f to Color(0xFF7F8CA6),
                        center = Offset(w * .38f, w * .32f), radius = w * .7f,
                    )
                )
                drawOval(Color(255, 255, 255, 200), topLeft = Offset(w * .24f, w * .18f), size = Size(w * .22f, w * .15f))
            }
            "logo" -> {
                drawRoundRect(Brush.linearGradient(listOf(C.accent, C.accent2), Offset.Zero, Offset(w, w)),
                    cornerRadius = CornerRadius(w * .28f))
                drawRoundRect(white, Offset(w * .25f, w * .3f), Size(w * .5f, w * .3f), CornerRadius(3 * s))
                drawRect(C.accent, Offset(w * .31f, w * .36f), Size(w * .38f, w * .18f))
                drawCircle(white, w * .06f, Offset(w * .5f, w * .72f))
            }
            else -> {
                drawCircle(COIN_COLORS[kind] ?: C.accent)
                val c = Offset(w / 2, w / 2)
                when (kind) {
                    "btc" -> rotate(14f, c) {
                        val t = measurer.measure("B", TextStyle(fontSize = (24 * s).toSp(), fontWeight = FontWeight.Bold, color = white))
                        drawText(t, topLeft = Offset(c.x - t.size.width / 2f, c.y - t.size.height / 2f))
                        for (x in listOf(-3 * s, 2 * s)) {
                            drawLine(white, Offset(c.x + x, c.y - 12 * s), Offset(c.x + x, c.y - 8.5f * s), 2.2f * s)
                            drawLine(white, Offset(c.x + x, c.y + 8.5f * s), Offset(c.x + x, c.y + 12 * s), 2.2f * s)
                        }
                    }
                    "tron" -> {
                        val st = Stroke(2.2f * s, cap = StrokeCap.Round, join = StrokeJoin.Round)
                        val p = Path().apply {
                            moveTo(10 * s, 11 * s); lineTo(31 * s, 15 * s); lineTo(19 * s, 31 * s); close()
                            moveTo(10 * s, 11 * s); lineTo(22 * s, 21 * s); lineTo(31 * s, 15 * s)
                            moveTo(22 * s, 21 * s); lineTo(19 * s, 31 * s)
                        }
                        drawPath(p, white, style = st)
                    }
                    "bsc" -> rotate(45f, c) {
                        val dark = Color(0xFF1E2026)
                        for ((dx, dy, sq) in listOf(
                            Triple(0f, 0f, 6f), Triple(-7.5f, -7.5f, 4f), Triple(7.5f, 7.5f, 4f),
                            Triple(-7.5f, 7.5f, 4f), Triple(7.5f, -7.5f, 4f),
                        )) drawRect(dark, Offset(c.x + (dx - sq / 2) * s, c.y + (dy - sq / 2) * s), Size(sq * s, sq * s))
                    }
                    "eth" -> {  // ромб Ethereum: верхняя пирамида и нижний шеврон
                        val top = Path().apply {
                            moveTo(20 * s, 6 * s); lineTo(29 * s, 20.5f * s); lineTo(20 * s, 25.5f * s); lineTo(11 * s, 20.5f * s); close()
                        }
                        val bottom = Path().apply {
                            moveTo(20 * s, 27.5f * s); lineTo(29 * s, 22.5f * s); lineTo(20 * s, 34 * s); lineTo(11 * s, 22.5f * s); close()
                        }
                        drawPath(top, white)
                        drawPath(bottom, white.copy(alpha = 0.8f))
                    }
                    "ton" -> {  // перевёрнутый треугольник с чертой посередине
                        val st = Stroke(2.2f * s, cap = StrokeCap.Round, join = StrokeJoin.Round)
                        val p = Path().apply {
                            moveTo(11 * s, 13 * s); lineTo(29 * s, 13 * s); lineTo(20 * s, 30 * s); close()
                            moveTo(20 * s, 13 * s); lineTo(20 * s, 30 * s)
                        }
                        drawPath(p, white, style = st)
                    }
                    "usdc" -> {
                        drawCircle(white, 10 * s, c, style = Stroke(2.2f * s))
                        val t = measurer.measure("$", TextStyle(fontSize = (15 * s).toSp(), fontWeight = FontWeight.Bold, color = white))
                        drawText(t, topLeft = Offset(c.x - t.size.width / 2f, c.y - t.size.height / 2f))
                    }
                    "usdt" -> {
                        drawRect(white, Offset(11 * s, 10 * s), Size(18 * s, 4 * s))
                        drawRect(white, Offset(18 * s, 10 * s), Size(4 * s, 20 * s))
                        drawOval(white, Offset(9 * s, 16 * s), Size(22 * s, 6 * s), style = Stroke(2 * s))
                    }
                }
            }
        }
    }
}

// ---------------- QR-код ----------------

@Composable
fun QrCode(text: String, size: Dp, center: String? = null) {
    val matrix = remember(text) {
        runCatching {
            QRCodeWriter().encode(
                text, BarcodeFormat.QR_CODE, 0, 0,
                mapOf(EncodeHintType.MARGIN to 0, EncodeHintType.ERROR_CORRECTION to ErrorCorrectionLevel.H),
            )
        }.getOrNull()
    }
    Box(
        Modifier
            .size(size)
            .clip(RoundedCornerShape(18.dp))
            .background(Color.White)
            .padding(14.dp),
        contentAlignment = Alignment.Center,
    ) {
        if (matrix != null) Canvas(Modifier.fillMaxWidth().height(size - 28.dp)) {
            val n = matrix.width
            val cell = this.size.width / n
            val dark = C.bg
            for (y in 0 until n) for (x in 0 until n) if (matrix.get(x, y))
                drawRect(dark, Offset(x * cell, y * cell), Size(cell + 0.6f, cell + 0.6f))
        }
        if (center != null) Box(
            Modifier
                .size(size * 0.2f)
                .clip(CircleShape)
                .background(Color.White)
                .padding(4.dp),
        ) { CoinIcon(center, size * 0.2f - 8.dp) }
    }
}

@Composable
fun ResultIcon(ok: Boolean) {
    val col = if (ok) C.ok else C.bad
    Box(
        Modifier.size(72.dp).clip(CircleShape).background(col.copy(alpha = 0.16f)),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            if (ok) Icons.Rounded.Check else Icons.Rounded.Close,
            null, tint = col, modifier = Modifier.size(38.dp),
        )
    }
}

@Composable
fun Divider() {
    Box(Modifier.fillMaxWidth().height(1.dp).background(C.line))
}

/** Монет на устройстве нет: с v0.6 они ставятся приложениями. */
@Composable
fun NoCoins(onApps: () -> Unit) {
    Card {
        Text("Монеты не установлены", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(8.dp))
        Text(
            "С версии 0.6 Bitcoin, Pearl, TRON, BNB Chain и Ethereum — приложения на устройстве. " +
                "Поставь нужные: адреса останутся прежними, они выводятся из того же seed.",
            color = C.muted, fontSize = 14.sp, lineHeight = 20.sp,
        )
        Spacer(Modifier.height(16.dp))
        PrimaryButton("Приложения", onApps, Modifier.fillMaxWidth())
    }
}
