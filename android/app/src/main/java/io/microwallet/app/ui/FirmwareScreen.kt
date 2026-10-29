package io.microwallet.app.ui

import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material.icons.rounded.CloudDownload
import androidx.compose.material.icons.rounded.Computer
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.SystemUpdate
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.microwallet.app.core.FwImage
import io.microwallet.app.core.FwRelease
import io.microwallet.app.core.DEFAULT_REPO
import io.microwallet.app.core.fwAssetName

enum class FwPhase { Pick, Downloading, Ready, Confirm, Sending, Done, Error }

/** Состояние обновления живёт в ViewModel: передача идёт, даже если экран пересоздался. */
class FwState {
    var source by mutableStateOf("github")  // github | pc | file
    var repo by mutableStateOf("")
    var pcUrl by mutableStateOf("http://localhost:8777")
    var releases by mutableStateOf<Load<List<FwRelease>>?>(null)
    var image by mutableStateOf<FwImage?>(null)
    var imageFrom by mutableStateOf("")
    var imageNet by mutableStateOf<String?>(null)  // по имени файла: testnet | mainnet
    var phase by mutableStateOf(FwPhase.Pick)
    var progress by mutableStateOf(0f)
    var detail by mutableStateOf("")
    var error by mutableStateOf("")

    /** Новое обновление с чистого листа: после установки или ошибки прошлое состояние не нужно. */
    fun restart() {
        phase = FwPhase.Pick
        image = null
        imageNet = null
        releases = null
        progress = 0f
        detail = ""
        error = ""
    }
}

fun chipName(hw: String?) = when (hw) {
    "esp32s3" -> "ESP32-S3"
    "esp32s3-ws147" -> "Waveshare ESP32-S3 Touch LCD 1.47"
    else -> hw ?: "?"
}

fun kb(n: Long) = if (n >= 1024 * 1024) "%.1f МБ".format(n / 1048576.0) else "${(n + 512) / 1024} КБ"

@Composable
fun FirmwareScreen(vm: AppViewModel) {
    val w = vm.wallet ?: return
    val f = vm.fw
    val busy = f.phase == FwPhase.Confirm || f.phase == FwPhase.Sending || f.phase == FwPhase.Downloading
    BackHandler(enabled = !busy) { vm.showFirmware = false }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.fwFromFile(uri)
    }
    val hw = vm.info["hw"] ?: w.info["hw"] ?: "?"
    val net = vm.info["net"] ?: w.net

    Column(
        Modifier
            .fillMaxSize()
            .imePadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        ScreenTitle("Прошивка", "Обновление по Bluetooth, с подтверждением на устройстве")
        Card {
            Text("Сейчас на устройстве", color = C.muted, fontSize = 13.sp)
            Spacer(Modifier.height(4.dp))
            Text(
                "v${vm.info["fw"] ?: "?"} · ${chipName(hw)} · $net",
                style = MaterialTheme.typography.titleMedium,
            )
            vm.info["build"]?.takeIf { it != "-" }?.let {
                Text("сборка $it", color = C.faint, fontSize = 12.sp, fontFamily = Mono)
            }
        }
        Spacer(Modifier.height(14.dp))

        when (f.phase) {
            FwPhase.Pick, FwPhase.Downloading -> PickSource(vm, hw, net) { pick.launch(arrayOf("*/*")) }
            FwPhase.Ready -> ImageCard(vm, f.image!!)
            FwPhase.Confirm, FwPhase.Sending -> Sending(f)
            FwPhase.Done -> Card {
                Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                    ResultIcon(true)
                    Spacer(Modifier.height(14.dp))
                    Text("Прошивка установлена", style = MaterialTheme.typography.titleLarge)
                    Spacer(Modifier.height(6.dp))
                    Text(
                        "Устройство перезагружается. Приложение переподключится само.",
                        color = C.muted, fontSize = 14.sp, textAlign = TextAlign.Center,
                    )
                    Spacer(Modifier.height(16.dp))
                    PrimaryButton("Готово", { vm.fw.restart(); vm.showFirmware = false }, Modifier.fillMaxWidth())
                }
            }
            FwPhase.Error -> Card {
                Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                    ResultIcon(false)
                    Spacer(Modifier.height(14.dp))
                    Text("Не обновлено", style = MaterialTheme.typography.titleLarge)
                    Spacer(Modifier.height(6.dp))
                    Text(f.error, color = C.bad, fontSize = 14.sp, textAlign = TextAlign.Center)
                    Spacer(Modifier.height(16.dp))
                    PrimaryButton("Назад", { f.phase = if (f.image != null) FwPhase.Ready else FwPhase.Pick },
                        Modifier.fillMaxWidth())
                }
            }
        }
        Spacer(Modifier.height(14.dp))
        if (!busy) SecondaryButton("Закрыть", { vm.showFirmware = false }, Modifier.fillMaxWidth())
        Spacer(Modifier.height(16.dp))
    }
}

