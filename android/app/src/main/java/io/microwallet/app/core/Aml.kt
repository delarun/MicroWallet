package io.microwallet.app.core

import org.json.JSONArray
import org.json.JSONObject
import java.math.BigDecimal
import java.math.BigInteger
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.Future

/*
 * AML-проверка адреса BSC (USDT BEP-20): санкции + прямая (1-hop) экспозиция контрагентов.
 * Перенос aml-check (app.py) в приложение:
 *   - NodeReal MegaNode nr_getAssetTransfers — история переводов USDT (нужен ключ, бесплатный);
 *   - Chainalysis Sanctions API — сам адрес и крупнейшие контрагенты по санкционным спискам
 *     (ключ бесплатный, без него — только по разметке);
 *   - разметка aml_labels.json в APK (tools/aml/update_labels.py: OFAC SDN, HTX, своя).
 * Скор 0–100: адрес под санкциями — 100; иначе доля оборота с помеченными контрагентами,
 * взвешенная по риску; хоть один контрагент под санкциями — не ниже 75.
 */

class AmlLabel(val label: String, val risk: String, val source: String)

class AmlKeys(val nodereal: String, val chainalysis: String) {
    val ready: Boolean get() = nodereal.isNotBlank()
}

class AmlParty(
    val address: String,
    val inAmount: Double,
    val outAmount: Double,
    val count: Int,
    val last: Long,          // unix, с
    val risk: String?,       // sanctions | high | medium | low | trusted | null
    val label: String?,
    val source: String?,
    val screened: Boolean,   // проверен по санкциям Chainalysis
) {
    val volume: Double get() = inAmount + outAmount
}

class AmlReport(
    val address: String,
    val days: Int,
    val score: Int,
    val verdict: String,                 // low | medium | high
    val targetSanctioned: Boolean,
    val targetNames: List<String>,       // имена в санкционных списках
    val targetLabel: AmlLabel?,          // сам адрес в разметке
    val totalIn: Double,
    val totalOut: Double,
    val transfers: Int,
    val counterpartyCount: Int,
    val flagged: List<AmlParty>,
    val top: List<AmlParty>,             // крупнейшие контрагенты (до 50)
    val unscreened: Int,
    val sanctionsEnabled: Boolean,
    val historyChecked: Boolean,         // false — без ключа NodeReal: только сам адрес
)

class Aml(private val http: Http, private val labels: Map<String, AmlLabel>) {
    companion object {
        val RISK_WEIGHT = mapOf("sanctions" to 1.0, "high" to 0.7, "medium" to 0.4, "low" to 0.1, "trusted" to 0.0)
        const val BLOCKS_PER_DAY = 115_200L  // ~0.75 с на блок
        const val WINDOW = 100_000L          // больше nr_getAssetTransfers за раз не отдаёт
        const val MAX_COUNTERPARTIES = 150
        private const val SANCTIONS_TTL_MS = 24 * 3600_000L
        private val EVM = Regex("^0x[0-9a-fA-F]{40}$")

        fun parseLabels(json: String): Map<String, AmlLabel> {
            val arr = JSONObject(json).optJSONArray("addresses") ?: JSONArray()
            return (0 until arr.length()).map { arr.getJSONObject(it) }.associate {
                it.getString("address").lowercase() to
                    AmlLabel(it.optString("label"), it.optString("risk", "medium"), it.optString("source"))
            }
        }

        fun verdict(score: Int) = if (score >= 50) "high" else if (score >= 15) "medium" else "low"

        fun nodeRealUrl(net: String, key: String) =
            "https://bsc-${if (net == "mainnet") "mainnet" else "testnet"}.nodereal.io/v1/${key.trim()}"
    }

    private val sanctionsCache = ConcurrentHashMap<String, Pair<Long, List<String>>>()

    /** Имена в санкционных списках (пусто — не найден); null — без ключа Chainalysis. */
    private fun sanctions(addr: String, key: String): List<String>? {
        sanctionsCache[addr]?.let { (t, v) -> if (System.currentTimeMillis() - t < SANCTIONS_TTL_MS) return v }
        if (key.isBlank()) return null
        val r = hideKey(key) {
            http.get("https://public.chainalysis.com/api/v1/address/$addr",
                mapOf("X-API-Key" to key.trim(), "Accept" to "application/json")) as JSONObject
        }
        val ids = r.optJSONArray("identifications") ?: JSONArray()
        val names = (0 until ids.length()).map { ids.getJSONObject(it).optString("name").ifEmpty { "sanctioned" } }
        sanctionsCache[addr] = System.currentTimeMillis() to names
        return names
    }

