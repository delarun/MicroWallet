// Обновление самого приложения: баннер поверх экранов и карточка на экране «Устройство».
package io.microwallet.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.material.icons.rounded.SystemUpdate
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private val BUSY = setOf("downloading", "installing")

/** Новая версия приложения: что за версия, прогресс скачивания, ошибка, «Обновить». */
@Composable
fun AppUpdateBanner(vm: AppViewModel) {
    val u = vm.appUpdate ?: return
    val err = AppInstaller.error ?: vm.appUpdateError
    val shape = RoundedCornerShape(14.dp)
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 8.dp)
            .clip(shape)
            .background(C.card)
            .border(1.dp, C.accent, shape)
            .padding(start = 14.dp, end = 6.dp, top = 10.dp, bottom = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(Icons.Rounded.SystemUpdate, null, tint = C.accent, modifier = Modifier.size(20.dp))
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text("Новая версия приложения ${u.versionName}", color = C.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
            val sub = when {
                vm.appUpdatePhase == "downloading" -> "Скачиваю… ${(vm.appUpdateProgress * 100).toInt()}%"
                vm.appUpdatePhase == "installing" && err == null -> "Устанавливаю — приложение перезапустится"
                err != null -> "Не вышло: $err"
                else -> "Релиз ${u.tag} · ${u.size / 1024} КБ"
            }
            Text(sub, color = if (err != null) C.bad else C.muted, fontSize = 12.sp)
            if (vm.appUpdatePhase == "downloading") {
                Spacer(Modifier.height(6.dp))
                LinearProgressIndicator(progress = { vm.appUpdateProgress }, modifier = Modifier.fillMaxWidth())
            }
        }
        if (vm.appUpdatePhase !in BUSY || err != null) {
            TextButton(onClick = {
                AppInstaller.error = null
                vm.installAppUpdate()
            }) { Text(if (err != null) "Ещё раз" else "Обновить") }
        }
    }
}

/** Экран «Устройство»: версия приложения, автообновление, проверка вручную. */
@Composable
fun AppUpdateCard(vm: AppViewModel) {
    val (code, name) = vm.appVersion
    Card {
        Text("Приложение", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(6.dp))
        Text(
            "Версия $name ($code). Новые версии берутся из релизов того же источника, что и прошивки. " +
                "Автообновление ставит их само, пока телефон не подключён к устройству; иначе — баннер с кнопкой.",
            color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
        )
        Spacer(Modifier.height(10.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Обновлять автоматически", color = C.text, fontSize = 14.sp, modifier = Modifier.weight(1f))
            Switch(
                checked = vm.autoAppUpdate, onCheckedChange = { vm.changeAutoAppUpdate(it) },
                colors = SwitchDefaults.colors(checkedTrackColor = C.accent),
            )
        }
        Spacer(Modifier.height(10.dp))
        SecondaryButton(
            "Проверить обновления", { vm.checkAppUpdate(force = true) }, Modifier.fillMaxWidth(),
            enabled = vm.appUpdatePhase !in BUSY && vm.appUpdatePhase != "checking", icon = Icons.Rounded.SystemUpdate,
        )
    }
}
