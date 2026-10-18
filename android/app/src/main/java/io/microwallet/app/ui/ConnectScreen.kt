package io.microwallet.app.ui

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Bluetooth
import androidx.compose.material.icons.rounded.BluetoothDisabled
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import io.microwallet.app.ble.Found
import io.microwallet.app.ble.bluetoothEnabled

fun blePermissions(): Array<String> =
    if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

fun hasBlePermissions(ctx: Context) =
    blePermissions().all { ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED }

@Composable
fun ConnectScreen(vm: AppViewModel) {
    val ctx = LocalContext.current
    var perms by remember { mutableStateOf(hasBlePermissions(ctx)) }
    var btOn by remember { mutableStateOf(bluetoothEnabled(ctx)) }
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) {
        perms = hasBlePermissions(ctx)
        btOn = bluetoothEnabled(ctx)
    }
    val askPerms = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        perms = hasBlePermissions(ctx)
        btOn = bluetoothEnabled(ctx)
    }
    val enableBt = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        btOn = bluetoothEnabled(ctx)
    }
    LaunchedEffect(perms, btOn, vm.conn) {
        if (perms && btOn && vm.conn == Conn.Idle) vm.startScan()
    }

    Column(
        Modifier
            .fillMaxSize()
            .windowInsetsPadding(WindowInsets.systemBars)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 20.dp, vertical = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Spacer(Modifier.height(28.dp))
        CoinIcon("logo", 76.dp)
        Spacer(Modifier.height(18.dp))
        Text("MicroWallet", fontSize = 30.sp, fontWeight = FontWeight.Bold, color = C.text)
        Spacer(Modifier.height(6.dp))
        Text(
            "Аппаратный кошелёк. Ключи остаются на устройстве,\nтелефон только собирает и отправляет транзакции.",
            color = C.muted, fontSize = 14.sp, textAlign = TextAlign.Center, lineHeight = 20.sp,
        )
        Spacer(Modifier.height(16.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            for (c in listOf("btc", "prl", "tron", "bsc", "eth", "ton", "usdt")) CoinIcon(c, 28.dp)
        }
        Spacer(Modifier.height(32.dp))

        val c = vm.conn
        when {
            !perms -> Card {
                Text("Нужен доступ к Bluetooth", style = MaterialTheme.typography.titleMedium)
                Spacer(Modifier.height(6.dp))
                Text(
                    "Чтобы найти MicroWallet рядом и подключиться к нему." +
                        if (Build.VERSION.SDK_INT < 31) " На этой версии Android система называет это доступом к геопозиции." else "",
                    color = C.muted, fontSize = 14.sp,
                )
                Spacer(Modifier.height(16.dp))
                PrimaryButton("Разрешить", { askPerms.launch(blePermissions()) }, Modifier.fillMaxWidth())
            }
            !btOn -> Card {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Rounded.BluetoothDisabled, null, tint = C.warn)
                    Spacer(Modifier.width(10.dp))
                    Text("Bluetooth выключен", style = MaterialTheme.typography.titleMedium)
                }
                Spacer(Modifier.height(16.dp))
                PrimaryButton("Включить Bluetooth", {
                    runCatching { enableBt.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) }
                }, Modifier.fillMaxWidth())
            }
            c is Conn.Connecting || c is Conn.Pairing -> Card {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(Modifier.size(22.dp), color = C.accent, strokeWidth = 2.5.dp)
                    Spacer(Modifier.width(14.dp))
                    Text(
                        "Подключаюсь к ${if (c is Conn.Connecting) c.name else (c as Conn.Pairing).name}…",
                        style = MaterialTheme.typography.titleMedium,
                    )
                }
                Spacer(Modifier.height(16.dp))
                SecondaryButton("Отмена", { vm.cancelConnect() }, Modifier.fillMaxWidth())
            }
            else -> DeviceList(vm)
        }

        Spacer(Modifier.height(20.dp))
        Text(
            "Попробовать без устройства (демо)",
            color = C.accent2, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
            modifier = Modifier
                .clip(RoundedCornerShape(10.dp))
                .clickable { vm.demo() }
                .padding(horizontal = 12.dp, vertical = 10.dp),
        )
    }
}

