package io.microwallet.app.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
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
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Shield
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.microwallet.app.core.AmlParty
import io.microwallet.app.core.AmlReport
import io.microwallet.app.core.shortAddr
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val AML_DAYS = listOf(7, 30, 90)

private fun riskColor(verdict: String): Color = when (verdict) {
    "high" -> C.bad
    "medium" -> C.warn
    else -> C.ok
}

private fun verdictText(v: String) = when (v) {
    "high" -> "Высокий риск"
    "medium" -> "Средний риск"
    else -> "Низкий риск"
}

private fun riskText(r: String?) = when (r) {
    "sanctions" -> "санкции"
    "high" -> "высокий"
    "medium" -> "средний"
    "low" -> "низкий"
    "trusted" -> "доверенный"
    else -> ""
}

private fun usd(v: Double) = "%,.2f".format(Locale.US, v).replace(',', ' ')

/** AML-проверка своего адреса BSC: открывается из карточки BSC на «Обзоре». */
@Composable
fun AmlScreen(vm: AppViewModel) {
    val addr = vm.amlTarget ?: return
    BackHandler { vm.amlTarget = null }
    var days by rememberSaveable { mutableStateOf(vm.amlDays) }
    val key = vm.amlKey(addr, days)
    val state = vm.amlReports[key]
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        ScreenTitle("AML-проверка", "USDT BEP-20 · BNB Smart Chain")
        Card {
            Text("Адрес", color = C.muted, fontSize = 13.sp)
            Spacer(Modifier.height(6.dp))
            AddressBox(addr)
            Spacer(Modifier.height(14.dp))
            FieldLabel("Период")
            Segmented(AML_DAYS.map { it.toString() to "$it дн." }, days.toString(), { days = it.toInt() }, Modifier.fillMaxWidth())
            Spacer(Modifier.height(14.dp))
            PrimaryButton(
                if (state == Load.Loading) "Проверяю…" else if (state == null) "Проверить" else "Проверить заново",
                { vm.amlCheck(addr, days, force = true) },
                Modifier.fillMaxWidth(), enabled = state != Load.Loading, icon = Icons.Rounded.Shield,
                loading = state == Load.Loading,
            )
            if (!vm.amlKeys.ready) Text(
                "Без ключа NodeReal проверяется только сам адрес (разметка и санкции), без истории переводов. " +
                    "Ключи — ниже.",
                color = C.warn, fontSize = 12.sp, lineHeight = 17.sp, modifier = Modifier.padding(top = 10.dp),
            )
        }
        Spacer(Modifier.height(14.dp))
        when (state) {
            null -> {}
            Load.Loading -> AmlProgress(vm, key)
            is Load.Err -> Card { Text(state.msg, color = C.bad, fontSize = 14.sp) }
            is Load.Ok -> AmlReportView(state.v, full = true)
        }
        Spacer(Modifier.height(14.dp))
        AmlSettings(vm)
        Spacer(Modifier.height(14.dp))
        SecondaryButton("Закрыть", { vm.amlTarget = null }, Modifier.fillMaxWidth())
        Spacer(Modifier.height(16.dp))
    }
}

@Composable
private fun AmlProgress(vm: AppViewModel, key: String) {
    Card {
        val p = vm.amlProgress[key]
        Text("Загружаю историю переводов…", color = C.muted, fontSize = 14.sp)
        Spacer(Modifier.height(10.dp))
        if (p != null && p.second > 0) {
            LinearProgressIndicator(progress = { p.first.toFloat() / p.second }, Modifier.fillMaxWidth(), color = C.accent)
            Text("${p.first} из ${p.second} запросов", color = C.faint, fontSize = 12.sp, modifier = Modifier.padding(top = 6.dp))
        } else LinearProgressIndicator(Modifier.fillMaxWidth(), color = C.accent)
    }
}

