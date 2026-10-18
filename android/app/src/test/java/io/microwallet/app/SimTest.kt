package io.microwallet.app

import io.microwallet.app.ble.BleDevice
import io.microwallet.app.ble.ChannelError
import io.microwallet.app.ble.MemoryBondStore
import io.microwallet.app.core.DeviceError
import io.microwallet.app.core.Http
import io.microwallet.app.core.SendRequest
import io.microwallet.app.core.Wallet
import io.microwallet.app.core.info
import io.microwallet.app.core.inspectApp
import org.json.JSONObject
import org.json.JSONTokener
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.math.BigDecimal

/** Подменённая сеть: ответы из vectors.json (их записал gen_vectors.py). */
class FakeHttp(private val responses: JSONObject) : Http {
    private fun answer(key: String): Any {
        if (!responses.has(key)) throw AssertionError("нет ответа для $key")
        return JSONTokener(responses.get(key).toString()).nextValue()
    }

    override fun get(url: String) = answer("GET $url")
    override fun getText(url: String) = responses.getString("TEXT $url")
    // как в gen_vectors.py: JSON-RPC — по method, контракты TRON — по function_selector и fee_limit
    override fun post(url: String, body: JSONObject) = answer(
        listOf("POST", url, body.optString("method").ifEmpty { body.optString("function_selector") }, body.optString("fee_limit"))
            .filter { it.isNotEmpty() }.joinToString(" ")
    )
    override fun postRaw(url: String, raw: String) = responses.getString("RAW $url")
    override fun download(url: String, onProgress: (Long, Long) -> Unit) = throw AssertionError("download $url")
}

class SimTest {
    private lateinit var vectors: JSONObject

    @Before
    fun setUp() {
        Sim.require()
        vectors = JSONObject(javaClass.getResource("/vectors.json")!!.readText())
    }

    private fun screenCode(dir: File): String? =
        Regex("""\[screen] Code: (\d{3} \d{3})""").findAll(File(dir, "stderr.txt").readText()).lastOrNull()?.groupValues?.get(1)

    /** Тот же код прошивки по шифрованному каналу: сопряжение, восстановление, повторное подключение, подмена. */
    @Test
    fun channel() {
        val dir = Sim.newDir()
        val bonds = MemoryBondStore()
        val codes = ArrayList<String>()

        // 1. первое подключение: код в приложении совпадает с «экраном»
        var dev = BleDevice("AA:BB", "sim", SimFrames(Sim.start("frames", dir)), bonds) { codes.add(it) }
        assertTrue(dev.channel.pairedNow)
        assertEquals(screenCode(dir), codes.single())
        val w = Wallet(dev, FakeHttp(vectors.getJSONObject("responses")), Sim.words)
        assertFalse(w.ready)

        // 2. восстановление по BLE, установка приложений-монет по каналу, адреса — как у Python-хоста
        assertTrue(w.restore("abandon ".repeat(11) + "about"))
        assertTrue(Sim.apps.size >= 4)
        for (f in Sim.apps) w.installApp(inspectApp(f.readBytes())) { _, _ -> }
        assertEquals(listOf("btc", "prl", "tron", "bsc", "eth", "ton"), w.coins)
        val addrs = vectors.getJSONObject("addresses")
        for (c in listOf("btc", "prl", "tron", "bsc", "ton")) assertEquals(addrs.getString(c), w.address(c))

        // 3. длинная команда через канал: подпись Pearl
        val d = w.prepareSend(SendRequest("prl", w.address("prl", 7), "5.5", feerate = BigDecimal(2)))
        assertEquals(312, d.sign().first.length / 2)
        dev.close()

        // 4. знакомый телефон — без кода
        codes.clear()
        dev = BleDevice("AA:BB", "sim", SimFrames(Sim.start("frames", dir)), bonds) { codes.add(it) }
        assertFalse(dev.channel.pairedNow)
        assertTrue(codes.isEmpty())
        assertEquals("ready", dev.info()["state"])
        dev.close()

        // 5. отказ кнопкой — DeviceError("rejected")
        dev = BleDevice("AA:BB", "sim", SimFrames(Sim.start("frames", dir, mapOf("MW_SIM_ANSWER" to "reject"))), bonds)
        val e = assertThrows(DeviceError::class.java) { dev.call("app btc show 0") }
        assertEquals("rejected", e.message)
        dev.close()

        // 6. устройство «забыло» телефон, а телефон помнит ключ — снова код
        File(dir, "bonds.bin").delete()
        codes.clear()
        dev = BleDevice("AA:BB", "sim", SimFrames(Sim.start("frames", dir)), bonds) { codes.add(it) }
        assertTrue(dev.channel.pairedNow)
        assertEquals(screenCode(dir), codes.single())
        dev.close()
    }

