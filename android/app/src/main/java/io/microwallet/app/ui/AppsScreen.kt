package io.microwallet.app.ui

import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.CloudDownload
import androidx.compose.material.icons.rounded.Delete
import androidx.compose.material.icons.rounded.FolderOpen
import androidx.compose.material.icons.rounded.InstallMobile
import androidx.compose.material.icons.rounded.KeyboardArrowDown
import androidx.compose.material.icons.rounded.KeyboardArrowUp
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.microwallet.app.core.AppInfo
import io.microwallet.app.core.COINS
import io.microwallet.app.core.FwAsset
import io.microwallet.app.core.FwRelease
import io.microwallet.app.core.WasmApp

enum class AppPhase { List, Downloading, Ready, Sending, Confirm, Done, Error }

class AppsState {
    var installed by mutableStateOf<Load<List<AppInfo>>?>(null)
    var catalog by mutableStateOf<Load<List<Pair<FwAsset, FwRelease>>>?>(null)
    var app by mutableStateOf<WasmApp?>(null)
    var phase by mutableStateOf(AppPhase.List)
    var progress by mutableStateOf(0f)
    var result by mutableStateOf("")
    var removing by mutableStateOf<String?>(null)

    fun reset() {
        installed = null
        app = null
        phase = AppPhase.List
        removing = null
    }
}

/** Иконка приложения из манифеста: 8 строк по 8 точек — как на экране устройства. */
@Composable
fun PixelIcon(icon: ByteArray?, size: Dp, color: Color = C.text) {
    Box(
        Modifier.size(size).clip(RoundedCornerShape(size * 0.28f)).background(C.card2),
        contentAlignment = Alignment.Center,
    ) {
        if (icon != null && icon.size == 8) Canvas(Modifier.size(size * 0.62f)) {
            val cell = this.size.width / 8
            for (y in 0 until 8) for (x in 0 until 8)
                if ((icon[y].toInt() shr (7 - x)) and 1 == 1)
                    drawRect(color, Offset(x * cell, y * cell), Size(cell * 0.92f, cell * 0.92f))
        }
    }
}

/** Иконка установленного приложения: у известных монет — значок монеты. */
@Composable
private fun AppIcon(id: String, size: Dp) {
    if (id in COIN_COLORS || id == "eth") CoinIcon(id, size) else PixelIcon(null, size)
}

@Composable
fun AppsScreen(vm: AppViewModel) {
    val s = vm.apps
    val busy = s.phase == AppPhase.Sending || s.phase == AppPhase.Confirm || s.phase == AppPhase.Downloading
    BackHandler(enabled = !busy) { if (s.phase == AppPhase.Ready) s.phase = AppPhase.List else vm.showApps = false }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.appFromFile(uri)
    }
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        ScreenTitle("Приложения", "Монеты и функции, которые ставятся на устройство отдельно")
        when (s.phase) {
            AppPhase.List, AppPhase.Downloading -> {
                Installed(vm)
                Spacer(Modifier.height(14.dp))
                Catalog(vm) { pick.launch(arrayOf("*/*")) }
            }
            AppPhase.Ready -> AppCard(vm, s.app!!)
            AppPhase.Sending, AppPhase.Confirm -> Card(padding = PaddingValues(horizontal = 22.dp, vertical = 30.dp)) {
                Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                    if (s.phase == AppPhase.Sending) {
                        Text("Передаю ${(s.progress * 100).toInt()}%", style = MaterialTheme.typography.titleLarge)
                        Spacer(Modifier.height(12.dp))
                        LinearProgressIndicator(
                            progress = { s.progress }, Modifier.fillMaxWidth().height(8.dp).clip(RoundedCornerShape(4.dp)),
                            color = C.accent, trackColor = C.line,
                        )
                    } else {
                        CircularProgressIndicator(Modifier.size(56.dp), color = C.accent, strokeWidth = 4.dp)
                        Spacer(Modifier.height(18.dp))
                        Text("Подтверди на устройстве", style = MaterialTheme.typography.titleLarge)
                        Spacer(Modifier.height(8.dp))
                        Text(
                            "Сверь имя, версию, пути ключей и SHA-256 и удерживай кнопку на «Install».",
                            color = C.muted, fontSize = 14.sp, textAlign = TextAlign.Center, lineHeight = 20.sp,
                        )
                        s.app?.let {
                            Spacer(Modifier.height(14.dp))
                            AddressBox(it.sha256)
                        }
                    }
                }
            }
            AppPhase.Done, AppPhase.Error -> Card {
                Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                    ResultIcon(s.phase == AppPhase.Done)
                    Spacer(Modifier.height(14.dp))
                    Text(if (s.phase == AppPhase.Done) "Готово" else "Не установлено", style = MaterialTheme.typography.titleLarge)
                    Spacer(Modifier.height(6.dp))
                    Text(s.result, color = if (s.phase == AppPhase.Done) C.muted else C.bad, fontSize = 14.sp,
                        textAlign = TextAlign.Center)
                    Spacer(Modifier.height(16.dp))
                    PrimaryButton("К списку", { s.phase = AppPhase.List; s.app = null }, Modifier.fillMaxWidth())
                }
            }
        }
        Spacer(Modifier.height(14.dp))
        if (!busy) SecondaryButton("Закрыть", { vm.showApps = false }, Modifier.fillMaxWidth())
        Spacer(Modifier.height(16.dp))
    }
}

