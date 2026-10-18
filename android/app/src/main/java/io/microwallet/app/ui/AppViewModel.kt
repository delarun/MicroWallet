package io.microwallet.app.ui

import android.app.Application
import android.content.Context
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.microwallet.app.ble.BleDevice
import io.microwallet.app.ble.BleScanner
import io.microwallet.app.ble.Found
import io.microwallet.app.ble.GattTransport
import io.microwallet.app.ble.KeystoreBondStore
import io.microwallet.app.core.Balance
import io.microwallet.app.core.COINS
import io.microwallet.app.core.DEFAULT_REPO
import io.microwallet.app.core.DemoWallet
import io.microwallet.app.core.DeviceError
import io.microwallet.app.core.UrlHttp
import io.microwallet.app.core.Wallet
import io.microwallet.app.core.WalletApi
import io.microwallet.app.core.WalletError
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import android.net.Uri
import io.microwallet.app.core.FwAsset
import io.microwallet.app.core.FwRelease
import io.microwallet.app.core.inspectApp
import io.microwallet.app.core.inspectImage
import io.microwallet.app.core.isAppAsset
import io.microwallet.app.core.parseReleases
import io.microwallet.app.core.releasesUrl
import org.json.JSONArray

sealed interface Conn {
    data object Idle : Conn
    data class Connecting(val name: String) : Conn
    data class Pairing(val name: String, val code: String) : Conn
    data object Ready : Conn
}

sealed interface Load<out T> {
    data object Loading : Load<Nothing>
    data class Ok<T>(val v: T) : Load<T>
    data class Err(val msg: String) : Load<Nothing>
}

enum class Tab(val title: String) { Overview("Обзор"), Receive("Получить"), Send("Отправить"), Device("Устройство") }

fun human(e: Throwable): String = when {
    e is DeviceError && e.message == "rejected" -> "Отклонено на устройстве"
    e is DeviceError -> "Устройство: ${e.message}"
    e is WalletError -> e.message ?: "Ошибка"
    else -> "${e::class.simpleName}: ${e.message}"
}

class AppViewModel(app: Application) : AndroidViewModel(app) {
    private val ctx: Context get() = getApplication()
    private val prefs = app.getSharedPreferences("app", Context.MODE_PRIVATE)
    private val bonds by lazy { KeystoreBondStore(ctx) }
    private val scanner by lazy { BleScanner(ctx) }

    val words: List<String> by lazy {
        ctx.assets.open("bip39_english.txt").bufferedReader().readLines().map { it.trim() }.filter { it.isNotEmpty() }
    }

    var conn by mutableStateOf<Conn>(Conn.Idle)
        private set
    var wallet by mutableStateOf<WalletApi?>(null)
        private set
    var info by mutableStateOf<Map<String, String>>(emptyMap())
        private set
    val found = mutableStateListOf<Found>()
    var scanning by mutableStateOf(false)
        private set
    var scanError by mutableStateOf<String?>(null)
        private set
    val balances = mutableStateMapOf<String, Load<Balance>>()
    var message by mutableStateOf<String?>(null)
    var tab by mutableStateOf(Tab.Overview)
    /** Монета, выбранная кнопкой «Отправить»/«Получить» на карточке обзора. */
    var coinRequest by mutableStateOf<String?>(null)
    var showRestore by mutableStateOf(false)
    var showFirmware by mutableStateOf(false)
    var showApps by mutableStateOf(false)
    val apps = AppsState()
    /** Монеты меняются, когда ставят или удаляют приложение — счётчик заставляет экраны перечитать. */
    private var coinsVersion by mutableStateOf(0)
    val coins: List<String>
        get() {
            coinsVersion
            return wallet?.coins.orEmpty()
        }
    val send = SendState()
    val fw = FwState().apply {
        // Версия 2 настроек: прошивки и приложения по умолчанию — из релизов DEFAULT_REPO
        // (до неё на тестовых телефонах мог остаться источник «ПК» и пустой репозиторий)
        if (prefs.getInt("prefs_v", 1) < 2)
            prefs.edit().putString("fw_source", "github").putString("fw_repo", DEFAULT_REPO).putInt("prefs_v", 2).apply()
        source = prefs.getString("fw_source", "github") ?: "github"
        repo = prefs.getString("fw_repo", DEFAULT_REPO)?.ifBlank { DEFAULT_REPO } ?: DEFAULT_REPO
        pcUrl = prefs.getString("fw_pc", "http://localhost:8777") ?: "http://localhost:8777"
    }
    @Volatile private var fwRebooting = false
    /** Идёт подпись или проверка на устройстве — не отключаться в фоне. */
    var busy by mutableStateOf(false)

