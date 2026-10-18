package io.microwallet.app.core

/** Устройство: построчные команды (USB или шифрованный канал BLE). */
interface Device {
    val label: String
    /** Команда -> текст после "ok". "err ..." -> DeviceError. Может ждать кнопку. */
    fun call(line: String): String
    fun close()
}

/** "ok ..." -> текст после ok; "err ..." -> DeviceError. */
fun parseReply(resp: String): String {
    if (resp.startsWith("ok")) return if (resp.length > 3) resp.substring(3) else ""
    throw DeviceError(if (resp.startsWith("err ")) resp.substring(4) else resp.ifEmpty { "нет ответа" })
}

fun parseInfo(s: String): Map<String, String> =
    s.split(' ').filter { '=' in it }.associate { it.substringBefore('=') to it.substringAfter('=') }

fun Device.info(): Map<String, String> = parseInfo(call("info"))
