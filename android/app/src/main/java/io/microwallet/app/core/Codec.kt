// Кодировки и форматы: hex, суммы, bech32/bech32m, base58 (TRON), EIP-55, RLP.
// Перенос host/wallet.py — поведение и тексты ошибок те же.
package io.microwallet.app.core

import org.bouncycastle.crypto.digests.KeccakDigest
import java.io.ByteArrayOutputStream
import java.math.BigDecimal
import java.math.BigInteger
import java.security.MessageDigest

/** Понятная пользователю ошибка (сеть, ввод, нехватка средств). */
open class WalletError(message: String) : Exception(message)

/** Устройство ответило "err ...". Например, "rejected" — отказ кнопкой. */
class DeviceError(message: String) : WalletError(message)

// ---------------- hex и хеши ----------------

private val HEX = "0123456789abcdef".toCharArray()

fun ByteArray.hex(): String {
    val out = CharArray(size * 2)
    for (i in indices) {
        val v = this[i].toInt() and 0xFF
        out[2 * i] = HEX[v ushr 4]
        out[2 * i + 1] = HEX[v and 15]
    }
    return String(out)
}

fun String.unhex(): ByteArray {
    val s = removePrefix("0x")
    if (s.length % 2 != 0) throw WalletError("нечётная длина hex")
    return ByteArray(s.length / 2) {
        val hi = Character.digit(s[2 * it], 16)
        val lo = Character.digit(s[2 * it + 1], 16)
        if (hi < 0 || lo < 0) throw WalletError("неверный hex")
        ((hi shl 4) or lo).toByte()
    }
}

fun sha256(vararg parts: ByteArray): ByteArray {
    val md = MessageDigest.getInstance("SHA-256")
    parts.forEach { md.update(it) }
    return md.digest()
}

fun dsha(b: ByteArray): ByteArray = sha256(sha256(b))

fun keccak256(b: ByteArray): ByteArray {
    val d = KeccakDigest(256)
    d.update(b, 0, b.size)
    return ByteArray(32).also { d.doFinal(it, 0) }
}

/** Little-endian запись для транзакций и запросов к устройству. */
class Buf {
    private val out = ByteArrayOutputStream()
    fun u8(v: Int) = apply { out.write(v and 0xFF) }
    fun u32(v: Long) = apply { for (i in 0 until 4) out.write(((v ushr (8 * i)) and 0xFF).toInt()) }
    fun u64(v: Long) = apply { for (i in 0 until 8) out.write(((v ushr (8 * i)) and 0xFF).toInt()) }
    fun bytes(b: ByteArray) = apply { out.write(b) }
    fun toByteArray(): ByteArray = out.toByteArray()
}

// ---------------- суммы ----------------

fun toUnits(s: String, decimals: Int): BigInteger {
    val d = try {
        BigDecimal(s.trim().replace(',', '.'))
    } catch (e: NumberFormatException) {
        throw WalletError("Неверная сумма: $s")
    }
    val v = d.movePointRight(decimals)
    if (d.signum() <= 0 || v.stripTrailingZeros().scale() > 0)
        throw WalletError("Неверная сумма: $s (больше 0, не более $decimals знаков после точки)")
    return v.toBigIntegerExact()
}

fun fmtUnits(v: BigInteger, decimals: Int): String =
    if (v.signum() == 0) "0" else BigDecimal(v, decimals).stripTrailingZeros().toPlainString()

fun fmtUnits(v: Long, decimals: Int): String = fmtUnits(BigInteger.valueOf(v), decimals)

fun BigInteger.unsignedBytes(): ByteArray {
    if (signum() == 0) return ByteArray(0)
    val b = toByteArray()
    return if (b[0].toInt() == 0) b.copyOfRange(1, b.size) else b
}

fun BigInteger.toBytes(n: Int): ByteArray {
    val b = unsignedBytes()
    require(b.size <= n)
    return ByteArray(n - b.size) + b
}