    @Volatile private var transport: GattTransport? = null
    private var device: Found? = null
    private var autoConnect = true
    private var scanJob: Job? = null
    private var bgJob: Job? = null
    private var attempt = 0  // отменённая попытка подключения не должна ничего менять

    private var lastDevice: String?
        get() = prefs.getString("last_device", null)
        set(v) = prefs.edit().putString("last_device", v).apply()

    val deviceAddress: String? get() = device?.address
    val knownDevice: String? get() = lastDevice
    val isDemo: Boolean get() = wallet is DemoWallet

    /** Блокирующая операция в фоне; результат — в главном потоке. */
    fun <T> run(op: () -> T, done: (T) -> Unit = {}, failed: (String) -> Unit = { message = it }) =
        viewModelScope.launch {
            try {
                done(withContext(Dispatchers.IO) { op() })
            } catch (e: Exception) {
                failed(human(e))
            }
        }

    // ---------------- поиск и подключение ----------------

    fun startScan() {
        if (scanning || conn != Conn.Idle) return
        found.clear()
        scanError = null
        scanning = true
        scanner.start(
            onFound = { f ->
                viewModelScope.launch {
                    val i = found.indexOfFirst { it.address == f.address }
                    if (i >= 0) found[i] = f else found.add(f)
                    if (autoConnect && f.address == lastDevice && conn == Conn.Idle) connect(f)
                }
            },
            onError = { msg ->
                viewModelScope.launch {
                    scanError = msg
                    scanning = false
                }
            },
        )
        scanJob?.cancel()
        scanJob = viewModelScope.launch {
            delay(30_000)
            stopScan()
        }
    }

    fun stopScan() {
        scanner.stop()
        scanning = false
        scanJob?.cancel()
    }

    fun connect(f: Found) {
        stopScan()
        device = f
        conn = Conn.Connecting(f.name)
        val my = ++attempt
        run({
            val t = GattTransport.connect(ctx, f.address, onDisconnect = { viewModelScope.launch { lost(f.address) } })
            transport = t
            val dev = BleDevice(f.address, f.name, t, bonds) { code ->
                viewModelScope.launch { conn = Conn.Pairing(f.name, code) }
            }
            dev.channel.pairedNow to Wallet(dev, UrlHttp(), words)
        }, done = { (paired, w) ->
            if (my != attempt) {
                viewModelScope.launch(Dispatchers.IO) { w.close() }
                return@run
            }
            wallet = w
            info = w.info
            conn = Conn.Ready
            lastDevice = f.address
            autoConnect = true
            tab = Tab.Overview
            if (paired) message = "Телефон сопряжён с ${f.name}"
            refresh()
        }, failed = {
            if (my == attempt) {
                transport = null
                conn = Conn.Idle
                message = it
            }
        })
    }

    /** Отмена во время подключения или сопряжения. */
    fun cancelConnect() {
        attempt++
        autoConnect = false
        transport?.close()
        transport = null
        conn = Conn.Idle
    }

    fun demo(empty: Boolean = false) {
        stopScan()
        val w = DemoWallet(empty, words)
        wallet = w
        info = w.info
        conn = Conn.Ready
        tab = Tab.Overview
        refresh()
    }

    /** forget — ещё и удалить ключ сопряжения: в следующий раз снова код на экране. */
    fun disconnect(forget: Boolean = false, auto: Boolean = false) {
        val w = wallet
        val addr = device?.address
        wallet = null
        transport = null
        conn = Conn.Idle
        balances.clear()
        info = emptyMap()
        send.reset()
        showRestore = false
        showFirmware = false
        showApps = false
        apps.reset()
        // Связь пропала посреди скачивания или передачи — к выбору прошивки
        if (fw.phase == FwPhase.Downloading || fw.phase == FwPhase.Confirm || fw.phase == FwPhase.Sending)
            fw.phase = if (fw.image != null) FwPhase.Ready else FwPhase.Pick
        autoConnect = auto
        if (forget && addr != null) {
            bonds.drop(addr)
            if (lastDevice == addr) lastDevice = null
        }
        viewModelScope.launch(Dispatchers.IO) { w?.close() }
    }

