// PSBT на телефоне: кошелёк ведёт Sparrow/Electrum (watch-only), подписывает устройство.
package io.microwallet.app.ui

import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.ContentPaste
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.Send
import androidx.compose.material.icons.rounded.Share
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun PsbtCard(vm: AppViewModel) {
    val p = vm.psbt
    val ctx = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val open = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.psbtLoad(runCatching { ctx.contentResolver.openInputStream(uri)?.use { it.readBytes() } }.getOrNull())
    }
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
        if (uri != null) runCatching { ctx.contentResolver.openOutputStream(uri)?.use { it.write(p.signed.toByteArray()) } }
            .onSuccess { vm.message = "PSBT сохранён" }.onFailure { vm.message = "Не сохранилось: ${it.message}" }
    }
    fun share(text: String, title: String) = runCatching {
        ctx.startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, text), title))
    }

    Card {
        Text("Bitcoin: PSBT и Sparrow", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(6.dp))
        Text(
            "Кошелёк ведёт Sparrow или Electrum (watch-only), подписывает устройство: получателей, сдачу и " +
                "суммы входов оно проверяет само.",
            color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
        )
        Spacer(Modifier.height(12.dp))
        val d = p.descriptor
        if (d == null) {
            SecondaryButton("Дескриптор для Sparrow", { vm.loadDescriptor() }, Modifier.fillMaxWidth(), icon = Icons.Rounded.Share)
        } else {
            Text("Sparrow: File → New Wallet → Watch Only → Output Descriptor", color = C.faint, fontSize = 12.sp)
            Spacer(Modifier.height(6.dp))
            Text(d, fontFamily = Mono, fontSize = 12.sp, color = C.text, lineHeight = 17.sp)
            Spacer(Modifier.height(8.dp))
            Row {
                SecondaryButton("Скопировать", { clipboard.setText(AnnotatedString(d)); vm.message = "Дескриптор скопирован" },
                    Modifier.weight(1f), icon = Icons.Rounded.ContentCopy)
                Spacer(Modifier.width(10.dp))
                SecondaryButton("Поделиться", { share(d, "Дескриптор MicroWallet") }, Modifier.weight(1f), icon = Icons.Rounded.Share)
            }
        }
        Spacer(Modifier.height(14.dp))
        Divider()
        Spacer(Modifier.height(12.dp))
        when (p.phase) {
            "", "error" -> {
                if (p.phase == "error") {
                    Text(p.error, color = C.bad, fontSize = 13.sp, lineHeight = 18.sp)
                    Spacer(Modifier.height(10.dp))
                }
                Text("Подписать PSBT", color = C.text, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.height(8.dp))
                Row {
                    SecondaryButton("Из файла", { open.launch(arrayOf("*/*")) }, Modifier.weight(1f), icon = Icons.Rounded.FolderOpen)
                    Spacer(Modifier.width(10.dp))
                    SecondaryButton("Из буфера", { vm.psbtLoad(clipboard.getText()?.text?.toByteArray()) }, Modifier.weight(1f),
                        icon = Icons.Rounded.ContentPaste)
                }
            }
            "loaded" -> {
                Text("PSBT: ${p.summary}", color = C.text, fontSize = 14.sp)
                Text("Устройство покажет получателей, суммы и комиссию — сверь их с Sparrow.", color = C.faint, fontSize = 12.sp)
                Spacer(Modifier.height(10.dp))
                PrimaryButton("Подписать", { vm.psbtSign(send = false) }, Modifier.fillMaxWidth())
                Spacer(Modifier.height(8.dp))
                SecondaryButton("Подписать и отправить", { vm.psbtSign(send = true) }, Modifier.fillMaxWidth(), icon = Icons.Rounded.Send)
                Spacer(Modifier.height(8.dp))
                SecondaryButton("Отмена", { p.reset() }, Modifier.fillMaxWidth())
            }
            "signing" -> Text("Подтверди на устройстве: сверь получателей и комиссию, удерживай «Accept»…",
                color = C.warn, fontSize = 14.sp, lineHeight = 20.sp)
            "sending" -> Text("Отправляю в сеть…", color = C.muted, fontSize = 14.sp)
            "signed", "sent" -> {
                Text(if (p.phase == "sent") "Отправлено" else "Подписано", color = C.ok, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                if (p.txid.isNotEmpty()) {
                    Spacer(Modifier.height(6.dp))
                    Text("txid ${p.txid}", fontFamily = Mono, fontSize = 12.sp, color = C.muted)
                }
                Spacer(Modifier.height(10.dp))
                Row {
                    SecondaryButton("Поделиться", { share(p.signed, "Подписанный PSBT") }, Modifier.weight(1f), icon = Icons.Rounded.Share)
                    Spacer(Modifier.width(10.dp))
                    SecondaryButton("Скопировать", { clipboard.setText(AnnotatedString(p.signed)); vm.message = "PSBT скопирован" },
                        Modifier.weight(1f), icon = Icons.Rounded.ContentCopy)
                }
                Spacer(Modifier.height(8.dp))
                Row {
                    SecondaryButton("Сохранить файл", { save.launch("signed.psbt") }, Modifier.weight(1f))
                    Spacer(Modifier.width(10.dp))
                    SecondaryButton("Готово", { p.reset() }, Modifier.weight(1f))
                }
            }
        }
    }
}
