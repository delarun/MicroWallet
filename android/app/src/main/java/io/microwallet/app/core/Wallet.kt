// Кошелёк: балансы и сборка транзакций. Перенос класса Wallet из host/wallet.py.
// Телефон НЕ знает приватных ключей: он собирает транзакцию, а устройство само
// показывает получателя/сумму/комиссию и подписывает только после кнопки.
package io.microwallet.app.core

import org.json.JSONArray
import org.json.JSONObject
import java.math.BigDecimal
import java.math.BigInteger
import java.math.RoundingMode
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.ceil

data class Asset(val id: String, val symbol: String, val decimals: Int, val amount: BigInteger)

data class Utxo(
    val txid: String, val vout: Int, val value: Long, val confirmed: Boolean,
    val chain: Int, val index: Int, val address: String, val confirmations: Int = 0,
)

data class Balance(
    val address: String,
    val assets: List<Asset>,
    val pending: BigInteger = BigInteger.ZERO,  // Pearl: награды за блок, ещё не созрели
    val notes: List<String> = emptyList(),
    val utxos: List<Utxo> = emptyList(),
    val immature: List<Utxo> = emptyList(),
)

data class SendRequest(
    val coin: String,
    val dest: String,
    val amount: String,                 // в целых монетах, "12.5"
    val asset: String? = null,
    val index: Int = 0,
    val feerate: BigDecimal? = null,    // BTC/PRL: sat/vB
    val feeLimit: String? = null,       // TRON USDT: TRX; null — 40 или оценка + 20 %
    val memo: String? = null,           // TRON: заметка к переводу
    val gasPrice: String? = null,       // BSC: gwei
)

/** Собранная, но ещё не подписанная транзакция: что показать пользователю. */
class Draft(
    val coin: String,
    val asset: String,
    val symbol: String,
    val fromAddr: String,
    val to: String,
    val amount: String,
    val fee: String,
    val feeSymbol: String,
    val feeLabel: String = "Комиссия",
    val notes: List<String> = emptyList(),
    val sign: () -> Pair<String, String>,   // -> (raw hex, txid); спрашивает устройство
    val broadcast: (String) -> Unit,
    val txUrl: String = "",
)

data class SendResult(val raw: String, val txid: String, val url: String, val broadcast: Boolean)

/** Приложение WebAssembly на устройстве. */
data class AppInfo(val id: String, val version: String, val name: String)

/** То, что нужно интерфейсу. Реализации: Wallet (устройство + сеть) и DemoWallet. */
interface WalletApi {
    val info: Map<String, String>
    val net: String
    val coins: List<String>
    val ready: Boolean
    /** Ждёт PIN на устройстве (после включения): ключей и монет пока нет. */
    val locked: Boolean get() = info["state"] == "locked"
    val label: String
    val cfg: NetConfig get() = NETS.getValue(net)
    fun symbol(asset: String): String = cfg.symbol(asset)
    fun address(coin: String, index: Int = 0, chain: Int = 0): String
    fun showAddress(coin: String, index: Int = 0): String
    /** skip — токены, баланс которых не запрашивать (убраны с главной). */
    fun balance(coin: String, skip: Set<String> = emptySet()): Balance
    fun feeLevels(coin: String): Map<String, BigDecimal>
    fun prepareSend(req: SendRequest): Draft
    fun signAndSend(d: Draft, broadcast: Boolean = true): SendResult {
        val (raw, txid) = d.sign()
        if (broadcast) d.broadcast(raw)
        val url = if ("{txid}" in d.txUrl) d.txUrl.replace("{txid}", txid) else d.txUrl + txid
        return SendResult(raw, txid, if (broadcast) url else "", broadcast)
    }
    fun restore(phrase: String): Boolean
    /** Приложения на устройстве (пустой список у прошивок без них). */
    fun apps(): List<AppInfo> = emptyList()
    /** Установить или обновить приложение: устройство покажет имя, пути ключей и хеш. */
    fun installApp(app: WasmApp, onProgress: (Int, Int) -> Unit) {
        throw WalletError("Приложения недоступны")
    }
    fun removeApp(id: String) {
        throw WalletError("Приложения недоступны")
    }
    /** Покажет ли Pearl Wallet с той же фразой те же адреса PRL (он выводит ключи btcd-функцией
     *  DeriveNonStandard, у ~0.8 % фраз адреса другие). Ключи не раскрываются — один бит. */
    fun pearlWalletSame(): Boolean = throw WalletError("Нужно устройство")
    /** Watch-only кошелёк для Sparrow/Electrum: дескриптор wpkh с отпечатком, путём и xpub (BIP380). */
    fun descriptor(): String = throw WalletError("Нужно устройство с приложением Bitcoin")
    /** PSBT (двоичный или base64) -> (подписанный PSBT base64, сырая транзакция при finalize). */
    fun signPsbt(data: ByteArray, finalize: Boolean = false): Pair<String, String?> =
        throw WalletError("Нужно устройство с приложением Bitcoin")
    /** Отправить готовую BTC-транзакцию -> txid. */
    fun broadcastBtc(raw: String): String = throw WalletError("Нужно устройство с приложением Bitcoin")
    /** Порядок приложений на главном экране устройства (прошивки с v0.7). */
    fun setAppOrder(ids: List<String>) {
        throw WalletError("Приложения недоступны")
    }
    /** Обновить прошивку ESP32 (по USB или BLE). Устройство попросит подтверждение. */
    fun updateFirmware(img: FwImage, onProgress: (Int, Int) -> Unit) {
        throw WalletError("Обновление прошивки недоступно")
    }
    fun refreshInfo(): Map<String, String> = info
    /** Сколько адресов для получения — настройка устройства «Addresses» (info addrs=):
     *  столько показывает «Получить» и столько проверяется для баланса. */
    val addrCount: Int get() = info["addrs"]?.toIntOrNull()?.coerceIn(1, 20) ?: ADDRS_DEFAULT
    /** Сбросить кэш балансов (обновление вручную). */
    fun forgetCache() {}
    fun setAddrCount(n: Int) {
        throw WalletError("Настройка недоступна")
    }
    fun close()
}

// ---------------- UTXO-транзакции (BTC, Pearl) ----------------

class TxIn(val txid: String, val vout: Int, val value: Long, val chain: Int, val index: Int, val sequence: Long)
class TxOut(val value: Long, val script: ByteArray, val change: Boolean, val changeIndex: Int = 0)
class UtxoTx(val ins: List<TxIn>, val outs: List<TxOut>, val version: Long = 2, val locktime: Long = 0)

fun vsizeEstimate(nIn: Int, outScripts: List<ByteArray>, inVbytes: Double): Long =
    // P2WPKH-вход ~68 vB, P2TR-вход ~57.5 vB, выход = 8 + 1 + len(script), заголовок ~11 vB
    ceil(11 + inVbytes * nIn + outScripts.sumOf { 9 + it.size }).toLong()

/** Запрос на подпись для прошивки (формат src/tx.c). */
/** zlib (уровень 9) — для сжатого обновления прошивки (ota zbegin). */
fun deflate(data: ByteArray): ByteArray {
    val d = java.util.zip.Deflater(9)
    d.setInput(data)
    d.finish()
    val out = java.io.ByteArrayOutputStream(data.size / 2)
    val buf = ByteArray(64 * 1024)
    while (!d.finished()) out.write(buf, 0, d.deflate(buf))
    d.end()
    return out.toByteArray()
}

/** Запрос на подпись блоком (sign @): запрос, затем для BTC — предыдущие транзакции входов
 *  целиком (u32 длина + байты): по ним устройство само проверяет суммы входов. */
fun signBlob(tx: UtxoTx, prev: List<ByteArray> = emptyList()): ByteArray {
    val b = java.io.ByteArrayOutputStream()
    b.write(buildRequest(tx).unhex())
    for (raw in prev) {
        b.write(Buf().u32(raw.size.toLong()).bytes(raw).toByteArray())
    }
    return b.toByteArray()
}

