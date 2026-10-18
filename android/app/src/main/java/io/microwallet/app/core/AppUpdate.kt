// Обновление самого приложения из релизов (GitHub или сервер с ПК — формат один).
// В релизе рядом с microwallet-android.apk лежит microwallet-android.json:
//   {"versionCode": 5, "versionName": "0.6.1", "sha256": "…"}
// — по нему видно, новее ли сборка, не скачивая её.
package io.microwallet.app.core

import org.json.JSONArray
import org.json.JSONObject

const val APK_ASSET = "microwallet-android.apk"
const val APK_META = "microwallet-android.json"

data class AppUpdate(
    val versionCode: Long,
    val versionName: String,
    val sha256: String,
    val apkUrl: String,
    val size: Long,
    val tag: String,
)

/**
 * Самая свежая сборка приложения в релизах источника, если она новее current; иначе null.
 * Тестовые релизы GitHub (prerelease) пропускаются; у сервера с ПК релиз один — берётся он.
 */
fun findAppUpdate(http: Http, source: String, current: Long): AppUpdate? {
    val github = releasesUrl(source).startsWith("https://api.github.com/")
    val rel = parseReleases(http.get(releasesUrl(source)) as JSONArray)
        .filter { !(github && it.prerelease) }
        .firstOrNull { r -> r.assets.any { it.name == APK_META } && r.assets.any { it.name == APK_ASSET } }
        ?: return null
    val meta = JSONObject(String(http.download(rel.assets.first { it.name == APK_META }.url)))
    val code = meta.getLong("versionCode")
    if (code <= current) return null
    val apk = rel.assets.first { it.name == APK_ASSET }
    return AppUpdate(code, meta.optString("versionName", rel.tag), meta.getString("sha256").lowercase(), apk.url, apk.size, rel.tag)
}

/** Скачать APK и сверить SHA-256 с описанием релиза. */
fun downloadAppUpdate(http: Http, u: AppUpdate, onProgress: (Long, Long) -> Unit = { _, _ -> }): ByteArray {
    val apk = http.download(u.apkUrl, onProgress)
    if (sha256(apk).hex() != u.sha256) throw WalletError("Обновление скачалось повреждённым: SHA-256 не совпал")
    return apk
}
