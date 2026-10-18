package io.microwallet.app.core

/**
 * Диагностический журнал (в приложении — logcat, тег MicroWallet; в тестах — никуда).
 * Пишем только служебное: начало команды, ok/err, время, размеры. Seed, адреса, подписи,
 * данные кусков — никогда.
 */
object Trace {
    @Volatile var sink: (String) -> Unit = {}

    fun log(msg: String) = sink(msg)

    /** Безопасное описание команды: «app btc sign», «ota b64 2732 B», «restore». */
    fun cmd(line: String): String {
        val w = line.split(' ')
        return when {
            w[0] == "app" && w.getOrNull(1) in setOf("add", "put") -> "${w[0]} ${w[1]} ${w.getOrNull(2)?.take(4) ?: ""} ${line.length} B"
            w[0] == "app" -> w.take(3).joinToString(" ") + if (w.size > 3) " … ${line.length} B" else ""
            w[0] == "ota" -> "ota ${w.getOrNull(1) ?: ""}" + if (w.getOrNull(1) in setOf("b64", "data")) " ${line.length} B" else w.drop(2).take(1).joinToString(" ", " ")
            else -> w[0]
        }
    }
}