// ---------------- bech32 / bech32m (BIP173, BIP350) ----------------

private const val CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
private val GEN = intArrayOf(0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3)

private fun polymod(values: List<Int>): Int {
    var chk = 1
    for (v in values) {
        val b = chk ushr 25
        chk = ((chk and 0x1FFFFFF) shl 5) xor v
        for (i in 0 until 5) if ((b ushr i) and 1 == 1) chk = chk xor GEN[i]
    }
    return chk
}

private fun hrpExpand(hrp: String): List<Int> =
    hrp.map { it.code ushr 5 } + 0 + hrp.map { it.code and 31 }

private fun convertBits(data: List<Int>, from: Int, to: Int, pad: Boolean): List<Int>? {
    var acc = 0
    var bits = 0
    val out = ArrayList<Int>()
    val maxv = (1 shl to) - 1
    for (v in data) {
        acc = (acc shl from) or v
        bits += from
        while (bits >= to) {
            bits -= to
            out.add((acc ushr bits) and maxv)
        }
    }
    if (pad && bits > 0) out.add((acc shl (to - bits)) and maxv)
    else if (!pad && (bits >= from || ((acc shl (to - bits)) and maxv) != 0)) return null
    return out
}

/** bech32-адрес -> scriptPubKey. pearl = только Taproot (v1) и P2MR (v2). */
/** Версии старых адресов сети (base58): P2PKH и P2SH. */
class B58Versions(val pkh: List<Int>, val sh: List<Int>)

fun b58checkDecode(addr: String): ByteArray {
    var n = BigInteger.ZERO
    for (c in addr) {
        val i = B58.indexOf(c)
        if (i < 0) throw WalletError("неверный адрес")
        n = n.multiply(BigInteger.valueOf(58)).add(BigInteger.valueOf(i.toLong()))
    }
    val zeros = addr.length - addr.trimStart('1').length
    val raw = ByteArray(zeros) + (if (n.signum() == 0) ByteArray(0) else n.unsignedBytes())
    if (raw.size < 5) throw WalletError("неверный адрес")
    val body = raw.copyOfRange(0, raw.size - 4)
    if (!dsha(body).copyOfRange(0, 4).contentEquals(raw.copyOfRange(raw.size - 4, raw.size)))
        throw WalletError("неверная контрольная сумма адреса")
    return body
}

/** Старый P2SH Litecoin «3…» (testnet «2…»): такой же, как у Bitcoin. -> тот же адрес как M…/Q…,
 *  иначе null. Устройство покажет именно M… — с ним и сверять (как ltc_old_p2sh в wallet.py). */
fun ltcOldP2sh(addr: String, net: String): String? {
    val body = try { b58checkDecode(addr.trim()) } catch (e: WalletError) { return null }
    val (old, new) = if (net == "mainnet") 0x05 to 0x32 else 0xC4 to 0x3A
    if (body.size != 21 || (body[0].toInt() and 0xFF) != old) return null
    val b = byteArrayOf(new.toByte()) + body.copyOfRange(1, 21)
    val full = b + dsha(b).copyOfRange(0, 4)
    var n = BigInteger(1, full)
    val sb = StringBuilder()
    while (n.signum() > 0) {
        val (q, r) = n.divideAndRemainder(BigInteger.valueOf(58))
        sb.append(B58[r.toInt()])
        n = q
    }
    return sb.reverse().toString()
}

const val LTC_OLD_P2SH_NOTE = "Адрес формата 3… такой же у Bitcoin: убедись, что получатель ждёт именно Litecoin. " +
    "Устройство покажет его как %s"