    @Test
    fun tamperedFrameRejected() {
        val dir = Sim.newDir()
        val bonds = MemoryBondStore()
        val frames = SimFrames(Sim.start("frames", dir))
        val dev = BleDevice("AA:BB", "sim", frames, bonds)
        val ch = dev.channel
        val frame = ch.sealForTest(ch.txNext, "info")
        frame[12] = (frame[12].toInt() xor 1).toByte()
        frames.send(frame)
        assertThrows(ChannelError::class.java) { ch.recvLineForTest(5000) }
        dev.close()
    }

    /** Те же транзакции, что собрал и подписал host/wallet.py, — байт в байт. */
    @Test
    fun psbt() {
        // PSBT строит embit, эталон подписи — Python-хост (tools/sim/gen_psbt_vectors.py)
        val v = JSONObject(javaClass.getResource("/psbt.json")!!.readText())
        val dir = Sim.newDir()
        val dev = LineDevice(Sim.start("lines", dir))
        dev.call("restore " + "00".repeat(16))
        val responses = JSONObject()
        val prev = v.getJSONObject("prev")
        val api = io.microwallet.app.core.NETS.getValue("testnet").btcApi
        for (txid in prev.keys()) responses.put("TEXT $api/tx/$txid/hex", prev.getString(txid))
        val w = Wallet(dev, FakeHttp(responses), Sim.words)
        for (f in Sim.apps) w.installApp(inspectApp(f.readBytes())) { _, _ -> }
        val psbt = v.getString("psbt").toByteArray()
        assertEquals("подпись", v.getString("signed"), w.signPsbt(psbt).first)
        val (fin, raw) = w.signPsbt(psbt, finalize = true)
        assertEquals("финализация", v.getString("signed_final"), fin)
        assertEquals("транзакция", v.getString("raw"), raw)
        assertEquals("только witness_utxo", v.getString("signed_wonly"), w.signPsbt(v.getString("psbt_wonly").toByteArray()).first)
        assertEquals("дескриптор", v.getString("descriptor"), w.descriptor())
        dev.close()
    }

    /** Pearl Wallet (btcd DeriveNonStandard): у фразы с энтропией …8e аккаунт PRL выводится иначе. */
    @Test
    fun pearlWallet() {
        for ((entropy, same) in listOf("00".repeat(16) to true, "0".repeat(30) + "8e" to false)) {
            val dev = LineDevice(Sim.start("lines", Sim.newDir()))
            dev.call("restore $entropy")
            assertEquals(entropy, same, Wallet(dev, FakeHttp(JSONObject()), Sim.words).pearlWalletSame())
            dev.close()
        }
    }

    @Test
    fun vectors() {
        val dir = Sim.newDir()
        val dev = LineDevice(Sim.start("lines", dir))
        dev.call("restore " + "00".repeat(16))
        Wallet(dev, FakeHttp(vectors.getJSONObject("responses")), Sim.words).let { setup ->
            for (f in Sim.apps) setup.installApp(inspectApp(f.readBytes())) { _, _ -> }
        }
        val w = Wallet(dev, FakeHttp(vectors.getJSONObject("responses")), Sim.words)
        w.tonValidUntil = { 1800000000 }  // как в gen_vectors.py
        val cases = vectors.getJSONArray("cases")
        for (i in 0 until cases.length()) {
            val c = cases.getJSONObject(i)
            val req = SendRequest(
                coin = c.getString("coin"), dest = c.getString("dest"), amount = c.getString("amount"),
                asset = c.optString("asset").ifEmpty { null },
                feerate = c.optString("feerate").ifEmpty { null }?.let { BigDecimal(it) },
                feeLimit = c.optString("feeLimit").ifEmpty { null },
                memo = c.optString("memo").ifEmpty { null },
                index = c.optInt("index"),
            )
            val d = w.prepareSend(req)
            val r = w.signAndSend(d, broadcast = false)
            val what = "${req.coin} ${req.asset ?: ""}"
            assertEquals("fee $what", c.getString("fee"), d.fee)
            assertEquals("txid $what", c.getString("txid"), r.txid)
            if (c.getBoolean("exact")) assertEquals("raw $what", c.getString("raw"), r.raw)
            else assertEquals("размер $what", c.getString("raw").length, r.raw.length)
            assertFalse(r.broadcast)
            c.optJSONArray("notes")?.let { n -> assertEquals("notes $what", (0 until n.length()).map { n.getString(it) }, d.notes) }
            if (c.has("balance")) {
                val b = w.balance("prl")
                assertEquals(c.getString("balance"), b.assets[0].amount.toString())
                assertEquals(c.getString("pending"), b.pending.toString())
            }
        }
        dev.close()
    }
}
