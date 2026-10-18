// PSBT (BIP174, версия 0) — то же, что host/psbt.py: разбор, сборка, финализация P2WPKH,
// xpub для дескриптора. Устройство PSBT не разбирает: кошелёк переводит его в обычный запрос
// (входы с путями, выходы, предыдущие транзакции), а подписи вставляет обратно. Поля, которые
// нам не нужны, сохраняются как есть и в том же порядке.
package io.microwallet.app.core

import java.io.ByteArrayOutputStream

class PsbtTx(
    val version: Int,
    val ins: List<PsbtIn>,
    val outs: List<PsbtOut>,
    val locktime: Long,
    val witness: List<List<ByteArray>> = emptyList(),
) {
    /** txid — по сериализации без witness, как в обозревателях (обратный порядок байт). */
    val txid: String get() = dsha256(serialize(false)).reversedArray().hex()

    fun serialize(withWitness: Boolean = true): ByteArray {
        val useWit = withWitness && witness.any { it.isNotEmpty() }
        val b = ByteArrayOutputStream()
        b.write(Buf().u32(version.toLong() and 0xFFFFFFFFL).toByteArray())
        if (useWit) b.write(byteArrayOf(0, 1))
        b.write(compact(ins.size.toLong()))
        for (i in ins) {
            b.write(i.txid.unhex().reversedArray())
            b.write(Buf().u32(i.vout).toByteArray())
            b.write(compact(i.scriptSig.size.toLong()))
            b.write(i.scriptSig)
            b.write(Buf().u32(i.sequence).toByteArray())
        }
        b.write(compact(outs.size.toLong()))
        for (o in outs) {
            b.write(Buf().u64(o.value).toByteArray())
            b.write(compact(o.script.size.toLong()))
            b.write(o.script)
        }
        if (useWit) for (stack in witness) {
            b.write(compact(stack.size.toLong()))
            for (x in stack) {
                b.write(compact(x.size.toLong()))
                b.write(x)
            }
        }
        b.write(Buf().u32(locktime).toByteArray())
        return b.toByteArray()
    }

    companion object {
        fun parse(raw: ByteArray): PsbtTx {
            val r = Reader(raw)
            val version = r.u32().toInt()
            val segwit = raw.size > 6 && raw[4] == 0.toByte() && raw[5] == 1.toByte()
            if (segwit) r.take(2)
            val ins = (0 until r.compact()).map {
                val prev = r.take(32)
                val vout = r.u32()
                val script = r.take(r.compact())
                PsbtIn(prev.reversedArray().hex(), vout, script, r.u32())
            }
            val outs = (0 until r.compact()).map {
                val value = r.u64()
                PsbtOut(value, r.take(r.compact()))
            }
            val wit = if (segwit) ins.map { (0 until r.compact()).map { r.take(r.compact()) } } else emptyList()
            val lock = r.u32()
            if (!r.done) throw WalletError("PSBT: лишние байты в транзакции")
            return PsbtTx(version, ins, outs, lock, wit)
        }
    }
}

class PsbtIn(val txid: String, val vout: Long, val scriptSig: ByteArray, val sequence: Long)
class PsbtOut(val value: Long, val script: ByteArray)

/** Ключ карты PSBT: тип (первый байт) + данные ключа. */
class PsbtKv(val key: ByteArray, val value: ByteArray) {
    val type: Int get() = key[0].toInt() and 0xFF
}

class Psbt(val glob: List<PsbtKv>, val inputs: MutableList<MutableList<PsbtKv>>, val outputs: List<List<PsbtKv>>) {
    val tx: PsbtTx = PsbtTx.parse(get(glob, G_UNSIGNED_TX) ?: throw WalletError("PSBT: нет неподписанной транзакции (нужна версия 0)"))

    init {
        if (tx.ins.any { it.scriptSig.isNotEmpty() } || tx.witness.any { it.isNotEmpty() })
            throw WalletError("PSBT: неподписанная транзакция содержит подписи")
        if (inputs.size != tx.ins.size || outputs.size != tx.outs.size)
            throw WalletError("PSBT: число карт не совпадает с транзакцией")
    }

    fun serialize(): ByteArray {
        val b = ByteArrayOutputStream()
        b.write(MAGIC)
        fun map(m: List<PsbtKv>) {
            for (kv in m) {
                b.write(compact(kv.key.size.toLong()))
                b.write(kv.key)
                b.write(compact(kv.value.size.toLong()))
                b.write(kv.value)
            }
            b.write(0)
        }
        map(glob)
        inputs.forEach { map(it) }
        outputs.forEach { map(it) }
        return b.toByteArray()
    }

