// TON: ячейки, BoC, адреса и сообщения кошелька W5 (v5R1) — то же, что host/ton.py.
// Подписывает устройство (apps/ton): оно разбирает подписываемую ячейку и подписывает её хеш.
package io.microwallet.app.core

import java.io.ByteArrayOutputStream
import java.math.BigInteger
import java.security.MessageDigest
import java.util.Base64

object Ton {
    /** Код W5R1 (@ton/ton WalletContractV5R1) — нужен в StateInit первой отправки. */
    private const val W5_CODE_BOC =
        "b5ee9c7241021401000281000114ff00f4a413f4bcf2c80b01020120020d020148030402dcd020d749c120915b8f6320d70b1f20" +
            "82106578746ebd21821073696e74bdb0925f03e082106578746eba8eb48020d72101d074d721fa4030fa44f828fa443058bd915b" +
            "e0ed44d0810141d721f4058307f40e6fa1319130e18040d721707fdb3ce03120d749810280b99130e070e2100f02012005" +
            "0c020120060902016e07080019adce76a2684020eb90eb85ffc00019af1df6a2684010eb90eb858fc00201480a0b0017b325" +
            "fb51341c75c875c2c7e00011b262fb513435c280200019be5f0f6a2684080a0eb90fa02c0102f20e011e20d70b1f82107369" +
            "676ebaf2e08a7f0f01e68ef0eda2edfb218308d722028308d723208020d721d31fd31fd31fed44d0d200d31f20d31fd3ffd7" +
            "0a000af90140ccf9109a28945f0adb31e1f2c087df02b35007b0f2d0845125baf2e0855036baf2e086f823bbf2d0882292f8" +
            "00de01a47fc8ca00cb1f01cf16c9ed542092f80fde70db3cd81003f6eda2edfb02f404216e926c218e4c0221d73930709421" +
            "c700b38e2d01d72820761e436c20d749c008f2e09320d74ac002f2e09320d71d06c712c2005230b0f2d089d74cd7393001a4" +
            "e86c128407bbf2e093d74ac000f2e093ed55e2d20001c000915be0ebd72c08142091709601d72c081c12e25210b1e30f20d7" +
            "4a111213009601fa4001fa44f828fa443058baf2e091ed44d0810141d718f405049d7fc8ca0040048307f453f2e08b8e1403" +
            "8307f45bf2e08c22d70a00216e01b3b0f2d090e2c85003cf1612f400c9ed54007230d72c08248e2d21f2e092d200ed44d0d2" +
            "005113baf2d08f54503091319c01810140d721d70a00f2e08ee2c8ca0058cf16c9ed5493f2c08de20010935bdb31e1d74cd0" +
            "b4d6c35e"
    /** USDT: код кошелька жетона — library-ячейка со ссылкой на этот хеш. */
    private const val USDT_WALLET_CODE = "8f452d7a4dfd74066b682365177259ed05734435be76b5fd4bd5d8af2b7c3d68"

    private const val OP_SIGNED_EXTERNAL = 0x7369676EL
    private const val OP_SEND_MSG = 0x0EC3C86DL
    private const val OP_JETTON_TRANSFER = 0x0F8A7EA5L
    private const val MODE = 3L  // комиссия отдельно + игнорировать ошибки
    const val COMMENT_MAX = 96
    const val USDT_ATTACH = 50_000_000L  // 0.05 GRAM на пересылку жетона, лишнее вернётся

    /** Адрес: workchain и хеш. */
    data class Addr(val wc: Int, val hash: ByteArray) {
        override fun equals(other: Any?) = other is Addr && wc == other.wc && hash.contentEquals(other.hash)
        override fun hashCode() = hash.contentHashCode()
    }

    class Cell(val bits: String = "", val refs: List<Cell> = emptyList(), val exotic: Boolean = false) {
        init {
            if (bits.length > 1023 || refs.size > 4) throw WalletError("ячейка TON переполнена")
        }

        internal fun data(): ByteArray {
            var b = bits
            if (b.length % 8 != 0) b += "1" + "0".repeat(7 - b.length % 8)
            return ByteArray(b.length / 8) { b.substring(it * 8, it * 8 + 8).toInt(2).toByte() }
        }

