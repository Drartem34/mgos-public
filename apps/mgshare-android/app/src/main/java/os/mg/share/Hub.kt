package os.mg.share

import android.annotation.SuppressLint
import android.app.ForegroundServiceStartNotAllowedException
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.OpenableColumns
import android.service.quicksettings.TileService
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CopyOnWriteArraySet
import kotlin.concurrent.thread

/**
 * Серце застосунку (одне на процес): прийом файлів, «привіт» для ПК, надсилання.
 * Працює, поки є хоч одна причина: "ui" — відкрите вікно, "bg" — фоновий прийом (плитка / перемикач).
 * Немає причин — сервер закритий, «бувай» усім ПК, нуль фону.
 */
@SuppressLint("StaticFieldLeak")   // лише applicationContext
object Hub {
    lateinit var app: Context; private set
    lateinit var st: Store; private set
    val devices = CopyOnWriteArrayList<Device>()

    interface Listener {
        fun onRecv(e: Receiver.Event) {}
        fun onSend(e: SendEvent) {}
        fun onDevices() {}
    }

    sealed class SendEvent {
        data class Progress(val text: String, val pct: Int, val indeterminate: Boolean = false) : SendEvent()
        data class Done(val msg: String, val ok: Boolean) : SendEvent()
    }

    val listeners = CopyOnWriteArraySet<Listener>()
    private val ui = Handler(Looper.getMainLooper())
    private val reasons = HashSet<String>()
    private var recv: Receiver? = null
    private var gen = 0
    private val wake = Object()
    @Volatile var sending = 0; private set
    @Volatile var serviceUp = false

    fun init(ctx: Context) {
        if (::app.isInitialized) return
        app = ctx.applicationContext
        st = Store(app)
        devices.addAll(st.devices())
        Notif.channels(app)
        watchNetwork()
    }

    val uiVisible: Boolean get() = synchronized(this) { "ui" in reasons }

    @Synchronized fun acquire(r: String) {
        val was = reasons.isNotEmpty()
        if (!reasons.add(r)) return
        if (!was) start() else kick()
    }

    @Synchronized fun release(r: String) {
        if (!reasons.remove(r)) return
        if (reasons.isEmpty()) stop() else kick()
    }

    private fun start() {
        recv = Receiver(app, st) { e -> onRecv(e) }.also { it.start() }
        presenceLoop(++gen)
    }

    private fun stop() {
        gen++
        recv?.stop(); recv = null
        val online = devices.filter { it.online }
        devices.forEach { it.online = false }
        if (sending == 0) thread { online.forEach { Link.bye(st, it) } }
        changed()
    }

    /** Розбудити цикл присутності (відкрили вікно, з'явилась Wi-Fi, змінився профіль). */
    fun kick() = synchronized(wake) { wake.notifyAll() }

    // ---------- присутність ----------
    private fun presenceLoop(my: Int) = thread(name = "mg-presence", isDaemon = true) {
        while (my == gen) {
            refresh()
            // вікно відкрите — кожні 15 с (ПК вважає «онлайн» 45 с); лише фон — рідше: ПК і так стукає напряму
            val wait = if (uiVisible) 15_000L else 60_000L
            synchronized(wake) { if (my == gen) wake.wait(wait) }
        }
    }

    private fun refresh() {
        val list = devices.toList()
        if (list.isEmpty()) return changed()
        val found = Link.discover(st, list)
        for (d in list) {
            found[d.id]?.let { ip -> if (d.ip != ip) { d.ip = ip; d.port = Link.PC_PORT } }
            d.online = d.ip.isNotEmpty() && Link.hello(st, d)
        }
        st.updateKnown(list)
        changed()
    }

    private fun changed() {
        ui.post { listeners.forEach { it.onDevices() } }
        if (serviceUp) Notif.post(app, Notif.ID_SERVICE, Notif.service(app))
    }

    /** Ім'я чи аватарка змінились — розносимо всім ПК, яких видно (решта отримає при наступному «привіт»). */
    fun profileChanged() {
        thread { devices.filter { it.ip.isNotEmpty() }.forEach { d -> d.online = Link.hello(st, d) }; changed() }
    }

