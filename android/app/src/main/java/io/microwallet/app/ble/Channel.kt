// Шифрованный канал поверх BLE — тот же протокол, что в src/channel.c и host/ble.py:
//   X25519 -> HMAC-SHA256 (соль — хеш рукопожатия) -> ключи h2d/d2h;
//   кадры DATA: [0x10][счётчик 8 байт BE][ChaCha20-Poly1305(текст)], AAD = первые 9 байт;
//   новый телефон: 6-значный код на экране устройства и в приложении должен совпасть.
package io.microwallet.app.ble

import io.microwallet.app.core.Device
import io.microwallet.app.core.DeviceError
import io.microwallet.app.core.WalletError
import io.microwallet.app.core.parseReply
import io.microwallet.app.core.sha256
import io.microwallet.app.core.unhex
import org.bouncycastle.crypto.InvalidCipherTextException
import org.bouncycastle.crypto.modes.ChaCha20Poly1305
import org.bouncycastle.crypto.params.AEADParameters
import org.bouncycastle.crypto.params.KeyParameter
import org.bouncycastle.crypto.params.X25519PrivateKeyParameters
import org.bouncycastle.crypto.params.X25519PublicKeyParameters
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** Кадры: send() отправляет один кадр, recv() ждёт следующий.
 *  recv: null — истёк таймаут; пустой массив — связь разорвана. timeoutMs = null — ждать сколько угодно. */
interface FrameTransport {
    fun send(frame: ByteArray)
    fun recv(timeoutMs: Long?): ByteArray?
    fun close()
}

class ChannelError(message: String) : WalletError(message)

/** Ключи сопряжения на стороне телефона: устройство (BLE-адрес) -> (id 8 байт, ключ 32 байта). */
interface BondStore {
    fun get(dev: String): Pair<ByteArray, ByteArray>?
    fun put(dev: String, id: ByteArray, key: ByteArray)
    fun drop(dev: String)
}

class MemoryBondStore : BondStore {
    private val m = HashMap<String, Pair<ByteArray, ByteArray>>()
    override fun get(dev: String) = m[dev]
    override fun put(dev: String, id: ByteArray, key: ByteArray) { m[dev] = id to key }
    override fun drop(dev: String) { m.remove(dev) }
}

private const val T_HELLO = 0x01
private const val T_HELLO_R = 0x02
private const val T_ERROR = 0x03
private const val T_DATA = 0x10

private fun hmac(key: ByteArray, vararg msg: ByteArray): ByteArray {
    val m = Mac.getInstance("HmacSHA256")
    m.init(SecretKeySpec(key, "HmacSHA256"))
    msg.forEach { m.update(it) }
    return m.doFinal()
}

private fun expand(prk: ByteArray, label: String) = hmac(prk, label.toByteArray(), byteArrayOf(1))

private fun be64(v: Long) = ByteArray(8) { (v ushr (56 - 8 * it)).toByte() }

private fun nonce(ctr: Long) = ByteArray(4) + be64(ctr)

private fun aead(encrypt: Boolean, key: ByteArray, ctr: Long, input: ByteArray, aad: ByteArray): ByteArray {
    val c = ChaCha20Poly1305()
    c.init(encrypt, AEADParameters(KeyParameter(key), 128, nonce(ctr), aad))
    val out = ByteArray(c.getOutputSize(input.size))
    var n = c.processBytes(input, 0, input.size, out, 0)
    n += c.doFinal(out, n)
    return out.copyOf(n)
}