    /** Ключ — часть адреса NodeReal: в тексте ошибки его быть не должно. */
    private fun <T> hideKey(key: String, f: () -> T): T = try {
        f()
    } catch (e: WalletError) {
        val k = key.trim()
        throw WalletError((e.message ?: "Ошибка").let { if (k.length >= 4) it.replace(k, "…") else it })
    }

    private fun rpc(url: String, key: String, method: String, params: JSONArray): Any {
        val body = JSONObject().put("jsonrpc", "2.0").put("id", 1).put("method", method).put("params", params)
        val r = hideKey(key) { http.post(url, body) } as JSONObject
        r.optJSONObject("error")?.let { throw WalletError("NodeReal $method: ${it.optString("message", it.toString())}") }
        return r.get("result")
    }

    private fun window(url: String, key: String, usdt: String, address: String, dir: String, lo: Long, hi: Long): List<JSONObject> {
        val out = ArrayList<JSONObject>()
        var page: String? = null
        while (true) {
            val p = JSONObject()
                .put("category", JSONArray().put("20"))
                .put("contractAddresses", JSONArray().put(usdt.lowercase()))
                .put(dir, address)
                .put("fromBlock", "0x" + lo.toString(16))
                .put("toBlock", "0x" + hi.toString(16))
                .put("order", "desc")
                .put("maxCount", "0x3e8")
            if (page != null) p.put("pageKey", page)
            val res = rpc(url, key, "nr_getAssetTransfers", JSONArray().put(p)) as? JSONObject ?: return out
            val list = res.optJSONArray("transfers") ?: JSONArray()
            for (i in 0 until list.length()) out.add(list.getJSONObject(i))
            page = res.optString("pageKey").ifEmpty { res.optString("PageKey") }.ifEmpty { null }
            if (page == null || list.length() == 0) return out
        }
    }

    private fun amount(t: JSONObject): Double {
        val raw = t.optString("value").ifEmpty { "0x0" }
        val v = if (raw.startsWith("0x")) BigInteger(raw.drop(2).ifEmpty { "0" }, 16) else BigInteger(raw)
        val d = t.optString("decimal").ifEmpty { "18" }
        val dec = if (d.startsWith("0x")) d.drop(2).toInt(16) else d.toInt()
        return BigDecimal(v).movePointLeft(dec).toDouble()
    }

    private fun num(v: Any?): Long = when (v) {
        is Number -> v.toLong()
        is String -> if (v.startsWith("0x")) v.drop(2).toLongOrNull(16) ?: 0 else v.toLongOrNull() ?: 0
        else -> 0
    }