@Composable
private fun Installed(vm: AppViewModel) {
    val s = vm.apps
    Card(padding = PaddingValues(vertical = 14.dp)) {
        Row(Modifier.padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("На устройстве", style = MaterialTheme.typography.titleMedium)
                Text("в этом порядке — на главном экране; стрелки меняют", color = C.faint, fontSize = 12.sp)
            }
            val total = vm.info["apps"]?.toIntOrNull() ?: 0
            val free = vm.info["apps_free"]?.toIntOrNull()
            if (total > 0 && free != null) Text("свободно ${free} из $total КБ", color = C.faint, fontSize = 12.sp)
        }
        Spacer(Modifier.height(6.dp))
        when (val l = s.installed) {
            null, Load.Loading -> Box(Modifier.fillMaxWidth().padding(16.dp), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(Modifier.size(24.dp), color = C.accent, strokeWidth = 2.5.dp)
            }
            is Load.Err -> Text(l.msg, color = C.bad, fontSize = 14.sp, modifier = Modifier.padding(horizontal = 20.dp))
            is Load.Ok -> {
                if (l.v.isEmpty()) Text(
                    if (vm.info["apps"] == "0") "Нет раздела для приложений: один раз запиши новую таблицу разделов по USB (README)."
                    else "Пока ничего не установлено.",
                    color = C.muted, fontSize = 14.sp, lineHeight = 20.sp, modifier = Modifier.padding(horizontal = 20.dp),
                )
                for (a in l.v) Row(
                    Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    AppIcon(a.id, 40.dp)
                    Spacer(Modifier.width(14.dp))
                    Column(Modifier.weight(1f)) {
                        Text(a.name, fontWeight = FontWeight.SemiBold, fontSize = 16.sp, color = C.text)
                        Text("${a.id} · v${a.version}", fontSize = 12.sp, color = C.faint, fontFamily = Mono)
                    }
                    // порядок на главном экране устройства
                    val idx = l.v.indexOf(a)
                    SmallIconButton(Icons.Rounded.KeyboardArrowUp, "Выше", { vm.moveApp(a.id, -1) }, enabled = idx > 0 && s.removing == null)
                    SmallIconButton(Icons.Rounded.KeyboardArrowDown, "Ниже", { vm.moveApp(a.id, 1) },
                        enabled = idx < l.v.size - 1 && s.removing == null)
                    Spacer(Modifier.width(4.dp))
                    if (s.removing == a.id) CircularProgressIndicator(Modifier.size(20.dp), color = C.bad, strokeWidth = 2.dp)
                    else SmallIconButton(Icons.Rounded.Delete, "Удалить", { vm.removeApp(a.id, a.name) }, enabled = s.removing == null)
                }
                if (s.removing != null) Text(
                    "Подтверди удаление на устройстве. Ключи в seed не трогаются: поставишь снова — адреса те же.",
                    color = C.warn, fontSize = 12.sp, lineHeight = 17.sp, modifier = Modifier.padding(horizontal = 20.dp, vertical = 4.dp),
                )
            }
        }
    }
}