        internal fun d1d2() = byteArrayOf((refs.size + if (exotic) 8 else 0).toByte(), (bits.length / 8 + (bits.length + 7) / 8).toByte())

        val depth: Int by lazy { if (refs.isEmpty()) 0 else 1 + refs.maxOf { it.depth } }

        val hash: ByteArray by lazy {
            val md = MessageDigest.getInstance("SHA-256")
            md.update(d1d2())
            md.update(data())
            for (r in refs) md.update(byteArrayOf((r.depth shr 8).toByte(), r.depth.toByte()))
            for (r in refs) md.update(r.hash)
            md.digest()
        }
    }

    class Builder {
        val bits = StringBuilder()
        val refs = ArrayList<Cell>()

        fun uint(v: Long, n: Int) = uint(BigInteger.valueOf(v), n)
        fun uint(v: BigInteger, n: Int): Builder {
            if (v.signum() < 0 || v.bitLength() > n) throw WalletError("число не влезает в ячейку TON")
            if (n > 0) bits.append(v.toString(2).padStart(n, '0'))
            return this
        }
        fun bytes(b: ByteArray) = if (b.isEmpty()) this else uint(BigInteger(1, b), 8 * b.size)
        fun coins(v: BigInteger): Builder {
            val n = (v.bitLength() + 7) / 8
            return uint(n.toLong(), 4).uint(v, 8 * n)
        }
        fun coins(v: Long) = coins(BigInteger.valueOf(v))
        fun addr(a: Addr?): Builder =
            if (a == null) uint(0, 2) else uint(2, 2).uint(0, 1).uint((a.wc and 0xFF).toLong(), 8).bytes(a.hash)
        fun ref(c: Cell): Builder { refs += c; return this }
        fun maybeRef(c: Cell?) = if (c != null) uint(1, 1).ref(c) else uint(0, 1)
        fun end() = Cell(bits.toString(), refs.toList())
    }

    fun commentCell(text: String) = Builder().uint(0, 32).bytes(text.toByteArray()).end()

    // ---------------- BoC ----------------

    private fun crc32c(data: ByteArray): Long {
        var c = 0xFFFFFFFFL
        for (x in data) {
            c = c xor (x.toLong() and 0xFF)
            repeat(8) { c = if (c and 1L != 0L) (c ushr 1) xor 0x82F63B78L else c ushr 1 }
        }
        return c xor 0xFFFFFFFFL
    }

    private fun be(v: Long, n: Int) = ByteArray(n) { (v shr (8 * (n - 1 - it))).toByte() }

    /** Один корень, без индекса, с CRC32C. Порядок — по убыванию глубины: ссылки только вперёд. */
    fun toBoc(root: Cell): ByteArray {
        val seen = HashSet<String>()
        val post = ArrayList<Cell>()  // как в ton.py: обход в глубину, ячейка — после потомков
        fun visit(c: Cell) {
            if (!seen.add(c.hash.hex())) return
            c.refs.forEach { visit(it) }
            post += c
        }
        visit(root)
        val order = post.reversed().sortedByDescending { it.depth }
        val idx = order.withIndex().associate { it.value.hash.hex() to it.index }
        val n = order.size
        val s = maxOf(1, (32 - Integer.numberOfLeadingZeros(n) + 7) / 8)
        val body = ByteArrayOutputStream()
        for (c in order) {
            body.write(c.d1d2())
            body.write(c.data())
            for (r in c.refs) body.write(be(idx.getValue(r.hash.hex()).toLong(), s))
        }
        val bb = body.toByteArray()
        val off = maxOf(1, (64 - java.lang.Long.numberOfLeadingZeros(bb.size.toLong()) + 7) / 8)
        val head = ByteArrayOutputStream()
        head.write(byteArrayOf(0xb5.toByte(), 0xee.toByte(), 0x9c.toByte(), 0x72, (0x40 or s).toByte(), off.toByte()))
        head.write(be(n.toLong(), s)); head.write(be(1, s)); head.write(be(0, s))
        head.write(be(bb.size.toLong(), off)); head.write(be(0, s))
        head.write(bb)
        val data = head.toByteArray()
        val crc = crc32c(data)
        return data + ByteArray(4) { (crc shr (8 * it)).toByte() }
    }

