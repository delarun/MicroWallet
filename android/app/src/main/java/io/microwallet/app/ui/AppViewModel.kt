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
import io.microwallet.app.core.AppUpdate
import io.microwallet.app.core.Trace
import io.microwallet.app.core.fmtUnits
import io.microwallet.app.core.UrlHttp
import io.microwallet.app.core.downloadAppUpdate
import io.microwallet.app.core.findAppUpdate
import io.microwallet.app.core.Wallet
import io.microwallet.app.core.WalletApi
import io.microwallet.app.core.WalletError
import io.microwallet.app.core.DEFAULT_SHOWN_TOKENS
import io.microwallet.app.core.coinInfo
import io.microwallet.app.core.tokensFor
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
import io.microwallet.app.core.TRC10_TOKENS
import io.microwallet.app.core.isTrc10
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
                Trace.log("failed: ${e.javaClass.simpleName}: ${e.message}")
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
        pearlSame = null
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

    /** Сдвинуть приложение на главном экране устройства: delta -1 — выше, +1 — ниже. */
    fun moveApp(id: String, delta: Int) {
        val w = wallet ?: return
        val list = (apps.installed as? Load.Ok)?.v ?: return
        val i = list.indexOfFirst { it.id == id }
        val j = i + delta
        if (i < 0 || j !in list.indices) return
        val order = list.toMutableList().apply { add(j, removeAt(i)) }
        apps.installed = Load.Ok(order)  // сразу на экране; устройство — следом
        run({ w.setAppOrder(order.map { it.id }); w.apps() }, {
            apps.installed = Load.Ok(it)
            coinsVersion++
        }, {
            message = "Порядок не сохранён: $it"
            loadInstalled()
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

    /** Обновить вручную (потянуть вниз): мимо кэша балансов. */
    fun refreshNow() {
        wallet?.forgetCache()
        refresh()
    }

    fun refresh() {
        val w = wallet ?: return
        val wasLocked = w.locked
        run({ w.refreshInfo() }, {
            if (wallet === w) {
                info = it
                coinsVersion++  // после ввода PIN появились приложения
                // ждём PIN на устройстве: раз в 2 с; как только введён — загрузить балансы
                if (w.locked) viewModelScope.launch { delay(2000); if (wallet === w) refresh() }
                else if (wasLocked) refresh()
            }
        }, {})
        if (!w.ready) return
        for (c in COINS.map { it.id }.filter { it in w.coins }) loadBalance(c)
    }

    private fun loadBalance(c: String) {
        val w = wallet ?: return
        val skip = hiddenTokens(c)
        balances[c] = Load.Loading
        run({ w.balance(c, skip).also { b -> runCatching { w.cacheBalance(c, visible(c, b)) } } },  // и на устройство: видно в меню монеты
            { if (wallet === w) balances[c] = Load.Ok(it) }, { if (wallet === w) balances[c] = Load.Err(it) })
    }

    // ---------------- токены на главной ----------------

    /** Какие токены показывать на главной («монета:токен»). Скрытые не запрашиваются. */
    var shownTokens by mutableStateOf(prefs.getStringSet("shown_tokens", null)?.toSet() ?: DEFAULT_SHOWN_TOKENS)
        private set

    fun tokenShown(coin: String, asset: String) = "$coin:$asset" in shownTokens

    /** Токены монеты, убранные с главной. */
    fun hiddenTokens(coin: String): Set<String> =
        coinInfo(coin).tokensFor(wallet?.net ?: "testnet").filter { !tokenShown(coin, it) }.toSet()

    /** Баланс так, как его показывать: TRC10 — только если включены в настройке TRON. */
    fun visible(coin: String, b: Balance): Balance =
        if (tokenShown(coin, TRC10_TOKENS)) b else b.copy(assets = b.assets.filterNot { isTrc10(it.id) })

    fun setTokenShown(coin: String, asset: String, show: Boolean) {
        shownTokens = if (show) shownTokens + "$coin:$asset" else shownTokens - "$coin:$asset"
        prefs.edit().putStringSet("shown_tokens", shownTokens).apply()
        if (wallet?.ready == true && coin in coins) loadBalance(coin)
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

    // ---------------- PSBT (Sparrow, Electrum) ----------------

    /** PSBT: загружен (data, описание) -> подписывается -> подписан (signed, raw) -> отправлен (txid). */
    class PsbtState {
        var data by mutableStateOf<ByteArray?>(null)
        var summary by mutableStateOf("")
        var phase by mutableStateOf("")  // "", "loaded", "signing", "signed", "sending", "sent", "error"
        var signed by mutableStateOf("")
        var raw by mutableStateOf<String?>(null)
        var txid by mutableStateOf("")
        var error by mutableStateOf("")
        var descriptor by mutableStateOf<String?>(null)

        fun reset() {
            data = null; summary = ""; phase = ""; signed = ""; raw = null; txid = ""; error = ""
        }
    }
    val psbt = PsbtState()

    fun psbtLoad(bytes: ByteArray?) {
        psbt.reset()
        if (bytes == null || bytes.isEmpty()) {
            message = "Пусто: скопируй PSBT (base64) в Sparrow или выбери файл .psbt"
            return
        }
        try {
            val p = io.microwallet.app.core.Psbt.parse(bytes)
            val sum = p.tx.outs.sumOf { it.value }
            psbt.data = bytes
            psbt.summary = "${p.tx.ins.size} вход(ов), ${p.tx.outs.size} выход(ов), на выходах ${fmtUnits(java.math.BigInteger.valueOf(sum), 8)} BTC"
            psbt.phase = "loaded"
        } catch (e: Exception) {
            psbt.phase = "error"
            psbt.error = human(e)
        }
    }

    fun psbtSign(send: Boolean) {
        val w = wallet ?: return
        val d = psbt.data ?: return
        psbt.phase = "signing"
        busy = true
        run({ w.signPsbt(d, finalize = send) }, { (signed, raw) ->
            busy = false
            psbt.signed = signed
            psbt.raw = raw
            psbt.phase = "signed"
            if (send && raw != null) psbtBroadcast()
        }, {
            busy = false
            psbt.phase = "error"
            psbt.error = it
        })
    }

    fun psbtBroadcast() {
        val w = wallet ?: return
        val raw = psbt.raw ?: return
        psbt.phase = "sending"
        run({ w.broadcastBtc(raw) }, {
            psbt.txid = it
            psbt.phase = "sent"
            refreshSoon()
        }, {
            psbt.phase = "error"
            psbt.error = "Подписано, но сеть не приняла: $it"
        })
    }

    /** Совместимость адресов PRL с Pearl Wallet: null — не проверяли. */
    var pearlSame by mutableStateOf<Boolean?>(null)
    var pearlChecking by mutableStateOf(false)

    fun checkPearl() {
        val w = wallet ?: return
        pearlChecking = true
        run({ w.pearlWalletSame() }, { pearlSame = it; pearlChecking = false }, { message = it; pearlChecking = false })
    }

    fun loadDescriptor() {
        val w = wallet ?: return
        run({ w.descriptor() }, { psbt.descriptor = it })
    }

    // ---------------- обновление самого приложения ----------------

    /** Новая версия приложения в релизах (null — нет или ещё не проверяли). */
    var appUpdate by mutableStateOf<AppUpdate?>(null)
    /** "" — ничего, "checking", "downloading", "installing", "error" */
    var appUpdatePhase by mutableStateOf("")
    var appUpdateProgress by mutableStateOf(0f)
    var appUpdateError by mutableStateOf<String?>(null)
    var autoAppUpdate by mutableStateOf(prefs.getBoolean("app_auto_update", true))
        private set
    private var foreground = false

    /** versionCode и versionName установленного приложения */
    val appVersion: Pair<Long, String> = readAppVersion(app)

    private fun readAppVersion(ctx: Application): Pair<Long, String> {
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        @Suppress("DEPRECATION")
        val code = if (android.os.Build.VERSION.SDK_INT >= 28) pi.longVersionCode else pi.versionCode.toLong()
        return code to (pi.versionName ?: "?")
    }

    fun changeAutoAppUpdate(on: Boolean) {
        autoAppUpdate = on
        prefs.edit().putBoolean("app_auto_update", on).apply()
    }

    /** Источник — тот же, что для прошивок: GitHub-репозиторий или сервер с ПК. */
    private fun updateSource() = if (fw.source == "pc") fw.pcUrl else fw.repo.ifBlank { DEFAULT_REPO }

    /** Проверить релизы. Сама — не чаще раза в 6 часов; force — по кнопке. */
    fun checkAppUpdate(force: Boolean = false) {
        val now = System.currentTimeMillis()
        if (appUpdatePhase in setOf("checking", "downloading", "installing")) return
        if (!force && now - prefs.getLong("app_update_checked", 0) < 6 * 3600_000L) return
        prefs.edit().putLong("app_update_checked", now).apply()
        appUpdatePhase = "checking"
        run({ findAppUpdate(UrlHttp(), updateSource(), appVersion.first) }, { u ->
            appUpdate = u
            appUpdatePhase = ""
            when {
                u == null -> if (force) message = "Установлена последняя версия приложения"
                // Установка перезапускает приложение — сама только без связи с устройством
                autoAppUpdate && foreground && wallet == null && conn == Conn.Idle && !busy -> installAppUpdate()
            }
        }, {
            appUpdatePhase = ""
            if (force) message = it
        })
    }

    fun installAppUpdate() {
        val u = appUpdate ?: return
        appUpdatePhase = "downloading"
        appUpdateProgress = 0f
        appUpdateError = null
        run({
            downloadAppUpdate(UrlHttp(), u) { got, total -> if (total > 0) appUpdateProgress = got.toFloat() / total }
        }, { apk ->
            appUpdatePhase = "installing"
            try {
                AppInstaller.install(getApplication(), apk)
            } catch (e: Exception) {
                appUpdatePhase = "error"
                appUpdateError = e.message ?: "установка не удалась"
            }
        }, {
            appUpdatePhase = "error"
            appUpdateError = it
        })
    }

    // ---------------- фон ----------------

    /** Свернули приложение: через 2 минуты отключаемся, чтобы устройство могло уснуть. */
    fun onBackground() {
        foreground = false
        stopScan()
        bgJob?.cancel()
        bgJob = viewModelScope.launch {
            delay(120_000)
            if (wallet != null && !isDemo && !busy) disconnect(auto = true)
        }
    }

    fun onForeground() {
        foreground = true
        bgJob?.cancel()
        checkAppUpdate()
    }

    override fun onCleared() {
        stopScan()
        wallet?.close()
    }
}
