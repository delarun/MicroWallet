package io.microwallet.app.ui

import android.content.Intent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.Remove
import androidx.compose.material.icons.rounded.Share
import androidx.compose.material.icons.rounded.VerifiedUser
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.microwallet.app.core.ADDRS_DEFAULT
import io.microwallet.app.core.COINS
import io.microwallet.app.core.PRL_COINBASE_MATURITY
import io.microwallet.app.core.coinInfo

@Composable
fun ReceiveScreen(vm: AppViewModel) {
    val w = vm.wallet ?: return
    val ctx = LocalContext.current
    val clipboard = LocalClipboardManager.current
    val coins = COINS.map { it.id }.filter { it in vm.coins }
    if (coins.isEmpty()) {
        Column(Modifier.fillMaxSize().padding(16.dp)) {
            ScreenTitle("Получить")
            NoCoins { vm.openApps() }
        }
        return
    }
    var coin by rememberSaveable { mutableStateOf(vm.coinRequest ?: coins.first()) }
    var index by rememberSaveable { mutableStateOf(0) }
    var addr by remember { mutableStateOf<String?>(null) }
    var status by remember { mutableStateOf("info" to "") }
    var verifying by remember { mutableStateOf(false) }
    // vm.info — наблюдаемое состояние: после смены настройки экран перерисуется сам
    val count = vm.info["addrs"]?.toIntOrNull()?.coerceIn(1, 20) ?: ADDRS_DEFAULT
    LaunchedEffect(count) { if (index > count - 1) index = count - 1 }
    LaunchedEffect(vm.coinRequest) {
        vm.coinRequest?.let { coin = it }
        vm.coinRequest = null
    }
    LaunchedEffect(coin, index, w) {
        addr = null
        if (!w.ready) {
            status = "info" to if (w.locked) "Введи PIN на устройстве" else "На устройстве нет кошелька"
            return@LaunchedEffect
        }
        status = "info" to ""
        val c = coin
        val i = index
        vm.run({ w.address(c, i) }, {
            if (c == coin && i == index) {
                addr = it
                status = "info" to "Адрес ещё не проверен на устройстве"
            }
        }, { status = "bad" to it })
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        ScreenTitle("Получить", "Адрес для приёма — сверь его с экраном устройства")
        CoinPills(coins, coin) {
            coin = it
            index = 0
        }
        Card {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("${coinInfo(coin).name}  #$index", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
                SmallIconButton(Icons.Rounded.Remove, "Предыдущий адрес", { index-- }, enabled = index > 0)
                Text("адрес", color = C.muted, fontSize = 13.sp)
                SmallIconButton(Icons.Rounded.Add, "Следующий адрес", { index++ }, enabled = index < count - 1)
            }
            Spacer(Modifier.height(14.dp))
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                val a = addr
                if (a != null) QrCode(a, 232.dp, coin)
                else Box(
                    Modifier.size(232.dp).clip(RoundedCornerShape(18.dp)).background(C.card2),
                    contentAlignment = Alignment.Center,
                ) { CircularProgressIndicator(color = C.accent) }
            }
            Spacer(Modifier.height(16.dp))
            AddressBox(addr ?: "…")
            Spacer(Modifier.height(12.dp))
            Status(status.first, status.second)
            Spacer(Modifier.height(14.dp))
            PrimaryButton(
                "Проверить на устройстве",
                {
                    val c = coin
                    val i = index
                    verifying = true
                    vm.busy = true
                    status = "wait" to "Подтверди адрес на устройстве…"
                    vm.run({ w.showAddress(c, i) }, {
                        verifying = false
                        vm.busy = false
                        status = if (it == addr) "ok" to "✓  Адрес подтверждён на устройстве"
                        else "bad" to "Устройство показало другой адрес — не используй его!"
                    }, {
                        verifying = false
                        vm.busy = false
                        status = "bad" to it
                    })
                },
                Modifier.fillMaxWidth(),
                enabled = addr != null && !verifying,
                loading = verifying,
                icon = Icons.Rounded.VerifiedUser,
            )
            Spacer(Modifier.height(10.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                SecondaryButton("Скопировать", {
                    addr?.let {
                        clipboard.setText(AnnotatedString(it))
                        vm.message = "Адрес скопирован"
                    }
                }, Modifier.weight(1f), enabled = addr != null, icon = Icons.Rounded.ContentCopy)
                SecondaryButton("Поделиться", {
                    addr?.let {
                        val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, it)
                        ctx.startActivity(Intent.createChooser(send, null))
                    }
                }, Modifier.weight(1f), enabled = addr != null, icon = Icons.Rounded.Share)
            }
            Spacer(Modifier.height(14.dp))
            Text(
                "Устройство покажет адрес по шагам: сверь все символы и удерживай кнопку на «Approve». " +
                    "Если адрес отличается — удерживай на «Reject».",
                color = C.faint, fontSize = 12.sp, lineHeight = 17.sp,
            )
        }
        if (coin == "prl") {
            Spacer(Modifier.height(12.dp))
            Text(
                "Этот адрес можно указать как адрес выплат майнинга. Награды за блок созревают $PRL_COINBASE_MATURITY блоков.",
                color = C.faint, fontSize = 12.sp, lineHeight = 17.sp, modifier = Modifier.padding(horizontal = 4.dp),
            )
        }
        Spacer(Modifier.height(16.dp))
    }
}