    /** BoC с одним корнем -> Cell (код W5 для StateInit). */
    fun fromBoc(b: ByteArray): Cell {
        fun num(p: Int, n: Int): Int { var v = 0; for (i in 0 until n) v = (v shl 8) or (b[p + i].toInt() and 0xFF); return v }
        if (num(0, 4) != 0xb5ee9c72.toInt()) throw WalletError("не BoC")
        val flags = b[4].toInt() and 0xFF
        val off = b[5].toInt() and 0xFF
        val s = flags and 7
        var p = 6
        val n = num(p, s); val roots = num(p + s, s)
        p += 3 * s + off
        val root = num(p, s)
        p += s * roots
        if (flags and 0x80 != 0) p += n * off
        class Raw(val bits: String, val refs: List<Int>, val exotic: Boolean)
        val raw = ArrayList<Raw>()
        repeat(n) {
            val d1 = b[p].toInt() and 0xFF; val d2 = b[p + 1].toInt() and 0xFF
            p += 2
            val ln = (d2 + 1) / 2
            var bits = (0 until ln).joinToString("") { i -> (b[p + i].toInt() and 0xFF).toString(2).padStart(8, '0') }
            p += ln
            if (d2 and 1 == 1) bits = bits.trimEnd('0').dropLast(1)
            val refs = (0 until (d1 and 7)).map { num(p + it * s, s) }
            p += s * (d1 and 7)
            raw += Raw(bits, refs, d1 and 8 != 0)
        }
        val cells = arrayOfNulls<Cell>(n)
        for (i in n - 1 downTo 0) cells[i] = Cell(raw[i].bits, raw[i].refs.map { cells[it]!! }, raw[i].exotic)
        return cells[root]!!
    }

    // ---------------- адреса ----------------

    private fun crc16(data: ByteArray): Int {
        var c = 0
        for (x in data) {
            c = c xor ((x.toInt() and 0xFF) shl 8)
            repeat(8) { c = if (c and 0x8000 != 0) ((c shl 1) xor 0x1021) and 0xFFFF else (c shl 1) and 0xFFFF }
        }
        return c
    }

    fun friendly(a: Addr, bounceable: Boolean, testnet: Boolean): String {
        val b = byteArrayOf(((if (bounceable) 0x11 else 0x51) or (if (testnet) 0x80 else 0)).toByte(), a.wc.toByte()) + a.hash
        val c = crc16(b)
        return Base64.getUrlEncoder().encodeToString(b + byteArrayOf((c shr 8).toByte(), c.toByte()))
    }

    /** «EQ…/UQ…» (base64 или base64url) или «0:hex» -> (адрес, bounceable). */
    fun parseAddress(sIn: String, testnet: Boolean): Pair<Addr, Boolean> {
        val s = sIn.trim()
        if (':' in s) {
            val (wc, h) = s.split(':', limit = 2)
            val hb = try { h.unhex() } catch (e: Exception) { throw WalletError("Неверный адрес TON") }
            if (wc != "0" || hb.size != 32) throw WalletError("Поддерживаются только адреса basechain (0:…)")
            return Addr(0, hb) to false
        }
        val b = try {
            Base64.getUrlDecoder().decode(s.replace('+', '-').replace('/', '_'))
        } catch (e: IllegalArgumentException) {
            throw WalletError("Неверный адрес TON")
        }
        if (s.length != 48 || b.size != 36 || crc16(b.copyOfRange(0, 34)) != (((b[34].toInt() and 0xFF) shl 8) or (b[35].toInt() and 0xFF)))
            throw WalletError("Неверный адрес TON: контрольная сумма")
        val tag = b[0].toInt() and 0xFF
        if ((tag and 0x7F) != 0x11 && (tag and 0x7F) != 0x51) throw WalletError("Неверный адрес TON")
        if ((tag and 0x80 != 0) != testnet) throw WalletError("Адрес для другой сети TON (testnet/mainnet)")
        if (b[1].toInt() != 0) throw WalletError("Поддерживаются только адреса basechain")
        return Addr(0, b.copyOfRange(2, 34)) to (tag and 0x40 == 0)
    }

