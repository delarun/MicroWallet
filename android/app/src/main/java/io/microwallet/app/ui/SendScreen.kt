package io.microwallet.app.ui

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
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
import androidx.compose.material.icons.automirrored.rounded.OpenInNew
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.ContentPaste
import androidx.compose.material.icons.rounded.QrCodeScanner
import androidx.compose.material.icons.rounded.VerifiedUser
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import io.microwallet.app.core.COINS
import io.microwallet.app.core.Draft
import io.microwallet.app.core.NetConfig
import io.microwallet.app.core.Ton
import io.microwallet.app.core.SendRequest
import io.microwallet.app.core.SendResult
import io.microwallet.app.core.WalletError
import io.microwallet.app.core.addressToScript
import io.microwallet.app.core.TRON_MEMO_MAX
import io.microwallet.app.core.assetsFor
import io.microwallet.app.core.coinInfo
import io.microwallet.app.core.evmDecode
import io.microwallet.app.core.fmtUnits
import io.microwallet.app.core.groupAddr
import io.microwallet.app.core.shortAddr
import io.microwallet.app.core.tronDecode
import java.math.BigDecimal

enum class Step { Form, Review, Signing, Result }

/** Состояние перевода живёт в ViewModel — переключение вкладок его не сбрасывает. */
class SendState {
    var net = "testnet"
    var coin by mutableStateOf("btc")
    var asset by mutableStateOf("btc")
    var dest by mutableStateOf("")
    var amount by mutableStateOf("")
    var feeLevel by mutableStateOf("normal")
    var feeLimit by mutableStateOf("")  // пусто — 40 TRX или оценка + 20 %
    var memo by mutableStateOf("")
    var dry by mutableStateOf(false)
    var levels by mutableStateOf<Map<String, BigDecimal>?>(null)
    var step by mutableStateOf(Step.Form)
    var preparing by mutableStateOf(false)
    var formErr by mutableStateOf("")
    var draft by mutableStateOf<Draft?>(null)
    var result by mutableStateOf<SendResult?>(null)
    var error by mutableStateOf<String?>(null)

    fun selectCoin(c: String) {
        coin = c
        asset = coinInfo(c).assetsFor(net)[0].first
        levels = null
        formErr = ""
    }

    fun reset() {
        dest = ""
        amount = ""
        memo = ""
        draft = null
        result = null
        error = null
        formErr = ""
        step = Step.Form
    }
}

/** bitcoin:/tron:/ethereum:/pearl: URI или просто адрес -> (адрес, сумма). */
fun parsePaymentUri(s: String): Pair<String, String?> {
    var t = s.trim()
    val colon = t.indexOf(':')
    if (colon in 1..12) t = t.substring(colon + 1).removePrefix("//")
    var amount: String? = null
    val q = t.indexOf('?')
    if (q >= 0) {
        amount = t.substring(q + 1).split('&').map { it.split('=', limit = 2) }
            .firstOrNull { it.size == 2 && it[0] == "amount" }?.get(1)
        t = t.substring(0, q)
    }
    return t.substringBefore('@') to amount
}

fun validateDest(coin: String, cfg: NetConfig, text: String): String? = try {
    when (coin) {
        "btc" -> addressToScript(text, cfg.hrp)
        "prl" -> addressToScript(text, cfg.prlHrp, true)
        "tron" -> tronDecode(text)
        "ton" -> Ton.parseAddress(text, cfg.tonUsdt == null)
        else -> evmDecode(text)
    }
    null
} catch (e: WalletError) {
    e.message
}

private val HINTS = mapOf("btc" to "tb1… или bc1…", "prl" to "prl1p… (Taproot)", "tron" to "T…", "bsc" to "0x…", "eth" to "0x…", "ton" to "UQ… или EQ…")

@Composable
fun SendScreen(vm: AppViewModel) {
    val w = vm.wallet ?: return
    val s = vm.send
    LaunchedEffect(vm.coinRequest) {
        vm.coinRequest?.let {
            if (s.step == Step.Form) s.selectCoin(it)
        }
        vm.coinRequest = null
    }
    if (vm.coins.isEmpty()) {
        Column(Modifier.fillMaxSize().padding(16.dp)) {
            ScreenTitle("Отправить")
            NoCoins { vm.openApps() }
        }
        return
    }
    LaunchedEffect(w, vm.coins) {
        s.net = w.net
        if (s.coin !in vm.coins) s.selectCoin(COINS.map { it.id }.first { it in vm.coins })
    }
    BackHandler(enabled = s.step == Step.Review) { s.step = Step.Form }

    Column(
        Modifier
            .fillMaxSize()
            .imePadding()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        ScreenTitle("Отправить", "Транзакция подписывается только на устройстве")
        when (s.step) {
            Step.Form -> SendForm(vm)
            Step.Review -> Review(vm)
            Step.Signing -> Signing()
            Step.Result -> SendResultView(vm)
        }
        Spacer(Modifier.height(16.dp))
    }
}