@Composable
private fun PickSource(vm: AppViewModel, hw: String, net: String, pickFile: () -> Unit) {
    val f = vm.fw
    Card {
        FieldLabel("Откуда взять")
        Segmented(
            listOf("github" to "GitHub", "pc" to "ПК", "file" to "Файл"), f.source,
            { f.source = it; f.releases = null; vm.saveFwPrefs() }, Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(14.dp))
        when (f.source) {
            "github" -> {
                FieldLabel("Репозиторий")
                InputField(
                    f.repo, { f.repo = it.trim() }, placeholder = DEFAULT_REPO, mono = true,
                    keyboard = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Uri),
                )
                Spacer(Modifier.height(12.dp))
                PrimaryButton("Найти релизы", { vm.fwLoadReleases() }, Modifier.fillMaxWidth(),
                    enabled = f.repo.isNotBlank(), loading = f.releases == Load.Loading, icon = Icons.Rounded.CloudDownload)
            }
            "pc" -> {
                FieldLabel("Адрес сервера")
                InputField(
                    f.pcUrl, { f.pcUrl = it.trim() }, mono = true,
                    keyboard = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Uri),
                )
                Text(
                    "На ПК: python3 tools/fw_server.py\nи для телефона по USB: adb reverse tcp:8777 tcp:8777",
                    color = C.faint, fontSize = 12.sp, fontFamily = Mono, lineHeight = 17.sp,
                    modifier = Modifier.padding(top = 8.dp),
                )
                Spacer(Modifier.height(12.dp))
                PrimaryButton("Загрузить список", { vm.fwLoadReleases() }, Modifier.fillMaxWidth(),
                    enabled = f.pcUrl.isNotBlank(), loading = f.releases == Load.Loading, icon = Icons.Rounded.Computer)
            }
            else -> {
                Text(
                    "Выбери файл microwallet-${hw}-${net}-app.bin — например, скопированный на телефон с ПК.",
                    color = C.muted, fontSize = 14.sp, lineHeight = 20.sp,
                )
                Spacer(Modifier.height(12.dp))
                PrimaryButton("Выбрать файл", pickFile, Modifier.fillMaxWidth(), icon = Icons.Rounded.FolderOpen)
            }
        }
        if (f.phase == FwPhase.Downloading) {
            Spacer(Modifier.height(14.dp))
            Text(f.detail, color = C.muted, fontSize = 13.sp)
            Spacer(Modifier.height(6.dp))
            LinearProgressIndicator(
                progress = { f.progress }, Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                color = C.accent, trackColor = C.line,
            )
        }
    }
    when (val r = f.releases) {
        is Load.Err -> {
            Spacer(Modifier.height(12.dp))
            Text(r.msg, color = C.bad, fontSize = 14.sp, modifier = Modifier.padding(horizontal = 4.dp))
        }
        is Load.Ok -> {
            if (r.v.isEmpty()) {
                Spacer(Modifier.height(12.dp))
                Text("Релизов нет", color = C.muted, fontSize = 14.sp, modifier = Modifier.padding(horizontal = 4.dp))
            }
            if (r.v.isNotEmpty()) {
                Spacer(Modifier.height(12.dp))
                FirmwareVersions(vm, r.v, hw, net)
            }
        }
        else -> {}
    }
}

