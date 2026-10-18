// BLE на Android: поиск MicroWallet и GATT-транспорт для кадров канала.
// GATT: телефон пишет в RX, устройство шлёт уведомления с TX; кадр = [длина 2 байта BE][данные],
// режется на куски по (MTU - 3) байт.
package io.microwallet.app.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import android.util.Base64
import io.microwallet.app.core.WalletError
import java.security.KeyStore
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties

val SERVICE_UUID: UUID = UUID.fromString("6e7a0001-9a3b-4c2d-8f00-6d6963726f77")
val RX_UUID: UUID = UUID.fromString("6e7a0002-9a3b-4c2d-8f00-6d6963726f77")  // телефон пишет
val TX_UUID: UUID = UUID.fromString("6e7a0003-9a3b-4c2d-8f00-6d6963726f77")  // устройство уведомляет
private val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

data class Found(val address: String, val name: String, val rssi: Int)

fun bluetoothEnabled(ctx: Context): Boolean =
    ctx.getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled == true

/** Поиск устройств MicroWallet по UUID сервиса в рекламе. */
@SuppressLint("MissingPermission")
class BleScanner(ctx: Context) {
    private val adapter = ctx.getSystemService(BluetoothManager::class.java)?.adapter
    private var cb: ScanCallback? = null

    fun start(onFound: (Found) -> Unit, onError: (String) -> Unit) {
        stop()
        val scanner = adapter?.bluetoothLeScanner
        if (adapter == null || !adapter.isEnabled || scanner == null) {
            onError("Bluetooth выключен")
            return
        }
        val c = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                val name = r.scanRecord?.deviceName ?: r.device.name ?: "MicroWallet"
                onFound(Found(r.device.address, name, r.rssi))
            }

            override fun onScanFailed(errorCode: Int) = onError("Поиск не запустился (код $errorCode)")
        }
        cb = c
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(listOf(filter), settings, c)
    }

    fun stop() {
        val c = cb ?: return
        cb = null
        try {
            adapter?.bluetoothLeScanner?.stopScan(c)
        } catch (e: Exception) {
            // Bluetooth выключили во время поиска
        }
    }
}