    /**
     * Проверить адрес. Без ключа NodeReal — только сам адрес (разметка и санкции), без истории.
     * onProgress(сделано, всего) — запросы истории.
     */
    fun check(
        net: String, usdt: String, addressIn: String, keys: AmlKeys, days: Int = 30,
        onProgress: (Int, Int) -> Unit = { _, _ -> },
    ): AmlReport {
        if (!EVM.matches(addressIn.trim())) throw WalletError("Нужен адрес BSC вида 0x… (40 hex-символов)")
        val address = addressIn.trim().lowercase()
        val targetIds = sanctions(address, keys.chainalysis)
        val targetLabel = labels[address]

        data class Flow(var inAmt: Double = 0.0, var outAmt: Double = 0.0, var count: Int = 0, var last: Long = 0)
        val flows = LinkedHashMap<String, Flow>()
        var transfers = 0
        var screened: Map<String, List<String>?> = emptyMap()
        if (keys.ready) {
            val url = nodeRealUrl(net, keys.nodereal)
            val latest = num(rpc(url, keys.nodereal, "eth_blockNumber", JSONArray()))
            val start = maxOf(0L, latest - days * BLOCKS_PER_DAY)
            val windows = ArrayList<Pair<Long, Long>>()
            var hi = latest
            while (hi > start) {
                val lo = maxOf(start, hi - WINDOW + 1)
                windows.add(lo to hi)
                hi = lo - 1
            }
            val jobs = windows.flatMap { (lo, h) -> listOf("fromAddress", "toAddress").map { Triple(it, lo, h) } }
            val pool = Executors.newFixedThreadPool(4)
            val all = try {
                var done = 0
                onProgress(0, jobs.size)
                val fs: List<Future<List<JSONObject>>> = jobs.map { (d, lo, h) ->
                    pool.submit<List<JSONObject>> { window(url, keys.nodereal, usdt, address, d, lo, h) }
                }
                fs.flatMap { f ->
                    val r = try {
                        f.get()
                    } catch (e: java.util.concurrent.ExecutionException) {
                        throw e.cause ?: e
                    }
                    onProgress(++done, jobs.size)
                    r
                }
            } finally {
                pool.shutdownNow()
            }
            val seen = HashSet<String>()
            for (t in all) {
                val k = listOf(t.optString("hash"), t.optString("from"), t.optString("to"), t.optString("value")).joinToString("|")
                if (!seen.add(k)) continue
                transfers++
                val st = t.opt("receiptsStatus")?.toString()
                if (st == "0" || st == "0x0") continue  // не прошёл
                val from = t.optString("from").lowercase()
                val to = t.optString("to").lowercase()
                val incoming = to == address
                val other = if (incoming) from else to
                if (other.isEmpty() || other == address) continue
                val f = flows.getOrPut(other) { Flow() }
                if (incoming) f.inAmt += amount(t) else f.outAmt += amount(t)
                f.count++
                f.last = maxOf(f.last, num(t.opt("blockTimeStamp") ?: t.opt("blockTimestamp")))
            }
            // санкции — только крупнейшим контрагентам: у бесплатного ключа есть предел
            if (keys.chainalysis.isNotBlank()) {
                val top = flows.entries.sortedByDescending { it.value.inAmt + it.value.outAmt }.take(MAX_COUNTERPARTIES).map { it.key }
                val p2 = Executors.newFixedThreadPool(5)
                screened = try {
                    top.map { a -> a to p2.submit<List<String>?> { runCatching { sanctions(a, keys.chainalysis) }.getOrNull() } }
                        .associate { (a, f) -> a to f.get() }
                } finally {
                    p2.shutdownNow()
                }
            }
        }

        val totalIn = flows.values.sumOf { it.inAmt }
        val totalOut = flows.values.sumOf { it.outAmt }
        var weighted = 0.0
        var unscreened = 0
        val parties = flows.entries.sortedByDescending { it.value.inAmt + it.value.outAmt }.map { (a, f) ->
            val hits = screened[a]
            if (a in screened && hits == null) unscreened++
            val label = labels[a]
            val risk = if (!hits.isNullOrEmpty()) "sanctions" else label?.risk
            if (risk != null) weighted += (f.inAmt + f.outAmt) * (RISK_WEIGHT[risk] ?: 0.4)
            AmlParty(
                a, f.inAmt, f.outAmt, f.count, f.last, risk,
                if (!hits.isNullOrEmpty()) hits[0] else label?.label,
                if (!hits.isNullOrEmpty()) "Chainalysis Sanctions" else label?.source,
                a in screened,
            )
        }
        val total = totalIn + totalOut
        var score = if (total > 0) Math.round(100 * weighted / total).toInt() else 0
        if (parties.any { it.risk == "sanctions" }) score = maxOf(score, 75)
        // сам адрес в разметке (например, кошелёк из OFAC SDN или биржи под санкциями)
        if (targetLabel != null) score = maxOf(score, Math.round(100 * (RISK_WEIGHT[targetLabel.risk] ?: 0.4)).toInt())
        val sanctioned = !targetIds.isNullOrEmpty() || targetLabel?.risk == "sanctions"
        if (sanctioned) score = 100
        score = score.coerceIn(0, 100)
        return AmlReport(
            address = address, days = days, score = score, verdict = verdict(score),
            targetSanctioned = sanctioned, targetNames = targetIds.orEmpty(), targetLabel = targetLabel,
            totalIn = totalIn, totalOut = totalOut, transfers = transfers, counterpartyCount = flows.size,
            flagged = parties.filter { it.risk != null && it.risk != "trusted" },
            top = parties.take(50), unscreened = unscreened,
            sanctionsEnabled = keys.chainalysis.isNotBlank(), historyChecked = keys.ready,
        )
    }
}
