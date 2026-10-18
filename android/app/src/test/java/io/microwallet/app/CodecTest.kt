package io.microwallet.app

import io.microwallet.app.core.WalletError
import io.microwallet.app.core.addressToScript
import io.microwallet.app.core.evmChecksum
import io.microwallet.app.core.evmDecode
import io.microwallet.app.core.fmtUnits
import io.microwallet.app.core.hex
import io.microwallet.app.core.mnemonicToEntropy
import io.microwallet.app.core.rlp
import io.microwallet.app.core.toUnits
import io.microwallet.app.core.tronDecode
import io.microwallet.app.core.unhex
import io.microwallet.app.ui.parsePaymentUri
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.File
import java.math.BigInteger

class CodecTest {
    @Test
    fun bech32() {
        // BIP173 / BIP350
        assertEquals(
            "0014751e76e8199196d454941c45d1b3a323f1433bd6",
            addressToScript("BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4", "bc").hex(),
        )
        assertEquals(
            "00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262",
            addressToScript("tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7", "tb").hex(),
        )
        assertEquals(
            "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
            addressToScript("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0", "bc").hex(),
        )
        // bech32m-адрес с bech32-контрольной суммой — ошибка
        assertThrows(WalletError::class.java) {
            addressToScript("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqh2y7hd", "bc")
        }
        assertThrows(WalletError::class.java) { addressToScript("tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7", "bc") }
        // Pearl: только Taproot
        assertThrows(WalletError::class.java) {
            addressToScript("tprl1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx", "tprl", pearl = true)
        }
    }

    @Test
    fun tronAndEvm() {
        assertEquals("41", tronDecode("TUEZSdKsoDHQMeZwihtdoBiN46zxhGWYdH").copyOfRange(0, 1).hex())
        assertThrows(WalletError::class.java) { tronDecode("TUEZSdKsoDHQMeZwihtdoBiN46zxhGWYdJ") }
        val a = "0x9858EfFD232B4033E47d90003D41EC34EcaEda94"
        assertEquals(a, evmChecksum(evmDecode(a)))
        assertEquals(a, evmChecksum(evmDecode(a.lowercase())))
        assertThrows(WalletError::class.java) { evmDecode("0x9858efFD232B4033E47d90003D41EC34EcaEda94") }
    }

    @Test
    fun rlpVectors() {
        assertEquals("83646f67", rlp("dog".toByteArray()).hex())
        assertEquals("c88363617483646f67", rlp(listOf("cat".toByteArray(), "dog".toByteArray())).hex())
        assertEquals("80", rlp(0).hex())
        assertEquals("0f", rlp(15).hex())
        assertEquals("820400", rlp(1024).hex())
        assertEquals("c0", rlp(emptyList<Any>()).hex())
        val long = ByteArray(56) { 0x61 }
        assertEquals("b838" + long.hex(), rlp(long).hex())
    }

    @Test
    fun units() {
        assertEquals(BigInteger.valueOf(150_000_000), toUnits("1.5", 8))
        assertEquals(BigInteger.valueOf(150_000_000), toUnits("1,5", 8))
        assertEquals(BigInteger.TEN.pow(18).multiply(BigInteger.valueOf(12)).add(BigInteger.TEN.pow(17).multiply(BigInteger.valueOf(5))), toUnits("12.5", 18))
        assertThrows(WalletError::class.java) { toUnits("0.000000001", 8) }
        assertThrows(WalletError::class.java) { toUnits("0", 8) }
        assertThrows(WalletError::class.java) { toUnits("abc", 8) }
        assertEquals("1.5", fmtUnits(150_000_000L, 8))
        assertEquals("0", fmtUnits(0L, 8))
        assertEquals("100", fmtUnits(BigInteger.TEN.pow(20), 18))
        assertEquals("0.00000001", fmtUnits(1L, 8))
    }

    @Test
    fun bip39() {
        val words = File(System.getProperty("mw.words")).readLines().filter { it.isNotBlank() }
        assertEquals("00".repeat(16), mnemonicToEntropy("abandon ".repeat(11) + "about", words).hex())
        assertEquals(
            "7f".repeat(16),
            mnemonicToEntropy("legal winner thank year wave sausage worth useful legal winner thank yellow", words).hex(),
        )
        assertEquals(
            "ff".repeat(32),
            mnemonicToEntropy("zoo ".repeat(23) + "vote", words).hex(),
        )
        assertThrows(WalletError::class.java) { mnemonicToEntropy("abandon ".repeat(12), words) }
        assertThrows(WalletError::class.java) { mnemonicToEntropy("abandon ".repeat(11) + "abcd", words) }
    }

    @Test
    fun paymentUri() {
        assertEquals("tb1qxyz" to "0.5", parsePaymentUri("bitcoin:tb1qxyz?amount=0.5&label=x"))
        assertEquals("TUEZSdKsoDHQMeZwihtdoBiN46zxhGWYdH" to null, parsePaymentUri(" TUEZSdKsoDHQMeZwihtdoBiN46zxhGWYdH "))
        assertEquals("0xabc" to null, parsePaymentUri("ethereum:0xabc@56"))
    }

    @Test
    fun hexRoundTrip() {
        assertEquals("00ff10", "00ff10".unhex().hex())
        assertThrows(WalletError::class.java) { "0g".unhex() }
    }
}
