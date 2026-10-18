package io.microwallet.app

import io.microwallet.app.core.APK_ASSET
import io.microwallet.app.core.APK_META
import io.microwallet.app.core.Http
import io.microwallet.app.core.WalletError
import io.microwallet.app.core.downloadAppUpdate
import io.microwallet.app.core.findAppUpdate
import io.microwallet.app.core.hex
import io.microwallet.app.core.sha256
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Test

/** Обновление приложения из релизов: какая версия новее, тестовые релизы, порча APK. */
class AppUpdateTest {
    private val apk = ByteArray(5000) { (it * 7).toByte() }

    private fun release(tag: String, code: Int, pre: Boolean = false, sha: String = sha256(apk).hex()) = JSONObject()
        .put("tag_name", tag).put("name", tag).put("prerelease", pre).put("published_at", "2026-10-18T00:00:00Z")
        .put("assets", JSONArray()
            .put(JSONObject().put("name", APK_META).put("size", 80).put("browser_download_url", "https://x/$tag/meta"))
            .put(JSONObject().put("name", APK_ASSET).put("size", apk.size).put("browser_download_url", "https://x/$tag/apk")))
        .also { metas[tag] = JSONObject().put("versionCode", code).put("versionName", tag.removePrefix("v")).put("sha256", sha) }

    private val metas = HashMap<String, JSONObject>()

    private fun http(rels: JSONArray) = object : Http {
        override fun get(url: String): Any {
            assertEquals("https://api.github.com/repos/delarun/MicroWallet/releases", url)
            return rels
        }
        override fun getText(url: String) = throw AssertionError(url)
        override fun post(url: String, body: JSONObject) = throw AssertionError(url)
        override fun postRaw(url: String, raw: String) = throw AssertionError(url)
        override fun download(url: String, onProgress: (Long, Long) -> Unit): ByteArray {
            val tag = url.split('/')[3]
            return if (url.endsWith("/meta")) metas.getValue(tag).toString().toByteArray() else apk
        }
    }

    @Test
    fun newerVersionFound() {
        val rels = JSONArray().put(release("v0.8.0", 9, pre = true)).put(release("v0.7.0", 6)).put(release("v0.6.0", 4))
        val u = findAppUpdate(http(rels), "delarun/MicroWallet", 5)!!
        assertEquals(6L, u.versionCode)  // тестовый релиз GitHub пропущен
        assertEquals("v0.7.0", u.tag)
        assertEquals(apk.size, downloadAppUpdate(http(rels), u).size)
    }

    @Test
    fun sameOrOlderIsNotAnUpdate() {
        val rels = JSONArray().put(release("v0.7.0", 6))
        assertNull(findAppUpdate(http(rels), "delarun/MicroWallet", 6))
        assertNull(findAppUpdate(http(rels), "delarun/MicroWallet", 7))
    }

    @Test
    fun corruptedApkRejected() {
        val rels = JSONArray().put(release("v0.7.0", 6, sha = "00".repeat(32)))
        val u = findAppUpdate(http(rels), "delarun/MicroWallet", 5)!!
        assertThrows(WalletError::class.java) { downloadAppUpdate(http(rels), u) }
    }
}
