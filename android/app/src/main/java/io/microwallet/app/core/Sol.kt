// Solana: base58, сообщение перевода SOL (legacy), транзакция — то же, что host/sol.py.
// Подписывает устройство (apps/sol): оно разбирает сообщение и подписывает его целиком.
package io.microwallet.app.core

import java.io.ByteArrayOutputStream
import java.math.BigInteger

object Sol {
    private const val B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    private val SYSTEM = ByteArray(32)
    private val COMPUTE_BUDGET = "0306466fe5211732ffecadba72c39be7bc8ce5bbc5f7126b2c439b3a40000000".unhex()
    const val CU_LIMIT = 1000L  // перевод и две инструкции ComputeBudget — около 450 единиц
    private val TOKEN = "06ddf6e1d765a193d9cbe146ceeb79ac1cb485ed5f5b37913a8cf5857eff00a9".unhex()
    private val ATA_PROGRAM = "8c97258f4e2489f1bb3d1029148e0d830b5a1399daff1084048e7bd8dbe9f859".unhex()
    const val TOKEN_CU = 15000L         // TransferChecked — около 6 тыс. единиц
    const val TOKEN_CREATE_CU = 50000L  // + создание счёта получателя — около 25 тыс.
    const val TOKEN_ACCOUNT_RENT = 2039280L  // lamports: рента токен-счёта (165 байт)

    private val P = BigInteger.TWO.pow(255) - BigInteger.valueOf(19)
    private val D = (BigInteger.valueOf(-121665) * BigInteger.valueOf(121666).modInverse(P)).mod(P)

    /** Лежит ли 32 байта на кривой ed25519 (RFC 8032): адрес кошелька — да, PDA (ATA) — нет. */
    fun onCurve(b: ByteArray): Boolean {
        val y = BigInteger(1, b.reversedArray()).clearBit(255)
        if (y >= P) return false
        val y2 = y * y
        val x2 = ((y2 - BigInteger.ONE) * (D * y2 + BigInteger.ONE).modInverse(P)).mod(P)
        var x = x2.modPow((P + BigInteger.valueOf(3)).shiftRight(3), P)
        if ((x * x).mod(P) != x2) x = (x * BigInteger.TWO.modPow((P - BigInteger.ONE).shiftRight(2), P)).mod(P)
        if ((x * x).mod(P) != x2) return false
        return !(x.signum() == 0 && (b[31].toInt() and 0x80) != 0)
    }

    /** Счёт токена владельца (Associated Token Account) и его bump. */
    fun ata(owner: ByteArray, mint: ByteArray): Pair<ByteArray, Int> {
        for (bump in 255 downTo 0) {
            val h = java.security.MessageDigest.getInstance("SHA-256").digest(
                owner + TOKEN + mint + byteArrayOf(bump.toByte()) + ATA_PROGRAM + "ProgramDerivedAddress".toByteArray()
            )
            if (!onCurve(h)) return h to bump
        }
        throw WalletError("нет адреса PDA")
    }

    fun b58encode(b: ByteArray): String {
        var n = BigInteger(1, b)
        val sb = StringBuilder()
        val k = BigInteger.valueOf(58)
        while (n.signum() > 0) {
            val (q, r) = n.divideAndRemainder(k)
            sb.append(B58[r.toInt()])
            n = q
        }
        repeat(b.takeWhile { it == 0.toByte() }.size) { sb.append('1') }
        return sb.reverse().toString()
    }

    fun b58decode(s: String, size: Int = 32): ByteArray {
        var n = BigInteger.ZERO
        for (c in s) {
            val i = B58.indexOf(c)
            if (i < 0) throw WalletError("Неверный адрес Solana")
            n = n.multiply(BigInteger.valueOf(58)).add(BigInteger.valueOf(i.toLong()))
        }
        val zeros = s.length - s.trimStart('1').length
        val raw = ByteArray(zeros) + (if (n.signum() == 0) ByteArray(0) else n.unsignedBytes())
        if (raw.size != size) throw WalletError("Неверный адрес Solana")
        return raw
    }

    fun parseAddress(s: String): ByteArray {
        val t = s.trim()
        if (t.length !in 32..44) throw WalletError("Неверный адрес Solana")
        return b58decode(t)
    }