@Composable
private fun DeviceList(vm: AppViewModel) {
    Card(padding = androidx.compose.foundation.layout.PaddingValues(vertical = 14.dp)) {
        Row(Modifier.padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("Устройства рядом", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            if (vm.scanning) CircularProgressIndicator(Modifier.size(18.dp), color = C.accent, strokeWidth = 2.dp)
            else SmallIconButton(Icons.Rounded.Refresh, "Искать снова", { vm.startScan() })
        }
        Spacer(Modifier.height(8.dp))
        if (vm.found.isEmpty()) {
            Text(
                vm.scanError ?: if (vm.scanning)
                    "Ищу MicroWallet… Если экран устройства погас, нажми на нём кнопку: выключенное устройство " +
                        "не видно по Bluetooth. Bluetooth на устройстве: Settings → Bluetooth."
                else "Ничего не нашлось. Разбуди устройство кнопкой и нажми «обновить».",
                color = if (vm.scanError != null) C.bad else C.muted, fontSize = 14.sp, lineHeight = 20.sp,
                modifier = Modifier.padding(horizontal = 20.dp, vertical = 6.dp),
            )
        }
        for (f in vm.found.sortedByDescending { it.rssi }) DeviceRow(f, f.address == vm.knownDevice) { vm.connect(f) }
    }
}

@Composable
private fun DeviceRow(f: Found, known: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 20.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(42.dp).clip(RoundedCornerShape(12.dp)).background(C.card2),
            contentAlignment = Alignment.Center,
        ) { Icon(Icons.Rounded.Bluetooth, null, tint = C.accent2) }
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(f.name, fontWeight = FontWeight.SemiBold, fontSize = 16.sp, color = C.text)
                if (known) {
                    Spacer(Modifier.width(8.dp))
                    Badge("ЗНАКОМОЕ", C.ok)
                }
            }
            Text(f.address, fontFamily = Mono, fontSize = 12.sp, color = C.faint)
        }
        SignalBars(f.rssi)
        Spacer(Modifier.width(6.dp))
        Icon(Icons.AutoMirrored.Rounded.KeyboardArrowRight, null, tint = C.faint)
    }
}

@Composable
private fun SignalBars(rssi: Int) {
    val level = when {
        rssi > -60 -> 4
        rssi > -70 -> 3
        rssi > -80 -> 2
        else -> 1
    }
    Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(2.dp)) {
        for (i in 1..4) Box(
            Modifier
                .size(width = 4.dp, height = (4 + 3 * i).dp)
                .clip(RoundedCornerShape(1.dp))
                .background(if (i <= level) C.accent2 else C.line)
        )
    }
}

@Composable
fun PairDialog(vm: AppViewModel) {
    val c = vm.conn as? Conn.Pairing ?: return
    Dialog(onDismissRequest = {}, properties = DialogProperties(dismissOnBackPress = false, dismissOnClickOutside = false)) {
        Card(padding = androidx.compose.foundation.layout.PaddingValues(24.dp)) {
            Box(
                Modifier.size(52.dp).clip(RoundedCornerShape(16.dp)).background(C.accent.copy(alpha = 0.18f)),
                contentAlignment = Alignment.Center,
            ) { Icon(Icons.Rounded.Bluetooth, null, tint = C.accent) }
            Spacer(Modifier.height(16.dp))
            Text("Сопряжение с ${c.name}", style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.height(8.dp))
            Text(
                "Сверь код с экраном устройства и удерживай кнопку на «Pair». Если код другой — " +
                    "удерживай на «Reject»: кто-то пытается встать посередине.",
                color = C.muted, fontSize = 14.sp, lineHeight = 20.sp,
            )
            Spacer(Modifier.height(20.dp))
            Text(
                c.code, fontFamily = Mono, fontSize = 40.sp, fontWeight = FontWeight.Bold, color = C.text,
                letterSpacing = 4.sp, textAlign = TextAlign.Center,
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(14.dp))
                    .background(C.card2)
                    .padding(vertical = 16.dp),
            )
            Spacer(Modifier.height(20.dp))
            SecondaryButton("Отмена", { vm.cancelConnect() }, Modifier.fillMaxWidth())
        }
    }
}
