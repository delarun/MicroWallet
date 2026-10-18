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
    val feeLimit: String? = null,       // TRON USDT: TRX
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
    val label: String
    val cfg: NetConfig get() = NETS.getValue(net)
    fun symbol(asset: String): String = cfg.symbol(asset)
    fun address(coin: String, index: Int = 0, chain: Int = 0): String
    fun showAddress(coin: String, index: Int = 0): String
    fun balance(coin: String): Balance
    fun feeLevels(coin: String): Map<String, BigDecimal>
    fun prepareSend(req: SendRequest): Draft
    fun signAndSend(d: Draft, broadcast: Boolean = true): SendResult {
        val (raw, txid) = d.sign()
        if (broadcast) d.broadcast(raw)
        return SendResult(raw, txid, if (broadcast) d.txUrl + txid else "", broadcast)
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
    private val chainId = info["bsc_chain"]?.toLongOrNull() ?: if (net == "mainnet") 56 else 97

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
        if (coin !in coins) throw WalletError("На устройстве нет приложения ${coinInfo(coin).name} — установи его")
        if (!ready) throw WalletError("На устройстве нет кошелька — создай его на самом устройстве")
    }

    override fun refreshInfo(): Map<String, String> = dev.info().also { info = it }

    override fun setAddrCount(n: Int) {
        if (info["addrs"] == null) throw WalletError("Нужна прошивка новее: обнови устройство")
        dev.call("addrs $n")
        refreshInfo()
        scans.clear()
    }

    override fun address(coin: String, index: Int, chain: Int): String {
        need(coin)
        return addrCache.getOrPut("$coin/$chain/$index") {
            dev.call(cmd(coin, if (coin == "btc" || coin == "prl") "addr $chain $index" else "addr $index"))
        }
    }

    /** Адрес, подтверждённый на устройстве. Отказ кнопкой — DeviceError("rejected"). */
    override fun showAddress(coin: String, index: Int): String {
        need(coin)
        return dev.call(cmd(coin, if (coin == "btc" || coin == "prl") "show 0 $index" else "show $index"))
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
        try {
            dev.call("ota zbegin ${image.size} ${img.sha256}")  // ждёт кнопку на устройстве
            z = deflate(image)
        } catch (e: DeviceError) {
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
                dev.stream(lines, 4) { k -> onProgress(minOf(image.size, (k.toLong() * image.size / lines.size).toInt()), image.size) }
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

    override fun balance(coin: String): Balance {
        need(coin)
        if (coin == "btc" || coin == "prl") {
            val s = scan(coin)
            return Balance(
                address = address(coin, 0),
                assets = listOf(Asset(coin, symbol(coin), 8, BigInteger.valueOf(s.utxos.sumOf { it.value }))),
                pending = BigInteger.valueOf(s.immature.sumOf { it.value }),
                utxos = s.utxos, immature = s.immature,
            )
        }
        val addr = address(coin, 0)
        if (coin == "tron") {
            val acc = tronPost("/wallet/getaccount", JSONObject().put("address", addr).put("visible", true)) as JSONObject
            val notes = if (acc.length() == 0) listOf("Аккаунт не активирован: нужно получить хотя бы 1 TRX") else emptyList()
            return Balance(
                addr, listOf(
                    Asset("usdt", "USDT", 6, tronUsdt(addr)),
                    Asset("trx", "TRX", 6, BigInteger.valueOf(acc.optLong("balance", 0))),
                ), notes = notes
            )
        }
        val chain = cfg.evm(coin, chainId)
        val bal = hexInt(rpc(chain, "eth_getBalance", addr, "latest"))
        return Balance(
            addr,
            chain.tokens.map { Asset(it.asset, it.symbol, it.decimals, tokenBalance(chain, it, addr)) } +
                Asset(if (coin == "eth") "eth" else "bnb", chain.native, 18, bal),
        )
    }

    // ---- отправка ----

    override fun prepareSend(req: SendRequest): Draft {
        need(req.coin)
        val asset = req.asset ?: coinInfo(req.coin).assetsFor(net)[0].first
        return when (req.coin) {
            "btc", "prl" -> prepareUtxo(req.coin, req.dest, req.amount, req.feerate)
            "tron" -> prepareTron(req.index, req.dest, req.amount, asset, req.feeLimit)
            else -> prepareEvm(cfg.evm(req.coin, chainId), req.index, req.dest, req.amount, asset, req.gasPrice)
        }
    }

    // ================= BTC и Pearl =================

    private fun btcGet(path: String) = http.get(cfg.btcApi + path)

    /** Предыдущая транзакция целиком — с кэшем: она не меняется. */
    private val prevCache = HashMap<String, ByteArray>()
    private fun prevTx(txid: String): ByteArray =
        prevCache.getOrPut(txid) { http.getText(cfg.btcApi + "/tx/$txid/hex").unhex() }
    private fun prlGet(path: String) = http.get(cfg.prlApi + "/api/v2" + path)

    class Scan(val utxos: List<Utxo>, val immature: List<Utxo>, val changeIndex: Int)

    /** Последний скан по монете: повторное подключение или обновление в течение
     *  SCAN_TTL_MS не идёт в сеть. После отправки сбрасывается. */
    private val scans = HashMap<String, Pair<Long, Scan>>()

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
                val info = btcGet("/address/$addr") as JSONObject
                info.getJSONObject("chain_stats").getInt("tx_count") +
                    info.getJSONObject("mempool_stats").getInt("tx_count") > 0
            }
            if (chain == 1 && !used && firstUnusedChange == null) firstUnusedChange = i
            if (!used) continue
            val list = (if (prl) prlGet("/utxo/$addr") else btcGet("/address/$addr/utxo")) as JSONArray
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
            val r = btcGet("/v1/fees/recommended") as JSONObject
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
        val hrp = if (prl) cfg.prlHrp else cfg.hrp
        val dest = destIn.trim()
        val units = toUnits(amount, 8).toLong()
        val feerate = feerateIn ?: feeLevels(coin).getValue(if (prl) "fast" else "normal")
        val destScript = addressToScript(dest, hrp, prl)
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
        if (change >= 546) outs.add(TxOut(change, changeScript, true, s.changeIndex))  // иначе "пыль" — в комиссию
        else fee = total - units
        val tx = UtxoTx(
            ins = chosen.map { TxIn(it.txid, it.vout, it.value, it.chain, it.index, 0xFFFFFFFDL) },  // RBF
            outs = outs,
        )

        // BTC: предыдущие транзакции входов — устройство само проверит по ним суммы
        val prev = if (prl || legacy) emptyList() else chosen.map { prevTx(it.txid) }
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
                http.postRaw(cfg.btcApi + "/tx", raw)
            }
            Unit
        }
        val unit = if (prl) "grain/vB" else "sat/vB"
        return Draft(
            coin, coin, symbol(coin), address(coin, 0), dest, fmtUnits(units, 8), fmtUnits(fee, 8), symbol(coin),
            notes = listOf("Входов: ${chosen.size}, ставка ${feerate.toPlainString()} $unit"),
            sign = sign, broadcast = broadcast,
            txUrl = if (prl) "${cfg.prlApi}/tx/" else cfg.btcTxUrl,
        )
    }

    // ================= TRON =================

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

    private fun prepareTron(index: Int, destIn: String, amount: String, asset: String, feeLimitIn: String?): Draft {
        val dest = destIn.trim()
        val owner = address("tron", index)
        val to21 = tronDecode(dest)
        val feeLimit = try {
            BigDecimal(feeLimitIn?.trim()?.replace(',', '.') ?: "40")
        } catch (e: NumberFormatException) {
            throw WalletError("Неверный лимит комиссии: $feeLimitIn")
        }
        val units = toUnits(amount, 6)
        val tx: JSONObject
        if (asset == "usdt") {
            val param = (ByteArray(12) + to21.copyOfRange(1, 21)).hex() + units.toBytes(32).hex()
            val r = tronPost(
                "/wallet/triggersmartcontract", JSONObject()
                    .put("owner_address", owner).put("contract_address", cfg.tronUsdt)
                    .put("function_selector", "transfer(address,uint256)").put("parameter", param)
                    .put("fee_limit", toUnits(feeLimit.toPlainString(), 6).toLong()).put("call_value", 0)
                    .put("visible", true)
            ) as JSONObject
            val res = r.optJSONObject("result")
            if (res == null || !res.optBoolean("result"))
                throw WalletError("Нода отказалась собрать транзакцию: ${tronErr(res ?: r)}")
            tx = r.getJSONObject("transaction")
        } else {
            tx = tronPost(
                "/wallet/createtransaction", JSONObject()
                    .put("owner_address", owner).put("to_address", dest).put("amount", units.toLong()).put("visible", true)
            ) as JSONObject
            if (!tx.has("raw_data_hex")) throw WalletError("Нода отказалась собрать транзакцию: ${tronErr(tx)}")
        }
        val raw = tx.getString("raw_data_hex").unhex()
        val txId = tx.getString("txID")
        if (sha256(raw).hex() != txId)
            throw WalletError("Нода вернула несогласованную транзакцию (txID != sha256(raw_data))")

        val sign = {
            val sig = dev.call(cmd("tron", "sign $index ${raw.hex()}")).unhex()
            val s = sig.copyOfRange(0, 64) + byteArrayOf((sig[64] + 27).toByte())
            val signed = byteArrayOf(0x0a) + varint(raw.size) + raw + byteArrayOf(0x12) + varint(s.size) + s
            signed.hex() to txId
        }
        val broadcast = { rawHex: String ->
            val r = tronPost("/wallet/broadcasthex", JSONObject().put("transaction", rawHex)) as JSONObject
            if (!r.optBoolean("result")) throw WalletError("Сеть отклонила транзакцию: ${r.opt("code")} ${tronErr(r)}")
        }
        val usdt = asset == "usdt"
        return Draft(
            "tron", asset, if (usdt) "USDT" else "TRX", owner, dest, fmtUnits(units, 6),
            if (usdt) feeLimit.toPlainString() else "~0.3", "TRX",
            feeLabel = if (usdt) "Макс. комиссия" else "Комиссия",
            notes = if (usdt) listOf("Реально спишется меньше — по расходу энергии")
            else listOf("Платится сетью за bandwidth; новому адресу +1 TRX за активацию"),
            sign = sign, broadcast = broadcast, txUrl = cfg.tronTxUrl,
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
        val gasPrice = if (gasPriceIn != null) toUnits(gasPriceIn, 9) else hexInt(rpc(chain, "eth_gasPrice"))

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
        val fee = gas.multiply(gasPrice)
        val bal = hexInt(rpc(chain, "eth_getBalance", frm, "latest"))
        if (bal < fee + value)
            throw WalletError(
                "Недостаточно ${chain.native}: есть ${fmtUnits(bal, 18)}, нужно ${fmtUnits(fee + value, 18)} (с комиссией)"
            )

        val fields = listOf<Any>(nonce, gasPrice, gas, to, value, data)
        val cid = BigInteger.valueOf(chain.chainId)
        val unsigned = rlp(fields + listOf(cid, 0, 0))

        val sign = {
            val sig = dev.call(cmd(chain.coin, "sign $index ${unsigned.hex()}")).unhex()
            val r = BigInteger(1, sig.copyOfRange(0, 32))
            val s = BigInteger(1, sig.copyOfRange(32, 64))
            val v = cid.multiply(BigInteger.valueOf(2)).add(BigInteger.valueOf(35L + sig[64]))
            val signed = rlp(fields + listOf(v, r, s))
            "0x" + signed.hex() to "0x" + keccak256(signed).hex()
        }
        val broadcast = { rawHex: String -> rpc(chain, "eth_sendRawTransaction", rawHex); Unit }
        return Draft(
            chain.coin, asset, sym, frm, evmChecksum(dest20), fmtUnits(units, token?.decimals ?: 18), fmtUnits(fee, 18),
            chain.native, feeLabel = "Макс. комиссия",
            notes = listOf("nonce $nonce, gas $gas × ${fmtUnits(gasPrice, 9)} gwei"),
            sign = sign, broadcast = broadcast, txUrl = chain.txUrl,
        )
    }
}