/** Прошивка: строка на сеть (текущая и другая — ею сеть и переключается), версия — в меню. */
@Composable
private fun FirmwareVersions(vm: AppViewModel, rels: List<FwRelease>, hw: String, net: String) {
    val secure = vm.info["secure"] == "1"
    Card(padding = androidx.compose.foundation.layout.PaddingValues(18.dp)) {
        Text("Прошивка", style = MaterialTheme.typography.titleMedium)
        if (secure) Text("Плата с Secure Boot — только подписанные сборки", color = C.faint, fontSize = 12.sp)
        for (n in listOf(net, if (net == "mainnet") "testnet" else "mainnet")) {
            val options = rels.mapNotNull { rel ->
                rel.assets.firstOrNull { it.name == fwAssetName(hw, n, secure) }?.let { asset ->
                    VersionOption(rel.tag, "${rel.date} · ${kb(asset.size)}", rel.prerelease) { vm.fwDownload(asset, rel, n) }
                }
            }.sortedWith { a, b -> versionCmp(b.version, a.version) }
            VersionRow(
                n.uppercase(), if (n == net) vm.info["fw"] else null, options,
                enabled = vm.fw.phase == FwPhase.Pick,
                subtitle = if (n == net) null else "сменить сеть",
            ) { CoinIcon("logo", 36.dp) }
        }
        val latest = rels.firstOrNull { !it.prerelease } ?: rels.first()
        val notes = latest.notes.lines().filter { it.isNotBlank() }.take(6)
        if (notes.isNotEmpty()) {
            Spacer(Modifier.height(12.dp))
            Text("${latest.name} · ${latest.date}", color = C.muted, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            Text(notes.joinToString("\n"), color = C.faint, fontSize = 12.sp, lineHeight = 17.sp, maxLines = 6)
        }
    }
}

@Composable
private fun ImageCard(vm: AppViewModel, img: FwImage) {
    val f = vm.fw
    val hw = vm.info["hw"] ?: "?"
    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            CoinIcon("logo", 40.dp)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text("MicroWallet v${img.version}", style = MaterialTheme.typography.titleMedium)
                Text(f.imageFrom, color = C.faint, fontSize = 12.sp)
            }
        }
        Spacer(Modifier.height(10.dp))
        FwRow("Плата", chipName(img.chip), if (img.chip == hw) C.text else C.bad)
        val curNet = vm.info["net"] ?: vm.wallet?.net
        f.imageNet?.let { n -> FwRow("Сеть", n, if (n == "mainnet") C.ok else C.warn) }
        FwRow("Собрана", img.built)
        FwRow("Сборка", img.buildId)
        FwRow("Размер", "${img.bytes.size} байт")
        Divider()
        Spacer(Modifier.height(10.dp))
        Text("SHA-256 — сверь с экраном устройства", color = C.muted, fontSize = 13.sp)
        Spacer(Modifier.height(6.dp))
        AddressBox(img.sha256)
        if (img.buildId == vm.info["build"]) {
            Spacer(Modifier.height(10.dp))
            Text("Эта сборка уже установлена", color = C.warn, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        }
        val newNet = f.imageNet
        if (newNet != null && curNet != null && newNet != curNet) {
            Spacer(Modifier.height(12.dp))
            Text(
                "Устройство переключится с $curNet на $newNet. Seed тот же: адреса TRON и BSC не изменятся, " +
                    "а у BTC и Pearl будут другие — монеты из другой сети здесь видны не будут." +
                    if (newNet == "mainnet") " В обычной сборке seed хранится без шифрования — держи на устройстве только небольшие суммы." else "",
                color = C.warn, fontSize = 13.sp, lineHeight = 19.sp,
            )
        }
        if (img.chip != hw) {
            Spacer(Modifier.height(10.dp))
            Text("Образ для ${chipName(img.chip)}, а устройство — ${chipName(hw)}", color = C.bad, fontSize = 13.sp)
        }
        Spacer(Modifier.height(16.dp))
        PrimaryButton("Установить", { vm.fwInstall() }, Modifier.fillMaxWidth(), enabled = img.chip == hw,
            icon = Icons.Rounded.SystemUpdate)
        Spacer(Modifier.height(10.dp))
        SecondaryButton("Другой файл", { f.image = null; f.phase = FwPhase.Pick }, Modifier.fillMaxWidth())
    }
}

@Composable
private fun FwRow(k: String, v: String, color: androidx.compose.ui.graphics.Color = C.text) {
    Divider()
    Row(Modifier.fillMaxWidth().padding(vertical = 10.dp)) {
        Text(k, color = C.muted, fontSize = 14.sp, modifier = Modifier.width(96.dp))
        Text(v, color = color, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
    }
}

@Composable
private fun Sending(f: FwState) {
    Card(padding = androidx.compose.foundation.layout.PaddingValues(horizontal = 22.dp, vertical = 30.dp)) {
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
            if (f.phase == FwPhase.Confirm) {
                CircularProgressIndicator(Modifier.size(56.dp), color = C.accent, strokeWidth = 4.dp)
                Spacer(Modifier.height(18.dp))
                Text("Подтверди на устройстве", style = MaterialTheme.typography.titleLarge)
                Spacer(Modifier.height(8.dp))
                Text(
                    "Пролистай коротким нажатием размер и SHA-256, сверь их с этим экраном и удерживай кнопку " +
                        "на «Install». Отказаться — удерживай на «Reject».",
                    color = C.muted, fontSize = 14.sp, lineHeight = 20.sp, textAlign = TextAlign.Center,
                )
                f.image?.let {
                    Spacer(Modifier.height(14.dp))
                    AddressBox(it.sha256)
                }
            } else {
                Text("${(f.progress * 100).toInt()}%", fontSize = 40.sp, fontWeight = FontWeight.Bold, color = C.text)
                Spacer(Modifier.height(12.dp))
                LinearProgressIndicator(
                    progress = { f.progress }, Modifier.fillMaxWidth().height(8.dp).clip(RoundedCornerShape(4.dp)),
                    color = C.accent, trackColor = C.line,
                )
                Spacer(Modifier.height(12.dp))
                Text(f.detail, color = C.muted, fontSize = 13.sp)
                Spacer(Modifier.height(10.dp))
                Text(
                    "Не закрывай приложение и держи телефон рядом с устройством.",
                    color = C.faint, fontSize = 12.sp, textAlign = TextAlign.Center,
                )
            }
        }
    }
}
