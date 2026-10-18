package io.microwallet.app.core

import java.math.BigInteger

/** 12/18/24 слова -> энтропия; проверяет словарь и контрольную сумму (как в wallet.py). */
fun mnemonicToEntropy(phrase: String, wordlist: List<String>): ByteArray {
    val words = phrase.lowercase().split(Regex("\\s+")).filter { it.isNotEmpty() }
    if (words.size !in setOf(12, 18, 24)) throw WalletError("Нужно 12, 18 или 24 слова, а введено ${words.size}")
    val index = wordlist.withIndex().associate { it.value to it.index }
    val bad = words.filter { it !in index }
    if (bad.isNotEmpty()) throw WalletError("Слов нет в словаре BIP39: ${bad.joinToString(", ")}")
    val bits = words.joinToString("") { Integer.toBinaryString(index.getValue(it)).padStart(11, '0') }
    val entBits = bits.length * 32 / 33
    val ent = BigInteger(bits.substring(0, entBits), 2).toBytes(entBits / 8)
    val chk = Integer.toBinaryString(sha256(ent)[0].toInt() and 0xFF).padStart(8, '0')
        .substring(0, bits.length - entBits)
    if (bits.substring(entBits) != chk) throw WalletError("Неверная контрольная сумма фразы — проверь слова и их порядок")
    return ent
}
