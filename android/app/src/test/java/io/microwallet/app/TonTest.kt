package io.microwallet.app

import io.microwallet.app.core.Ton
import io.microwallet.app.core.WalletError
import io.microwallet.app.core.hex
import io.microwallet.app.core.unhex
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.File
import java.math.BigInteger

/** TON без симулятора: адреса W5, адрес кошелька USDT и подписываемые ячейки — против эталонов
 *  официальной @ton/ton (tools/sim/ton_vectors.json, их же проверяет устройство в test_ton.py). */
class TonTest {
    private val v = JSONObject(File("../../tools/sim/ton_vectors.json").readText())
    private val dest = Ton.Addr(0, v.getString("dest").substringAfter(':').unhex())

    @Test
    fun addresses() {
        for (net in listOf("testnet", "mainnet")) {
            val n = v.getJSONObject("nets").getJSONObject(net)
            val tn = net == "testnet"
            for (i in 0 until 3) {
                val a = Ton.w5Address(n.getJSONArray("pubkeys").getString(i).unhex(), tn)
                assertEquals("$net #$i", n.getJSONArray("addrs").getString(i), Ton.friendly(a, false, tn))
                assertEquals(a, Ton.parseAddress(Ton.friendly(a, true, tn), tn).first)
            }
            assertThrows(WalletError::class.java) { Ton.parseAddress(n.getJSONArray("addrs").getString(0), !tn) }
        }
        assertThrows(WalletError::class.java) { Ton.parseAddress("UQCr0pJvwmgWeI7Wu0TaRn77bD0m7JkkbnGRCxRUxhbyvbdT", false) }
    }

    @Test
    fun signingMessages() {
        for (net in listOf("testnet", "mainnet")) {
            val n = v.getJSONObject("nets").getJSONObject(net)
            val tn = net == "testnet"
            val own = Ton.w5Address(n.getJSONArray("pubkeys").getString(0).unhex(), tn)
            val cases = n.getJSONArray("cases").let { a -> (0 until a.length()).associate { a.getJSONObject(it).getString("name") to a.getJSONObject(it) } }
            fun check(name: String, msg: Ton.Cell) {
                val c = cases.getValue(name)
                assertEquals("$net $name", c.getString("hash"), Ton.signingMessage(tn, 7, 1800000000, msg).hash.hex())
                assertEquals("$net $name BoC", c.getString("hash"), Ton.fromBoc(c.getString("boc").unhex()).hash.hex())
            }
            check("gram", Ton.internal(dest, BigInteger.valueOf(1_500_000_000), false))
            check("gram_comment", Ton.internal(dest, BigInteger.valueOf(250_000_000), true, Ton.commentCell("invoice 42")))
            check("gram_long_comment", Ton.internal(dest, BigInteger.ONE, false, Ton.commentCell("x".repeat(96))))
            if (!tn) {
                val master = Ton.parseAddress("EQCxE6mUtQJKFnGfaROTKOt1lZbDiiX1kCixRv7Nw2Id_sDs", false).first
                val jw = Ton.usdtWallet(own, master)
                assertEquals(n.getString("usdt_wallet0"), Ton.rawStr(jw))
                val attach = BigInteger.valueOf(Ton.USDT_ATTACH)
                check("usdt", Ton.internal(jw, attach, true, Ton.jettonTransfer(BigInteger.valueOf(12_500_000), dest, own, null)))
                check("usdt_comment", Ton.internal(jw, attach, true, Ton.jettonTransfer(BigInteger.valueOf(1_000_000), dest, own, "for coffee")))
            }
        }
    }
}
