package io.microwallet.app.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.CallMade
import androidx.compose.material.icons.automirrored.rounded.CallReceived
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.Restore
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.microwallet.app.core.Balance
import io.microwallet.app.core.COINS
import io.microwallet.app.core.PRL_COINBASE_MATURITY
import io.microwallet.app.core.assetsFor
import io.microwallet.app.core.coinInfo
import io.microwallet.app.core.fmtUnits
import io.microwallet.app.core.shortAddr

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun OverviewScreen(vm: AppViewModel) {
    val w = vm.wallet ?: return
    if (vm.info["state"] == "locked") {
        Column(Modifier.fillMaxSize().padding(16.dp)) {
            ScreenTitle("Обзор")
            LockedCard()
        }
        return
    }
    if (!w.ready) {
        EmptyDevice(vm)
        return
    }
    val coins = COINS.map { it.id }.filter { it in vm.coins }
    if (coins.isEmpty()) {
        Column(Modifier.fillMaxSize().padding(16.dp)) {
            ScreenTitle("Обзор")
            NoCoins { vm.openApps() }
        }
        return
    }
    val loading = coins.any { vm.balances[it] == Load.Loading }
    PullToRefreshBox(isRefreshing = loading, onRefresh = { vm.refreshNow() }, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 24.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            // подсказка — сверху, там, где жест и начинается
            item { ScreenTitle("Обзор", "Балансы всех сетей · потяни вниз, чтобы обновить") }
            items(coins) { c -> ChainCard(vm, c, w.net) }
        }
    }
}

@Composable
private fun EmptyDevice(vm: AppViewModel) {
    Column(Modifier.fillMaxSize().padding(16.dp)) {
        ScreenTitle("Обзор")
        Card {
            Text("Устройство пустое", style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(8.dp))
            Text(
                "Новый кошелёк лучше создать на самом устройстве: удерживай кнопку на «New wallet» и запиши " +
                    "слова на бумагу. Если у тебя уже есть seed-фраза, её можно ввести здесь.",
                color = C.muted, fontSize = 14.sp, lineHeight = 20.sp,
            )
            Spacer(Modifier.height(18.dp))
            PrimaryButton("Восстановить из seed-фразы", { vm.showRestore = true }, Modifier.fillMaxWidth(),
                icon = Icons.Rounded.Restore)
            Spacer(Modifier.height(10.dp))
            SecondaryButton("Я создал кошелёк на устройстве — обновить", { vm.refresh() }, Modifier.fillMaxWidth())
        }
    }
}

@Composable
private fun ChainCard(vm: AppViewModel, coin: String, net: String) {
    val clipboard = LocalClipboardManager.current
    val info = coinInfo(coin)
    val state = vm.balances[coin]
    Card(padding = PaddingValues(18.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            CoinIcon(coin, 40.dp)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(info.name, style = MaterialTheme.typography.titleMedium)
                Text(info.nets[net] ?: net, color = C.faint, fontSize = 12.sp)
            }
            val b = (state as? Load.Ok)?.v
            if (b != null) Row(
                Modifier
                    .clip(RoundedCornerShape(10.dp))
                    .clickable {
                        clipboard.setText(AnnotatedString(b.address))
                        vm.message = "Адрес скопирован"
                    }
                    .padding(horizontal = 8.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(shortAddr(b.address, 6), color = C.accent2, fontSize = 13.sp, fontFamily = Mono,
                    maxLines = 1, overflow = TextOverflow.Clip)
                Spacer(Modifier.width(6.dp))
                Icon(Icons.Rounded.ContentCopy, "Скопировать", tint = C.accent2, modifier = Modifier.size(15.dp))
            }
        }
        Spacer(Modifier.height(14.dp))
        when (state) {
            null, Load.Loading -> repeat(info.assetsFor(net).size) { SkeletonRow() }
            is Load.Err -> Text(state.msg, color = C.bad, fontSize = 14.sp)
            is Load.Ok -> Assets(coin, state.v)
        }
        Spacer(Modifier.height(14.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            SecondaryButton("Отправить", {
                vm.coinRequest = coin
                vm.tab = Tab.Send
            }, Modifier.weight(1f), icon = Icons.AutoMirrored.Rounded.CallMade)
            SecondaryButton("Получить", {
                vm.coinRequest = coin
                vm.tab = Tab.Receive
            }, Modifier.weight(1f), icon = Icons.AutoMirrored.Rounded.CallReceived)
        }
    }
}

@Composable
private fun Assets(coin: String, b: Balance) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        for (a in b.assets) Row(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(14.dp))
                .background(C.card2)
                .padding(horizontal = 12.dp, vertical = 11.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val token = a.id == "usdt" || a.id == "usdc"
            CoinIcon(if (token) a.id else coin, 26.dp)
            Spacer(Modifier.width(10.dp))
            Text(
                a.symbol + if (token) " · " + mapOf("tron" to "TRC20", "bsc" to "BEP20", "eth" to "ERC20", "ton" to "TON")[coin].orEmpty() else "",
                color = C.muted, fontSize = 14.sp, modifier = Modifier.weight(1f),
            )
            Text(fmtUnits(a.amount, a.decimals), fontSize = 18.sp, fontWeight = FontWeight.Bold, color = C.text,
                maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (b.pending.signum() > 0) Text(
            "Созревает: ${fmtUnits(b.pending, 8)} ${b.assets[0].symbol} (награды за блок, $PRL_COINBASE_MATURITY подтверждений)",
            color = C.warn, fontSize = 12.sp, lineHeight = 17.sp,
        )
        for (n in b.notes) Text(n, color = C.faint, fontSize = 12.sp)
    }
}

@Composable
private fun SkeletonRow() {
    Box(
        Modifier
            .fillMaxWidth()
            .padding(bottom = 8.dp)
            .height(48.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(C.card2.copy(alpha = 0.6f))
    )
}

/** Устройство ждёт PIN после включения: приложение само продолжит, как только его введут. */
@Composable
fun LockedCard() {
    Card {
        Text("Введи PIN на устройстве", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(8.dp))
        Text(
            "Устройство заблокировано после включения. Цифра — длительность нажатия: клик — 1, дальше +1 за " +
                "каждые полсекунды удержания. После четырёх цифр — клик, чтобы ввести. Балансы появятся сами.",
            color = C.muted, fontSize = 14.sp, lineHeight = 20.sp,
        )
    }
}