class Channel(
    private val t: FrameTransport,
    private val devKey: String,
    private val bonds: BondStore,
    private val onCode: ((String) -> Unit)? = null,
) {
    var txNext = 0L
        private set
    private var rxNext = 0L
    private lateinit var kTx: ByteArray
    private lateinit var kRx: ByteArray
    var pairedNow = false
        private set

    fun handshake(pairTimeoutMs: Long = 180_000) {
        val eph = X25519PrivateKeyParameters(SecureRandom())
        val hostPub = eph.generatePublicKey().encoded
        val bond = bonds.get(devKey)
        val hostId = bond?.first ?: ByteArray(8)
        t.send(byteArrayOf(T_HELLO.toByte()) + hostPub + hostId)
        val r = recvRaw(15_000)
        if (r[0].toInt() != T_HELLO_R || r.size != 34) throw ChannelError("неожиданный ответ устройства на рукопожатие")
        val devPub = r.copyOfRange(1, 33)
        val mode = r[33].toInt()
        val shared = ByteArray(32)
        try {
            eph.generateSecret(X25519PublicKeyParameters(devPub, 0), shared, 0)
        } catch (e: IllegalStateException) {
            throw ChannelError("неверный ключ устройства")
        }
        if (mode == 1 && bond == null) throw ChannelError("устройство считает телефон знакомым, но ключа сопряжения нет")
        val th = sha256("MicroWallet BLE v1".toByteArray(), hostPub, devPub, hostId, byteArrayOf(mode.toByte()))
        val prk = hmac(th, shared, if (mode == 1) bond!!.second else ByteArray(0))
        kTx = expand(prk, "h2d")
        kRx = expand(prk, "d2h")
        if (mode == 1) {
            if (recvData(15_000) != "ready") throw ChannelError("устройство не подтвердило сессию")
            return
        }
        // Новый телефон (или устройство нас забыло): код на экране и здесь должен совпасть
        if (bond != null) bonds.drop(devKey)
        val sas = expand(prk, "sas")
        val v = (((sas[0].toLong() and 0xFF) shl 24) or ((sas[1].toLong() and 0xFF) shl 16) or
            ((sas[2].toLong() and 0xFF) shl 8) or (sas[3].toLong() and 0xFF)) % 1_000_000
        onCode?.invoke("%03d %03d".format(v / 1000, v % 1000))
        val msg = recvData(pairTimeoutMs)
        if (!msg.startsWith("paired ")) throw ChannelError("сопряжение не удалось: $msg")
        bonds.put(devKey, msg.substring(7).unhex(), expand(prk, "bond"))
        pairedNow = true
    }

    private fun recvRaw(timeoutMs: Long?): ByteArray {
        val f = t.recv(timeoutMs) ?: throw ChannelError("устройство не отвечает")
        if (f.isEmpty()) throw ChannelError("связь с устройством потеряна")
        if (f[0].toInt() == T_ERROR) {
            val msg = String(f, 1, f.size - 1)
            throw if (msg == "rejected") DeviceError(msg) else ChannelError("устройство: $msg")
        }
        return f
    }

    private fun recvData(timeoutMs: Long?): String {
        val f = recvRaw(timeoutMs)
        if (f[0].toInt() != T_DATA || f.size < 9 + 16) throw ChannelError("неожиданный кадр")
        var ctr = 0L
        for (i in 1..8) ctr = (ctr shl 8) or (f[i].toLong() and 0xFF)
        if (ctr < rxNext) throw ChannelError("повтор кадра")
        val pt = try {
            aead(false, kRx, ctr, f.copyOfRange(9, f.size), f.copyOfRange(0, 9))
        } catch (e: InvalidCipherTextException) {
            throw ChannelError("подпись кадра не сошлась — канал небезопасен")
        }
        rxNext = ctr + 1
        return String(pt)
    }

    fun sendLine(line: String) {
        val hdr = byteArrayOf(T_DATA.toByte()) + be64(txNext)
        val ct = aead(true, kTx, txNext, line.toByteArray(), hdr)
        txNext++
        t.send(hdr + ct)
    }

    /** Команда и ответ. timeoutMs = null: ждать, пока человек нажимает кнопку на устройстве. */
    fun call(line: String, timeoutMs: Long? = null): String {
        sendLine(line)
        return recvData(timeoutMs)
    }

    /** Следующий ответ (для отправки строк конвейером: sendLine подряд, ответы по порядку). */
    fun recvLine(timeoutMs: Long?): String = recvData(timeoutMs)

    /** Для тестов: зашифровать кадр текущим ключом (проверка подмены). */
    internal fun sealForTest(ctr: Long, line: String): ByteArray {
        val hdr = byteArrayOf(T_DATA.toByte()) + be64(ctr)
        return hdr + aead(true, kTx, ctr, line.toByteArray(), hdr)
    }

    internal fun recvLineForTest(timeoutMs: Long) = recvData(timeoutMs)
}

/** Устройство по BLE: то же, что USB, но через шифрованный канал. */
class BleDevice(
    val address: String,
    name: String,
    private val transport: FrameTransport,
    bonds: BondStore,
    onCode: ((String) -> Unit)? = null,
) : Device {
    override val label = name
    val channel = Channel(transport, address, bonds, onCode)

    init {
        try {
            channel.handshake()
        } catch (e: Exception) {
            transport.close()
            throw if (e is WalletError) e else ChannelError("рукопожатие: ${e.message}")
        }
    }

    @Synchronized
    override fun call(line: String): String = parseReply(channel.call(line))

    /** До window строк в полёте: радио не простаивает, пока устройство пишет флеш. */
    @Synchronized
    override fun stream(lines: List<String>, window: Int, onReply: (Int) -> Unit) {
        var sent = 0
        var got = 0
        while (got < lines.size) {
            while (sent < lines.size && sent - got < window) channel.sendLine(lines[sent++])
            val r = channel.recvLine(60_000)
            got++
            try {
                parseReply(r)
            } catch (e: DeviceError) {
                repeat(sent - got) { runCatching { channel.recvLine(60_000) } }  // дочитать отправленное
                throw e
            }
            onReply(got)
        }
    }

    override fun close() = transport.close()
}