@SuppressLint("MissingPermission")
class GattTransport private constructor(
    private val ctx: Context,
    private val device: BluetoothDevice,
    private val onDisconnect: () -> Unit,
) : FrameTransport {

    private sealed class Ev {
        data class State(val up: Boolean, val status: Int) : Ev()
        data class Mtu(val mtu: Int, val status: Int) : Ev()
        data class Services(val status: Int) : Ev()
        data class Written(val status: Int) : Ev()
        data class Desc(val status: Int) : Ev()
    }

    private val events = LinkedBlockingQueue<Ev>()
    private val frames = LinkedBlockingQueue<ByteArray>()
    private var buf = ByteArray(0)
    @Volatile private var mtu = 23
    @Volatile private var closed = false
    @Volatile private var opened = false
    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null

    private val cb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            val up = newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS
            events.put(Ev.State(up, status))
            if (!up) {
                frames.put(ByteArray(0))
                if (opened && !closed) onDisconnect()
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, value: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) mtu = value
            events.put(Ev.Mtu(value, status))
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) = events.put(Ev.Services(status))

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) =
            events.put(Ev.Written(status))

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) =
            events.put(Ev.Desc(status))

        @Deprecated("до Android 13")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) @Suppress("DEPRECATION") onData(c.value ?: return)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) =
            onData(value)
    }

    @Synchronized
    private fun onData(part: ByteArray) {
        buf += part
        while (buf.size >= 2) {
            val n = ((buf[0].toInt() and 0xFF) shl 8) or (buf[1].toInt() and 0xFF)
            if (buf.size < 2 + n) break
            frames.put(buf.copyOfRange(2, 2 + n))
            buf = buf.copyOfRange(2 + n, buf.size)
        }
    }

    private inline fun <reified T : Ev> expect(timeoutMs: Long, what: String): T {
        val end = System.currentTimeMillis() + timeoutMs
        while (true) {
            val left = end - System.currentTimeMillis()
            val e = (if (left > 0) events.poll(left, TimeUnit.MILLISECONDS) else null)
                ?: throw WalletError("Bluetooth: нет ответа ($what)")
            if (e is T) return e
            if (e is Ev.State && !e.up) throw WalletError("Bluetooth: связь разорвана ($what, код ${e.status})")
        }
    }

    private fun open(timeoutMs: Long) {
        val g = device.connectGatt(ctx, false, cb, BluetoothDevice.TRANSPORT_LE)
            ?: throw WalletError("Bluetooth: не удалось начать подключение")
        gatt = g
        try {
            val st = expect<Ev.State>(timeoutMs, "подключение")
            if (!st.up) throw WalletError("Bluetooth: не подключилось (код ${st.status})")
            if (g.requestMtu(247)) runCatching { expect<Ev.Mtu>(5_000, "MTU") }
            // 2M PHY вдвое быстрее (обновление прошивки); не поддерживается — останется 1M
            g.setPreferredPhy(BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_OPTION_NO_PREFERRED)
            if (!g.discoverServices()) throw WalletError("Bluetooth: не удалось запросить сервисы")
            if (expect<Ev.Services>(10_000, "сервисы").status != BluetoothGatt.GATT_SUCCESS)
                throw WalletError("Bluetooth: ошибка чтения сервисов")
            val svc = g.getService(SERVICE_UUID) ?: throw WalletError("Это не MicroWallet: нет нужного сервиса")
            rx = svc.getCharacteristic(RX_UUID) ?: throw WalletError("MicroWallet: нет характеристики RX")
            val tx = svc.getCharacteristic(TX_UUID) ?: throw WalletError("MicroWallet: нет характеристики TX")
            g.setCharacteristicNotification(tx, true)
            val d = tx.getDescriptor(CCCD) ?: throw WalletError("MicroWallet: нет дескриптора уведомлений")
            val on = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            val started = if (Build.VERSION.SDK_INT >= 33) {
                g.writeDescriptor(d, on) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                d.value = on
                @Suppress("DEPRECATION")
                g.writeDescriptor(d)
            }
            if (!started || expect<Ev.Desc>(5_000, "уведомления").status != BluetoothGatt.GATT_SUCCESS)
                throw WalletError("Bluetooth: не удалось включить уведомления")
            g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
            opened = true
        } catch (e: Exception) {
            close()
            throw e
        }
    }

    @Synchronized
    override fun send(frame: ByteArray) {
        val g = gatt ?: throw ChannelError("связь с устройством потеряна")
        val c = rx ?: throw ChannelError("связь с устройством потеряна")
        val data = byteArrayOf((frame.size ushr 8).toByte(), frame.size.toByte()) + frame
        val chunk = maxOf(20, mtu - 3)
        // Запись без подтверждения: несколько пакетов за одно событие связи (в разы быстрее
        // для прошивки). Потерь нет — поток регулирует контроллер, а onCharacteristicWrite
        // приходит, когда пакет принят в очередь.
        val type = if (c.properties and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0)
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE else BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        var off = 0
        while (off < data.size) {
            val part = data.copyOfRange(off, minOf(data.size, off + chunk))
            val ok = if (Build.VERSION.SDK_INT >= 33) {
                g.writeCharacteristic(c, part, type) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                c.writeType = type
                @Suppress("DEPRECATION")
                c.value = part
                @Suppress("DEPRECATION")
                g.writeCharacteristic(c)
            }
            if (!ok) throw ChannelError("Bluetooth: запись не удалась")
            val w = try {
                expect<Ev.Written>(10_000, "запись")
            } catch (e: WalletError) {
                throw ChannelError(e.message ?: "Bluetooth: запись не удалась")
            }
            if (w.status != BluetoothGatt.GATT_SUCCESS) throw ChannelError("Bluetooth: запись не удалась (код ${w.status})")
            off += part.size
        }
    }

    override fun recv(timeoutMs: Long?): ByteArray? =
        if (timeoutMs == null) frames.take() else frames.poll(timeoutMs, TimeUnit.MILLISECONDS)

    override fun close() {
        if (closed) return
        closed = true
        gatt?.let {
            try {
                it.disconnect()
                it.close()
            } catch (e: Exception) {
                // уже закрыто
            }
        }
        frames.put(ByteArray(0))
    }

    companion object {
        /** Подключиться и подготовить GATT. Блокирует поток — звать не из UI. */
        fun connect(ctx: Context, address: String, onDisconnect: () -> Unit, timeoutMs: Long = 15_000): GattTransport {
            val adapter = ctx.getSystemService(BluetoothManager::class.java)?.adapter
                ?: throw WalletError("Bluetooth недоступен")
            if (!adapter.isEnabled) throw WalletError("Включи Bluetooth")
            val dev = adapter.getRemoteDevice(address)
            var last: Exception? = null
            // Android иногда отвечает ошибкой 133 на первую попытку — пробуем ещё раз
            repeat(2) {
                try {
                    return GattTransport(ctx.applicationContext, dev, onDisconnect).also { it.open(timeoutMs) }
                } catch (e: Exception) {
                    last = e
                    Thread.sleep(600)
                }
            }
            throw last ?: WalletError("Не удалось подключиться по Bluetooth")
        }
    }
}

/** Ключи сопряжения в SharedPreferences, зашифрованные ключом из Android Keystore. */
class KeystoreBondStore(ctx: Context) : BondStore {
    private val prefs = ctx.getSharedPreferences("ble_bonds", Context.MODE_PRIVATE)
    private val alias = "microwallet_bonds"

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(alias, null) as? SecretKey)?.let { return it }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return gen.generateKey()
    }

    override fun get(dev: String): Pair<ByteArray, ByteArray>? {
        val s = prefs.getString(dev, null) ?: return null
        return try {
            val b = Base64.decode(s, Base64.NO_WRAP)
            val c = Cipher.getInstance("AES/GCM/NoPadding")
            c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, b, 0, 12))
            val plain = c.doFinal(b, 12, b.size - 12)
            plain.copyOfRange(0, 8) to plain.copyOfRange(8, 40)
        } catch (e: Exception) {
            null  // ключ Keystore пропал (сброс блокировки экрана и т. п.) — сопряжёмся заново
        }
    }

    override fun put(dev: String, id: ByteArray, key: ByteArray) {
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, key())
        val b = c.iv + c.doFinal(id + key)
        prefs.edit().putString(dev, Base64.encodeToString(b, Base64.NO_WRAP)).apply()
    }

    override fun drop(dev: String) {
        prefs.edit().remove(dev).apply()
    }
}
