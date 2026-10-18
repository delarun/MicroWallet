package io.microwallet.app.core

import org.json.JSONObject
import org.json.JSONTokener
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/** Сеть. JSON-ответы — JSONObject или JSONArray. В тестах подменяется. */
interface Http {
    fun get(url: String): Any
    /** GET с текстовым ответом (сырая транзакция в hex у mempool.space). */
    fun getText(url: String): String
    fun post(url: String, body: JSONObject): Any
    /** POST сырого hex (mempool, Blockbook); ответ — текст. */
    fun postRaw(url: String, raw: String): String
    /** Скачать файл (прошивку). onProgress(получено, всего; всего = -1, если неизвестно). */
    fun download(url: String, onProgress: (Long, Long) -> Unit = { _, _ -> }): ByteArray
}

private const val RETRIES = 3

class UrlHttp : Http {
    private fun host(url: String) = url.split('/').getOrElse(2) { url }

    /** Код, текст и Retry-After (секунды, -1 — нет заголовка). */
    private class Reply(val code: Int, val text: String, val retryAfter: Int)

    /** 429/503 — сервер просит подождать (mempool.space ограничивает частоту): ждём,
     *  сколько он скажет в Retry-After (не больше 30 с), иначе 2, 4, 8 с, и повторяем. */
    private fun requestRetrying(url: String, body: String?, json: Boolean): Reply {
        var attempt = 0
        while (true) {
            val r = request(url, body, json)
            if (r.code !in setOf(429, 503) || attempt == RETRIES) return r
            val wait = if (r.retryAfter > 0) minOf(r.retryAfter, 30) else 2 shl attempt
            Thread.sleep(wait * 1000L)
            attempt++
        }
    }

    private fun request(url: String, body: String?, json: Boolean): Reply {
        val c = URL(url).openConnection() as HttpURLConnection
        try {
            c.connectTimeout = 20_000
            c.readTimeout = 20_000
            c.setRequestProperty("User-Agent", "microwallet-android/0.5")
            if (json) c.setRequestProperty("Content-Type", "application/json")
            if (body != null) {
                c.requestMethod = "POST"
                c.doOutput = true
                c.outputStream.use { it.write(body.toByteArray()) }
            }
            val code = c.responseCode
            val stream = if (code in 200..299) c.inputStream else c.errorStream
            val text = stream?.bufferedReader()?.use { it.readText() } ?: ""
            return Reply(code, text, c.getHeaderField("Retry-After")?.trim()?.toIntOrNull() ?: -1)
        } finally {
            c.disconnect()
        }
    }

    private fun json(url: String, body: JSONObject?): Any {
        val r = try {
            requestRetrying(url, body?.toString(), true)
        } catch (e: IOException) {
            throw WalletError("Нет связи с ${host(url)}: ${e.message}")
        }
        val code = r.code
        val text = r.text
        if (code == 429) throw WalletError("${host(url)}: слишком много запросов, попробуй через минуту")
        if (code !in 200..299) throw WalletError("HTTP $code от $url: ${text.take(300)}")
        return try {
            JSONTokener(text).nextValue()
        } catch (e: Exception) {
            throw WalletError("Непонятный ответ от ${host(url)}")
        }
    }

    override fun get(url: String) = json(url, null)

    override fun getText(url: String): String {
        val r = try {
            requestRetrying(url, null, false)
        } catch (e: IOException) {
            throw WalletError("Нет связи с ${host(url)}: ${e.message}")
        }
        if (r.code == 429) throw WalletError("${host(url)}: слишком много запросов, попробуй через минуту")
        if (r.code !in 200..299) throw WalletError("HTTP ${r.code} от $url: ${r.text.take(300)}")
        return r.text.trim()
    }
    override fun post(url: String, body: JSONObject) = json(url, body)

    override fun download(url: String, onProgress: (Long, Long) -> Unit): ByteArray {
        try {
            val c = URL(url).openConnection() as HttpURLConnection
            try {
                c.connectTimeout = 20_000
                c.readTimeout = 60_000
                c.setRequestProperty("User-Agent", "microwallet-android/0.5")
                c.setRequestProperty("Accept", "application/octet-stream")
                val code = c.responseCode
                if (code !in 200..299) throw WalletError("HTTP $code при скачивании ${url.substringAfterLast('/')}")
                val total = c.contentLengthLong
                val out = java.io.ByteArrayOutputStream(if (total > 0) total.toInt() else 1 shl 20)
                val buf = ByteArray(16 * 1024)
                c.inputStream.use { inp ->
                    while (true) {
                        val n = inp.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        if (out.size() > 8 shl 20) throw WalletError("Файл слишком большой для прошивки")
                        onProgress(out.size().toLong(), total)
                    }
                }
                return out.toByteArray()
            } finally {
                c.disconnect()
            }
        } catch (e: IOException) {
            throw WalletError("Нет связи с ${host(url)}: ${e.message}")
        }
    }

    override fun postRaw(url: String, raw: String): String {
        val r = try {
            requestRetrying(url, raw, false)
        } catch (e: IOException) {
            throw WalletError("Нет связи с ${host(url)}: ${e.message}")
        }
        if (r.code == 429) throw WalletError("${host(url)}: слишком много запросов, попробуй через минуту")
        if (r.code !in 200..299) throw WalletError("Сеть отклонила транзакцию: ${r.text.take(300)}")
        return r.text
    }
}