@Composable
private fun AmlSettings(vm: AppViewModel) {
    var nodereal by rememberSaveable { mutableStateOf(vm.amlKeys.nodereal) }
    var chainalysis by rememberSaveable { mutableStateOf(vm.amlKeys.chainalysis) }
    var days by rememberSaveable { mutableStateOf(vm.amlDays) }
    Card {
        Text("Ключи и период", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(6.dp))
        Text(
            "NodeReal MegaNode (dashboard.nodereal.io) — история переводов USDT, без него проверка неполная. " +
                "Chainalysis Sanctions API — санкционные списки для адреса и 150 крупнейших контрагентов; без " +
                "него — только встроенная разметка (OFAC SDN, HTX). Оба ключа бесплатные и хранятся только на телефоне.",
            color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
        )
        Spacer(Modifier.height(12.dp))
        FieldLabel("Ключ NodeReal")
        InputField(nodereal, { nodereal = it.trim() }, placeholder = "API key", mono = true,
            keyboard = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Password))
        Spacer(Modifier.height(10.dp))
        FieldLabel("Ключ Chainalysis (необязательно)")
        InputField(chainalysis, { chainalysis = it.trim() }, placeholder = "API key", mono = true,
            keyboard = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Password))
        Spacer(Modifier.height(10.dp))
        FieldLabel("Период по умолчанию (и для получателя при отправке)")
        Segmented(AML_DAYS.map { it.toString() to "$it дн." }, days.toString(), { days = it.toInt() }, Modifier.fillMaxWidth())
        Spacer(Modifier.height(14.dp))
        val changed = nodereal != vm.amlKeys.nodereal || chainalysis != vm.amlKeys.chainalysis || days != vm.amlDays
        SecondaryButton("Сохранить", {
            vm.setAmlSettings(nodereal, chainalysis, days)
            vm.message = "Сохранено"
        }, Modifier.fillMaxWidth(), enabled = changed)
    }
}

/** Отчёт: скор, вердикт, обороты, помеченные контрагенты; full — ещё крупнейшие и оговорки. */
@Composable
fun AmlReportView(r: AmlReport, full: Boolean) {
    val color = riskColor(r.verdict)
    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("${r.score}", fontSize = 40.sp, fontWeight = FontWeight.Bold, color = color)
            Text(" / 100", color = C.faint, fontSize = 16.sp, modifier = Modifier.padding(top = 14.dp))
            Spacer(Modifier.weight(1f))
            Badge(verdictText(r.verdict).uppercase(), color)
        }
        if (r.targetSanctioned) AmlWarn("Адрес в санкционном списке" +
            (r.targetNames.firstOrNull() ?: r.targetLabel?.label)?.let { ": $it" }.orEmpty(), C.bad)
        else r.targetLabel?.let { AmlWarn("Адрес помечен: ${it.label} (${riskText(it.risk)} риск)", riskColor(if (it.risk == "trusted" || it.risk == "low") "low" else "high")) }
        Spacer(Modifier.height(10.dp))
        if (r.historyChecked) {
            Text(
                "За ${r.days} дн.: получено ${usd(r.totalIn)} USDT, отправлено ${usd(r.totalOut)} USDT · " +
                    "${r.transfers} переводов, ${r.counterpartyCount} контрагентов",
                color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
            )
            if (r.flagged.isEmpty()) Text(
                "Среди прямых контрагентов помеченных нет.", color = C.ok, fontSize = 13.sp,
                modifier = Modifier.padding(top = 6.dp),
            )
        } else Text("История переводов не проверялась: нет ключа NodeReal.", color = C.warn, fontSize = 13.sp)
    }
    if (r.flagged.isNotEmpty()) {
        Spacer(Modifier.height(14.dp))
        Card {
            Text("Помеченные контрагенты", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(8.dp))
            for (p in r.flagged.take(if (full) 30 else 5)) PartyRow(p)
            if (!full && r.flagged.size > 5) Text("и ещё ${r.flagged.size - 5}", color = C.faint, fontSize = 12.sp)
        }
    }
    if (full && r.top.isNotEmpty()) {
        Spacer(Modifier.height(14.dp))
        Card {
            Text("Крупнейшие контрагенты", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(8.dp))
            for (p in r.top.take(15)) PartyRow(p)
        }
    }
    if (full) {
        Spacer(Modifier.height(14.dp))
        Card {
            Text("Что это значит", style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(6.dp))
            val notes = listOfNotNull(
                "Скор — доля оборота USDT с помеченными адресами, взвешенная по риску (санкции 1, высокий 0,7, " +
                    "средний 0,4, низкий 0,1). Хоть один контрагент под санкциями — не ниже 75; адрес сам под " +
                    "санкциями — 100.",
                "Это только прямые (1-hop) переводы. Платные AML (Chainalysis KYT, BitOK, AMLBot) смотрят на " +
                    "несколько переходов и склеивают адреса по сервисам — их оценка может быть выше.",
                if (!r.sanctionsEnabled) "Без ключа Chainalysis санкции проверяются только по встроенной разметке." else null,
                if (r.unscreened > 0) "Не удалось проверить по санкциям: ${r.unscreened} контрагентов (лимит API)." else null,
                if (r.counterpartyCount > 150 && r.sanctionsEnabled) "По санкциям проверены 150 крупнейших контрагентов." else null,
            )
            for (n in notes) Text(n, color = C.muted, fontSize = 12.sp, lineHeight = 17.sp, modifier = Modifier.padding(top = 4.dp))
        }
    }
}

