// Какие токены показывать на главной: скрытые не запрашиваются (меньше запросов к сетям).
package io.microwallet.app.ui

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.microwallet.app.core.COINS
import io.microwallet.app.core.coinInfo
import io.microwallet.app.core.tokensFor
import io.microwallet.app.core.TRC10_TOKENS

@Composable
fun TokensCard(vm: AppViewModel) {
    val w = vm.wallet ?: return
    val coins = COINS.map { it.id }.filter { it in vm.coins && coinInfo(it).tokensFor(w.net).isNotEmpty() }
    if (coins.isEmpty()) return
    Card {
        Text("Токены на главной", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(6.dp))
        Text(
            "Выключенные токены не показываются и их баланс не запрашивается — меньше запросов к сетям. " +
                "Отправить выключенный токен можно, включив его здесь.",
            color = C.muted, fontSize = 13.sp, lineHeight = 19.sp,
        )
        for (c in coins) {
            Spacer(Modifier.height(12.dp))
            Text(coinInfo(c).name, color = C.faint, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            for (t in coinInfo(c).tokensFor(w.net)) Row(
                Modifier.fillMaxWidth().padding(vertical = 2.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CoinIcon(t, 24.dp)
                Spacer(Modifier.width(10.dp))
                Text(w.symbol(t), color = C.text, fontSize = 15.sp, modifier = Modifier.weight(1f))
                Switch(
                    checked = vm.tokenShown(c, t), onCheckedChange = { vm.setTokenShown(c, t, it) },
                    colors = SwitchDefaults.colors(checkedTrackColor = C.accent),
                )
            }
            if (c == "tron") Row(  // TRC10: их рассылают по чужим адресам, обычно это спам без названия
                Modifier.fillMaxWidth().padding(vertical = 2.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                CoinIcon("tron", 24.dp)
                Spacer(Modifier.width(10.dp))
                Column(Modifier.weight(1f)) {
                    Text("TRC10", color = C.text, fontSize = 15.sp)
                    Text("токены без названия (#1005193) — чаще всего спам", color = C.faint, fontSize = 12.sp)
                }
                Switch(
                    checked = vm.tokenShown(c, TRC10_TOKENS), onCheckedChange = { vm.setTokenShown(c, TRC10_TOKENS, it) },
                    colors = SwitchDefaults.colors(checkedTrackColor = C.accent),
                )
            }
        }
    }
}