/** Адрес -> scriptPubKey. b58 — старые адреса сети: P2PKH 76a914<20>88ac и P2SH a914<20>87. */
fun addressToScript(addr: String, hrp: String, pearl: Boolean = false, b58: B58Versions? = null): ByteArray {
    if (b58 != null && !addr.lowercase().startsWith(hrp + "1")) {
        val pos = addr.lastIndexOf('1')
        val pre = if (pos > 0) addr.lowercase().substring(0, pos) else ""
        if (pre in setOf("bc", "tb", "ltc", "tltc", "prl", "tprl"))
            throw WalletError("адрес другой сети (${pre}1…): ожидается ${hrp}1… или старый адрес этой сети")
        val body = b58checkDecode(addr)
        val v = body[0].toInt() and 0xFF
        if (body.size == 21 && v in b58.pkh)
            return byteArrayOf(0x76, 0xa9.toByte(), 0x14) + body.copyOfRange(1, 21) + byteArrayOf(0x88.toByte(), 0xac.toByte())
        if (body.size == 21 && v in b58.sh)
            return byteArrayOf(0xa9.toByte(), 0x14) + body.copyOfRange(1, 21) + byteArrayOf(0x87.toByte())
        throw WalletError("адрес другой сети: ожидается ${hrp}1… или старый адрес этой сети")
    }
    val a = addr.lowercase()
    if (addr != a && addr != addr.uppercase()) throw WalletError("смешанный регистр в адресе")
    val pos = a.lastIndexOf('1')
    if (pos < 0 || a.substring(0, pos) != hrp)
        throw WalletError("поддерживаются только bech32-адреса этой сети (${hrp}1...)")
    val data = a.substring(pos + 1).map { CHARSET.indexOf(it) }
    if (-1 in data || data.size < 7) throw WalletError("неверный адрес")
    val const = polymod(hrpExpand(hrp) + data)
    val ver = data[0]
    if ((ver == 0 && const != 1) || (ver != 0 && const != 0x2BC830A3))
        throw WalletError("неверная контрольная сумма адреса")
    val prog = convertBits(data.subList(1, data.size - 6), 5, 8, false)
    if (prog == null || prog.size !in 2..40) throw WalletError("неверный адрес")
    if (ver == 0 && prog.size != 20 && prog.size != 32) throw WalletError("неверный адрес")
    if (pearl) {
        if ((ver != 1 && ver != 2) || prog.size != 32)
            throw WalletError("Pearl поддерживает только адреса prl1p... (Taproot) и P2MR")
    } else if (!(ver == 0 || (ver == 1 && prog.size == 32))) {
        throw WalletError("устройство пока понимает только v0 и taproot")
    }
    return byteArrayOf((if (ver != 0) 0x50 + ver else 0).toByte(), prog.size.toByte()) +
        ByteArray(prog.size) { prog[it].toByte() }
}

// ---------------- TRON (base58check) и EVM (EIP-55) ----------------

private const val B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

/** T... -> 21 байт (0x41 + 20). */
fun tronDecode(addr: String): ByteArray {
    var n = BigInteger.ZERO
    val b58 = BigInteger.valueOf(58)
    for (c in addr) {
        val i = B58.indexOf(c)
        if (i < 0) throw WalletError("неверный TRON-адрес")
        n = n.multiply(b58).add(BigInteger.valueOf(i.toLong()))
    }
    val raw = if (n.bitLength() <= 200) n.toBytes(25) else ByteArray(0)
    if (raw.size != 25 || raw[0] != 0x41.toByte()) throw WalletError("неверный TRON-адрес (ожидается T...)")
    val body = raw.copyOfRange(0, 21)
    if (!dsha(body).copyOfRange(0, 4).contentEquals(raw.copyOfRange(21, 25)))
        throw WalletError("неверная контрольная сумма TRON-адреса")
    return body
}

fun evmChecksum(a20: ByteArray): String {
    val h = a20.hex()
    val kh = keccak256(h.toByteArray()).hex()
    return "0x" + h.mapIndexed { i, c -> if (Character.digit(kh[i], 16) >= 8) c.uppercaseChar() else c }
        .joinToString("")
}

