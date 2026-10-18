package io.microwallet.app

import io.microwallet.app.ble.FrameTransport
import io.microwallet.app.core.Device
import io.microwallet.app.core.parseReply
import org.junit.Assume
import java.io.DataInputStream
import java.io.File
import java.nio.file.Files
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/** Симулятор устройства (tools/sim): тот же код прошивки, собранный для ПК. */
object Sim {
    val binary: String = System.getProperty("mw.sim").orEmpty()
    val words: List<String> by lazy { File(System.getProperty("mw.words")).readLines().map { it.trim() }.filter { it.isNotEmpty() } }

    fun require() = Assume.assumeTrue("MW_SIM не задан — тесты с симулятором пропущены", binary.isNotEmpty())

    fun newDir(): File = Files.createTempDirectory("mwsim").toFile()

    /** Собранные приложения (apps/dist): btc, prl, tron, bsc, eth. */
    val apps: List<File> by lazy {
        File(System.getProperty("mw.apps")).listFiles { f -> f.name.startsWith("microwallet-app-") }.orEmpty().sortedBy { it.name }
    }

    fun start(mode: String, dir: File, env: Map<String, String> = emptyMap()): Process =
        ProcessBuilder(binary, mode)
            .redirectError(File(dir, "stderr.txt"))
            .apply { environment()["MW_SIM_DIR"] = dir.absolutePath; environment().putAll(env) }
            .start()
}

/** Построчный протокол — как USB. */
class LineDevice(private val p: Process) : Device {
    private val out = p.outputStream.bufferedWriter()
    private val inp = p.inputStream.bufferedReader()
    override val label = "sim"

    @Synchronized
    override fun call(line: String): String {
        out.write(line + "\n")
        out.flush()
        return parseReply(inp.readLine() ?: "")
    }

    override fun close() {
        out.close()
        p.waitFor(5, TimeUnit.SECONDS)
    }
}

/** Кадры канала через stdin/stdout симулятора — как GATT. */
class SimFrames(val p: Process) : FrameTransport {
    private val frames = LinkedBlockingQueue<ByteArray>()

    init {
        thread(isDaemon = true) {
            val inp = DataInputStream(p.inputStream)
            try {
                while (true) {
                    val n = inp.readUnsignedShort()
                    frames.put(ByteArray(n).also { inp.readFully(it) })
                }
            } catch (e: Exception) {
                frames.put(ByteArray(0))
            }
        }
    }

    override fun send(frame: ByteArray) {
        p.outputStream.write(byteArrayOf((frame.size ushr 8).toByte(), frame.size.toByte()) + frame)
        p.outputStream.flush()
    }

    override fun recv(timeoutMs: Long?): ByteArray? =
        if (timeoutMs == null) frames.take() else frames.poll(timeoutMs, TimeUnit.MILLISECONDS)

    override fun close() {
        runCatching { p.outputStream.close() }
        p.waitFor(5, TimeUnit.SECONDS)
    }
}