@Composable
private fun SendForm(vm: AppViewModel) {
    val w = vm.wallet ?: return
    val s = vm.send
    val clipboard = LocalClipboardManager.current
    val utxo = s.coin == "btc" || s.coin == "prl"
    val scan = rememberLauncherForActivityResult(ScanContract()) { r ->
        r.contents?.let {
            val (addr, amount) = parsePaymentUri(it)
            s.dest = addr
            if (amount != null) s.amount = amount
        }
    }
    LaunchedEffect(s.coin, w) {
        val c = s.coin
        if ((c == "btc" || c == "prl") && w.ready && s.levels == null)
            vm.run({ w.feeLevels(c) }, { if (s.coin == c) s.levels = it }, {})
    }

    if (vm.info["state"] == "locked") {
        LockedCard()
        return
    }
    if (!w.ready) {
        Card { Text("На устройстве нет кошелька", color = C.muted) }
        return
    }
    CoinPills(vm.coins, s.coin) { s.selectCoin(it) }
    Card {
        // TRC10 — те, что есть на балансе (их ID знает только нода)
        val trc10 = if (s.coin != "tron") emptyList() else (vm.balances["tron"] as? Load.Ok)?.v?.assets
            ?.map { it.id }?.filter { it.startsWith("trc10:") }.orEmpty().take(2)
        val assets = coinInfo(s.coin).assetsFor(w.net).map { it.first } + trc10
        if (assets.size > 1) {
            FieldLabel("Актив")
            Segmented(assets.map { it to w.symbol(it) }, s.asset, { s.asset = it }, Modifier.fillMaxWidth())
            Spacer(Modifier.height(10.dp))
        }

        FieldLabel("Получатель")
        InputField(
            s.dest, { s.dest = it.trim() },
            placeholder = "Адрес ${coinInfo(s.coin).name}: ${HINTS[s.coin]}",
            mono = true, singleLine = false,
            keyboard = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Ascii),
        ) {
            SmallIconButton(Icons.Rounded.ContentPaste, "Вставить", {
                clipboard.getText()?.text?.let {
                    val (a, amt) = parsePaymentUri(it)
                    s.dest = a
                    if (amt != null) s.amount = amt
                }
            })
            SmallIconButton(Icons.Rounded.QrCodeScanner, "Сканировать QR", {
                scan.launch(
                    ScanOptions().setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                        .setPrompt("Наведи камеру на QR-код адреса").setBeepEnabled(false).setOrientationLocked(true)
                )
            })
        }
        val destErr = if (s.dest.isEmpty()) null else validateDest(s.coin, w.cfg, s.dest)
        if (s.dest.isNotEmpty()) Text(
            destErr ?: "✓  Адрес корректный", color = if (destErr == null) C.ok else C.bad, fontSize = 12.sp,
            modifier = Modifier.padding(top = 6.dp),
        )
        Spacer(Modifier.height(10.dp))

        FieldLabel("Сумма")
        InputField(
            s.amount, { s.amount = it.replace(',', '.').filter { ch -> ch.isDigit() || ch == '.' } },
            placeholder = "0.00", big = true,
            keyboard = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        ) {
            Text(w.symbol(s.asset), fontSize = 17.sp, fontWeight = FontWeight.Bold, color = C.muted,
                modifier = Modifier.padding(end = 10.dp))
        }
        val bal = (vm.balances[s.coin] as? Load.Ok)?.v
        val a = bal?.assets?.firstOrNull { it.id == s.asset }
        if (a != null) Text(
            "Доступно: ${fmtUnits(a.amount, a.decimals)} ${a.symbol}", color = C.faint, fontSize = 12.sp,
            modifier = Modifier.padding(top = 6.dp),
        )
        Spacer(Modifier.height(14.dp))

        when {
            utxo -> {
                FieldLabel("Скорость подтверждения")
                val unit = if (s.coin == "prl") "grain/vB" else "sat/vB"
                val lv = s.levels
                Segmented(
                    listOf("slow" to "Медленно", "normal" to "Обычно", "fast" to "Быстро").map { (k, t) ->
                        k to if (lv != null) "$t\n${lv.getValue(k).toPlainString()} $unit" else t
                    },
                    s.feeLevel, { s.feeLevel = it }, Modifier.fillMaxWidth(),
                )
                Hint("Комиссия считается по размеру транзакции и выбранной ставке")
            }
            s.coin == "tron" -> {
                if (s.asset == "usdt") {
                    FieldLabel("Лимит комиссии, TRX")
                    InputField(s.feeLimit, { s.feeLimit = it.replace(',', '.') }, placeholder = "авто",
                        keyboard = KeyboardOptions(keyboardType = KeyboardType.Decimal), modifier = Modifier.width(160.dp))
                    Hint("Энергия оплачивается сжиганием TRX; это верхняя граница. Пусто — по оценке сети")
                } else {
                    FieldLabel("Комиссия")
                    Hint("Оценка — на следующем шаге: bandwidth, новому адресу +1 TRX")
                }
                Spacer(Modifier.height(10.dp))
                FieldLabel("Memo (необязательно)")
                InputField(s.memo, { s.memo = it.filter { ch -> ch.code in 0x20..0x7E }.take(TRON_MEMO_MAX) },
                    placeholder = "видно в блокчейне всем",
                    keyboard = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Ascii))
                if (s.memo.isNotEmpty()) Hint("Memo стоит ~1 TRX и показывается на устройстве")
            }
            s.coin == "ton" -> {
                FieldLabel("Комиссия")
                Hint(if (s.asset == "usdt") "Тысячные доли GRAM; ещё 0.05 GRAM на пересылку жетона, лишнее вернётся"
                     else "Тысячные доли GRAM, оценка — на следующем шаге")
                Spacer(Modifier.height(10.dp))
                FieldLabel("Комментарий (необязательно)")
                InputField(s.memo, { s.memo = it.filter { ch -> ch.code in 0x20..0x7E }.take(Ton.COMMENT_MAX) },
                    placeholder = "видно в блокчейне всем",
                    keyboard = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Ascii))
            }
            else -> {
                FieldLabel("Комиссия")
                Hint("Цена газа — по сети, оплачивается в ${w.symbol(if (s.coin == "eth") "eth" else "bnb")}")
            }
        }
        Spacer(Modifier.height(8.dp))
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).clickable { s.dry = !s.dry },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Checkbox(s.dry, { s.dry = it }, colors = CheckboxDefaults.colors(checkedColor = C.accent, uncheckedColor = C.faint))
            Text("Только подписать, не отправлять в сеть", color = C.text, fontSize = 14.sp)
        }
        Spacer(Modifier.height(10.dp))

        val amountOk = s.amount.toBigDecimalOrNull()?.let { it.signum() > 0 } == true
        PrimaryButton(
            if (s.preparing) "Собираю транзакцию…" else "Продолжить",
            {
                s.formErr = ""
                s.preparing = true
                val req = SendRequest(
                    coin = s.coin, dest = s.dest, amount = s.amount, asset = s.asset,
                    feerate = if (utxo) s.levels?.get(s.feeLevel) else null,
                    feeLimit = if (s.coin == "tron" && s.asset == "usdt") s.feeLimit.ifBlank { null } else null,
                    memo = if (s.coin == "tron" || s.coin == "ton") s.memo.ifEmpty { null } else null,
                )
                vm.run({ w.prepareSend(req) }, {
                    s.preparing = false
                    s.draft = it
                    s.step = Step.Review
                }, {
                    s.preparing = false
                    s.formErr = it
                })
            },
            Modifier.fillMaxWidth(),
            enabled = s.dest.isNotEmpty() && destErr == null && amountOk,
            loading = s.preparing,
        )
        if (s.formErr.isNotEmpty()) Text(s.formErr, color = C.bad, fontSize = 13.sp, modifier = Modifier.padding(top = 10.dp))
    }
}

