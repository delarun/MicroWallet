package io.microwallet.app.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.CallMade
import androidx.compose.material.icons.automirrored.rounded.CallReceived
import androidx.compose.material.icons.rounded.BatteryChargingFull
import androidx.compose.material.icons.rounded.Bluetooth
import androidx.compose.material.icons.rounded.DeveloperBoard
import androidx.compose.material.icons.rounded.GridView
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

class MainActivity : ComponentActivity() {
    private val vm: AppViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(C.bg.toArgb()),
            navigationBarStyle = SystemBarStyle.dark(C.side.toArgb()),
        )
        setContent { MicroTheme { App(vm) } }
    }

    override fun onStart() {
        super.onStart()
        vm.onForeground()
    }

    override fun onStop() {
        super.onStop()
        vm.onBackground()
    }
}

@Composable
fun App(vm: AppViewModel) {
    val snackbar = remember { SnackbarHostState() }
    LaunchedEffect(vm.message) {
        vm.message?.let {
            vm.message = null
            snackbar.showSnackbar(it)
        }
    }
    val w = vm.wallet
    Scaffold(
        containerColor = C.bg,
        snackbarHost = { SnackbarHost(snackbar) },
        topBar = { if (w != null) TopBar(vm) },
        bottomBar = { if (w != null && !vm.showRestore && !vm.showFirmware && !vm.showApps) BottomBar(vm) },
    ) { pad ->
        Box(Modifier.fillMaxSize().padding(pad)) {
            when {
                w == null -> ConnectScreen(vm)
                vm.showRestore -> RestoreScreen(vm)
                vm.showFirmware -> FirmwareScreen(vm)
                vm.showApps -> AppsScreen(vm)
                else -> AnimatedContent(vm.tab, label = "tab", transitionSpec = { fadeIn() togetherWith fadeOut() }) { t ->
                    when (t) {
                        Tab.Overview -> OverviewScreen(vm)
                        Tab.Receive -> ReceiveScreen(vm)
                        Tab.Send -> SendScreen(vm)
                        Tab.Device -> DeviceScreen(vm)
                    }
                }
            }
        }
    }
    PairDialog(vm)
}

@Composable
private fun TopBar(vm: AppViewModel) {
    val net = vm.info["net"] ?: vm.wallet?.net ?: "testnet"
    Row(
        Modifier
            .fillMaxWidth()
            .background(C.bg)
            .windowInsetsPadding(WindowInsets.statusBars)
            .padding(horizontal = 20.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        CoinIcon("logo", 34.dp)
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text("MicroWallet", fontSize = 18.sp, fontWeight = FontWeight.Bold, color = C.text)
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (!vm.isDemo) Icon(Icons.Rounded.Bluetooth, null, tint = C.accent2, modifier = Modifier.size(14.dp))
                Text(
                    vm.wallet?.label ?: "", fontSize = 12.sp, color = C.muted,
                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                )
            }
        }
        val pct = batteryPct(vm.info["batt"]?.toIntOrNull() ?: -1)
        if (vm.info["chg"] == "1") {
            Icon(Icons.Rounded.BatteryChargingFull, "Заряжается", tint = C.ok, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
        } else if (pct >= 0) {
            BatteryChip(pct)
            Spacer(Modifier.width(8.dp))
        }
        Badge(net.uppercase(), if (net == "mainnet") C.ok else C.warn)
    }
}

@Composable
fun BatteryChip(pct: Int) {
    val color = when {
        pct <= 15 -> C.bad
        pct <= 35 -> C.warn
        else -> C.muted
    }
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        // Батарейка рисуется прямоугольниками — как на экране устройства
        Box(
            Modifier
                .size(width = 22.dp, height = 11.dp)
                .clip(RoundedCornerShape(3.dp))
                .background(C.line)
                .padding(2.dp),
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .padding(end = (18 * (100 - pct) / 100).dp)
                    .clip(RoundedCornerShape(1.dp))
                    .background(color),
            )
        }
        Text("$pct%", fontSize = 12.sp, color = color)
    }
}

/** Как ui_battery_pct() в прошивке. */
fun batteryPct(mv: Int): Int {
    if (mv <= 0) return -1
    // 0% — 3.5 В: ниже плата не работает (диод и стабилизатор между аккумулятором и чипом)
    val pts = listOf(3500 to 0, 3600 to 8, 3700 to 20, 3750 to 30, 3800 to 40, 3850 to 50,
        3900 to 60, 4000 to 75, 4100 to 90, 4200 to 100)
    if (mv <= 3500) return 0
    for (i in 0 until pts.size - 1) {
        val (v0, p0) = pts[i]
        val (v1, p1) = pts[i + 1]
        if (mv in (v0 + 1)..v1) return p0 + (p1 - p0) * (mv - v0) / (v1 - v0)
    }
    return 100
}

@Composable
private fun BottomBar(vm: AppViewModel) {
    NavigationBar(containerColor = C.side, tonalElevation = 0.dp) {
        val icons = mapOf(
            Tab.Overview to Icons.Rounded.GridView,
            Tab.Receive to Icons.AutoMirrored.Rounded.CallReceived,
            Tab.Send to Icons.AutoMirrored.Rounded.CallMade,
            Tab.Device to Icons.Rounded.DeveloperBoard,
        )
        for (t in Tab.entries) NavigationBarItem(
            selected = vm.tab == t,
            onClick = { vm.tab = t },
            icon = { Icon(icons.getValue(t), null) },
            label = { Text(t.title, fontSize = 12.sp) },
            colors = NavigationBarItemDefaults.colors(
                selectedIconColor = C.text,
                selectedTextColor = C.text,
                indicatorColor = C.accent.copy(alpha = 0.28f),
                unselectedIconColor = C.muted,
                unselectedTextColor = C.muted,
            ),
        )
    }
}