    private fun lost(address: String) {
        if (wallet == null || isDemo || device?.address != address) return
        val rebooting = fwRebooting
        fwRebooting = false
        disconnect(auto = true)
        message = if (rebooting) "Прошивка установлена — устройство перезагружается, переподключаюсь…"
        else "Связь с устройством потеряна"
    }

    // ---------------- приложения WebAssembly ----------------

    fun openApps() {
        if (apps.phase == AppPhase.Done || apps.phase == AppPhase.Error) apps.phase = AppPhase.List
        showApps = true
        loadInstalled()
    }

    fun loadInstalled() {
        val w = wallet ?: return
        apps.installed = Load.Loading
        run({ w.apps() to w.refreshInfo() }, { (list, inf) ->
            apps.installed = Load.Ok(list)
            info = inf
            coinsVersion++
        }, { apps.installed = Load.Err(it) })
    }

    fun loadCatalog() {
        saveFwPrefs()
        val src = if (fw.source == "pc") fw.pcUrl else fw.repo
        apps.catalog = Load.Loading
        run({
            val rels = parseReleases(UrlHttp().get(releasesUrl(src)) as? JSONArray ?: throw WalletError("Неожиданный ответ сервера"))
            // Самая свежая версия каждого файла
            rels.flatMap { r -> r.assets.filter { isAppAsset(it.name) }.map { it to r } }.distinctBy { it.first.name }
        }, { apps.catalog = Load.Ok(it) }, { apps.catalog = Load.Err(it) })
    }

    fun downloadApp(asset: FwAsset) {
        apps.phase = AppPhase.Downloading
        run({ inspectApp(UrlHttp().download(asset.url)) }, {
            apps.app = it
            apps.phase = AppPhase.Ready
        }, {
            apps.phase = AppPhase.List
            message = it
        })
    }

    fun appFromFile(uri: Uri) {
        run({ inspectApp(ctx.contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: throw WalletError("Файл не открылся")) }, {
            apps.app = it
            apps.phase = AppPhase.Ready
        })
    }

    fun installApp() {
        val w = wallet ?: return
        val a = apps.app ?: return
        apps.phase = AppPhase.Sending
        apps.progress = 0f
        busy = true
        run({
            w.installApp(a) { done, total ->
                viewModelScope.launch {
                    apps.progress = done.toFloat() / total
                    if (done == total) apps.phase = AppPhase.Confirm  // всё передано — ждём кнопку
                }
            }
        }, {
            busy = false
            apps.phase = AppPhase.Done
            apps.result = "${a.name} ${a.version} установлено"
            coinsVersion++
            loadInstalled()
            refresh()
        }, {
            busy = false
            apps.phase = AppPhase.Error
            apps.result = it
        })
    }

    fun removeApp(id: String, name: String) {
        val w = wallet ?: return
        apps.removing = id
        busy = true
        run({ w.removeApp(id) }, {
            busy = false
            apps.removing = null
            message = "$name удалено"
            balances.remove(id)
            coinsVersion++
            loadInstalled()
        }, {
            busy = false
            apps.removing = null
            message = it
        })
    }

    // ---------------- прошивка ----------------

    fun openFirmware() {
        // Прошлое обновление закончилось (или сорвалось) — начинаем заново, список перечитываем
        if (fw.phase == FwPhase.Done || fw.phase == FwPhase.Error) fw.restart()
        showFirmware = true
    }

    fun saveFwPrefs() {
        prefs.edit().putString("fw_source", fw.source).putString("fw_repo", fw.repo).putString("fw_pc", fw.pcUrl).apply()
    }

    fun fwLoadReleases() {
        saveFwPrefs()
        val src = if (fw.source == "pc") fw.pcUrl else fw.repo
        fw.releases = Load.Loading
        run({ parseReleases(UrlHttp().get(releasesUrl(src)) as? JSONArray ?: throw WalletError("Неожиданный ответ сервера")) },
            { fw.releases = Load.Ok(it) }, {
                fw.releases = Load.Err(
                    if ("Cleartext" in it || "CLEARTEXT" in it) "Без https можно только http://localhost — подключи телефон по USB и выполни adb reverse tcp:8777 tcp:8777"
                    else it
                )
            })
    }

