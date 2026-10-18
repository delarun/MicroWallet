// Приложения WebAssembly для устройства: разбор модуля и манифеста (формат — src/apps.h).
package io.microwallet.app.core

class WasmApp(
    val bytes: ByteArray,
    val id: String,
    val name: String,
    val version: String,
    val icon: ByteArray?,     // 8 строк по 8 точек, старший бит слева
    val paths: List<String>,  // разрешённые пути ключей: 44'/60'
) {
    val sha256: String = sha256(bytes).hex()
}

private fun leb(b: ByteArray, pos: IntArray): Long {
    var r = 0L
    var sh = 0
    while (pos[0] < b.size && sh < 35) {
        val x = b[pos[0]++].toInt() and 0xFF
        r = r or ((x and 0x7F).toLong() shl sh)
        if (x and 0x80 == 0) return r
        sh += 7
    }
    throw WalletError("Повреждённый модуль WebAssembly")
}

fun inspectApp(b: ByteArray): WasmApp {
    val magic = byteArrayOf(0, 0x61, 0x73, 0x6d, 1, 0, 0, 0)
    if (b.size < 8 || !b.copyOfRange(0, 8).contentEquals(magic)) throw WalletError("Это не приложение (модуль WebAssembly)")
    if (b.size > 65024) throw WalletError("Приложение больше 63.5 КБ — на устройство не поместится")
    val pos = intArrayOf(8)
    var manifest: String? = null
    while (pos[0] < b.size) {
        val id = b[pos[0]++].toInt()
        val size = leb(b, pos)
        val end = pos[0] + size
        if (end > b.size) throw WalletError("Повреждённый модуль WebAssembly")
        if (id == 0) {
            val p = intArrayOf(pos[0])
            val nl = leb(b, p).toInt()
            if (String(b, p[0], nl) == "microwallet") manifest = String(b, p[0] + nl, (end - p[0] - nl).toInt())
        }
        pos[0] = end.toInt()
    }
    val m = (manifest ?: throw WalletError("В модуле нет манифеста MicroWallet")).lines()
        .filter { '=' in it }.map { it.substringBefore('=') to it.substringAfter('=') }
    fun one(k: String) = m.firstOrNull { it.first == k }?.second ?: throw WalletError("В манифесте нет $k")
    return WasmApp(
        bytes = b,
        id = one("id"),
        name = one("name"),
        version = one("version"),
        icon = m.firstOrNull { it.first == "icon" }?.second?.let { runCatching { it.unhex() }.getOrNull() },
        paths = m.filter { it.first == "path" }.map { it.second },
    )
}

/** Файлы приложений в релизе: microwallet-app-<id>-<версия>.wasm */
fun isAppAsset(name: String) = name.startsWith("microwallet-app-") && name.endsWith(".wasm")
