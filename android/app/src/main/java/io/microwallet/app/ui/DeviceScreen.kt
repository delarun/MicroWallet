package io.microwallet.app.ui

import android.app.Activity
import android.view.WindowManager
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Apps
import androidx.compose.material.icons.rounded.Bluetooth
import androidx.compose.material.icons.rounded.LinkOff
import androidx.compose.material.icons.rounded.Restore
import androidx.compose.material.icons.rounded.SystemUpdate
import androidx.compose.material.icons.rounded.Warning
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.microwallet.app.core.WalletError
import io.microwallet.app.core.ADDRS_CHOICES
import io.microwallet.app.core.coinInfo
import io.microwallet.app.core.mnemonicToEntropy

@Composable
fun DeviceScreen(vm: AppViewModel) {
    val w = vm.wallet ?: return
    val info = vm.info
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        ScreenTitle("Устройство", if (vm.isDemo) "Демо-режим: без устройства и сети" else "Подключено по Bluetooth")
        Card {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier.size(48.dp).clip(RoundedCornerShape(14.dp)).background(C.accent.copy(alpha = 0.16f)),
                    contentAlignment = Alignment.Center,
                ) { Icon(Icons.Rounded.Bluetooth, null, tint = C.accent) }
                Spacer(Modifier.width(14.dp))
                Column(Modifier.weight(1f)) {
                    Text(w.label, style = MaterialTheme.typography.titleMedium)
                    vm.deviceAddress?.takeIf { !vm.isDemo }?.let { Text(it, fontFamily = Mono, fontSize = 12.sp, color = C.faint) }
                }
            }
            Spacer(Modifier.height(12.dp))
            InfoRow("Прошивка", "v${info["fw"] ?: "?"}" + (info["build"]?.takeIf { it != "-" }?.let { " · $it" } ?: ""))
            InfoRow("Плата", when (info["hw"]) {
                "esp32s3" -> "ESP32-S3"
                else -> info["hw"] ?: "?"
            })
            InfoRow("Сеть", info["net"] ?: w.net, if (w.net == "mainnet") C.ok else C.warn)
            InfoRow("Кошелёк", if (info["state"] == "locked") "заблокирован" else if (w.ready) "есть" else "пусто", if (w.ready) C.ok else C.warn)
            info["pin"]?.let { InfoRow("PIN", if (it == "1") "включён" else "не задан", if (it == "1") C.ok else C.warn) }
            info["secure"]?.let {
                InfoRow("Защита", if (it == "1") "Secure Boot, шифрование" else "выключена", if (it == "1") C.ok else C.warn)
            }
            val mv = info["batt"]?.toIntOrNull() ?: -1
            if (info["chg"] == "1") InfoRow("Аккумулятор", "заряжается от USB", C.ok)
            else if (mv > 0) InfoRow("Аккумулятор", "%.2f В · %d%%".format(mv / 1000.0, batteryPct(mv)))
            InfoRow("Монеты", vm.coins.joinToString(", ") { coinInfo(it).name })
            info["boot"]?.let {
                InfoRow("Запуск", bootReason(it), if (it.substringBefore('@') in setOf("panic", "wdt", "brown")) C.bad else C.text)
            }
        }
        Spacer(Modifier.height(14.dp))
        val locked = info["state"] == "locked"
        if (locked) {
            LockedCard()
            Spacer(Modifier.height(14.dp))
        } else if (!w.ready) {
            Card {
                Text("Восстановление", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(6.dp))
                Text("Устройство пустое. Можно записать на него существующую seed-фразу.", color = C.muted, fontSize = 14.sp)
                Spacer(Modifier.height(14.dp))
                PrimaryButton("Восстановить из seed-фразы", { vm.showRestore = true }, Modifier.fillMaxWidth(),
                    icon = Icons.Rounded.Restore)
            }
            Spacer(Modifier.height(14.dp))
        }
        if (info["hw"]?.startsWith("esp32") == true && w.ready) {
            Card {
                Text("Приложения", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(6.dp))
                Text(
                    "Новые монеты ставятся на устройство отдельно — например, Ethereum. Каждое приложение " +
                        "работает в песочнице и видит только свои ключи.",
                    color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
                )
                Spacer(Modifier.height(14.dp))
                PrimaryButton("Приложения", { vm.openApps() }, Modifier.fillMaxWidth(), icon = Icons.Rounded.Apps)
            }
            Spacer(Modifier.height(14.dp))
        }
        info["addrs"]?.let { cur ->
            Card {
                Text("Адреса для получения", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(6.dp))
                Text(
                    "Сколько адресов показывать в «Получить» на устройстве и в приложении. Столько же " +
                        "проверяется для баланса BTC и Pearl: меньше адресов — меньше запросов к сети. " +
                        "Если уже получал на адрес с большим номером, не уменьшай: его баланс пропадёт из виду.",
                    color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
                )
                Spacer(Modifier.height(14.dp))
                Segmented(ADDRS_CHOICES.map { it.toString() to it.toString() }, cur, onSelect = {
                    if (it != cur) vm.setAddrCount(it.toInt())
                })
            }
            Spacer(Modifier.height(14.dp))
        }
        if (info["hw"]?.startsWith("esp32") == true) {
            Card {
                Text("Прошивка", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(6.dp))
                Text(
                    "Новая версия с GitHub, с ПК или из файла — по Bluetooth. Устройство покажет размер и SHA-256 " +
                        "и установит прошивку только после удержания кнопки.",
                    color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
                )
                Spacer(Modifier.height(14.dp))
                PrimaryButton("Обновить прошивку", { vm.openFirmware() }, Modifier.fillMaxWidth(),
                    icon = Icons.Rounded.SystemUpdate)
            }
            Spacer(Modifier.height(14.dp))
        }
        if (w.ready && "btc" in vm.coins && !vm.isDemo) {
            PsbtCard(vm)
            Spacer(Modifier.height(14.dp))
        }
        if (w.ready && "prl" in vm.coins && !vm.isDemo) {
            PearlCard(vm)
            Spacer(Modifier.height(14.dp))
        }
        AppUpdateCard(vm)
        Spacer(Modifier.height(14.dp))
        Card {
            Text("Связь", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(6.dp))
            Text(
                "Пока телефон подключён, устройство не уходит в сон. Свернёшь приложение — через 2 минуты " +
                    "оно отключится само. Без связи устройство гасит экран через минуту и выключается через две; " +
                    "разбудить — кнопкой.",
                color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
            )
            Spacer(Modifier.height(14.dp))
            SecondaryButton(if (vm.isDemo) "Выйти из демо" else "Отключиться", { vm.disconnect() }, Modifier.fillMaxWidth(),
                icon = Icons.Rounded.LinkOff)
            if (!vm.isDemo) {
                Spacer(Modifier.height(10.dp))
                SecondaryButton("Забыть устройство", { vm.disconnect(forget = true) }, Modifier.fillMaxWidth(), danger = true)
                Text(
                    "Удалит ключ сопряжения на телефоне: при следующем подключении снова появится код.",
                    color = C.faint, fontSize = 12.sp, modifier = Modifier.padding(top = 8.dp),
                )
            }
        }
        Spacer(Modifier.height(16.dp))
    }
}

