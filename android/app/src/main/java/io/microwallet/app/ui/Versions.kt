// Каталоги версий (прошивка, приложения): одна строка на позицию, версия выбирается при
// скачивании; установленное — зелёным, доступное обновление — акцентом.
package io.microwallet.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.CloudDownload
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** "v0.7.0" и "0.7" равны; сравнение по числам, недостающие — нули. */
fun versionCmp(a: String, b: String): Int {
    fun parts(s: String) = s.trim().removePrefix("v").split('.', '-').map { it.toIntOrNull() ?: 0 }
    val x = parts(a)
    val y = parts(b)
    for (i in 0 until maxOf(x.size, y.size)) {
        val c = (x.getOrElse(i) { 0 }).compareTo(y.getOrElse(i) { 0 })
        if (c != 0) return c
    }
    return 0
}

/** Одна версия в меню выбора. */
class VersionOption(val version: String, val note: String, val test: Boolean = false, val pick: () -> Unit)

/**
 * Строка каталога: значок, название, последняя версия. installed — что стоит сейчас (null —
 * не стоит). Если стоит последняя — строка зелёная; если старее — «обновление до …».
 * «Скачать» открывает меню всех версий (новые сверху), установленная отмечена галочкой.
 */
@Composable
fun VersionRow(
    title: String,
    installed: String?,
    options: List<VersionOption>,
    enabled: Boolean,
    subtitle: String? = null,
    icon: @Composable () -> Unit,
) {
    val latest = options.firstOrNull()?.version
    val upToDate = installed != null && latest != null && versionCmp(installed, latest) >= 0
    val update = installed != null && latest != null && versionCmp(installed, latest) < 0
    val tint = when {
        upToDate -> C.ok
        update -> C.accent
        else -> null
    }
    val shape = RoundedCornerShape(14.dp)
    var open by remember { mutableStateOf(false) }
    Row(
        Modifier
            .fillMaxWidth()
            .padding(top = 10.dp)
            .clip(shape)
            .background(tint?.copy(alpha = 0.10f) ?: Color.Transparent)
            .border(1.dp, tint?.copy(alpha = 0.55f) ?: C.line, shape)
            .padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        icon()
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(title, color = C.text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
            val status = when {
                upToDate -> "установлена v${installed!!.removePrefix("v")} — последняя"
                update -> "установлена v${installed!!.removePrefix("v")} · обновление до v${latest!!.removePrefix("v")}"
                latest != null -> "последняя v${latest.removePrefix("v")}"
                else -> "нет в релизах"
            }
            Text(status, color = tint ?: C.faint, fontSize = 12.sp, fontWeight = if (tint != null) FontWeight.SemiBold else null)
            if (subtitle != null) Text(subtitle, color = C.faint, fontSize = 12.sp)
        }
        Spacer(Modifier.width(8.dp))
        Box {
            SecondaryButton(
                if (update) "Обновить" else "Скачать", { open = true }, Modifier.height(44.dp),
                enabled = enabled && options.isNotEmpty(), icon = Icons.Rounded.CloudDownload,
            )
            DropdownMenu(expanded = open, onDismissRequest = { open = false }, containerColor = C.card2) {
                for (o in options) {
                    val mine = installed != null && versionCmp(o.version, installed) == 0
                    DropdownMenuItem(
                        text = {
                            Column {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Text("v${o.version.removePrefix("v")}", color = if (mine) C.ok else C.text,
                                        fontWeight = FontWeight.SemiBold, fontSize = 15.sp)
                                    if (o.test) {
                                        Spacer(Modifier.width(8.dp))
                                        Badge("ТЕСТ", C.warn)
                                    }
                                }
                                Text(if (mine) "установлена · ${o.note}" else o.note, color = C.faint, fontSize = 12.sp)
                            }
                        },
                        trailingIcon = { if (mine) Icon(Icons.Rounded.Check, null, tint = C.ok, modifier = Modifier.size(18.dp)) },
                        onClick = {
                            open = false
                            o.pick()
                        },
                    )
                }
            }
        }
    }
}