    fun toBase64(): String = java.util.Base64.getEncoder().encodeToString(serialize())

    /** Подпись входа k (ключ 33 байта, DER + sighash). Прежняя подпись тем же ключом заменяется. */
    fun setPartialSig(k: Int, pub: ByteArray, sig: ByteArray) {
        val key = byteArrayOf(I_PARTIAL_SIG.toByte()) + pub
        inputs[k].removeAll { it.key.contentEquals(key) }
        inputs[k].add(PsbtKv(key, sig))
    }

    /** Свидетели [подпись, ключ] из partial_sig; лишние поля входа убираются. -> сырая транзакция hex. */
    fun finalizeP2wpkh(): String {
        val wit = inputs.mapIndexed { k, m ->
            val sigs = m.filter { it.type == I_PARTIAL_SIG }
            if (sigs.size != 1) throw WalletError("PSBT: вход #$k — нужна ровно одна подпись")
            val stack = listOf(sigs[0].value, sigs[0].key.copyOfRange(1, sigs[0].key.size))
            val keep = m.filter { it.type == I_NON_WITNESS_UTXO || it.type == I_WITNESS_UTXO }.toMutableList()
            val w = ByteArrayOutputStream()
            w.write(compact(2))
            for (x in stack) {
                w.write(compact(x.size.toLong()))
                w.write(x)
            }
            keep.add(PsbtKv(byteArrayOf(I_FINAL_SCRIPTWITNESS.toByte()), w.toByteArray()))
            inputs[k] = keep
            stack
        }
        return PsbtTx(tx.version, tx.ins, tx.outs, tx.locktime, wit).serialize().hex()
    }

    companion object {
        val MAGIC = byteArrayOf(0x70, 0x73, 0x62, 0x74, 0xFF.toByte())
        const val G_UNSIGNED_TX = 0x00
        const val I_NON_WITNESS_UTXO = 0x00
        const val I_WITNESS_UTXO = 0x01
        const val I_PARTIAL_SIG = 0x02
        const val I_SIGHASH_TYPE = 0x03
        const val I_BIP32_DERIVATION = 0x06
        const val I_FINAL_SCRIPTWITNESS = 0x08
        const val O_BIP32_DERIVATION = 0x02
        const val HARD = 0x80000000L

        /** Двоичный PSBT или base64 (как сохраняет Sparrow). */
        fun parse(data: ByteArray): Psbt {
            var b = data
            if (!b.startsWith(MAGIC)) {
                b = try {
                    java.util.Base64.getDecoder().decode(String(data).trim())
                } catch (e: IllegalArgumentException) {
                    throw WalletError("Это не PSBT (ни двоичный, ни base64)")
                }
            }
            if (!b.startsWith(MAGIC)) throw WalletError("Это не PSBT")
            val r = Reader(b)
            r.take(MAGIC.size)
            fun map(): MutableList<PsbtKv> {
                val m = ArrayList<PsbtKv>()
                while (true) {
                    val kl = r.compact()
                    if (kl == 0) return m
                    val k = r.take(kl)
                    m.add(PsbtKv(k, r.take(r.compact())))
                }
            }
            val glob = map()
            val raw = get(glob, G_UNSIGNED_TX) ?: throw WalletError("PSBT: нет неподписанной транзакции (нужна версия 0)")
            val t = PsbtTx.parse(raw)
            val ins = MutableList(t.ins.size) { map() }
            val outs = List(t.outs.size) { map() }
            if (!r.done) throw WalletError("PSBT: лишние байты")
            return Psbt(glob, ins, outs)
        }

        fun get(m: List<PsbtKv>, type: Int): ByteArray? = m.firstOrNull { it.type == type && it.key.size == 1 }?.value

        /** BIP32-пути карты: (ключ 33 байта, отпечаток hex, индексы). */
        fun derivations(m: List<PsbtKv>, type: Int): List<Triple<ByteArray, String, List<Long>>> =
            m.filter { it.type == type && it.key.size == 34 && it.value.size >= 4 && (it.value.size - 4) % 4 == 0 }.map { kv ->
                val r = Reader(kv.value)
                val fp = r.take(4).hex()
                Triple(kv.key.copyOfRange(1, 34), fp, (0 until (kv.value.size - 4) / 4).map { r.u32() })
            }
    }
}

private fun ByteArray.startsWith(p: ByteArray) = size >= p.size && copyOfRange(0, p.size).contentEquals(p)

fun dsha256(b: ByteArray): ByteArray = sha256(sha256(b))