@Composable
private fun Hint(text: String) {
    Text(text, color = C.faint, fontSize = 12.sp, lineHeight = 17.sp, modifier = Modifier.padding(top = 6.dp))
}

@Composable
private fun Review(vm: AppViewModel) {
    val w = vm.wallet ?: return
    val s = vm.send
    val d = s.draft ?: return
    Card(padding = PaddingValues(20.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            CoinIcon(if (d.asset == "usdt" || d.asset == "usdc") d.asset else d.coin, 44.dp)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text("${d.amount} ${d.symbol}", fontSize = 24.sp, fontWeight = FontWeight.Bold, color = C.text)
                Text("${coinInfo(d.coin).name} · ${coinInfo(d.coin).nets[w.net]}", color = C.muted, fontSize = 13.sp)
            }
            if (s.dry) Badge("БЕЗ ОТПРАВКИ", C.warn)
        }
        if (w.net == "mainnet") {
            Spacer(Modifier.height(12.dp))
            Text("MAINNET — настоящие деньги. Отменить перевод будет нельзя.", color = C.warn, fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold)
        }
        Spacer(Modifier.height(16.dp))
        ReviewRow("Откуда", d.fromAddr, mono = true)
        ReviewRow("Куда", d.to, mono = true)
        ReviewRow("Сумма", "${d.amount} ${d.symbol}")
        ReviewRow(d.feeLabel, "${d.fee} ${d.feeSymbol}")
        Divider()
        for (n in d.notes) Text(n, color = C.faint, fontSize = 12.sp, modifier = Modifier.padding(top = 8.dp))
    }
    Spacer(Modifier.height(16.dp))
    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        SecondaryButton("Назад", { s.step = Step.Form }, Modifier.weight(1f).height(52.dp))
        PrimaryButton("Подписать на устройстве", {
            s.step = Step.Signing
            vm.busy = true
            vm.run({ w.signAndSend(d, !s.dry) }, {
                vm.busy = false
                s.result = it
                s.error = null
                s.step = Step.Result
                vm.refreshSoon()
            }, {
                vm.busy = false
                s.result = null
                s.error = it
                s.step = Step.Result
            })
        }, Modifier.weight(2f), icon = Icons.Rounded.VerifiedUser)
    }
}