/** 0x... -> 20 байт. Адрес в смешанном регистре проверяется по EIP-55. */
fun evmDecode(addr: String): ByteArray {
    if (!(addr.startsWith("0x") && addr.length == 42)) throw WalletError("неверный BSC-адрес (ожидается 0x + 40 hex)")
    val a20 = try {
        addr.substring(2).unhex()
    } catch (e: WalletError) {
        throw WalletError("неверный BSC-адрес")
    }
    val body = addr.substring(2)
    if (body != body.lowercase() && body != body.uppercase() && evmChecksum(a20) != addr)
        throw WalletError("неверная контрольная сумма EIP-55 — опечатка в адресе?")
    return a20
}

// ---------------- RLP и protobuf varint ----------------

private fun rlpLength(n: Int, off: Int): ByteArray {
    if (n < 56) return byteArrayOf((off + n).toByte())
    val b = BigInteger.valueOf(n.toLong()).unsignedBytes()
    return byteArrayOf((off + 55 + b.size).toByte()) + b
}

fun rlp(x: Any): ByteArray = when (x) {
    is List<*> -> {
        val body = ByteArrayOutputStream().apply { x.forEach { write(rlp(it!!)) } }.toByteArray()
        rlpLength(body.size, 0xC0) + body
    }
    is Int -> rlp(BigInteger.valueOf(x.toLong()))
    is Long -> rlp(BigInteger.valueOf(x))
    is BigInteger -> rlp(x.unsignedBytes())
    is ByteArray -> if (x.size == 1 && (x[0].toInt() and 0xFF) < 0x80) x else rlpLength(x.size, 0x80) + x
    else -> throw IllegalArgumentException("rlp: ${x::class}")
}

fun varint(value: Int): ByteArray {
    var n = value
    val out = ByteArrayOutputStream()
    while (true) {
        val b = n and 0x7F
        n = n ushr 7
        if (n != 0) out.write(b or 0x80) else {
            out.write(b)
            return out.toByteArray()
        }
    }
}

const val TRON_MEMO_MAX = 96  // столько устройство показывает целиком (печатный текст)

/** raw_data + поле 10 (data) на своё место: нода пересобирает raw_data по порядку полей и
 *  считает txID по нему — поле не по порядку дало бы подпись не под тем хешем. */
fun tronWithMemo(raw: ByteArray, memo: ByteArray): ByteArray {
    val out = ByteArrayOutputStream()
    val field = byteArrayOf(0x52) + varint(memo.size) + memo
    var pos = 0
    var done = false
    fun readVarint(): Long {
        var v = 0L
        var shift = 0
        while (true) {
            if (pos >= raw.size || shift > 63) throw WalletError("битый protobuf")
            val c = raw[pos++].toInt() and 0xFF
            v = v or ((c and 0x7F).toLong() shl shift)
            shift += 7
            if (c and 0x80 == 0) return v
        }
    }
    while (pos < raw.size) {
        val start = pos
        val key = readVarint()
        val num = key ushr 3
        when ((key and 7).toInt()) {
            0 -> readVarint()
            2 -> {
                val n = readVarint().toInt()  // не «pos += readVarint()»: pos прочитался бы до вызова
                pos += n
            }
            else -> throw WalletError("неожиданное поле в raw_data")
        }
        if (num == 10L) throw WalletError("в транзакции уже есть memo")
        if (num > 10 && !done) {
            out.write(field)
            done = true
        }
        out.write(raw, start, minOf(pos, raw.size) - start)
    }
    if (!done) out.write(field)
    return out.toByteArray()
}

// ---------------- для показа ----------------

fun shortAddr(addr: String, n: Int = 10): String =
    if (addr.length <= 2 * n + 1) addr else "${addr.take(n)}…${addr.takeLast(n - 2)}"

/** Адрес группами по 4 символа — так его легче сверять с экраном устройства. */
fun groupAddr(addr: String): String = addr.chunked(4).joinToString(" ")