    fun fwDownload(asset: FwAsset, rel: FwRelease, net: String) {
        fw.phase = FwPhase.Downloading
        fw.progress = 0f
        fw.detail = "Скачиваю ${asset.name}…"
        run({
            inspectImage(UrlHttp().download(asset.url) { got, total ->
                val t = if (total > 0) total else asset.size
                viewModelScope.launch {
                    if (t > 0) fw.progress = got.toFloat() / t
                    fw.detail = "Скачиваю: ${kb(got)}" + if (t > 0) " из ${kb(t)}" else ""
                }
            })
        }, {
            fw.image = it
            fw.imageFrom = "${rel.name} · ${if (fw.source == "pc") "с ПК" else "GitHub"}"
            fw.imageNet = net
            fw.phase = FwPhase.Ready
        }, {
            fw.phase = FwPhase.Pick
            message = it
        })
    }

    fun fwFromFile(uri: Uri) {
        run({
            val b = ctx.contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: throw WalletError("Файл не открылся")
            inspectImage(b)
        }, {
            fw.image = it
            val name = uri.lastPathSegment?.substringAfterLast('/') ?: "файл"
            fw.imageFrom = name
            fw.imageNet = listOf("mainnet", "testnet").firstOrNull { it in name }
            fw.phase = FwPhase.Ready
        })
    }

    fun fwInstall() {
        val w = wallet ?: return
        val img = fw.image ?: return
        fw.phase = FwPhase.Confirm
        fw.progress = 0f
        busy = true
        var started = 0L
        run({
            w.updateFirmware(img) { done, total ->
                val now = System.currentTimeMillis()
                if (started == 0L) started = now
                if (done == total) fwRebooting = !isDemo  // дальше ota end и перезагрузка
                val speed = if (now > started) done * 1000L / (now - started) else 0
                val left = if (speed > 0) (total - done) / speed else 0
                viewModelScope.launch {
                    fw.phase = FwPhase.Sending
                    fw.progress = done.toFloat() / total
                    fw.detail = "${kb(done.toLong())} из ${kb(total.toLong())} · ${kb(speed)}/с" +
                        if (left > 0) " · осталось ~${if (left >= 60) "${left / 60} мин ${left % 60} с" else "$left с"}" else ""
                }
            }
            fwRebooting = !isDemo
        }, {
            busy = false
            fw.phase = FwPhase.Done
            fw.image = null
            if (isDemo) message = "Демо: прошивка «установлена»"
        }, {
            busy = false
            fw.error = it
            fw.phase = FwPhase.Error
        })
    }

    // ---------------- данные ----------------

    fun refresh() {
        val w = wallet ?: return
        run({ w.refreshInfo() }, { if (wallet === w) info = it }, {})
        if (!w.ready) return
        for (c in COINS.map { it.id }.filter { it in w.coins }) {
            balances[c] = Load.Loading
            run({ w.balance(c) }, { if (wallet === w) balances[c] = Load.Ok(it) }, { if (wallet === w) balances[c] = Load.Err(it) })
        }
    }

    /** Настройка устройства «Addresses»: сколько адресов для получения (1, 5, 10, 20). */
    fun setAddrCount(n: Int) {
        val w = wallet ?: return
        run({ w.setAddrCount(n); w.info }, {
            if (wallet === w) {
                info = it
                refresh()
            }
        })
    }

    /** После отправки: сеть узнаёт о транзакции не сразу. */
    fun refreshSoon() {
        viewModelScope.launch {
            delay(4000)
            refresh()
        }
    }

    fun restored() {
        info = wallet?.info ?: emptyMap()
        showRestore = false
        message = "Кошелёк восстановлен"
        refresh()
    }

    // ---------------- фон ----------------

    /** Свернули приложение: через 2 минуты отключаемся, чтобы устройство могло уснуть. */
    fun onBackground() {
        stopScan()
        bgJob?.cancel()
        bgJob = viewModelScope.launch {
            delay(120_000)
            if (wallet != null && !isDemo && !busy) disconnect(auto = true)
        }
    }

    fun onForeground() {
        bgJob?.cancel()
    }

    override fun onCleared() {
        stopScan()
        wallet?.close()
    }
}