fun compact(n: Long): ByteArray = when {
    n < 0xfd -> byteArrayOf(n.toByte())
    n <= 0xffff -> byteArrayOf(0xfd.toByte(), n.toByte(), (n ushr 8).toByte())
    n <= 0xffffffffL -> byteArrayOf(0xfe.toByte()) + Buf().u32(n).toByteArray()
    else -> byteArrayOf(0xff.toByte()) + Buf().u64(n).toByteArray()
}

private class Reader(val b: ByteArray) {
    var i = 0
    val done get() = i == b.size

    fun take(n: Int): ByteArray {
        if (n < 0 || i + n > b.size) throw WalletError("PSBT обрывается")
        return b.copyOfRange(i, i + n).also { i += n }
    }

    fun u32(): Long = take(4).let { (0..3).fold(0L) { acc, k -> acc or ((it[k].toLong() and 0xFF) shl (8 * k)) } }
    fun u64(): Long = take(8).let { (0..7).fold(0L) { acc, k -> acc or ((it[k].toLong() and 0xFF) shl (8 * k)) } }
    fun compact(): Int {
        val c = take(1)[0].toInt() and 0xFF
        return when (c) {
            0xfd -> take(2).let { (it[0].toInt() and 0xFF) or ((it[1].toInt() and 0xFF) shl 8) }
            0xfe -> u32().toInt()
            0xff -> u64().toInt()
            else -> c
        }
    }
}

// ---------------- xpub и дескриптор ----------------

private const val B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

private fun b58decode(s: String): ByteArray {
    var n = java.math.BigInteger.ZERO
    for (c in s) n = n * java.math.BigInteger.valueOf(58) + java.math.BigInteger.valueOf(B58.indexOf(c).toLong().also {
        if (it < 0) throw WalletError("base58: недопустимый символ")
    })
    var raw = n.toByteArray().let { if (it.size > 1 && it[0] == 0.toByte()) it.copyOfRange(1, it.size) else it }
    raw = ByteArray(s.takeWhile { it == '1' }.length) + raw
    if (!dsha256(raw.copyOfRange(0, raw.size - 4)).copyOfRange(0, 4).contentEquals(raw.copyOfRange(raw.size - 4, raw.size)))
        throw WalletError("base58: плохая контрольная сумма")
    return raw.copyOfRange(0, raw.size - 4)
}

private fun b58encode(data: ByteArray): String {
    val raw = data + dsha256(data).copyOfRange(0, 4)
    var n = java.math.BigInteger(1, raw)
    val sb = StringBuilder()
    val k58 = java.math.BigInteger.valueOf(58)
    while (n > java.math.BigInteger.ZERO) {
        val qr = n.divideAndRemainder(k58)
        sb.append(B58[qr[1].toInt()])
        n = qr[0]
    }
    repeat(raw.takeWhile { it == 0.toByte() }.size) { sb.append('1') }
    return sb.reverse().toString()
}

/** zpub/vpub (SLIP-132) -> xpub/tpub: дескриптор сам задаёт тип скрипта. */
fun plainXpub(x: String): String {
    val raw = b58decode(x)
    val v = mapOf("04b24746" to "0488b21e", "045f1cf6" to "043587cf")[raw.copyOfRange(0, 4).hex()] ?: return x
    return b58encode(v.unhex() + raw.copyOfRange(4, raw.size))
}

private const val INPUT_CHARSET = "0123456789()[],'/*abcdefgh@:\$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~ijklmnopqrstuvwxyzABCDEFGH`#\"\\ "
private const val CHECKSUM_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"

/** Контрольная сумма дескриптора (BIP380): s + "#" + 8 символов. */
fun descsum(s: String): String {
    val gen = longArrayOf(0xf5dee51989, 0xa9fdca3312, 0x1bab10e32d, 0x3706b1677a, 0x644d626ffd)
    fun polymod(c0: Long, v: Int): Long {
        val top = c0 ushr 35
        var c = ((c0 and 0x7ffffffffL) shl 5) xor v.toLong()
        for (i in 0 until 5) if ((top ushr i) and 1L == 1L) c = c xor gen[i]
        return c
    }
    var c = 1L
    var cls = 0
    var count = 0
    for (ch in s) {
        val pos = INPUT_CHARSET.indexOf(ch)
        if (pos < 0) throw WalletError("дескриптор: недопустимый символ")
        c = polymod(c, pos and 31)
        cls = cls * 3 + (pos shr 5)
        if (++count == 3) {
            c = polymod(c, cls)
            cls = 0
            count = 0
        }
    }
    if (count > 0) c = polymod(c, cls)
    repeat(8) { c = polymod(c, 0) }
    c = c xor 1L
    return s + "#" + (0 until 8).map { CHECKSUM_CHARSET[((c ushr (5 * (7 - it))) and 31).toInt()] }.joinToString("")
}