    fun rawStr(a: Addr) = "${a.wc}:${a.hash.hex()}"

    // ---------------- кошелёк W5 ----------------

    fun walletId(testnet: Boolean): Long = ((if (testnet) -3L else -239L) xor 0x80000000L.unaryMinus()) and 0xFFFFFFFFL

    private fun stateInit(code: Cell, data: Cell) = Builder().uint(0, 2).maybeRef(code).maybeRef(data).uint(0, 1).end()

    fun w5Init(pubkey: ByteArray, testnet: Boolean): Cell {
        val data = Builder().uint(1, 1).uint(0, 32).uint(walletId(testnet), 32).bytes(pubkey).uint(0, 1).end()
        return stateInit(fromBoc(W5_CODE_BOC.unhex()), data)
    }

    fun w5Address(pubkey: ByteArray, testnet: Boolean) = Addr(0, w5Init(pubkey, testnet).hash)

    fun usdtWallet(owner: Addr, master: Addr): Addr {
        val lib = Cell("00000010" + USDT_WALLET_CODE.unhex().joinToString("") { (it.toInt() and 0xFF).toString(2).padStart(8, '0') }, exotic = true)
        val data = Builder().uint(0, 4).coins(0).addr(owner).addr(master).end()
        return Addr(0, stateInit(lib, data).hash)
    }

    /** MessageRelaxed: служебные поля нулевые; тело — в той же ячейке, если влезает (как в ton.py). */
    fun internal(dest: Addr, value: BigInteger, bounce: Boolean, body: Cell? = null): Cell {
        val b = Builder().uint(0, 1).uint(1, 1).uint(if (bounce) 1 else 0, 1).uint(0, 1).addr(null).addr(dest)
            .coins(value).uint(0, 1).coins(0).coins(0).uint(0, 64).uint(0, 32).uint(0, 1)
        if (body == null) return b.uint(0, 1).end()
        if (b.bits.length + 1 + body.bits.length <= 1023 && body.refs.size <= 4) {
            b.uint(0, 1).bits.append(body.bits)
            b.refs += body.refs
            return b.end()
        }
        return b.uint(1, 1).ref(body).end()
    }

    fun jettonTransfer(amount: BigInteger, dest: Addr, response: Addr, comment: String?): Cell {
        val b = Builder().uint(OP_JETTON_TRANSFER, 32).uint(0, 64).coins(amount).addr(dest).addr(response)
            .uint(0, 1).coins(if (comment != null) 1 else 0)
        return (if (comment != null) b.uint(1, 1).ref(commentCell(comment)) else b.uint(0, 1)).end()
    }

    /** То, что подписывает устройство: op «sign», wallet_id, срок, seqno, одно действие send_msg. */
    fun signingMessage(testnet: Boolean, seqno: Long, validUntil: Long, msg: Cell): Cell {
        val actions = Builder().ref(Cell()).uint(OP_SEND_MSG, 32).uint(MODE, 8).ref(msg).end()
        return Builder().uint(OP_SIGNED_EXTERNAL, 32).uint(walletId(testnet), 32).uint(validUntil, 32)
            .uint(seqno, 32).maybeRef(actions).uint(0, 1).end()
    }

    fun signedBody(signing: Cell, sig: ByteArray) =
        Cell(signing.bits + sig.joinToString("") { (it.toInt() and 0xFF).toString(2).padStart(8, '0') }, signing.refs)

    fun external(dest: Addr, body: Cell, init: Cell? = null): Cell {
        var b = Builder().uint(2, 2).addr(null).addr(dest).coins(0)
        b = if (init != null) b.uint(1, 1).uint(1, 1).ref(init) else b.uint(0, 1)
        return b.uint(1, 1).ref(body).end()
    }

    /** Нормализованный хеш внешнего сообщения (TEP-467) — по нему транзакцию находят обозреватели. */
    fun normalizedHash(dest: Addr, body: Cell) = external(dest, body).hash
}
