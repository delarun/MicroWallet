// Сети и монеты — как NETS и COINS в host/wallet.py.
package io.microwallet.app.core

class NetConfig(
    val btc: String, val btcApi: String, val btcTxUrl: String, val hrp: String,
    val tronApi: String, val tronTxUrl: String, val tronUsdt: String,
    val bscRpc: String, val bscTxUrl: String, val bscUsdt: String, val bnb: String,
    val prlApi: String, val prlHrp: String, val prl: String,
    // Ethereum — приложение WebAssembly на устройстве (apps/eth)
    val ethRpc: String, val ethTxUrl: String, val ethChain: Long, val eth: String, val ethTokens: List<EvmToken>,
    // TON (приложение apps/ton): GRAM — бывший Toncoin; USDT только в mainnet
    val tonApi: String, val tonTxUrl: String, val tonUsdt: String?, val gram: String,
)

class EvmToken(val asset: String, val symbol: String, val contract: String, val decimals: Int)

/** EVM-сеть: BSC или Ethereum (приложения bsc и eth на устройстве). */
class EvmChain(
    val coin: String, val rpc: String, val chainId: Long, val native: String, val txUrl: String,
    val tokens: List<EvmToken>,
    /** Транзакции EIP-1559 (тип 2); иначе legacy EIP-155. */
    val eip1559: Boolean = false,
)

fun NetConfig.evm(coin: String, bscChain: Long): EvmChain = if (coin == "eth")
    EvmChain("eth", ethRpc, ethChain, eth, ethTxUrl, ethTokens, eip1559 = true)
else
    EvmChain("bsc", bscRpc, bscChain, bnb, bscTxUrl, listOf(EvmToken("usdt", "USDT", bscUsdt, 18)))

val NETS = mapOf(
    "testnet" to NetConfig(
        btc = "tBTC",
        btcApi = "https://mempool.space/testnet4/api",
        btcTxUrl = "https://mempool.space/testnet4/tx/",
        hrp = "tb",
        tronApi = "https://nile.trongrid.io",
        tronTxUrl = "https://nile.tronscan.org/#/transaction/",
        tronUsdt = "TXYZopYRdj2D9XRtbG411XZZ3kM5VkAeBf",
        bscRpc = "https://bsc-testnet-rpc.publicnode.com",
        bscTxUrl = "https://testnet.bscscan.com/tx/",
        bscUsdt = "0x337610d27c682E347C9cD60BD4b3b107C9d34dDd",
        bnb = "tBNB",
        prlApi = "https://blockbook.testnet.pearlresearch.ai",
        prlHrp = "tprl",
        prl = "tPRL",
        ethRpc = "https://ethereum-sepolia-rpc.publicnode.com",
        ethTxUrl = "https://sepolia.etherscan.io/tx/",
        ethChain = 11155111,
        eth = "SepoliaETH",
        ethTokens = listOf(EvmToken("usdc", "USDC", "0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238", 6)),
        tonApi = "https://testnet.toncenter.com/api",
        tonTxUrl = "https://testnet.tonviewer.com/transaction/",
        tonUsdt = null,
        gram = "tGRAM",
    ),
    "mainnet" to NetConfig(
        btc = "BTC",
        btcApi = "https://mempool.space/api",
        btcTxUrl = "https://mempool.space/tx/",
        hrp = "bc",
        tronApi = "https://api.trongrid.io",
        tronTxUrl = "https://tronscan.org/#/transaction/",
        tronUsdt = "TR7NHqjeKQxGTCi8q8ZY4pL8otSzgjLj6t",
        bscRpc = "https://bsc-dataseed.bnbchain.org",
        bscTxUrl = "https://bscscan.com/tx/",
        bscUsdt = "0x55d398326f99059fF775485246999027B3197955",
        bnb = "BNB",
        prlApi = "https://blockbook.pearlresearch.ai",
        prlHrp = "prl",
        prl = "PRL",
        ethRpc = "https://ethereum-rpc.publicnode.com",
        ethTxUrl = "https://etherscan.io/tx/",
        ethChain = 1,
        eth = "ETH",
        ethTokens = listOf(
            EvmToken("usdt", "USDT", "0xdAC17F958D2ee523a2206206994597C13D831ec7", 6),
            EvmToken("usdc", "USDC", "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48", 6),
        ),
        tonApi = "https://toncenter.com/api",
        tonTxUrl = "https://tonviewer.com/transaction/",
        tonUsdt = "EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id_sDs",
        gram = "GRAM",
    ),
)

fun NetConfig.symbol(asset: String): String = when (asset) {
    "usdt" -> "USDT"
    "usdc" -> "USDC"
    "trx" -> "TRX"
    "eth" -> eth
    "btc" -> btc
    "bnb" -> bnb
    "prl" -> prl
    "gram" -> gram
    else -> if (asset.startsWith("trc10:")) "#" + asset.removePrefix("trc10:") else asset.uppercase()
}

class CoinInfo(
    val id: String, val name: String, val assets: List<Pair<String, Int>>, val nets: Map<String, String>,
    val app: Boolean = true,  // с v0.6 все монеты — приложения WebAssembly на устройстве
)

/** Активы монеты в сети: у Ethereum набор токенов зависит от сети. */
fun CoinInfo.assetsFor(net: String): List<Pair<String, Int>> = when {
    id == "eth" -> NETS.getValue(net).ethTokens.map { it.asset to it.decimals } + ("eth" to 18)
    id == "ton" && NETS.getValue(net).tonUsdt == null -> assets.filter { it.first != "usdt" }  // в testnet USDT нет
    else -> assets
}

/** Порядок — как на устройстве и в приложении для ПК. */
val COINS = listOf(
    CoinInfo("btc", "Bitcoin", listOf("btc" to 8), mapOf("testnet" to "testnet4", "mainnet" to "mainnet")),
    CoinInfo("prl", "Pearl", listOf("prl" to 8), mapOf("testnet" to "testnet2", "mainnet" to "mainnet")),
    CoinInfo("tron", "TRON", listOf("usdt" to 6, "trx" to 6), mapOf("testnet" to "Nile", "mainnet" to "mainnet")),
    CoinInfo("bsc", "BNB Chain", listOf("usdt" to 18, "bnb" to 18), mapOf("testnet" to "testnet", "mainnet" to "mainnet")),
    CoinInfo("eth", "Ethereum", listOf("eth" to 18), mapOf("testnet" to "Sepolia", "mainnet" to "mainnet")),
    CoinInfo("ton", "TON", listOf("gram" to 9, "usdt" to 6), mapOf("testnet" to "testnet", "mainnet" to "mainnet")),
)

fun coinInfo(id: String): CoinInfo = COINS.first { it.id == id }

const val CHANGE_MAX = 20    // адреса сдачи идут подряд: проверяем до первого пустого, не дальше
const val SCAN_TTL_MS = 30_000L  // баланс BTC/PRL не чаще раза в 30 с (mempool.space отвечает 429)
const val TX_MAX_IN = 64     // лимит приложений btc/prl (запрос уходит блоком)
const val PRL_COINBASE_MATURITY = 100
const val ADDRS_DEFAULT = 20  // адресов для получения, если прошивка не сообщает addrs= (до 0.6)
val ADDRS_CHOICES = listOf(1, 5, 10, 20)  // настройка устройства «Addresses»
