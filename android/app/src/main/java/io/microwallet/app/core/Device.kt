package io.microwallet.app.core

/** Устройство: построчные команды (USB или шифрованный канал BLE). */
interface Device {
    val label: String
    /** Команда -> текст после "ok". "err ..." -> DeviceError. Может ждать кнопку. */
    fun call(line: String): String
    /** Строки подряд, до window в полёте; ответы по порядку. Первый «err» — DeviceError. */
    fun stream(lines: List<String>, window: Int = 4, onReply: (Int) -> Unit = {}) {
        lines.forEachIndexed { i, l -> call(l); onReply(i + 1) }
    }
    fun close()
}

const val LOCKED_MSG = "Устройство заблокировано — введи PIN на нём кнопкой"

/** "ok ..." -> текст после ok; "err ..." -> DeviceError. */
fun parseReply(resp: String): String {
    if (resp.startsWith("ok")) return if (resp.length > 3) resp.substring(3) else ""
    if (resp == "err locked") throw DeviceError(LOCKED_MSG)  // после включения устройство ждёт PIN
    throw DeviceError(if (resp.startsWith("err ")) resp.substring(4) else resp.ifEmpty { "нет ответа" })
}

fun parseInfo(s: String): Map<String, String> =
    s.split(' ').filter { '=' in it }.associate { it.substringBefore('=') to it.substringAfter('=') }

fun Device.info(): Map<String, String> = parseInfo(call("info"))

private const val CHUNK = 2048  // байт в одной строке app put b64 / app get

/**
 * Команда приложению. blob — большой вход: уходит заранее блоком (app put), к команде
 * добавляется « @». Ответ длиннее строки приходит как «@<размер> <sha256>» — забираем кусками.
 */
fun Device.appCall(cmd: String, blob: ByteArray? = null): String {
    val b64 = java.util.Base64.getEncoder()
    var line = cmd
    if (blob != null) {
        call("app put ${blob.size} ${sha256(blob).hex()}")
        for (off in blob.indices step CHUNK)
            call("app put b64 " + b64.encodeToString(blob.copyOfRange(off, minOf(off + CHUNK, blob.size))))
        call("app put end")
        line += " @"
    }
    val r = call(line)
    if (!r.startsWith("@")) return r
    val (size, sha) = r.substring(1).split(' ').let { it[0].toInt() to it[1] }
    val out = java.io.ByteArrayOutputStream(size)
    while (out.size() < size) {
        val part = java.util.Base64.getDecoder().decode(call("app get ${out.size()}"))
        if (part.isEmpty()) break
        out.write(part)
    }
    val data = out.toByteArray()
    if (data.size != size || sha256(data).hex() != sha) throw DeviceError("длинный ответ пришёл повреждённым")
    return String(data)
}