@Composable
private fun ReviewRow(k: String, v: String, mono: Boolean = false) {
    Divider()
    Column(Modifier.fillMaxWidth().padding(vertical = 12.dp)) {
        Text(k, color = C.muted, fontSize = 13.sp)
        Spacer(Modifier.height(4.dp))
        Text(
            if (mono) groupAddr(v) else v,
            color = C.text, fontSize = if (mono) 14.sp else 16.sp,
            fontFamily = if (mono) Mono else null,
            fontWeight = if (mono) FontWeight.Normal else FontWeight.SemiBold,
            lineHeight = 21.sp,
        )
    }
}

@Composable
private fun Signing() {
    Card(padding = PaddingValues(horizontal = 24.dp, vertical = 40.dp)) {
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
            CircularProgressIndicator(Modifier.size(60.dp), color = C.accent, strokeWidth = 4.dp)
            Spacer(Modifier.height(22.dp))
            Text("Подтверди на устройстве", style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(10.dp))
            Text(
                "Коротким нажатием пролистай сумму, адрес и комиссию.\nЕсли всё верно — удерживай кнопку на «Accept».\n" +
                    "Чтобы отказаться — удерживай на «Reject».",
                color = C.muted, fontSize = 14.sp, lineHeight = 21.sp, textAlign = TextAlign.Center,
            )
        }
    }
}

@Composable
private fun SendResultView(vm: AppViewModel) {
    val s = vm.send
    val ctx = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val r = s.result
    val d = s.draft
    Card(padding = PaddingValues(horizontal = 22.dp, vertical = 32.dp)) {
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
            ResultIcon(r != null)
            Spacer(Modifier.height(16.dp))
            Text(
                when {
                    r == null -> "Не отправлено"
                    r.broadcast -> "Отправлено"
                    else -> "Подписано"
                },
                style = MaterialTheme.typography.headlineMedium,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                when {
                    r == null -> s.error ?: ""
                    r.broadcast && d != null -> "${d.amount} ${d.symbol} → ${shortAddr(d.to, 12)}"
                    else -> "Транзакция подписана, но не отправлена. Сырую транзакцию можно скопировать."
                },
                color = if (r == null) C.bad else C.muted, fontSize = 14.sp, textAlign = TextAlign.Center, lineHeight = 20.sp,
            )
            if (r != null) {
                Spacer(Modifier.height(16.dp))
                Text("TXID", color = C.faint, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(4.dp))
                AddressBox(r.txid.removePrefix("0x"))
                Spacer(Modifier.height(16.dp))
                if (r.url.isNotEmpty()) {
                    SecondaryButton("Открыть в обозревателе", {
                        runCatching { ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(r.url))) }
                    }, Modifier.fillMaxWidth(), icon = Icons.AutoMirrored.Rounded.OpenInNew)
                    Spacer(Modifier.height(10.dp))
                }
                SecondaryButton(if (r.broadcast) "Скопировать TXID" else "Скопировать транзакцию", {
                    clipboard.setText(AnnotatedString(if (r.broadcast) r.txid else r.raw))
                    vm.message = "Скопировано"
                }, Modifier.fillMaxWidth(), icon = Icons.Rounded.ContentCopy)
            }
            Spacer(Modifier.height(10.dp))
            PrimaryButton(if (r == null) "Назад к переводу" else "Новый перевод", {
                if (r == null) s.step = Step.Form else s.reset()
            }, Modifier.fillMaxWidth())
        }
    }
}
