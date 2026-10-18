package io.microwallet.app.core

import java.math.BigDecimal
import java.math.BigInteger
import kotlin.random.Random

/** Тот же интерфейс, что у Wallet, но без устройства и сети (как --demo в приложении для ПК). */
class DemoWallet(empty: Boolean = false, private val wordlist: List<String> = emptyList()) : WalletApi {
    override var info = mapOf(
        "fw" to "0.5", "net" to "testnet", "state" to if (empty) "empty" else "ready",
        "coins" to "btc,tron,bsc,prl", "bsc_chain" to "97", "hw" to "esp32s3", "batt" to "3920",
        "addrs" to "20",
    )
        private set
    override val net = "testnet"
    override val coins = listOf("btc", "prl", "tron", "bsc", "eth")
    override val ready get() = info["state"] == "ready"
    override val label = "Демо"

    private val addrs = mapOf(
        "btc" to "tb1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu",
        "tron" to "TUEZSdKsoDHQMeZwihtdoBiN46zxhGWYdH",
        "bsc" to "0x9858EfFD232B4033E47d90003D41EC34EcaEda94",
        "eth" to "0x9858EfFD232B4033E47d90003D41EC34EcaEda94",
        "prl" to "tprl1pr6yuq8u2r95wjzzgpdy8cpnncpl7l8zgy6x5q0367pnc53s2famqg7pt74",
    )

    override fun address(coin: String, index: Int, chain: Int): String {
        Thread.sleep(150)
        val a = addrs.getValue(coin)
        return if (index == 0) a else a.dropLast(4) + "%04d".format(index)
    }

    override fun showAddress(coin: String, index: Int): String {
        Thread.sleep(2500)
        return address(coin, index)
    }

    override fun restore(phrase: String): Boolean {
        if (wordlist.isNotEmpty()) mnemonicToEntropy(phrase, wordlist)
        Thread.sleep(2500)
        info = info + ("state" to "ready")
        return true
    }

    override fun balance(coin: String): Balance {
        Thread.sleep(Random.nextLong(400, 1200))
        val e18 = BigInteger.TEN.pow(18)
        val assets = when (coin) {
            "btc" -> listOf(Asset("btc", "tBTC", 8, BigInteger.valueOf(4_120_000)))
            "prl" -> listOf(Asset("prl", "tPRL", 8, BigInteger.valueOf(451_069_071_991)))
            "tron" -> listOf(
                Asset("usdt", "USDT", 6, BigInteger.valueOf(250_000_000)),
                Asset("trx", "TRX", 6, BigInteger.valueOf(120_500_000)),
            )
            "eth" -> listOf(
                Asset("usdc", "USDC", 6, BigInteger.valueOf(40_000_000)),
                Asset("eth", "SepoliaETH", 18, e18.divide(BigInteger.valueOf(20))),
            )
            else -> listOf(
                Asset("usdt", "USDT", 18, e18.multiply(BigInteger.valueOf(75))),
                Asset("bnb", "tBNB", 18, e18.divide(BigInteger.valueOf(100)).multiply(BigInteger.valueOf(3))),
            )
        }
        val pending = if (coin == "prl") BigInteger.valueOf(225_390_153_581) else BigInteger.ZERO
        return Balance(addrs.getValue(coin), assets, pending)
    }

    override fun feeLevels(coin: String): Map<String, BigDecimal> {
        Thread.sleep(300)
        return if (coin == "prl") mapOf("fast" to BigDecimal(1164), "normal" to BigDecimal("1.4"), "slow" to BigDecimal.ONE)
        else mapOf("fast" to BigDecimal(6), "normal" to BigDecimal(3), "slow" to BigDecimal.ONE)
    }

    override fun prepareSend(req: SendRequest): Draft {
        Thread.sleep(800)
        when (req.coin) {
            "btc" -> addressToScript(req.dest.trim(), "tb")
            "prl" -> addressToScript(req.dest.trim(), "tprl", true)
            "tron" -> tronDecode(req.dest.trim())
            "eth" -> evmDecode(req.dest.trim())
            else -> evmDecode(req.dest.trim())
        }
        val asset = req.asset ?: coinInfo(req.coin).assetsFor(net)[0].first
        val (fee, feeSym) = when (req.coin) {
            "btc" -> "0.00000424" to "tBTC"
            "prl" -> "0.00246824" to "tPRL"
            "tron" -> (req.feeLimit ?: "40") to "TRX"
            "eth" -> "0.000042" to "SepoliaETH"
            else -> "0.00000612" to "tBNB"
        }
        return Draft(
            req.coin, asset, symbol(asset), addrs.getValue(req.coin), req.dest.trim(), req.amount, fee, feeSym,
            feeLabel = if (req.coin in setOf("tron", "bsc", "eth")) "Макс. комиссия" else "Комиссия",
            notes = listOf("Демо-режим: устройство не подключено"),
            sign = {
                Thread.sleep(3500)
                "00".repeat(120) to ByteArray(32).also { Random.nextBytes(it) }.hex()
            },
            broadcast = { Thread.sleep(500) },
            txUrl = "https://example.invalid/tx/",
        )
    }

    override fun updateFirmware(img: FwImage, onProgress: (Int, Int) -> Unit) {
        Thread.sleep(2500)
        val n = img.bytes.size
        for (off in 0..n step 16 * 1024) {
            Thread.sleep(40)
            onProgress(minOf(off, n), n)
        }
        onProgress(n, n)
    }

    private var demoApps = mutableListOf(AppInfo("eth", "1.0", "Ethereum"))
    override fun apps(): List<AppInfo> = demoApps.toList()
    override fun installApp(app: WasmApp, onProgress: (Int, Int) -> Unit) {
        Thread.sleep(2000)
        onProgress(app.bytes.size, app.bytes.size)
        demoApps.removeAll { it.id == app.id }
        demoApps.add(AppInfo(app.id, app.version, app.name))
    }
    override fun removeApp(id: String) {
        Thread.sleep(1500)
        demoApps.removeAll { it.id == id }
    }

    override fun setAddrCount(n: Int) {
        info = info + ("addrs" to n.toString())
    }

    override fun close() {}
}