fun buildRequest(tx: UtxoTx): String {
    val b = Buf().u32(tx.version).u32(tx.locktime).u8(tx.ins.size)
    for (i in tx.ins) {
        b.bytes(i.txid.unhex().reversedArray())
        b.u32(i.vout.toLong()).u32(i.sequence).u64(i.value).u8(i.chain).u32(i.index.toLong())
    }
    b.u8(tx.outs.size)
    for (o in tx.outs) {
        b.u64(o.value).u8(o.script.size).bytes(o.script)
        b.u8(if (o.change) 1 else 0).u32(o.changeIndex.toLong())
    }
    return b.toByteArray().hex()
}

/** witnesses: по стеку элементов на вход ([sig, pub] для P2WPKH, [sig] для P2TR). -> (raw, txid) */
fun serialize(tx: UtxoTx, witnesses: List<List<ByteArray>>): Pair<String, String> {
    fun body(withWitness: Boolean): ByteArray {
        val b = Buf().u32(tx.version)
        if (withWitness) b.u8(0).u8(1)
        b.u8(tx.ins.size)
        for (i in tx.ins) {
            b.bytes(i.txid.unhex().reversedArray()).u32(i.vout.toLong())
            b.u8(0).u32(i.sequence)
        }
        b.u8(tx.outs.size)
        for (o in tx.outs) b.u64(o.value).u8(o.script.size).bytes(o.script)
        if (withWitness) for (stack in witnesses) {
            b.u8(stack.size)
            for (item in stack) b.u8(item.size).bytes(item)
        }
        return b.u32(tx.locktime).toByteArray()
    }
    val txid = dsha(body(false)).reversedArray().hex()
    return body(true).hex() to txid
}

// ---------------- кошелёк ----------------