@Composable
private fun AmlWarn(text: String, color: Color) {
    Text(
        text, color = color, fontSize = 14.sp, fontWeight = FontWeight.SemiBold, lineHeight = 19.sp,
        modifier = Modifier.padding(top = 10.dp),
    )
}

@Composable
private fun PartyRow(p: AmlParty) {
    val clipboard = LocalClipboardManager.current
    val date = dateOf(p.last)
    Column(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(C.card2)
            .clickable { clipboard.setText(AnnotatedString(p.address)) }
            .padding(horizontal = 12.dp, vertical = 9.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(shortAddr(p.address, 6), fontFamily = Mono, fontSize = 13.sp, color = C.accent2, modifier = Modifier.weight(1f))
            if (p.risk != null) Badge(riskText(p.risk), riskColor(
                when (p.risk) { "sanctions", "high" -> "high"; "medium" -> "medium"; else -> "low" }
            ))
        }
        p.label?.let {
            Text(it, color = C.text, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.padding(top = 2.dp))
        }
        Text(
            listOfNotNull(
                if (p.inAmount > 0) "↓ ${usd(p.inAmount)}" else null,
                if (p.outAmount > 0) "↑ ${usd(p.outAmount)}" else null,
                "${p.count} пер.",
                date,
            ).joinToString(" · "),
            color = C.faint, fontSize = 12.sp, modifier = Modifier.padding(top = 2.dp),
        )
    }
}

private fun dateOf(last: Long): String? =
    if (last <= 0) null else SimpleDateFormat("dd.MM.yy", Locale.US).format(Date(last * 1000))

/** Проверка получателя перед подписью перевода в BSC. */
@Composable
fun RecipientAml(vm: AppViewModel, addr: String) {
    val days = vm.amlDays
    val key = vm.amlKey(addr, days)
    LaunchedEffect(key) { vm.amlCheck(addr, days) }
    val state = vm.amlReports[key]
    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("AML получателя", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            if (state == Load.Loading) CircularProgressIndicator(Modifier.size(18.dp), color = C.accent, strokeWidth = 2.dp)
        }
        Spacer(Modifier.height(6.dp))
        when (state) {
            null, Load.Loading -> {
                val p = vm.amlProgress[key]
                Text(
                    "Проверяю историю USDT получателя за $days дн." + (p?.let { " · ${it.first}/${it.second}" } ?: ""),
                    color = C.muted, fontSize = 13.sp,
                )
            }
            is Load.Err -> {
                Text("Не удалось проверить: ${state.msg}", color = C.warn, fontSize = 13.sp, lineHeight = 18.sp)
                Spacer(Modifier.height(8.dp))
                SecondaryButton("Повторить", { vm.amlCheck(addr, days, force = true) }, Modifier.fillMaxWidth())
            }
            is Load.Ok -> {
                val r = state.v
                val color = riskColor(r.verdict)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("${r.score}/100", fontSize = 22.sp, fontWeight = FontWeight.Bold, color = color)
                    Spacer(Modifier.width(10.dp))
                    Text(verdictText(r.verdict), color = color, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
                }
                when {
                    r.targetSanctioned -> AmlWarn("Получатель под санкциями — перевод могут заблокировать, а твой адрес пометят.", C.bad)
                    r.verdict == "high" -> AmlWarn("Получатель напрямую связан с помеченными адресами. Твой адрес после перевода тоже может получить пометку.", C.bad)
                    r.verdict == "medium" -> AmlWarn("Есть связи с помеченными адресами — посмотри, с какими.", C.warn)
                }
                r.targetLabel?.let { Text("Метка: ${it.label}", color = C.muted, fontSize = 13.sp, modifier = Modifier.padding(top = 6.dp)) }
                for (p in r.flagged.take(3)) PartyRow(p)
                if (!r.historyChecked) Text(
                    "Проверен только сам адрес: для истории переводов нужен ключ NodeReal (Обзор → BSC → AML).",
                    color = C.faint, fontSize = 12.sp, lineHeight = 17.sp, modifier = Modifier.padding(top = 6.dp),
                )
                else if (r.transfers == 0) Text(
                    "У получателя нет переводов USDT за $days дн. — новый или неиспользуемый адрес.",
                    color = C.faint, fontSize = 12.sp, lineHeight = 17.sp, modifier = Modifier.padding(top = 6.dp),
                )
            }
        }
    }
}
