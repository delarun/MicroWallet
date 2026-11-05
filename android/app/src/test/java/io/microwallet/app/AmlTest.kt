package io.microwallet.app

import io.microwallet.app.core.Aml
import io.microwallet.app.core.AmlKeys
import io.microwallet.app.core.WalletError
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/** AML-проверка BSC: скор как в aml-check (app.py) — доля помеченного оборота, санкции. */
class AmlTest {
    private val me = "0x1111111111111111111111111111111111111111"
    private val hot = "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"   // в разметке: high
    private val other = "0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    private val usdt = "0x55d398326f99059fF775485246999027B3197955"
    private val url = Aml.nodeRealUrl("mainnet", "KEY")
    private val labels = Aml.parseLabels(
        JSONObject().put("addresses", JSONArray().put(
            JSONObject().put("address", hot.uppercase().replace("0X", "0x")).put("label", "Exchange X").put("risk", "high").put("source", "test")
        )).toString()
    )

    private fun tr(hash: String, from: String, to: String, usdt: Long) = JSONObject()
        .put("hash", hash).put("from", from).put("to", to)
        .put("value", "0x" + java.math.BigInteger.TEN.pow(18).multiply(usdt.toBigInteger()).toString(16))
        .put("decimal", "0x12").put("blockTimeStamp", 1793700000)

    private fun responses(sanctionedOther: Boolean) = JSONObject()
        .put("POST $url eth_blockNumber", JSONObject().put("result", "0x7d6cde8"))
        // каждое окно и направление получают один и тот же ответ: дубли по hash отбрасываются
        .put("POST $url nr_getAssetTransfers", JSONObject().put("result", JSONObject().put("transfers", JSONArray()
            .put(tr("0x01", hot, me, 100))
            .put(tr("0x02", me, other, 100))
            .put(tr("0x03", me, other, 5).put("receiptsStatus", 0)))))  // не прошёл — не считается
        .put("GET https://public.chainalysis.com/api/v1/address/$me", JSONObject().put("identifications", JSONArray()))
        .put("GET https://public.chainalysis.com/api/v1/address/$hot", JSONObject().put("identifications", JSONArray()))
        .put("GET https://public.chainalysis.com/api/v1/address/$other", JSONObject().put("identifications",
            if (sanctionedOther) JSONArray().put(JSONObject().put("name", "SANCTIONS: Test Exchange")) else JSONArray()))

    @Test
    fun labelsOnly() {
        val r = Aml(FakeHttp(responses(false)), labels).check("mainnet", usdt, me, AmlKeys("KEY", ""), days = 1)
        assertEquals(35, r.score)  // 100 из 200 USDT с меткой high (0,7)
        assertEquals("medium", r.verdict)
        assertEquals(listOf(hot), r.flagged.map { it.address })
        assertEquals(100.0, r.totalIn, 1e-9)
        assertEquals(100.0, r.totalOut, 1e-9)
        assertEquals(3, r.transfers)
        assertFalse(r.sanctionsEnabled)
    }

    @Test
    fun sanctionedCounterparty() {
        val r = Aml(FakeHttp(responses(true)), labels).check("mainnet", usdt, me, AmlKeys("KEY", "CKEY"), days = 1)
        assertEquals(85, r.score)  // (0,7·100 + 1·100) / 200
        assertEquals("high", r.verdict)
        assertEquals("SANCTIONS: Test Exchange", r.flagged.first { it.address == other }.label)
        assertFalse(r.targetSanctioned)
    }

    @Test
    fun targetOnlyWithoutNodeReal() {
        val r = Aml(FakeHttp(JSONObject()), labels).check("mainnet", usdt, hot, AmlKeys("", ""))
        assertFalse(r.historyChecked)
        assertEquals(70, r.score)  // сам адрес в разметке с риском high
        assertEquals("Exchange X", r.targetLabel?.label)
        assertTrue(r.flagged.isEmpty())
    }

    @Test
    fun badAddress() {
        assertThrows(WalletError::class.java) { Aml(FakeHttp(JSONObject()), labels).check("mainnet", usdt, "0x123", AmlKeys("", "")) }
    }
}