    private var netCb: ConnectivityManager.NetworkCallback? = null
    private fun watchNetwork() {
        val cm = app.getSystemService(ConnectivityManager::class.java) ?: return
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) { ui.postDelayed({ kick() }, 1500) }
        }
        runCatching {
            cm.registerNetworkCallback(NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build(), cb)
            netCb = cb
        }
    }

    // ---------- пристрої ----------
    fun addDevice(d: Device) {
        st.add(d)
        devices.removeAll { it.id == d.id }
        devices += d
        Shortcuts.publish(app, devices)
        changed()
    }

    fun forget(id: String) {
        devices.removeAll { it.id == id }
        st.remove(id)
        Shortcuts.publish(app, devices)
        changed()
    }

    // ---------- фонова служба й плитка ----------
    /**
     * Привести все у відповідність до st.bgMode і поточних передач.
     * false — система не дала запустити службу з фону (тоді плитка відкриває вікно, і звідти вже можна).
     */
    fun sync(ctx: Context): Boolean {
        if (st.bgMode) acquire("bg") else release("bg")
        val need = st.bgMode || sending > 0
        var ok = true
        val i = Intent(ctx, BgService::class.java)
        if (need) {
            ok = try {
                if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
                true
            } catch (e: Exception) {
                if (Build.VERSION.SDK_INT >= 31 && e is ForegroundServiceStartNotAllowedException) false
                else if (e is IllegalStateException) false else throw e
            }
        } else ctx.stopService(i)
        runCatching { TileService.requestListeningState(ctx, ComponentName(ctx, ShareTile::class.java)) }
        ui.post { listeners.forEach { it.onDevices() } }
        return ok
    }

    // ---------- Wi-Fi без енергозбереження на час передачі ----------
    private var locks = 0
    private var wifiHi: WifiManager.WifiLock? = null
    private var wifiLow: WifiManager.WifiLock? = null
    private var cpu: PowerManager.WakeLock? = null

    /** Поки йде передача: Wi-Fi не засинає між пакетами (головна причина повільних великих файлів), CPU не спить. */
    @Synchronized fun transferBegin() {
        if (locks++ > 0) return
        runCatching {
            val wm = app.getSystemService(WifiManager::class.java)
            @Suppress("DEPRECATION")
            wifiHi = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "mgshare:hi").apply { setReferenceCounted(false); acquire() }
            if (Build.VERSION.SDK_INT >= 29)
                wifiLow = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "mgshare:low").apply { setReferenceCounted(false); acquire() }
        }
        runCatching {
            cpu = app.getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "mgshare:xfer").apply { setReferenceCounted(false); acquire(3 * 60 * 60 * 1000L) }
        }
    }

    @Synchronized fun transferEnd() {
        if (locks == 0 || --locks > 0) return
        runCatching { wifiHi?.release() }; runCatching { wifiLow?.release() }; runCatching { cpu?.release() }
        wifiHi = null; wifiLow = null; cpu = null
    }

    // ---------- прийом ----------
    private val rxMeter = HashMap<String, Meter>()
    private var rxNotifAt = 0L

    private fun onRecv(e: Receiver.Event) {
        ui.post {
            listeners.forEach { it.onRecv(e) }
            val bg = !uiVisible
            when (e) {
                is Receiver.Event.Start -> {
                    rxMeter[e.name] = Meter(e.size)
                    if (bg) Notif.post(app, Notif.ID_RX, Notif.progress(app, "Від ${e.from}", e.name, 0, false))
                }
                is Receiver.Event.Progress -> {
                    val t = System.currentTimeMillis()
                    if (bg && t - rxNotifAt > 700) {
                        rxNotifAt = t
                        val m = rxMeter[e.name]
                        Notif.post(app, Notif.ID_RX, Notif.progress(app, e.name, m?.text(e.got) ?: "", m?.pct(e.got) ?: 0, false))
                    }
                }
                is Receiver.Event.Failed -> {
                    rxMeter.remove(e.name); Notif.cancel(app, Notif.ID_RX)
                    if (bg) Notif.simple(app, "Обірвалось", e.name)
                }
                is Receiver.Event.Done -> {
                    rxMeter.remove(e.name); Notif.cancel(app, Notif.ID_RX)
                    if (bg) Notif.received(app, e.from, e.name, e.uri, e.mime)
                }
                // вікно закрите → спливаюче сповіщення (з фону Android не дає відкрити браузер сам); відкрите — рядок у шторці
                is Receiver.Event.LinkGot -> if (bg) Notif.link(app, e.from, e.url, e.text)
            }
        }
    }

    /** Посилання / текст → ПК. Крихітний запит, тож без фонової служби: окремий потік і подія Done. */
    fun sendLink(d: Device, url: String, text: String) {
        fun emit(ev: SendEvent) = ui.post {
            listeners.forEach { it.onSend(ev) }
            if (!uiVisible && ev is SendEvent.Done) Notif.simple(app, if (ev.ok) "Надіслано" else "Не надіслано", ev.msg)
        }
        thread(name = "mg-link") {
            emit(SendEvent.Progress("→ ${d.name}", 0, true))
            if (!d.online) {
                Link.discover(st, listOf(d))[d.id]?.let { d.ip = it; d.port = Link.PC_PORT }
                d.online = d.ip.isNotEmpty() && Link.hello(st, d)
            }
            val ok = d.online && Link.sendLink(st, d, url, text)
            emit(SendEvent.Done(when {
                ok && url.isNotEmpty() -> "Посилання на ${d.name}"
                ok -> "Текст на ${d.name} — уже в буфері обміну"
                !d.online -> "${d.name} не знайдено — ноут у тій самій Wi-Fi і увімкнений?"
                else -> "${d.name} не прийняв посилання — оновіть mgos-share на ПК"
            }, ok))
            changed()
        }
    }

    // ---------- надсилання ----------
    fun nameOf(u: Uri): Pair<String, Long> {
        var name = u.lastPathSegment ?: "file"
        var size = -1L
        runCatching {
            app.contentResolver.query(u, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    c.getString(0)?.let { name = it }
                    if (!c.isNull(1)) size = c.getLong(1)
                }
            }
        }
        return name to size
    }

    /**
     * Надіслати файли на ПК. Усі файли відкриваємо ОДРАЗУ (у вікні, поки є дозвіл від «Поділитися»):
     * тоді вікно можна закрити — передача доїде у фоновій службі з прогресом у шторці.
     */
    fun send(ctx: Context, d: Device, uris: List<Uri>): Boolean {
        if (sending > 0) return false
        val items = uris.mapNotNull { u ->
            val (name, size0) = nameOf(u)
            val fd = runCatching { app.contentResolver.openAssetFileDescriptor(u, "r") }.getOrNull() ?: return@mapNotNull null
            val size = if (size0 >= 0) size0 else fd.length
            if (size < 0) { fd.close(); null } else Triple(name, size, fd)
        }
        sending++
        sync(ctx)
        fun emit(ev: SendEvent) = ui.post {
            listeners.forEach { it.onSend(ev) }
            if (!uiVisible) when (ev) {
                is SendEvent.Progress -> Notif.post(app, Notif.ID_TX, Notif.progress(app, "→ ${d.name}", ev.text, ev.pct, ev.indeterminate))
                is SendEvent.Done -> { Notif.cancel(app, Notif.ID_TX); Notif.simple(app, if (ev.ok) "Надіслано" else "Не все надіслано", ev.msg) }
            } else if (ev is SendEvent.Done) Notif.cancel(app, Notif.ID_TX)
        }
        thread(name = "mg-send") {
            var ok = 0
            transferBegin()
            try {
                if (!d.online) {
                    emit(SendEvent.Progress("Шукаю ${d.name}…", 0, true))
                    Link.discover(st, listOf(d))[d.id]?.let { d.ip = it; d.port = Link.PC_PORT }
                    d.online = Link.hello(st, d)
                }
                if (!d.online) {
                    items.forEach { runCatching { it.third.close() } }
                    emit(SendEvent.Done("${d.name} не знайдено — ноут у тій самій Wi-Fi і увімкнений?", false))
                    return@thread
                }
                var lastAt = 0L
                for ((k, it) in items.withIndex()) {
                    val (name, size, fd) = it
                    val label = if (items.size > 1) "${k + 1}/${items.size} · $name" else name
                    val m = Meter(size)
                    emit(SendEvent.Progress(label, 0))
                    val r = runCatching {
                        Link.upload(st, d, name, size, fd.createInputStream()) { got ->
                            val t = System.currentTimeMillis()
                            if (t - lastAt > 150 || got == size) { lastAt = t; emit(SendEvent.Progress("$label\n${m.text(got)}", m.pct(got))) }
                        }
                    }.getOrDefault(false)
                    runCatching { fd.close() }
                    if (r) ok++
                }
                val all = ok == items.size && items.size == uris.size
                emit(SendEvent.Done(if (all) "Надіслано на ${d.name}" + (if (items.size > 1) " · ${items.size}" else "") else "Надіслано $ok з ${uris.size}", all))
            } finally {
                transferEnd()
                ui.post { sending--; sync(app) }
            }
        }
        return true
    }
}

/** Швидкість і залишок часу для великих файлів: «45 % · 23,4 МБ/с · ще 0:12». */
class Meter(private val size: Long) {
    private val t0 = System.currentTimeMillis()

    fun pct(got: Long) = if (size > 0) (got * 100 / size).toInt() else 100

    fun text(got: Long): String {
        val dt = (System.currentTimeMillis() - t0).coerceAtLeast(1) / 1000.0
        val bps = got / dt
        val sb = StringBuilder("${pct(got)} % · ${human(got)} з ${human(size)}")
        if (dt > 0.7 && bps > 0) {
            sb.append(" · ").append(human(bps.toLong())).append("/с")
            val left = ((size - got) / bps).toLong()
            if (got < size) sb.append(" · ще ").append(if (left >= 60) "${left / 60}:${"%02d".format(left % 60)}" else "$left с")
        }
        return sb.toString()
    }

    companion object {
        fun human(n: Long): String {
            var v = n.toDouble()
            for (u in arrayOf("Б", "КБ", "МБ", "ГБ")) {
                if (v < 1024 || u == "ГБ") return if (u == "Б") "${v.toLong()} $u" else "%.1f %s".format(v, u).replace('.', ',')
                v /= 1024
            }
            return "$n Б"
        }
    }
}
