// Прошивка: релизы (GitHub или tools/fw_server.py с ПК — формат один) и разбор образа ESP32.
package io.microwallet.app.core

import org.json.JSONArray

data class FwAsset(val name: String, val url: String, val size: Long)

data class FwRelease(
    val tag: String,
    val name: String,
    val notes: String,
    val date: String,          // "2026-10-17"
    val prerelease: Boolean,
    val assets: List<FwAsset>,
)

/** Образ приложения ESP32 (*-app.bin) и сведения из его заголовка. */
class FwImage(
    val bytes: ByteArray,
    val chip: String,       // плата (как hw в info): esp32s3, esp32s3-ws147; чужая — ставить нельзя
    val version: String,
    val built: String,      // "Oct 17 2026 18:45:12"
    val buildId: String,    // начало SHA-256 ELF — как build= в info устройства
) {
    val sha256: String = sha256(bytes).hex()
}

/** Репозиторий проекта: отсюда по умолчанию берутся прошивки и приложения. */
const val DEFAULT_REPO = "delarun/MicroWallet"

/** Источник: «owner/repo», ссылка на github.com или адрес сервера с ПК (http://localhost:8777). */
fun releasesUrl(source: String): String {
    val s = source.trim().removeSuffix("/")
    val gh = Regex("""^(?:https?://github\.com/)?([\w.-]+)/([\w.-]+?)(?:\.git)?$""").find(s)
    return when {
        gh != null && !s.startsWith("http://") && (s.startsWith("https://github.com/") || !s.contains("://")) ->
            "https://api.github.com/repos/${gh.groupValues[1]}/${gh.groupValues[2]}/releases"
        s.startsWith("http://") || s.startsWith("https://") -> if (s.endsWith("/releases")) s else "$s/releases"
        else -> throw WalletError("Укажи репозиторий как owner/repo или адрес сервера http://…")
    }
}

fun parseReleases(a: JSONArray): List<FwRelease> = (0 until a.length()).map { a.getJSONObject(it) }
    .filter { !it.optBoolean("draft") }
    .map { r ->
        val assets = r.optJSONArray("assets") ?: JSONArray()
        FwRelease(
            tag = r.optString("tag_name"),
            name = r.optString("name").ifBlank { r.optString("tag_name") },
            notes = r.optString("body"),
            date = r.optString("published_at").take(10),
            prerelease = r.optBoolean("prerelease"),
            assets = (0 until assets.length()).map { assets.getJSONObject(it) }.map {
                FwAsset(it.getString("name"), it.getString("browser_download_url"), it.optLong("size"))
            },
        )
    }

/** Имя файла прошивки в релизе для этого устройства. Плате с Secure Boot (info secure=1)
 *  нужна своя сборка: подписанная и с другой таблицей разделов. */
fun fwAssetName(hw: String, net: String, secure: Boolean = false) =
    "microwallet-$hw-$net${if (secure) "-secure" else ""}-app.bin"

private fun cstr(b: ByteArray, off: Int, len: Int): String {
    if (b.size < off + len) return ""
    val end = (off until off + len).firstOrNull { b[it].toInt() == 0 } ?: (off + len)
    return String(b, off, end - off, Charsets.US_ASCII)
}

private fun le32(b: ByteArray, off: Int) =
    (b[off].toLong() and 0xFF) or ((b[off + 1].toLong() and 0xFF) shl 8) or
        ((b[off + 2].toLong() and 0xFF) shl 16) or ((b[off + 3].toLong() and 0xFF) shl 24)

/** Для какой платы образ: метка «mw-board:<hw>;» (с v0.9) — esp32s3 или esp32s3-ws147 (чип у них
 *  один); без метки — по чипу из заголовка: старые образы только для SuperMini (hw=esp32s3). */
fun imageBoard(b: ByteArray, chip: String): String {
    val tag = "mw-board:".toByteArray()
    for (i in 0..b.size - tag.size) {
        var hit = true
        for (k in tag.indices) if (b[i + k] != tag[k]) { hit = false; break }
        if (!hit) continue
        val end = (i + tag.size until minOf(b.size, i + tag.size + 40)).firstOrNull { b[it] == ';'.code.toByte() } ?: return chip
        return String(b, i + tag.size, end - i - tag.size, Charsets.US_ASCII)
    }
    return chip
}

fun inspectImage(b: ByteArray): FwImage {
    if (b.size < 256 || b[0] != 0xE9.toByte()) throw WalletError("Это не образ прошивки ESP32 (нужен *-app.bin)")
    val chipId = (b[12].toInt() and 0xFF) or ((b[13].toInt() and 0xFF) shl 8)
    val chip = mapOf(5 to "esp32c3", 9 to "esp32s3")[chipId] ?: "?"
    // esp_app_desc_t — в начале первого сегмента: заголовок образа 24 байта + сегмента 8
    val d = 32
    if (le32(b, d) != 0xABCD5432L) throw WalletError("В образе нет описания приложения ESP-IDF")
    if (cstr(b, d + 48, 32) != "microwallet") throw WalletError("Это прошивка не MicroWallet (${cstr(b, d + 48, 32)})")
    return FwImage(
        bytes = b,
        chip = imageBoard(b, chip),
        version = cstr(b, d + 16, 32),
        built = "${cstr(b, d + 96, 16)} ${cstr(b, d + 80, 16)}",
        buildId = b.copyOfRange(d + 144, d + 148).hex(),
    )
}
