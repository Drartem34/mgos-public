package os.mg.share

import android.app.AlertDialog
import android.app.StatusBarManager
import android.content.ComponentName
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.Icon
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/**
 * Єдине вікно: список своїх пристроїв знизу екрана.
 * Поки воно відкрите — телефон видно ПК (привіт кожні 15 с) і він приймає файли. Закрили — тиша, нуль фону.
 */
class MainActivity : ComponentActivity() {
    private lateinit var st: Store
    private lateinit var recv: Receiver
    private var devices = mutableListOf<Device>()
    private val ui = Handler(Looper.getMainLooper())
    private val pending = mutableListOf<Uri>()        // файли з «Поділитися» / вибрані
    private var fromShare = false
    private var target: Device? = null
    @Volatile private var started = false
    @Volatile private var busy = false

    private lateinit var listBox: LinearLayout
    private lateinit var subtitle: TextView
    private lateinit var banner: TextView
    private lateinit var progRow: LinearLayout
    private lateinit var progText: TextView
    private lateinit var progBar: ProgressBar
    private lateinit var gotBox: LinearLayout

    private val accent = Color.parseColor("#0A84FF")
    private val fg = Color.parseColor("#F5F5F7")
    private val dim = Color.parseColor("#99F5F5F7")

    private val pick = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        val d = target ?: return@registerForActivityResult
        if (uris.isNotEmpty()) send(d, uris)
    }

    private val scan = registerForActivityResult(ScanContract()) { r ->
        val txt = r.contents ?: return@registerForActivityResult
        status("З'єднуюсь…")
        thread {
            try {
                val d = Link.pair(st, txt.trim())
                devices.removeAll { it.id == d.id }
                devices += d
                st.save(devices)
                d.online = Link.hello(st, d)
                ui.post { toast("З'єднано з ${d.name}"); render() }
            } catch (e: Exception) {
                ui.post { status(e.message ?: "Не вдалося з'єднатись"); toast(e.message ?: "Не вдалося з'єднатись") }
            }
        }
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
    private fun status(s: String) { subtitle.text = s }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        st = Store(this)
        recv = Receiver(this, st) { e -> ui.post { onRecv(e) } }
        devices = st.devices()
        buildUi()
        takeShare(intent)
        render()
        offerTile()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        takeShare(intent)
        render()
    }

    override fun onStart() {
        super.onStart()
        started = true
        recv.start()
        presenceLoop()
    }

    override fun onStop() {
        super.onStop()
        started = false
        recv.stop()
        val online = devices.filter { it.online }
        if (!busy) thread { online.forEach { Link.bye(st, it) } }
        devices.forEach { it.online = false }
    }

    // ---------- присутність: лише поки вікно на екрані ----------
    private fun presenceLoop() {
        thread(name = "mg-presence") {
            while (started) {
                refreshPresence()
                var waited = 0
                while (started && waited < 15000) { Thread.sleep(250); waited += 250 }
            }
        }
    }

    private fun refreshPresence() {
        val list = devices.toList()
        if (list.isEmpty()) return
        val found = Link.discover(st, list)
        var changed = false
        for (d in list) {
            found[d.id]?.let { ip -> if (d.ip != ip) { d.ip = ip; d.port = Link.PC_PORT; changed = true } }
            val was = d.online
            d.online = (d.ip.isNotEmpty()) && Link.hello(st, d)
            if (was != d.online) changed = true
        }
        st.save(list)
        if (changed || true) ui.post { if (!isFinishing) render() }
    }

    // ---------- «Поділитися» з інших застосунків ----------
    private fun takeShare(i: Intent?) {
        i ?: return
        val uris = when (i.action) {
            Intent.ACTION_SEND -> listOfNotNull(
                if (Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else @Suppress("DEPRECATION") i.getParcelableExtra(Intent.EXTRA_STREAM)
            )
            Intent.ACTION_SEND_MULTIPLE ->
                (if (Build.VERSION.SDK_INT >= 33) i.getParcelableArrayListExtra(Intent.EXTRA_STREAM, Uri::class.java)
                else @Suppress("DEPRECATION") i.getParcelableArrayListExtra(Intent.EXTRA_STREAM)) ?: emptyList()
            else -> emptyList()
        }
        if (uris.isNotEmpty()) { pending.clear(); pending += uris; fromShare = true }
    }

    private fun nameOf(u: Uri): Pair<String, Long> {
        var name = u.lastPathSegment ?: "file"
        var size = -1L
        contentResolver.query(u, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                c.getString(0)?.let { name = it }
                if (!c.isNull(1)) size = c.getLong(1)
            }
        }
        if (size < 0) size = contentResolver.openAssetFileDescriptor(u, "r")?.use { it.length } ?: -1L
        return name to size
    }

    // ---------- надсилання на ПК ----------
    private fun send(d: Device, uris: List<Uri>) {
        if (busy) return
        busy = true
        progRow.visibility = View.VISIBLE
        thread {
            var ok = 0
            if (!d.online) {
                ui.post { progText.text = "Шукаю ${d.name}…"; progBar.isIndeterminate = true }
                Link.discover(st, listOf(d))[d.id]?.let { d.ip = it; d.port = Link.PC_PORT }
                d.online = Link.hello(st, d)
            }
            if (!d.online) {
                ui.post { done("${d.name} не знайдено — ноут у тій самій Wi-Fi і увімкнений?", false) }
                return@thread
            }
            for ((k, u) in uris.withIndex()) {
                val (name, size) = nameOf(u)
                val label = if (uris.size > 1) "${k + 1}/${uris.size} · $name" else name
                ui.post { progText.text = "→ ${d.name}: $label"; progBar.isIndeterminate = false; progBar.progress = 0 }
                val r = runCatching {
                    val inp = contentResolver.openInputStream(u) ?: error("no stream")
                    if (size < 0) error("size")
                    Link.upload(st, d, name, size, inp) { p -> ui.post { progBar.progress = p } }
                }.getOrDefault(false)
                if (r) ok++
            }
            val all = ok == uris.size
            ui.post {
                done(if (all) "Надіслано на ${d.name}" + (if (uris.size > 1) " · ${uris.size}" else "") else "Надіслано $ok з ${uris.size}", all)
            }
        }
    }

    private fun done(msg: String, success: Boolean) {
        busy = false
        progBar.isIndeterminate = false
        progBar.progress = if (success) 100 else 0
        progText.text = msg
        toast(msg)
        if (success && fromShare) {
            pending.clear(); fromShare = false
            ui.postDelayed({ if (!busy) finish() }, 1200)
        }
        render()
    }

    // ---------- прийом ----------
    private fun onRecv(e: Receiver.Event) {
        when (e) {
            is Receiver.Event.Start -> { progRow.visibility = View.VISIBLE; progText.text = "← ${e.from}: ${e.name}"; progBar.isIndeterminate = false; progBar.progress = 0 }
            is Receiver.Event.Progress -> progBar.progress = e.pct
            is Receiver.Event.Failed -> progText.text = "Обірвалось: ${e.name}"
            is Receiver.Event.Done -> {
                progText.text = "Отримано від ${e.from}: ${e.name}"
                progBar.progress = 100
                addGot(e)
            }
        }
    }

    private fun addGot(e: Receiver.Event.Done) {
        val row = TextView(this).apply {
            text = "↓  ${e.name}   ·   відкрити"
            setTextColor(fg); textSize = 14f
            setPadding(dp(14), dp(12), dp(14), dp(12))
            background = round(Color.parseColor("#14FFFFFF"), dp(14).toFloat())
            setOnClickListener {
                runCatching {
                    startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(e.uri, e.mime).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
                }.onFailure { toast("Немає програми, щоб відкрити — файл у Завантаженнях/Mg Share") }
            }
        }
        gotBox.addView(row, 0, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
        gotBox.visibility = View.VISIBLE
    }

    // ---------- плитка в шторці (Android 13+ вміє попросити сам) ----------
    private fun offerTile() {
        if (st.tileOffered || Build.VERSION.SDK_INT < 33) return
        st.tileOffered = true
        runCatching {
            getSystemService(StatusBarManager::class.java).requestAddTileService(
                ComponentName(this, ShareTile::class.java), "Mg Share",
                Icon.createWithResource(this, R.drawable.ic_tile), Executors.newSingleThreadExecutor()) { }
        }
    }

    // ---------- інтерфейс ----------
    private fun round(color: Int, r: Float, top: Boolean = false) = GradientDrawable().apply {
        setColor(color)
        if (top) cornerRadii = floatArrayOf(r, r, r, r, 0f, 0f, 0f, 0f) else cornerRadius = r
    }

    private fun button(txt: String, primary: Boolean, onClick: () -> Unit) = TextView(this).apply {
        text = txt
        gravity = Gravity.CENTER
        setTextColor(if (primary) Color.WHITE else fg)
        textSize = 15f
        typeface = Typeface.DEFAULT_BOLD
        setPadding(dp(16), dp(11), dp(16), dp(11))
        background = round(if (primary) accent else Color.parseColor("#1FFFFFFF"), dp(14).toFloat())
        setOnClickListener { onClick() }
    }

    private fun buildUi() {
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        val root = FrameLayout(this).apply { setOnClickListener { if (!busy) finish() } }
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = round(Color.parseColor("#F2131317"), dp(28).toFloat(), top = true)
            setPadding(dp(20), dp(10), dp(20), dp(20))
            isClickable = true
        }
        root.setOnApplyWindowInsetsListener { _, ins ->
            @Suppress("DEPRECATION") card.setPadding(dp(20), dp(10), dp(20), dp(20) + ins.systemWindowInsetBottom)
            ins
        }
        card.addView(View(this).apply { background = round(Color.parseColor("#40FFFFFF"), dp(3).toFloat()) },
            LinearLayout.LayoutParams(dp(36), dp(5)).apply { gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(14) })

        val head = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(ImageView(this).apply {
            setImageResource(R.drawable.ic_tile)
            background = round(Color.BLACK, dp(12).toFloat())
            setPadding(dp(7), dp(7), dp(7), dp(7))
        }, LinearLayout.LayoutParams(dp(42), dp(42)))
        val tcol = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(12), 0, 0, 0) }
        tcol.addView(TextView(this).apply { text = "Mg Share"; setTextColor(fg); textSize = 20f; typeface = Typeface.DEFAULT_BOLD })
        subtitle = TextView(this).apply { setTextColor(dim); textSize = 13f }
        tcol.addView(subtitle)
        head.addView(tcol, LinearLayout.LayoutParams(0, -2, 1f))
        card.addView(head)

        banner = TextView(this).apply {
            setTextColor(fg); textSize = 14f
            setPadding(dp(14), dp(10), dp(14), dp(10))
            background = round(Color.parseColor("#330A84FF"), dp(12).toFloat())
            visibility = View.GONE
        }
        card.addView(banner, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(14) })

        listBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        card.addView(listBox, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(6) })

        progRow = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; visibility = View.GONE }
        progText = TextView(this).apply { setTextColor(fg); textSize = 13f; maxLines = 2 }
        progBar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { max = 100 }
        progRow.addView(progText)
        progRow.addView(progBar, LinearLayout.LayoutParams(-1, dp(10)).apply { topMargin = dp(6) })
        card.addView(progRow, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(14) })

        gotBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; visibility = View.GONE }
        card.addView(ScrollView(this).apply { addView(gotBox) }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) })

        card.addView(button("+  Додати пристрій", false) {
            scan.launch(ScanOptions().setPrompt("Наведіть на QR «Додати пристрій» у топбарі ноута")
                .setBeepEnabled(false).setOrientationLocked(false).setDesiredBarcodeFormats(ScanOptions.QR_CODE))
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(16) })

        root.addView(card, FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM))
        setContentView(root, ViewGroup.LayoutParams(-1, -1))
    }

    private fun render() {
        if (pending.isNotEmpty()) {
            val first = runCatching { nameOf(pending[0]).first }.getOrDefault("файл")
            banner.text = if (pending.size == 1) "Надіслати: $first" else "Надіслати: $first і ще ${pending.size - 1}"
            banner.visibility = View.VISIBLE
        } else banner.visibility = View.GONE
        val anyOn = devices.any { it.online }
        status(when {
            devices.isEmpty() -> "Додайте ноут: топбар → годинник → «Додати»"
            anyOn -> "Поки вікно відкрите — ноут може слати файли сюди"
            else -> "Шукаю свої пристрої…"
        })
        listBox.removeAllViews()
        for (d in devices) listBox.addView(deviceRow(d), LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
    }

    private fun deviceRow(d: Device): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(14), dp(12), dp(12), dp(12))
            background = round(Color.parseColor("#14FFFFFF"), dp(18).toFloat())
            setOnLongClickListener {
                AlertDialog.Builder(this@MainActivity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Забути ${d.name}?")
                    .setMessage("Щоб знову обмінюватись файлами, треба буде з'єднатись через QR.")
                    .setPositiveButton("Забути") { _, _ -> devices.removeAll { it.id == d.id }; st.save(devices); render() }
                    .setNegativeButton("Ні", null).show()
                true
            }
        }
        row.addView(ImageView(this).apply {
            setImageResource(R.drawable.ic_laptop)
            setColorFilter(if (d.online) fg else dim)
        }, LinearLayout.LayoutParams(dp(28), dp(28)))
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(12), 0, dp(8), 0) }
        col.addView(TextView(this).apply { text = d.name; setTextColor(fg); textSize = 16f; typeface = Typeface.DEFAULT_BOLD })
        col.addView(TextView(this).apply {
            text = if (d.online) "● на зв'язку" else "не видно"
            setTextColor(if (d.online) Color.parseColor("#30D158") else dim); textSize = 12f
        })
        row.addView(col, LinearLayout.LayoutParams(0, -2, 1f))
        row.addView(button(if (pending.isNotEmpty()) "Надіслати" else "Файл…", true) {
            if (busy) return@button
            if (pending.isNotEmpty()) send(d, pending.toList())
            else { target = d; pick.launch(arrayOf("*/*")) }
        })
        return row
    }
}