/** Причина последнего запуска (boot= в info) — для диагностики «само включилось».
 *  После сбоя прошивка добавляет шаг загрузки: brown@ble — просадка при запуске радио. */
fun bootReason(boot: String): String {
    val r = boot.substringBefore('@')
    val stage = boot.substringAfter('@', "")
    val where = when (stage) {
        "" -> ""
        "nvs", "pm", "usb" -> " при старте ($stage)"
        "ble" -> " при запуске Bluetooth"
        "init" -> " при запуске экрана и кнопок"
        "run" -> " во время работы"
        else -> " ($stage)"
    }
    return reasonText(r) + where
}

private fun reasonText(r: String): String = when {
    r == "pwr" -> "включение питания"
    r.startsWith("btn") -> "кнопка (из выключения)"
    r == "sw" -> "перезапуск (обновление и т. п.)"
    r == "panic" -> "сбой прошивки"
    r == "wdt" -> "сторожевой таймер"
    r == "brown" -> "просадка питания"
    r == "usb" -> "сброс по USB"
    r == "ext" -> "кнопка сброса"
    r.startsWith("wake") -> "пробуждение ($r)"
    else -> r
}

@Composable
private fun InfoRow(k: String, v: String, color: androidx.compose.ui.graphics.Color = C.text) {
    Divider()
    Row(Modifier.fillMaxWidth().padding(vertical = 11.dp)) {
        Text(k, color = C.muted, fontSize = 14.sp, modifier = Modifier.width(120.dp))
        Text(v, color = color, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
    }
}

// ---------------- восстановление из seed-фразы ----------------

@Composable
fun RestoreScreen(vm: AppViewModel) {
    val w = vm.wallet ?: return
    val ctx = LocalContext.current
    var phrase by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("info" to "") }

    // Фразу не должно быть видно на скриншотах и в списке недавних приложений
    DisposableEffect(Unit) {
        val win = (ctx as? Activity)?.window
        win?.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
        onDispose {
            win?.clearFlags(WindowManager.LayoutParams.FLAG_SECURE)
            phrase = ""
        }
    }
    BackHandler(enabled = !busy) { vm.showRestore = false }

    val words = phrase.lowercase().split(Regex("\\s+")).filter { it.isNotEmpty() }
    val check: Pair<Boolean, String> = when {
        words.isEmpty() -> false to ""
        words.size !in setOf(12, 18, 24) -> false to "Слов: ${words.size} (нужно 12, 18 или 24)"
        else -> try {
            mnemonicToEntropy(phrase, vm.words).fill(0)
            true to "✓  ${words.size} слов, контрольная сумма верна"
        } catch (e: WalletError) {
            false to (e.message ?: "")
        }
    }
    // Подсказки к последнему недописанному слову
    val last = if (phrase.isNotEmpty() && !phrase.last().isWhitespace()) words.lastOrNull() else null
    val suggestions = if (last != null && last.length >= 2 && last !in vm.words)
        vm.words.filter { it.startsWith(last) }.take(8) else emptyList()

    Column(
        Modifier
            .fillMaxSize()
            .imePadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        ScreenTitle("Восстановление", "Seed-фраза записывается на пустое устройство")
        Row(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(14.dp))
                .background(C.warn.copy(alpha = 0.12f))
                .padding(14.dp),
        ) {
            Icon(Icons.Rounded.Warning, null, tint = C.warn, modifier = Modifier.size(20.dp))
            Spacer(Modifier.width(10.dp))
            Text(
                "Вводить seed на телефоне небезопасно: так делают только на своём доверенном телефоне. " +
                    "Если фразы нет — создай новый кошелёк на самом устройстве.",
                color = C.warn, fontSize = 13.sp, lineHeight = 19.sp,
            )
        }
        Spacer(Modifier.height(14.dp))
        Card {
            FieldLabel("Слова через пробел")
            InputField(
                phrase, { phrase = it.lowercase() }, singleLine = false, minLines = 4, mono = true,
                placeholder = "abandon ability able …",
                keyboard = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Password),
            )
            if (suggestions.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (sgg in suggestions) Text(
                        sgg, color = C.text, fontSize = 14.sp, fontFamily = Mono,
                        modifier = Modifier
                            .clip(RoundedCornerShape(10.dp))
                            .background(C.card2)
                            .clickable { phrase = phrase.dropLast(last!!.length) + sgg + " " }
                            .padding(horizontal = 10.dp, vertical = 7.dp),
                    )
                }
            }
            Spacer(Modifier.height(10.dp))
            Status(if (check.first) "ok" else "bad", check.second)
            Spacer(Modifier.height(16.dp))
            PrimaryButton(
                "Записать на устройство",
                {
                    busy = true
                    vm.busy = true
                    status = "wait" to "Сверь первое и последнее слово на экране и удерживай кнопку на «Restore»…"
                    val p = phrase
                    vm.run({ w.restore(p) }, {
                        busy = false
                        vm.busy = false
                        phrase = ""
                        vm.restored()
                    }, {
                        busy = false
                        vm.busy = false
                        status = "bad" to it
                    })
                },
                Modifier.fillMaxWidth(), enabled = check.first, loading = busy, icon = Icons.Rounded.Restore,
            )
            Spacer(Modifier.height(10.dp))
            Status(status.first, status.second)
            Spacer(Modifier.height(6.dp))
            SecondaryButton("Отмена", { vm.showRestore = false }, Modifier.fillMaxWidth(), enabled = !busy)
        }
        Spacer(Modifier.height(16.dp))
    }
}

/** Pearl Wallet выводит ключи нестандартно (btcd #172): у части фраз его адреса PRL другие. */
@Composable
private fun PearlCard(vm: AppViewModel) {
    Card {
        Text("Pearl Wallet", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(6.dp))
        when (vm.pearlSame) {
            true -> Text("✓  Совместимо: Pearl Wallet с этой фразой покажет те же адреса PRL.",
                color = C.ok, fontSize = 13.sp, lineHeight = 19.sp)
            false -> Text(
                "Не совместимо: Pearl Wallet с этой фразой покажет другие адреса PRL. Монеты на адресах " +
                    "устройства в безопасности, но тратить их нужно этим устройством.",
                color = C.warn, fontSize = 13.sp, lineHeight = 19.sp,
            )
            null -> {
                Text(
                    "Pearl Wallet выводит ключи нестандартно, и примерно у 0.8 % фраз его адреса PRL отличаются " +
                        "от адресов устройства. Проверка не раскрывает ключи.",
                    color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
                )
                Spacer(Modifier.height(12.dp))
                SecondaryButton(if (vm.pearlChecking) "Проверяю…" else "Проверить совместимость", { vm.checkPearl() },
                    Modifier.fillMaxWidth())
            }
        }
    }
}