    private fun compact(nIn: Int): ByteArray {
        var n = nIn
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

    private fun le(v: Long, n: Int) = ByteArray(n) { (v ushr (8 * it)).toByte() }

    /** Сообщение (legacy) перевода lamports с payer на dest; cuPrice — микро-lamports за единицу. */
    fun transferMessage(payer: ByteArray, dest: ByteArray, lamports: Long, blockhash: ByteArray, cuPrice: Long): ByteArray {
        if (payer.contentEquals(dest)) throw WalletError("Это адрес самого кошелька")
        val keys = listOf(payer, dest, SYSTEM, COMPUTE_BUDGET)
        val ix = listOf(
            Triple(3, listOf<Int>(), byteArrayOf(2) + le(CU_LIMIT, 4)),
            Triple(3, listOf<Int>(), byteArrayOf(3) + le(cuPrice, 8)),
            Triple(2, listOf(0, 1), le(2, 4) + le(lamports, 8)),
        )
        val out = ByteArrayOutputStream()
        out.write(byteArrayOf(1, 0, 2))
        out.write(compact(keys.size))
        keys.forEach { out.write(it) }
        out.write(blockhash)
        out.write(compact(ix.size))
        for ((prog, acc, data) in ix) {
            out.write(prog)
            out.write(compact(acc.size))
            acc.forEach { out.write(it) }
            out.write(compact(data.size))
            out.write(data)
        }
        return out.toByteArray()
    }

    private fun message(keys: List<ByteArray>, roUnsigned: Int, blockhash: ByteArray, ix: List<Triple<Int, List<Int>, ByteArray>>): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(byteArrayOf(1, 0, roUnsigned.toByte()))
        out.write(compact(keys.size))
        keys.forEach { out.write(it) }
        out.write(blockhash)
        out.write(compact(ix.size))
        for ((prog, acc, data) in ix) {
            out.write(prog)
            out.write(compact(acc.size))
            acc.forEach { out.write(it) }
            out.write(compact(data.size))
            out.write(data)
        }
        return out.toByteArray()
    }

    class TokenMsg(val msg: ByteArray, val bump: Int, val limit: Long)

    /** Перевод токена владельцу owner (кошелёк), как token_message в host/sol.py. */
    fun tokenMessage(payer: ByteArray, owner: ByteArray, mint: ByteArray, decimals: Int, amount: Long,
                     blockhash: ByteArray, cuPrice: Long, create: Boolean): TokenMsg {
        if (payer.contentEquals(owner)) throw WalletError("Это адрес самого кошелька")
        val src = ata(payer, mint).first
        val (dst, bump) = ata(owner, mint)
        val keys = listOf(payer, dst, src) + (if (create) listOf(owner) else emptyList()) + listOf(mint) +
            (if (create) listOf(SYSTEM) else emptyList()) + listOf(TOKEN) +
            (if (create) listOf(ATA_PROGRAM) else emptyList()) + listOf(COMPUTE_BUDGET)
        fun k(key: ByteArray) = keys.indexOfFirst { it.contentEquals(key) }
        val limit = if (create) TOKEN_CREATE_CU else TOKEN_CU
        val ix = arrayListOf(
            Triple(k(COMPUTE_BUDGET), listOf<Int>(), byteArrayOf(2) + le(limit, 4)),
            Triple(k(COMPUTE_BUDGET), listOf<Int>(), byteArrayOf(3) + le(cuPrice, 8)),
        )
        if (create) ix += Triple(k(ATA_PROGRAM), listOf(0, k(dst), k(owner), k(mint), k(SYSTEM), k(TOKEN)), byteArrayOf(1))
        ix += Triple(k(TOKEN), listOf(k(src), k(mint), k(dst), 0), byteArrayOf(12) + le(amount, 8) + byteArrayOf(decimals.toByte()))
        return TokenMsg(message(keys, keys.size - 3, blockhash, ix), bump, limit)
    }

    /** Потолок комиссии, как его считает устройство: 5000 + цена × лимит / 1e6 (вверх). */
    fun maxFee(cuPrice: Long, limit: Long = CU_LIMIT): Long = 5000 + (cuPrice * limit + 999_999) / 1_000_000

    fun transaction(msg: ByteArray, sig: ByteArray) = compact(1) + sig + msg
}