@Composable
private fun Catalog(vm: AppViewModel, pickFile: () -> Unit) {
    val s = vm.apps
    val f = vm.fw
    Card {
        Text("Каталог", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(4.dp))
        Text(
            "Источник — тот же, что для прошивки: " + if (f.source == "pc") f.pcUrl else f.repo.ifBlank { "GitHub (не указан)" },
            color = C.faint, fontSize = 12.sp,
        )
        Spacer(Modifier.height(12.dp))
        Row {
            PrimaryButton("Загрузить каталог", { vm.loadCatalog() }, Modifier.weight(1f),
                loading = s.catalog == Load.Loading, icon = Icons.Rounded.CloudDownload,
                enabled = f.source != "file" && (f.source == "pc" || f.repo.isNotBlank()))
            Spacer(Modifier.width(10.dp))
            SecondaryButton("Файл", pickFile, Modifier.height(52.dp), icon = Icons.Rounded.FolderOpen)
        }
        if (s.phase == AppPhase.Downloading) {
            Spacer(Modifier.height(12.dp))
            LinearProgressIndicator(Modifier.fillMaxWidth(), color = C.accent, trackColor = C.line)
        }
        when (val c = s.catalog) {
            is Load.Err -> Text(c.msg, color = C.bad, fontSize = 14.sp, modifier = Modifier.padding(top = 12.dp))
            is Load.Ok -> {
                if (c.v.isEmpty()) Text("В релизах нет файлов microwallet-app-*.wasm", color = C.muted, fontSize = 14.sp,
                    modifier = Modifier.padding(top = 12.dp))
                // Одна строка на приложение; версия — в меню «Скачать»
                val have = (s.installed as? Load.Ok)?.v.orEmpty()
                val byId = c.v.groupBy { (asset, _) -> asset.name.removePrefix("microwallet-app-").substringBefore('-') }
                for ((id, list) in byId.entries.sortedBy { it.key }) {
                    val options = list.map { (asset, rel) ->
                        val ver = asset.name.removePrefix("microwallet-app-").removeSuffix(".wasm").substringAfter('-')
                        VersionOption(ver, "${rel.tag} · ${kb(asset.size)}", rel.prerelease) { vm.downloadApp(asset) }
                    }.sortedWith { a, b -> versionCmp(b.version, a.version) }
                    val mine = have.firstOrNull { it.id == id }
                    val name = mine?.name ?: COINS.firstOrNull { it.id == id }?.name ?: id
                    VersionRow(name, mine?.version, options, enabled = s.phase == AppPhase.List) { AppIcon(id, 36.dp) }
                }
            }
            else -> {}
        }
    }
}

@Composable
private fun AppCard(vm: AppViewModel, a: WasmApp) {
    val have = (vm.apps.installed as? Load.Ok)?.v?.firstOrNull { it.id == a.id }
    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            PixelIcon(a.icon, 44.dp)
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(a.name, style = MaterialTheme.typography.titleLarge)
                Text("${a.id} · v${a.version}", color = C.faint, fontSize = 13.sp, fontFamily = Mono)
            }
        }
        Spacer(Modifier.height(12.dp))
        AppRow("Размер", kb(a.bytes.size.toLong()))
        AppRow("Ключи", a.paths.joinToString(", ").ifEmpty { "нет" })
        if (have != null) AppRow("Сейчас", "v${have.version} — будет заменена", C.warn)
        Divider()
        Spacer(Modifier.height(10.dp))
        Text(
            "Приложение работает в песочнице: видит только ключи по своим путям и подписывает только после " +
                "«Accept» на устройстве.",
            color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
        )
        Spacer(Modifier.height(10.dp))
        Text("SHA-256 — сверь с экраном устройства", color = C.muted, fontSize = 13.sp)
        Spacer(Modifier.height(6.dp))
        AddressBox(a.sha256)
        Spacer(Modifier.height(16.dp))
        PrimaryButton(if (have != null) "Обновить" else "Установить", { vm.installApp() }, Modifier.fillMaxWidth(),
            icon = Icons.Rounded.InstallMobile)
        Spacer(Modifier.height(10.dp))
        SecondaryButton("Назад", { vm.apps.phase = AppPhase.List; vm.apps.app = null }, Modifier.fillMaxWidth())
    }
}

@Composable
private fun AppRow(k: String, v: String, color: Color = C.text) {
    Divider()
    Row(Modifier.fillMaxWidth().padding(vertical = 10.dp)) {
        Text(k, color = C.muted, fontSize = 14.sp, modifier = Modifier.width(96.dp))
        Text(v, color = color, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
    }
}