class Wallet(
    private val dev: Device,
    private val http: Http,
    private val wordlist: List<String> = emptyList(),
) : WalletApi {
    /** Срок подписанного сообщения TON: 10 минут на подтверждение (тесты подменяют). */
    var tonValidUntil: () -> Long = { System.currentTimeMillis() / 1000 + 600 }
    override var info: Map<String, String> = dev.info()
        private set
    override val net: String = info["net"] ?: "testnet"
    /** До v0.6 монеты были встроены в прошивку и команды шли без "app <id>". */
    private val legacy = (info["fw"] ?: "0.6").split('.').let { (it.getOrNull(0)?.toIntOrNull() ?: 0) * 100 + (it.getOrNull(1)?.toIntOrNull() ?: 0) } < 6
    private val builtin = if (legacy) (info["coins"] ?: "btc").split(',') else emptyList()
    private var installed: List<AppInfo> = loadApps()
    /** Встроенные монеты и монеты из установленных приложений (Ethereum). */
    override var coins: List<String> = coinsOf(installed)
        private set
    override val ready: Boolean get() = info["state"] == "ready"
    override val label: String get() = dev.label
    // chainId BSC — по сети сборки (как и в приложении bsc на устройстве)
    private val chainId = if (net == "mainnet") 56L else 97L

    /** Команда монеты: "app btc addr 0 1"; у старых прошивок — "addr 0 1", "tron …". */
    private fun cmd(coin: String, rest: String) =
        if (legacy && coin in builtin) (if (coin == "btc") rest else "$coin $rest") else "app $coin $rest"

    private fun loadApps(): List<AppInfo> = try {
        dev.call("apps").split(';').filter { it.isNotBlank() }.mapNotNull {
            val p = it.split('|', limit = 3)
            if (p.size == 3) AppInfo(p[0], p[1], p[2]) else null
        }
    } catch (e: DeviceError) {
        emptyList()  // прошивка без приложений
    }

    private fun coinsOf(apps: List<AppInfo>) =
        COINS.map { it.id }.filter { it in builtin || apps.any { a -> a.id == it } }

    override fun apps(): List<AppInfo> = loadApps().also {
        installed = it
        coins = coinsOf(it)
    }

    override fun installApp(app: WasmApp, onProgress: (Int, Int) -> Unit) {
        val b = app.bytes
        dev.call("app add ${b.size} ${app.sha256}")
        var off = 0
        while (off < b.size) {
            val end = minOf(b.size, off + 2048)
            dev.call("app add b64 " + java.util.Base64.getEncoder().encodeToString(b.copyOfRange(off, end)))
            off = end
            onProgress(off, b.size)
        }
        dev.call("app add end")  // экран проверки на устройстве
        apps()
    }

    override fun removeApp(id: String) {
        dev.call("app rm $id")
        addrCache.keys.removeAll { it.startsWith("$id/") }
        apps()
    }

    override fun pearlWalletSame(): Boolean {
        val c = if (net == "mainnet") 808276 else 1
        return try {
            dev.call("btcd 86h ${c}h 0h") == "same"
        } catch (e: WalletError) {
            if ("unknown command" in (e.message ?: "")) throw WalletError("Проверка есть в прошивке 0.7.3 и новее — обнови устройство")
            throw e
        }
    }

    // ---- PSBT (Sparrow, Electrum): то же, что host/wallet.py sign_psbt ----

    override fun descriptor(): String {
        need("btc")
        val c = if (net == "mainnet") 0 else 1
        val x = plainXpub(dev.call(cmd("btc", "xpub")))
        return descsum("wpkh([${dev.call("fingerprint")}/84h/${c}h/0h]$x/<0;1>/*)")
    }

    override fun broadcastBtc(raw: String): String = http.postRaw(cfg.btcApi + "/tx", raw).trim()

    /**
     * Кошелёк лишь переводит PSBT в запрос устройства: получателей, сдачу (её адрес устройство
     * выводит само) и суммы входов (по предыдущим транзакциям) проверяет устройство.
     * Входы — только свои P2WPKH m/84'/c'/0'/{0,1}/i, только SIGHASH_ALL.
     */
    override fun signPsbt(data: ByteArray, finalize: Boolean): Pair<String, String?> {
        need("btc")
        val p = Psbt.parse(data)
        val fp = dev.call("fingerprint")
        val c = if (net == "mainnet") 0L else 1L
        val acct = listOf(84L or Psbt.HARD, c or Psbt.HARD, Psbt.HARD)
        fun ours(m: List<PsbtKv>, type: Int) = Psbt.derivations(m, type).firstOrNull { (_, f, path) ->
            f == fp && path.size == 5 && path.subList(0, 3) == acct && path[3] in 0L..1L
        }
        val ins = ArrayList<TxIn>()
        val prev = ArrayList<ByteArray>()
        val pubs = ArrayList<ByteArray>()
        p.tx.ins.forEachIndexed { k, ti ->
            val m = p.inputs[k]
            val d = ours(m, Psbt.I_BIP32_DERIVATION) ?: throw WalletError(
                "Вход #$k не из этого кошелька: нет пути m/84'/$c'/0'/… с отпечатком $fp (мультиподпись и чужие входы не поддерживаются)")
            Psbt.get(m, Psbt.I_SIGHASH_TYPE)?.let { st ->
                if (st.size != 4 || st[0] != 1.toByte() || st[1] != 0.toByte() || st[2] != 0.toByte() || st[3] != 0.toByte())
                    throw WalletError("Вход #$k: устройство подписывает только SIGHASH_ALL")
            }
            val pt = PsbtTx.parse(Psbt.get(m, Psbt.I_NON_WITNESS_UTXO) ?: prevTx(ti.txid))
            if (pt.txid != ti.txid || ti.vout >= pt.outs.size) throw WalletError("Вход #$k: предыдущая транзакция в PSBT не та")
            ins.add(TxIn(ti.txid, ti.vout.toInt(), pt.outs[ti.vout.toInt()].value, d.third[3].toInt(), d.third[4].toInt(), ti.sequence))
            prev.add(PsbtTx(pt.version, pt.ins, pt.outs, pt.locktime).serialize(false))  // без witness: txid тот же
            pubs.add(d.first)
        }
        val outs = p.tx.outs.mapIndexed { k, o ->
            val d = ours(p.outputs[k], Psbt.O_BIP32_DERIVATION)
            if (d != null && d.third[3] == 1L) TxOut(o.value, o.script, true, d.third[4].toInt())  // сдачу сверит устройство
            else TxOut(o.value, o.script, false)
        }
        val tx = UtxoTx(ins = ins, outs = outs, version = p.tx.version.toLong(), locktime = p.tx.locktime)
        val parts = dev.appCall(cmd("btc", "sign"), signBlob(tx, prev)).split(' ').filter { it.isNotEmpty() }
        if (parts.size != ins.size) throw WalletError("Устройство вернуло не столько подписей, сколько входов")
        parts.forEachIndexed { k, part ->
            val (sig, pub) = part.split(':').map { it.unhex() }
            if (!pub.contentEquals(pubs[k])) throw WalletError("Вход #$k: устройство подписало другим ключом")
            p.setPartialSig(k, pub, sig)
        }
        val raw = if (finalize) p.finalizeP2wpkh() else null
        return p.toBase64() to raw
    }

    override fun setAppOrder(ids: List<String>) {
        try {
            dev.call("apps order " + ids.joinToString(","))
        } catch (e: DeviceError) {
            if (e.message == "unknown command") throw WalletError("Нужна прошивка новее: обнови устройство")
            throw e
        }
        apps()
    }

    // Адреса детерминированы — по BLE каждый запрос стоит времени, кэшируем
    private val addrCache = ConcurrentHashMap<String, String>()

    private fun need(coin: String) {
        if (locked) throw WalletError(LOCKED_MSG)
        if (coin !in coins) throw WalletError("На устройстве нет приложения ${coinInfo(coin).name} — установи его")
        if (!ready) throw WalletError("На устройстве нет кошелька — создай его на самом устройстве")
    }

    override fun refreshInfo(): Map<String, String> {
        val wasLocked = locked
        info = dev.info()
        if (wasLocked && !locked) {  // PIN введён: теперь видны приложения
            installed = loadApps()
            coins = coinsOf(installed)
        }
        return info
    }

    override fun setAddrCount(n: Int) {
        if (info["addrs"] == null) throw WalletError("Нужна прошивка новее: обнови устройство")
        dev.call("addrs $n")
        refreshInfo()
        scans.clear()
    }

    override fun address(coin: String, index: Int, chain: Int): String {
        need(coin)
        return addrCache.getOrPut("$coin/$chain/$index") {
            dev.call(cmd(coin, if (coin in UTXO) "addr $chain $index" else "addr $index"))
        }
    }

    /** Адрес, подтверждённый на устройстве. Отказ кнопкой — DeviceError("rejected"). */
    override fun showAddress(coin: String, index: Int): String {
        need(coin)
        return dev.call(cmd(coin, if (coin in UTXO) "show 0 $index" else "show $index"))
    }

    override fun restore(phrase: String): Boolean {
        if (ready) throw WalletError("На устройстве уже есть кошелёк")
        val ent = mnemonicToEntropy(phrase, wordlist)
        try {
            dev.call("restore ${ent.hex()}")
        } finally {
            ent.fill(0)
        }
        refreshInfo()
        addrCache.clear()
        return ready
    }

    override fun close() = dev.close()

    override fun updateFirmware(img: FwImage, onProgress: (Int, Int) -> Unit) {
        val hw = info["hw"] ?: "?"
        if (!hw.startsWith("esp32")) throw WalletError("Обновление по воздуху есть только у ESP32-S3")
        if (img.chip != hw) throw WalletError("Образ собран для ${img.chip}, а устройство — $hw")
        val image = img.bytes
        // Сжатый поток (прошивки с v0.7): данных на ~40% меньше; старые ответят «unknown»
        // ещё до экрана подтверждения — тогда как раньше
        var z: ByteArray? = null
        Trace.log("ota: image ${image.size} B, build ${img.buildId}, fw on device ${info["fw"]} ${info["build"]}")
        try {
            dev.call("ota zbegin ${image.size} ${img.sha256}")  // ждёт кнопку на устройстве
            z = deflate(image)
            Trace.log("ota: zbegin ok, compressed ${z.size} B")
        } catch (e: DeviceError) {
            Trace.log("ota: zbegin -> ${e.message}")
            if (e.message != "unknown ota command") throw e
        }
        if (z == null) try {
            dev.call("ota begin ${image.size} ${img.sha256}")
        } catch (e: DeviceError) {
            if (e.message?.startsWith("unknown") == true)
                throw WalletError("Эта прошивка не умеет обновляться по Bluetooth — один раз обнови её по USB")
            throw e
        }
        if (z != null) {
            try {
                val enc = java.util.Base64.getEncoder()
                val lines = (z.indices step 2048).map { "ota b64 " + enc.encodeToString(z.copyOfRange(it, minOf(it + 2048, z.size))) }
                // По Bluetooth — по одному куску с ответом: с конвейером обновление вставало на 1%
                // (данные приходили, пока плата пишет флеш). Сжатие даёт своё и так.
                dev.stream(lines, 1) { k -> onProgress(minOf(image.size, (k.toLong() * image.size / lines.size).toInt()), image.size) }
            } catch (e: Exception) {
                Trace.log("ota: stream failed: ${e.message}")
                runCatching { dev.call("ota abort") }
                throw e
            }
            try {
                dev.call("ota end")
                Trace.log("ota: end ok")
            } catch (e: DeviceError) {
                Trace.log("ota: end -> ${e.message}")
                throw e
            } catch (e: WalletError) {
                // Устройство могло перезагрузиться раньше, чем дошёл ответ, — это успех
                Trace.log("ota: end, no reply (rebooting?): ${e.message}")
            }
            return
        }
        try {
            // base64 по 2 КБ (прошивки с v0.5); старые понимают только hex по 1 КБ
            var b64 = true
            var off = 0
            while (off < image.size) {
                val end = minOf(image.size, off + if (b64) 2048 else 1024)
                val part = image.copyOfRange(off, end)
                try {
                    dev.call(if (b64) "ota b64 " + java.util.Base64.getEncoder().encodeToString(part) else "ota data " + part.hex())
                } catch (e: DeviceError) {
                    if (!b64 || off != 0 || e.message != "unknown ota command") throw e
                    b64 = false
                    continue
                }
                off = end
                onProgress(off, image.size)
            }
        } catch (e: Exception) {
            runCatching { dev.call("ota abort") }
            throw e
        }
        try {
            dev.call("ota end")
        } catch (e: DeviceError) {
            throw e
        } catch (e: WalletError) {
            // Устройство могло перезагрузиться раньше, чем дошёл ответ, — это успех
        }
    }

    // ---- баланс ----

    override fun balance(coin: String, skip: Set<String>): Balance {
        need(coin)
        if (coin in UTXO) {
            val s = scan(coin)
            return Balance(
                address = address(coin, 0),
                assets = listOf(Asset(coin, symbol(coin), 8, BigInteger.valueOf(s.utxos.sumOf { it.value }))),
                pending = BigInteger.valueOf(s.immature.sumOf { it.value }),
                utxos = s.utxos, immature = s.immature,
            )
        }
        val addr = address(coin, 0)
        if (coin == "ton") return tonBalance(addr, skip)
        if (coin == "sol") {
            val tokens = cfg.solMints.filterKeys { it !in skip }.map { (id, m) ->
                Asset(id, id.uppercase(), m.second, BigInteger.valueOf(solTokenBalance(addr, m.first)))
            }.sortedBy { it.id != "usdt" }
            return Balance(addr, tokens + Asset("sol", symbol("sol"), 9, BigInteger.valueOf(solBalance(addr))))
        }
        if (coin == "tron") {
            val acc = tronPost("/wallet/getaccount", JSONObject().put("address", addr).put("visible", true)) as JSONObject
            val notes = if (acc.length() == 0) listOf("Аккаунт не активирован: нужно получить хотя бы 1 TRX") else emptyList()
            val v2 = acc.optJSONArray("assetV2") ?: JSONArray()
            val trc10 = (0 until v2.length()).map { v2.getJSONObject(it) }.filter { it.optLong("value") > 0 }
                .map { Asset("trc10:" + it.getString("key"), "#" + it.getString("key"), 0, BigInteger.valueOf(it.getLong("value"))) }
            return Balance(
                addr, listOfNotNull(
                    if ("usdt" in skip) null else Asset("usdt", "USDT", 6, tronUsdt(addr)),
                    Asset("trx", "TRX", 6, BigInteger.valueOf(acc.optLong("balance", 0))),
                ) + trc10, notes = notes
            )
        }
        val chain = cfg.evm(coin, chainId)
        val bal = hexInt(rpc(chain, "eth_getBalance", addr, "latest"))
        return Balance(
            addr,
            chain.tokens.filter { it.asset !in skip }.map { Asset(it.asset, it.symbol, it.decimals, tokenBalance(chain, it, addr)) } +
                Asset(if (coin == "eth") "eth" else "bnb", chain.native, 18, bal),
        )
    }

    // ---- отправка ----

    override fun prepareSend(req: SendRequest): Draft {
        need(req.coin)
        val asset = req.asset ?: coinInfo(req.coin).assetsFor(net)[0].first
        return when (req.coin) {
            "btc", "prl", "ltc" -> prepareUtxo(req.coin, req.dest, req.amount, req.feerate)
            "tron" -> prepareTron(req.index, req.dest, req.amount, asset, req.feeLimit, req.memo)
            "ton" -> prepareTon(req.index, req.dest, req.amount, asset, req.memo)
            "sol" -> if (asset == "sol") prepareSol(req.index, req.dest, req.amount)
            else prepareSolToken(req.index, req.dest, req.amount, asset)
            else -> prepareEvm(cfg.evm(req.coin, chainId), req.index, req.dest, req.amount, asset, req.gasPrice)
        }
    }

    // ================= BTC и Pearl =================

    /** API как у mempool.space: Bitcoin — mempool.space, Litecoin — litecoinspace.org. */
    private fun api(coin: String) = if (coin == "ltc") cfg.ltcApi else cfg.btcApi
    private fun hrpOf(coin: String) = when (coin) { "ltc" -> cfg.ltcHrp; "prl" -> cfg.prlHrp; else -> cfg.hrp }
    private fun b58Of(coin: String) = when (coin) { "ltc" -> cfg.ltcB58; "prl" -> null; else -> cfg.btcB58 }
    private fun btcGet(path: String, coin: String = "btc") = http.get(api(coin) + path)

    /** Предыдущая транзакция целиком — с кэшем: она не меняется. */
    private val prevCache = HashMap<String, ByteArray>()
    private fun prevTx(txid: String, coin: String = "btc"): ByteArray =
        prevCache.getOrPut("$coin:$txid") { http.getText(api(coin) + "/tx/$txid/hex").unhex() }
    private fun prlGet(path: String) = http.get(cfg.prlApi + "/api/v2" + path)

    class Scan(val utxos: List<Utxo>, val immature: List<Utxo>, val changeIndex: Int)

    /** Последний скан по монете: повторное подключение или обновление в течение
     *  SCAN_TTL_MS не идёт в сеть. После отправки сбрасывается. */
    private val scans = HashMap<String, Pair<Long, Scan>>()

    /** Обновление «потянуть вниз»: следующий баланс — из сети, а не из кэша. */
    override fun forgetCache() = scans.clear()

    fun scan(coin: String): Scan {
        scans[coin]?.let { (at, s) -> if (System.currentTimeMillis() - at < SCAN_TTL_MS) return s }
        return scanNow(coin).also { scans[coin] = System.currentTimeMillis() to it }
    }

    /** UTXO на адресах получения 0..addrCount-1 и на адресах сдачи (подряд до первого
     *  пустого), незрелые награды, свободный адрес сдачи. */
    private fun scanNow(coin: String): Scan {
        val prl = coin == "prl"
        val utxos = ArrayList<Utxo>()
        val immature = ArrayList<Utxo>()
        var firstUnusedChange: Int? = null
        for (chain in 0..1) for (i in 0 until if (chain == 0) addrCount else CHANGE_MAX) {
            if (chain == 1 && firstUnusedChange != null) break  // сдача — на первый свободный, дальше пусто
            val addr = address(coin, i, chain)
            val used = if (prl) {
                val info = prlGet("/address/$addr?details=basic") as JSONObject
                info.optInt("txs", 0) + info.optInt("unconfirmedTxs", 0) > 0
            } else {
                val info = btcGet("/address/$addr", coin) as JSONObject
                info.getJSONObject("chain_stats").getInt("tx_count") +
                    info.getJSONObject("mempool_stats").getInt("tx_count") > 0
            }
            if (chain == 1 && !used && firstUnusedChange == null) firstUnusedChange = i
            if (!used) continue
            val list = (if (prl) prlGet("/utxo/$addr") else btcGet("/address/$addr/utxo", coin)) as JSONArray
            for (k in 0 until list.length()) {
                val u = list.getJSONObject(k)
                val conf = if (prl) u.optInt("confirmations", 0) else 0
                val utxo = Utxo(
                    txid = u.getString("txid"), vout = u.getInt("vout"),
                    value = u.get("value").toString().toLong(),
                    confirmed = if (prl) conf > 0 else u.getJSONObject("status").getBoolean("confirmed"),
                    chain = chain, index = i, address = addr, confirmations = conf,
                )
                // Blockbook не помечает coinbase в /utxo — смотрим саму транзакцию
                val coinbase = prl && conf < PRL_COINBASE_MATURITY &&
                    (prlGet("/tx/${utxo.txid}") as JSONObject).getJSONArray("vin").getJSONObject(0).has("coinbase")
                if (coinbase) immature.add(utxo) else utxos.add(utxo)
            }
        }
        return Scan(utxos, immature, firstUnusedChange ?: 0)
    }

    /** Ставки sat/vB (grain/vB): fast, normal, slow. */
    override fun feeLevels(coin: String): Map<String, BigDecimal> {
        if (coin == "prl") {  // Blockbook отдаёт PRL за kB на подтверждение за N блоков
            fun est(blocks: Int): BigDecimal {
                val v = BigDecimal((prlGet("/estimatefee/$blocks") as JSONObject).optString("result", "0"))
                return v.movePointRight(8).divide(BigDecimal(1000)).setScale(1, RoundingMode.HALF_EVEN).max(BigDecimal.ONE)
            }
            return mapOf("fast" to est(2), "normal" to est(6), "slow" to est(24))
        }
        return try {
            val r = btcGet("/v1/fees/recommended", coin) as JSONObject
            mapOf(
                "fast" to BigDecimal(maxOf(1, r.optInt("fastestFee", 2))),
                "normal" to BigDecimal(maxOf(1, r.optInt("halfHourFee", 2))),
                "slow" to BigDecimal(maxOf(1, r.optInt("hourFee", 2))),
            )
        } catch (e: WalletError) {
            mapOf("fast" to BigDecimal(5), "normal" to BigDecimal(2), "slow" to BigDecimal.ONE)
        }
    }

    private fun prepareUtxo(coin: String, destIn: String, amount: String, feerateIn: BigDecimal?): Draft {
        val prl = coin == "prl"
        val hrp = hrpOf(coin)
        val dest = destIn.trim()
        val units = toUnits(amount, 8).toLong()
        val feerate = feerateIn ?: feeLevels(coin).getValue(if (prl) "fast" else "normal")
        val destScript = addressToScript(dest, hrp, prl, b58Of(coin))
        val s = scan(coin)
        val utxos = s.utxos.sortedByDescending { it.value }
        val changeScript = addressToScript(address(coin, s.changeIndex, 1), hrp, prl)

        val chosen = ArrayList<Utxo>()
        var total = 0L
        var fee = 0L
        var enough = false
        for (u in utxos) {
            chosen.add(u)
            total += u.value
            val vsize = vsizeEstimate(chosen.size, listOf(destScript, changeScript), if (prl) 57.5 else 68.0)
            fee = BigDecimal(vsize).multiply(feerate).setScale(0, RoundingMode.CEILING).toLong()
            if (total >= units + fee) {
                enough = true
                break
            }
        }
        if (!enough) throw WalletError("Недостаточно средств: доступно ${fmtUnits(total, 8)} ${symbol(coin)}")
        if (chosen.size > TX_MAX_IN)
            throw WalletError(
                "Нужно ${chosen.size} входов, а устройство подписывает не больше $TX_MAX_IN. Отправь сумму " +
                    "поменьше или сначала объедини мелкие выходы, переведя их себе на адрес частями."
            )

        val outs = arrayListOf(TxOut(units, destScript, false))
        val change = total - units - fee
        if (change >= (if (coin == "ltc") 5460 else 546)) outs.add(TxOut(change, changeScript, true, s.changeIndex))  // иначе "пыль" — в комиссию
        else fee = total - units
        val tx = UtxoTx(
            ins = chosen.map { TxIn(it.txid, it.vout, it.value, it.chain, it.index, 0xFFFFFFFDL) },  // RBF
            outs = outs,
        )

        // BTC: предыдущие транзакции входов — устройство само проверит по ним суммы
        val prev = if (prl || legacy) emptyList() else chosen.map { prevTx(it.txid, coin) }
        val sign = {
            val resp = if (legacy) dev.call(cmd(coin, "sign " + buildRequest(tx)))  // прошивки до v0.6
            else dev.appCall(cmd(coin, "sign"), signBlob(tx, prev))
            val wit = resp.split(' ').filter { it.isNotEmpty() }.map { part ->
                if (prl) listOf(part.unhex()) else part.split(':').map { it.unhex() }
            }
            if (wit.size != chosen.size) throw WalletError("Устройство вернуло не столько подписей, сколько входов")
            serialize(tx, wit)
        }
        val broadcast = { raw: String ->
            scans.remove(coin)  // монеты потрачены — следующий баланс заново из сети
            if (prl) {
                val r = JSONObject(http.postRaw(cfg.prlApi + "/api/v2/sendtx/", raw))
                if (r.has("error")) throw WalletError("Сеть отклонила транзакцию: ${r.get("error")}")
            } else {
                http.postRaw(api(coin) + "/tx", raw)
            }
            Unit
        }
        val unit = if (prl) "grain/vB" else "sat/vB"
        val notes = arrayListOf("Входов: ${chosen.size}, ставка ${feerate.toPlainString()} $unit")
        if (coin == "ltc") ltcOldP2sh(dest, net)?.let { notes.add(0, LTC_OLD_P2SH_NOTE.format(it)) }
        return Draft(
            coin, coin, symbol(coin), address(coin, 0), dest, fmtUnits(units, 8), fmtUnits(fee, 8), symbol(coin),
            notes = notes,
            sign = sign, broadcast = broadcast,
            txUrl = if (prl) "${cfg.prlApi}/tx/" else if (coin == "ltc") cfg.ltcTxUrl else cfg.btcTxUrl,
        )
    }

    // ================= TRON =================

    // ================= TON (GRAM, USDT; кошелёк W5) — как _prepare_ton в wallet.py =================

    private fun tonGet(path: String, vararg q: Pair<String, Any>): JSONObject {
        val url = cfg.tonApi + path + if (q.isEmpty()) "" else
            "?" + q.joinToString("&") { "${it.first}=${java.net.URLEncoder.encode(it.second.toString(), "UTF-8")}" }
        return tonResult(path, http.get(url) as JSONObject)
    }

    private fun tonPost(path: String, body: JSONObject) = tonResult(path, http.post(cfg.tonApi + path, body) as JSONObject)

    private fun tonResult(path: String, r: JSONObject): JSONObject {
        if (!path.startsWith("/v2/")) return r
        if (!r.optBoolean("ok", true) || !r.has("result")) throw WalletError("toncenter: ${r.opt("error") ?: r}")
        return r.optJSONObject("result") ?: JSONObject().put("value", r.get("result"))
    }

    /** (баланс в нано-GRAM, кошелёк развёрнут, seqno) */
    private fun tonInfo(addr: String): Triple<BigInteger, Boolean, Long> {
        val info = tonGet("/v2/getAddressInformation", "address" to addr)
        val active = info.optString("state") == "active"
        var seqno = 0L
        if (active) {
            val r = tonPost("/v2/runGetMethod", JSONObject().put("address", addr).put("method", "seqno").put("stack", JSONArray()))
            if (r.optInt("exit_code", -1) != 0) throw WalletError("toncenter: seqno, код ${r.opt("exit_code")}")
            seqno = r.getJSONArray("stack").getJSONArray(0).getString(1).removePrefix("0x").toLong(16)
        }
        return Triple(BigInteger(info.optString("balance", "0").ifEmpty { "0" }), active, seqno)
    }

    private fun tonUsdt(addr: String): BigInteger {
        val master = cfg.tonUsdt ?: return BigInteger.ZERO
        val w = tonGet("/v3/jetton/wallets", "owner_address" to addr, "jetton_address" to master, "limit" to 1)
            .optJSONArray("jetton_wallets")
        return if (w != null && w.length() > 0) BigInteger(w.getJSONObject(0).getString("balance")) else BigInteger.ZERO
    }

    private fun tonBalance(addr: String, skip: Set<String>): Balance {
        val (bal, active, _) = tonInfo(addr)
        val assets = ArrayList<Asset>()
        if (cfg.tonUsdt != null && "usdt" !in skip) assets += Asset("usdt", "USDT", 6, tonUsdt(addr))
        assets += Asset("gram", symbol("gram"), 9, bal)
        return Balance(addr, assets, notes = if (active) emptyList() else listOf("Кошелёк ещё не развёрнут: первая отправка развернёт его"))
    }

    private fun tonFee(own: String, signing: Ton.Cell, init: Ton.Cell?): BigInteger? = try {
        val b64 = java.util.Base64.getEncoder()
        val req = JSONObject().put("address", own)
            .put("body", b64.encodeToString(Ton.toBoc(Ton.signedBody(signing, ByteArray(64))))).put("ignore_chksig", true)
        if (init != null) {
            req.put("init_code", b64.encodeToString(Ton.toBoc(init.refs[0])))
            req.put("init_data", b64.encodeToString(Ton.toBoc(init.refs[1])))
        }
        val f = tonPost("/v2/estimateFee", req).getJSONObject("source_fees")
        listOf("in_fwd_fee", "storage_fee", "gas_fee", "fwd_fee").fold(BigInteger.ZERO) { a, k -> a + BigInteger.valueOf(f.optLong(k)) }
    } catch (e: WalletError) {
        null
    } catch (e: org.json.JSONException) {
        null
    }

    private fun prepareTon(index: Int, destIn: String, amount: String, asset: String, memoIn: String?): Draft {
        val tn = net != "mainnet"
        val ownS = address("ton", index)
        val own = Ton.parseAddress(ownS, tn).first
        val (to, bounce) = Ton.parseAddress(destIn, tn)
        val memo = memoIn ?: ""
        if (memo.length > Ton.COMMENT_MAX || memo.any { it.code !in 0x20..0x7E })
            throw WalletError("Комментарий — печатные латинские символы, не длиннее ${Ton.COMMENT_MAX}")
        val (bal, active, seqno) = tonInfo(ownS)
        val usdt = asset == "usdt"
        val units: BigInteger
        val msg: Ton.Cell
        val shownTo: String
        val spend: BigInteger
        if (usdt) {
            val master = cfg.tonUsdt ?: throw WalletError("В TON testnet нет USDT")
            units = toUnits(amount, 6)
            if (units > tonUsdt(ownS)) throw WalletError("Недостаточно USDT")
            val jw = Ton.usdtWallet(own, Ton.parseAddress(master, false).first)
            msg = Ton.internal(jw, BigInteger.valueOf(Ton.USDT_ATTACH), true, Ton.jettonTransfer(units, to, own, memo.ifEmpty { null }))
            shownTo = Ton.friendly(to, false, tn)
            spend = BigInteger.valueOf(Ton.USDT_ATTACH)
        } else {
            units = toUnits(amount, 9)
            msg = Ton.internal(to, units, bounce, if (memo.isNotEmpty()) Ton.commentCell(memo) else null)
            shownTo = Ton.friendly(to, bounce, tn)
            spend = units
        }
        var init: Ton.Cell? = null
        if (!active) {  // первая отправка разворачивает кошелёк: нужен StateInit с ключом
            val pub = dev.call(cmd("ton", "pubkey $index")).unhex()
            if (Ton.w5Address(pub, tn) != own) throw WalletError("Ключ устройства не соответствует адресу")
            init = Ton.w5Init(pub, tn)
        }
        val signing = Ton.signingMessage(tn, seqno, tonValidUntil(), msg)
        val fee = tonFee(ownS, signing, init)
        val need = spend + (fee ?: BigInteger.ZERO)
        if (need > bal) throw WalletError("Недостаточно ${symbol("gram")}: нужно ≈${fmtUnits(need, 9)}")

        val sign = {
            val sig = dev.call(cmd("ton", "sign $index ${Ton.toBoc(signing).hex()}")).unhex()
            val body = Ton.signedBody(signing, sig)
            Ton.toBoc(Ton.external(own, body, init)).hex() to Ton.normalizedHash(own, body).hex()
        }
        val broadcast = { rawHex: String ->
            tonPost("/v2/sendBoc", JSONObject().put("boc", java.util.Base64.getEncoder().encodeToString(rawHex.unhex())))
            Unit
        }
        val notes = ArrayList<String>()
        if (usdt) notes += "Сеть ≈${fee?.let { fmtUnits(it, 9) } ?: "?"} ${symbol("gram")}; " +
            "из ${fmtUnits(BigInteger.valueOf(Ton.USDT_ATTACH), 9)} на пересылку жетона лишнее вернётся"
        if (init != null) notes += "Первая отправка развернёт кошелёк — комиссия чуть выше"
        if (memo.isNotEmpty()) notes += "Комментарий: $memo"
        return Draft(
            "ton", asset, symbol(asset), ownS, shownTo, fmtUnits(units, if (usdt) 6 else 9),
            if (usdt) fmtUnits(BigInteger.valueOf(Ton.USDT_ATTACH), 9) else fee?.let { "≈" + fmtUnits(it, 9) } ?: "?",
            symbol("gram"), feeLabel = if (usdt) "Макс. комиссия" else "Комиссия (оценка)",
            notes = notes, sign = sign, broadcast = broadcast, txUrl = cfg.tonTxUrl,
        )
    }

    // ================= Solana (SOL) — как _prepare_sol в wallet.py =================

    private fun sol(method: String, vararg params: Any): Any {
        val p = JSONArray()
        params.forEach { p.put(it) }
        val r = http.post(cfg.solRpc, JSONObject().put("jsonrpc", "2.0").put("id", 1).put("method", method).put("params", p)) as JSONObject
        r.optJSONObject("error")?.let { throw WalletError("Solana RPC $method: ${it.optString("message", it.toString())}") }
        return r.get("result")
    }

    private fun solBalance(addr: String): Long =
        (sol("getBalance", addr, JSONObject().put("commitment", "confirmed")) as JSONObject).getLong("value")

    /** Баланс токена на ATA владельца; счёта ещё нет — 0. */
    private fun solTokenBalance(owner: String, mint: String): Long {
        val ata = Sol.ata(Sol.b58decode(owner), Sol.b58decode(mint)).first
        return try {
            (sol("getTokenAccountBalance", Sol.b58encode(ata), JSONObject().put("commitment", "confirmed")) as JSONObject)
                .getJSONObject("value").getString("amount").toLong()
        } catch (e: WalletError) {
            0L
        }
    }

    private fun solPrice(): Long = try {
        val a = sol("getRecentPrioritizationFees") as JSONArray
        val fees = (0 until a.length()).map { a.getJSONObject(it).getLong("prioritizationFee") }.sorted()
        minOf(5_000_000L, maxOf(1000L, if (fees.isEmpty()) 1000L else fees[fees.size * 3 / 4]))
    } catch (e: WalletError) {
        1000L
    } catch (e: org.json.JSONException) {
        1000L
    }

    /** USDC/USDT: на ATA получателя; нет счёта — создаём (рента с этого кошелька). Как _prepare_sol_token. */
    private fun prepareSolToken(index: Int, destIn: String, amount: String, asset: String): Draft {
        val (mintS, dec) = cfg.solMints[asset] ?: throw WalletError("${asset.uppercase()} в этой сети Solana нет")
        val ownS = address("sol", index)
        val own = Sol.parseAddress(ownS)
        val owner = Sol.parseAddress(destIn)
        if (own.contentEquals(owner)) throw WalletError("Это адрес самого кошелька")
        if (!Sol.onCurve(owner)) throw WalletError("Нужен адрес кошелька получателя, а не адрес токен-счёта")
        val mint = Sol.b58decode(mintS)
        val units = toUnits(amount, dec).toLong()
        if (units > solTokenBalance(ownS, mintS)) throw WalletError("Недостаточно ${asset.uppercase()}")
        val dst = Sol.ata(owner, mint).first
        val info = sol("getAccountInfo", Sol.b58encode(dst), JSONObject().put("encoding", "base64")) as JSONObject
        val create = info.isNull("value")
        val price = solPrice()
        val bh = (sol("getLatestBlockhash", JSONObject().put("commitment", "finalized")) as JSONObject)
            .getJSONObject("value").getString("blockhash")
        val tm = Sol.tokenMessage(own, owner, mint, dec, units, Sol.b58decode(bh), price, create)
        val fee = Sol.maxFee(price, tm.limit)
        val need = fee + if (create) Sol.TOKEN_ACCOUNT_RENT else 0L
        if (need > solBalance(ownS)) throw WalletError("Недостаточно ${symbol("sol")} на комиссию: нужно ${fmtUnits(need, 9)}")
        val dest = destIn.trim()
        val sign = {
            val sig = dev.call(cmd("sol", "sign $index ${tm.msg.hex()} $dest ${tm.bump}")).unhex()
            Sol.transaction(tm.msg, sig).hex() to Sol.b58encode(sig)
        }
        val broadcast = { rawHex: String ->
            sol("sendTransaction", java.util.Base64.getEncoder().encodeToString(rawHex.unhex()),
                JSONObject().put("encoding", "base64").put("preflightCommitment", "confirmed"))
            Unit
        }
        val notes = arrayListOf("Блокхэш действует около минуты: подтверди на устройстве не мешкая")
        if (create) notes.add(0, "У получателя ещё нет счёта ${asset.uppercase()}: его создание — " +
            "${fmtUnits(Sol.TOKEN_ACCOUNT_RENT, 9)} ${symbol("sol")} (рента)")
        return Draft(
            "sol", asset, asset.uppercase(), ownS, dest, fmtUnits(units, dec), fmtUnits(fee, 9), symbol("sol"),
            feeLabel = "Макс. комиссия", notes = notes, sign = sign, broadcast = broadcast, txUrl = cfg.solTxUrl,
        )
    }

    private fun prepareSol(index: Int, destIn: String, amount: String): Draft {
        val ownS = address("sol", index)
        val own = Sol.parseAddress(ownS)
        val to = Sol.parseAddress(destIn)
        if (own.contentEquals(to)) throw WalletError("Это адрес самого кошелька")
        val lamports = toUnits(amount, 9).toLong()
        val price = solPrice()
        val fee = Sol.maxFee(price)
        val rent = (sol("getMinimumBalanceForRentExemption", 0) as Number).toLong()
        val bal = solBalance(ownS)
        val left = bal - lamports - fee
        if (left < 0) throw WalletError("Недостаточно ${symbol("sol")}: доступно ${fmtUnits(bal, 9)}")
        if (left in 1 until rent) throw WalletError("На счёте должно остаться 0 или не меньше ${fmtUnits(rent, 9)} (рента Solana)")
        if (lamports < rent && solBalance(destIn.trim()) == 0L)
            throw WalletError("Получатель новый: отправь не меньше ${fmtUnits(rent, 9)} (рента Solana)")
        val bh = ((sol("getLatestBlockhash", JSONObject().put("commitment", "finalized")) as JSONObject)
            .getJSONObject("value").getString("blockhash"))
        val msg = Sol.transferMessage(own, to, lamports, Sol.b58decode(bh), price)
        val sign = {
            val sig = dev.call(cmd("sol", "sign $index ${msg.hex()}")).unhex()
            Sol.transaction(msg, sig).hex() to Sol.b58encode(sig)
        }
        val broadcast = { rawHex: String ->
            sol("sendTransaction", java.util.Base64.getEncoder().encodeToString(rawHex.unhex()),
                JSONObject().put("encoding", "base64").put("preflightCommitment", "confirmed"))
            Unit
        }
        return Draft(
            "sol", "sol", symbol("sol"), ownS, destIn.trim(), fmtUnits(lamports, 9), fmtUnits(fee, 9), symbol("sol"),
            feeLabel = "Макс. комиссия", notes = listOf("Блокхэш действует около минуты: подтверди на устройстве не мешкая"),
            sign = sign, broadcast = broadcast, txUrl = cfg.solTxUrl,
        )
    }

    private fun tronPost(path: String, body: JSONObject) = http.post(cfg.tronApi + path, body)

    private fun tronErr(r: Any?): String {
        val msg = (r as? JSONObject)?.let { it.opt("message") ?: it.opt("Error") } ?: r
        val s = msg.toString()
        return try {  // нода часто отдаёт текст ошибки в hex
            String(s.unhex())
        } catch (e: Exception) {
            s
        }
    }

    private fun tronUsdt(owner: String): BigInteger {
        val param = (ByteArray(12) + tronDecode(owner).copyOfRange(1, 21)).hex()
        val r = tronPost(
            "/wallet/triggerconstantcontract", JSONObject()
                .put("owner_address", owner).put("contract_address", cfg.tronUsdt)
                .put("function_selector", "balanceOf(address)").put("parameter", param).put("visible", true)
        ) as JSONObject
        val res = r.optJSONArray("constant_result")
        val v = res?.optString(0).orEmpty()
        return if (v.isNotEmpty()) BigInteger(v, 16) else BigInteger.ZERO
    }

    /** Для оценки сожжённых TRX: параметры сети, ресурсы аккаунта, получатель новый.
     *  null — нода не ответила: подпишем без оценки, потолок fee_limit всё равно есть. */
    private class TronEst(val cp: Map<String, Long>, val res: JSONObject, val new: Boolean)

    private fun tronEstimate(owner: String, dest: String, activation: Boolean): TronEst? = try {
        val arr = (tronPost("/wallet/getchainparameters", JSONObject()) as JSONObject).optJSONArray("chainParameter") ?: JSONArray()
        val cp = (0 until arr.length()).map { arr.getJSONObject(it) }.associate { it.getString("key") to it.optLong("value") }
        val res = tronPost("/wallet/getaccountresource", JSONObject().put("address", owner).put("visible", true)) as JSONObject
        val new = activation &&
            (tronPost("/wallet/getaccount", JSONObject().put("address", dest).put("visible", true)) as JSONObject).length() == 0
        TronEst(cp, res, new)
    } catch (e: WalletError) {
        null
    } catch (e: org.json.JSONException) {
        null
    }

    /** Оценка сожжённых sun: bandwidth (подписанная транзакция + 64 Б, как считает нода), энергия
     *  сверх застейканной, активация нового адреса, плата за memo. Как _tron_burn в wallet.py. */
    private fun tronBurn(e: TronEst, rawLen: Int, energy: Long?, memo: Boolean): Long {
        val size = rawLen + varint(rawLen).size + 1 + 67 + 64
        val staked = e.res.optLong("NetLimit") - e.res.optLong("NetUsed")
        val free = e.res.optLong("freeNetLimit") - e.res.optLong("freeNetUsed")
        var burn = 0L
        if (e.new) {
            burn += e.cp["getCreateNewAccountFeeInSystemContract"] ?: 1_000_000
            if (staked < size) burn += e.cp["getCreateAccountFee"] ?: 100_000
        } else if (staked < size && free < size) {
            burn += size * (e.cp["getTransactionFee"] ?: 1000)
        }
        if (energy != null && energy > 0) {
            val avail = maxOf(0L, e.res.optLong("EnergyLimit") - e.res.optLong("EnergyUsed"))
            burn += maxOf(0L, energy - avail) * (e.cp["getEnergyFee"] ?: 420)
        }
        if (memo) burn += e.cp["getMemoFee"] ?: 0
        return burn
    }

    private fun prepareTron(index: Int, destIn: String, amount: String, asset: String, feeLimitIn: String?, memoIn: String?): Draft {
        val dest = destIn.trim()
        val owner = address("tron", index)
        val to21 = tronDecode(dest)
        val memo = (memoIn ?: "").toByteArray()
        if (memo.size > TRON_MEMO_MAX || memo.any { it < 0x20 || it > 0x7E })
            throw WalletError("Memo — печатные латинские символы, не длиннее $TRON_MEMO_MAX")
        val usdt = asset == "usdt"
        val trc10 = asset.startsWith("trc10:")
        val tokenId = asset.removePrefix("trc10:")
        if (trc10 && (tokenId.isEmpty() || !tokenId.all { it in '0'..'9' })) throw WalletError("Неверный ID токена TRC10: $tokenId")
        val units = toUnits(amount, if (trc10) 0 else 6)
        var energy: Long? = null
        val call = JSONObject()
        if (usdt) {
            val param = (ByteArray(12) + to21.copyOfRange(1, 21)).hex() + units.toBytes(32).hex()
            call.put("owner_address", owner).put("contract_address", cfg.tronUsdt)
                .put("function_selector", "transfer(address,uint256)").put("parameter", param).put("visible", true)
            energy = try {  // сколько энергии съест перевод (у нового держателя USDT — вдвое больше)
                (tronPost("/wallet/triggerconstantcontract", JSONObject(call.toString())) as JSONObject)
                    .optLong("energy_used").takeIf { it > 0 }
            } catch (e: WalletError) {
                null
            }
        }
        val est = tronEstimate(owner, dest, !usdt)
        // до сборки размер транзакции неизвестен: 300 Б — с запасом больше любой нашей raw_data
        val pre = if (est != null && energy != null) tronBurn(est, 300, energy, memo.isNotEmpty()) else null
        val feeLimit = if (feeLimitIn.isNullOrBlank()) {  // 40 TRX, а если энергия дороже — оценка + 20 %
            maxOf(BigDecimal(40), BigDecimal((pre ?: 0) * 12 / 10_000_000 + if ((pre ?: 0) * 12 % 10_000_000 != 0L) 1 else 0))
        } else try {
            BigDecimal(feeLimitIn.trim().replace(',', '.'))
        } catch (e: NumberFormatException) {
            throw WalletError("Неверный лимит комиссии: $feeLimitIn")
        }
        val feeLimitSun = toUnits(feeLimit.toPlainString(), 6).toLong()
        if (pre != null && pre > feeLimitSun)
            throw WalletError("Оценка комиссии ≈${fmtUnits(pre, 6)} TRX выше лимита ${feeLimit.toPlainString()} TRX — подними лимит")
        val tx: JSONObject
        if (usdt) {
            val r = tronPost("/wallet/triggersmartcontract", call.put("fee_limit", feeLimitSun).put("call_value", 0)) as JSONObject
            val res = r.optJSONObject("result")
            if (res == null || !res.optBoolean("result"))
                throw WalletError("Нода отказалась собрать транзакцию: ${tronErr(res ?: r)}")
            tx = r.getJSONObject("transaction")
        } else if (trc10) {
            tx = tronPost(
                "/wallet/transferasset", JSONObject()
                    .put("owner_address", owner).put("to_address", dest).put("asset_name", tokenId)
                    .put("amount", units.toLong()).put("visible", true)
            ) as JSONObject
        } else {
            tx = tronPost(
                "/wallet/createtransaction", JSONObject()
                    .put("owner_address", owner).put("to_address", dest).put("amount", units.toLong()).put("visible", true)
            ) as JSONObject
        }
        if (!tx.has("raw_data_hex")) throw WalletError("Нода отказалась собрать транзакцию: ${tronErr(tx)}")
        var raw = tx.getString("raw_data_hex").unhex()
        if (sha256(raw).hex() != tx.getString("txID"))
            throw WalletError("Нода вернула несогласованную транзакцию (txID != sha256(raw_data))")
        if (memo.isNotEmpty()) raw = tronWithMemo(raw, memo)
        val txId = sha256(raw).hex()
        val burn = est?.let { tronBurn(it, raw.size, energy, memo.isNotEmpty()) }
        if (usdt && burn != null && burn > feeLimitSun)
            throw WalletError("Оценка комиссии ≈${fmtUnits(burn, 6)} TRX выше лимита ${feeLimit.toPlainString()} TRX — подними лимит")

        val sign = {
            val arg = if (burn != null) " $burn" else ""
            val sig = dev.call(cmd("tron", "sign $index ${raw.hex()}$arg")).unhex()
            val s = sig.copyOfRange(0, 64) + byteArrayOf((sig[64] + 27).toByte())
            val signed = byteArrayOf(0x0a) + varint(raw.size) + raw + byteArrayOf(0x12) + varint(s.size) + s
            signed.hex() to txId
        }
        val broadcast = { rawHex: String ->
            val r = tronPost("/wallet/broadcasthex", JSONObject().put("transaction", rawHex)) as JSONObject
            if (!r.optBoolean("result")) throw WalletError("Сеть отклонила транзакцию: ${r.opt("code")} ${tronErr(r)}")
        }
        val notes = ArrayList<String>()
        if (usdt) notes += if (burn != null) "Оценка: сгорит ≈${fmtUnits(burn, 6)} TRX"
        else "Оценку получить не удалось; реально спишется меньше лимита"
        else if (est?.new == true) notes += "Получатель новый: +1 TRX за активацию адреса"
        if (memo.isNotEmpty()) notes += "Memo: ${String(memo)}"
        return Draft(
            "tron", asset, symbol(asset), owner, dest, fmtUnits(units, if (trc10) 0 else 6),
            if (usdt) feeLimit.toPlainString() else burn?.let { "≈" + fmtUnits(it, 6) } ?: "?", "TRX",
            feeLabel = if (usdt) "Макс. комиссия" else "Комиссия (оценка)",
            notes = notes, sign = sign, broadcast = broadcast, txUrl = cfg.tronTxUrl,
        )
    }

    // ================= EVM: BSC (встроенная) и Ethereum (приложение) =================

    private fun rpc(chain: EvmChain, method: String, vararg params: Any): Any {
        val p = JSONArray()
        params.forEach { p.put(it) }
        val r = http.post(
            chain.rpc, JSONObject().put("jsonrpc", "2.0").put("id", 1).put("method", method).put("params", p)
        ) as JSONObject
        if (r.has("error")) {
            val e = r.get("error")
            throw WalletError("${coinInfo(chain.coin).name} RPC $method: ${(e as? JSONObject)?.optString("message") ?: e}")
        }
        return r.get("result")
    }

    private fun hexInt(v: Any): BigInteger = BigInteger(v.toString().removePrefix("0x").ifEmpty { "0" }, 16)

    private fun tokenBalance(chain: EvmChain, t: EvmToken, addr: String): BigInteger {
        val data = "0x70a08231" + "00".repeat(12) + addr.substring(2).lowercase()
        val r = rpc(chain, "eth_call", JSONObject().put("to", t.contract).put("data", data), "latest").toString()
        return if (r == "0x" || r == "null") BigInteger.ZERO else hexInt(r)
    }

    private fun prepareEvm(chain: EvmChain, index: Int, destIn: String, amount: String, asset: String, gasPriceIn: String?): Draft {
        val frm = address(chain.coin, index)
        val dest20 = evmDecode(destIn.trim())
        val nonce = hexInt(rpc(chain, "eth_getTransactionCount", frm, "pending"))
        val gasPrice = when {
            gasPriceIn != null -> toUnits(gasPriceIn, 9)
            chain.eip1559 -> BigInteger.ZERO  // считается ниже из baseFee
            else -> hexInt(rpc(chain, "eth_gasPrice"))
        }

        val token = chain.tokens.firstOrNull { it.asset == asset }
        val units = toUnits(amount, token?.decimals ?: 18)
        val to: ByteArray
        val value: BigInteger
        val data: ByteArray
        val gas: BigInteger
        val sym: String
        if (token != null) {
            if (tokenBalance(chain, token, frm) < units) throw WalletError("Недостаточно ${token.symbol}")
            to = evmDecode(token.contract)
            value = BigInteger.ZERO
            data = "a9059cbb".unhex() + ByteArray(12) + dest20 + units.toBytes(32)
            val est = rpc(chain, "eth_estimateGas",
                JSONObject().put("from", frm).put("to", token.contract).put("data", "0x" + data.hex()))
            gas = hexInt(est).multiply(BigInteger.valueOf(12)).divide(BigInteger.TEN)  // запас 20%
            sym = token.symbol
        } else {
            to = dest20
            value = units
            data = ByteArray(0)
            gas = BigInteger.valueOf(21000)
            sym = chain.native
        }
        // EIP-1559: maxFee = 2 × baseFee + чаевые (запас на рост базы); вручную — цена газа = maxFee
        var tip = BigInteger.ZERO
        var maxFee = gasPrice
        if (chain.eip1559) {
            val base = hexInt((rpc(chain, "eth_getBlockByNumber", "latest", false) as JSONObject).getString("baseFeePerGas"))
            tip = runCatching { hexInt(rpc(chain, "eth_maxPriorityFeePerGas")) }.getOrElse { BigInteger.valueOf(1_500_000_000L) }
            maxFee = if (gasPriceIn != null) gasPrice else base.multiply(BigInteger.valueOf(2)).add(tip)
            if (tip > maxFee) tip = maxFee
        }
        val fee = gas.multiply(maxFee)
        val bal = hexInt(rpc(chain, "eth_getBalance", frm, "latest"))
        if (bal < fee + value)
            throw WalletError(
                "Недостаточно ${chain.native}: есть ${fmtUnits(bal, 18)}, нужно ${fmtUnits(fee + value, 18)} (с комиссией)"
            )

        val cid = BigInteger.valueOf(chain.chainId)
        // legacy: rlp([nonce, gasPrice, gas, to, value, data, chainId, 0, 0]);
        // тип 2: 0x02 || rlp([chainId, nonce, tip, maxFee, gas, to, value, data, []])
        val fields = if (chain.eip1559) listOf<Any>(cid, nonce, tip, maxFee, gas, to, value, data, emptyList<Any>())
        else listOf<Any>(nonce, gasPrice, gas, to, value, data)
        val unsigned = if (chain.eip1559) byteArrayOf(2) + rlp(fields) else rlp(fields + listOf(cid, 0, 0))

        val sign = {
            val sig = dev.call(cmd(chain.coin, "sign $index ${unsigned.hex()}")).unhex()
            val r = BigInteger(1, sig.copyOfRange(0, 32))
            val s = BigInteger(1, sig.copyOfRange(32, 64))
            val signed = if (chain.eip1559) byteArrayOf(2) + rlp(fields + listOf(sig[64].toInt(), r, s))
            else rlp(fields + listOf(cid.multiply(BigInteger.valueOf(2)).add(BigInteger.valueOf(35L + sig[64])), r, s))
            "0x" + signed.hex() to "0x" + keccak256(signed).hex()
        }
        val broadcast = { rawHex: String -> rpc(chain, "eth_sendRawTransaction", rawHex); Unit }
        return Draft(
            chain.coin, asset, sym, frm, evmChecksum(dest20), fmtUnits(units, token?.decimals ?: 18), fmtUnits(fee, 18),
            chain.native, feeLabel = "Макс. комиссия",
            notes = listOf(
                if (chain.eip1559) "EIP-1559: nonce $nonce, gas $gas × до ${fmtUnits(maxFee, 9)} gwei (чаевые ${fmtUnits(tip, 9)})"
                else "nonce $nonce, gas $gas × ${fmtUnits(gasPrice, 9)} gwei"
            ),
            sign = sign, broadcast = broadcast, txUrl = chain.txUrl,
        )
    }
}
