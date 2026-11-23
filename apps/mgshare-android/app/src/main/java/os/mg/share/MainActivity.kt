package os.mg.share

import android.Manifest
import android.animation.ValueAnimator
import android.app.AlertDialog
import android.app.StatusBarManager
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.ImageDecoder
import android.graphics.Outline
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.Icon
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.ViewOutlineProvider
import android.view.WindowManager
import android.view.animation.DecelerateInterpolator
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import java.io.ByteArrayOutputStream
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/**
 * Вікно-шторка знизу екрана зі «склом»: усе позаду розмивається (Android 12+, на Samsung — рідний блюр One UI).
 * Профіль (ім'я + аватарка — розлітаються на всі свої ПК), перемикач фонового прийому, список ПК.
 */
class MainActivity : ComponentActivity(), Hub.Listener {
    companion object {
        const val EXTRA_BG_START = "bg_start"
        const val EXTRA_DEV = "dev"
    }

    private val st get() = Hub.st
    private val pending = mutableListOf<Uri>()        // файли з «Поділитися» / вибрані
    private var fromShare = false
    private var target: Device? = null

    private lateinit var card: LinearLayout
    private lateinit var listBox: LinearLayout
    private lateinit var subtitle: TextView
    private lateinit var banner: TextView
    private lateinit var progRow: LinearLayout
    private lateinit var progText: TextView
    private lateinit var progBar: ProgressBar
    private lateinit var gotBox: LinearLayout
    private lateinit var avatarView: FrameLayout
    private lateinit var bgSwitch: Switch
    private lateinit var bgHint: TextView

    // ---------- кольори: світла / темна тема системи, акцент — з палітри One UI / Material You ----------
    private val night get() = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
    private val accent by lazy {
        if (Build.VERSION.SDK_INT >= 31) getColor(android.R.color.system_accent1_500) else Color.parseColor("#0A84FF")
    }
    private val fg get() = if (night) Color.parseColor("#F5F5F7") else Color.parseColor("#1C1C1E")
    private val dim get() = if (night) Color.parseColor("#99F5F5F7") else Color.parseColor("#8A1C1C1E")
    private val chip get() = if (night) Color.parseColor("#17FFFFFF") else Color.parseColor("#12000000")
    private val chip2 get() = if (night) Color.parseColor("#22FFFFFF") else Color.parseColor("#1A000000")
    private fun cardColor(glass: Boolean) = when {
        night && glass -> Color.parseColor("#A6141418")
        night -> Color.parseColor("#F2141418")
        glass -> Color.parseColor("#B8F4F4F7")
        else -> Color.parseColor("#F7F4F4F7")
    }
    private val green = Color.parseColor("#30D158")

    private val pick = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        val d = target ?: return@registerForActivityResult
        if (uris.isNotEmpty()) doSend(d, uris)
    }

    private val scan = registerForActivityResult(ScanContract()) { r ->
        val txt = r.contents ?: return@registerForActivityResult
        status("З'єднуюсь…")
        thread {
            try {
                val d = Link.pair(st, txt.trim())
                Hub.addDevice(d)
                d.online = Link.hello(st, d)
                runOnUiThread { toast("З'єднано з ${d.name}"); render() }
            } catch (e: Exception) {
                runOnUiThread { status(e.message ?: "Не вдалося з'єднатись"); toast(e.message ?: "Не вдалося з'єднатись") }
            }
        }
    }

    private val askNotif = registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    // фото для профілю — системний вибір фото (Photo Picker), без дозволу на всю галерею
    private var onPhoto: ((ByteArray) -> Unit)? = null
    private val pickPhoto = registerForActivityResult(ActivityResultContracts.PickVisualMedia()) { u ->
        val cb = onPhoto ?: return@registerForActivityResult
        u ?: return@registerForActivityResult
        thread {
            val png = runCatching { squarePng(u) }.getOrNull()
            runOnUiThread { if (png != null) cb(png) else toast("Не вдалося відкрити фото") }
        }
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()
    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
    private fun status(s: String) { subtitle.text = s }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Hub.init(this)
        buildUi()
        setupBlur()
        takeShare(intent)
        render()
        offerTile()
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            askNotif.launch(Manifest.permission.POST_NOTIFICATIONS)
        Shortcuts.publish(this, Hub.devices)
        if (intent.getBooleanExtra(EXTRA_BG_START, false)) Hub.sync(this)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        takeShare(intent)
        if (intent.getBooleanExtra(EXTRA_BG_START, false)) Hub.sync(this)
        render()
    }

    override fun onStart() {
        super.onStart()
        Hub.listeners += this
        Hub.acquire("ui")
        render()
    }

    override fun onStop() {
        super.onStop()
        Hub.listeners -= this
        Hub.release("ui")
    }

    // ---------- блюр ----------
    private var blurOn = false
    private fun setupBlur() {
        if (Build.VERSION.SDK_INT < 31) return applyGlass(false)
        window.addFlags(WindowManager.LayoutParams.FLAG_BLUR_BEHIND)
        // блюр вимикається системою в режимі енергозбереження / на слабких пристроях — тоді непрозора картка
        windowManager.addCrossWindowBlurEnabledListener(mainExecutor) { enabled -> applyGlass(enabled) }
    }

    private fun applyGlass(enabled: Boolean) {
        blurOn = enabled
        (card.background as? GradientDrawable)?.setColor(cardColor(enabled))
        window.setDimAmount(if (enabled) 0.18f else 0.45f)
        if (Build.VERSION.SDK_INT >= 31 && enabled) {
            // плавна поява розмиття, як у системних шторках One UI
            ValueAnimator.ofInt(0, dp(26)).apply {
                duration = 260
                interpolator = DecelerateInterpolator()
                addUpdateListener { a ->
                    window.attributes = window.attributes.apply { blurBehindRadius = a.animatedValue as Int }
                }
                start()
            }
        }
    }

    // ---------- «Поділитися» з інших застосунків + Direct Share ----------
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
        // вибрали конкретний ПК прямо в меню «Поділитися» → шлемо одразу
        val direct = i.getStringExtra(Intent.EXTRA_SHORTCUT_ID)?.removePrefix(Shortcuts.PREFIX)
            ?: i.getStringExtra(EXTRA_DEV)
        val dev = direct?.let { id -> Hub.devices.firstOrNull { it.id == id } }
        if (uris.isNotEmpty()) {
            pending.clear(); pending += uris; fromShare = true
            if (dev != null) doSend(dev, uris)
        } else if (dev != null && i.action == Intent.ACTION_VIEW) {
            target = dev; pick.launch(arrayOf("*/*"))
        }
    }

    // ---------- надсилання на ПК ----------
    private fun doSend(d: Device, uris: List<Uri>) {
        if (!Hub.send(this, d, uris)) return toast("Зачекайте — ще надсилаю попередні файли")
        progRow.visibility = View.VISIBLE
        progText.text = "→ ${d.name}"
        progBar.isIndeterminate = true
    }

    override fun onSend(e: Hub.SendEvent) {
        progRow.visibility = View.VISIBLE
        when (e) {
            is Hub.SendEvent.Progress -> {
                progText.text = e.text
                progBar.isIndeterminate = e.indeterminate
                if (!e.indeterminate) progBar.progress = e.pct
            }
            is Hub.SendEvent.Done -> {
                progBar.isIndeterminate = false
                progBar.progress = if (e.ok) 100 else 0
                progText.text = e.msg
                if (e.ok && fromShare) {
                    pending.clear(); fromShare = false
                    card.postDelayed({ if (Hub.sending == 0) finish() }, 1200)
                }
                render()
            }
        }
    }

    // ---------- прийом ----------
    private val rxMeter = HashMap<String, Meter>()
    override fun onRecv(e: Receiver.Event) {
        when (e) {
            is Receiver.Event.Start -> {
                rxMeter[e.name] = Meter(e.size)
                progRow.visibility = View.VISIBLE; progText.text = "← ${e.from}: ${e.name}"; progBar.isIndeterminate = false; progBar.progress = 0
            }
            is Receiver.Event.Progress -> {
                val m = rxMeter[e.name] ?: return
                progBar.progress = m.pct(e.got)
                progText.text = "← ${e.name}\n${m.text(e.got)}"
            }
            is Receiver.Event.Failed -> { rxMeter.remove(e.name); progText.text = "Обірвалось: ${e.name}" }
            is Receiver.Event.Done -> {
                rxMeter.remove(e.name)
                progText.text = "Отримано від ${e.from}: ${e.name}"
                progBar.progress = 100
                addGot(e)
            }
        }
    }

    override fun onDevices() { if (!isFinishing) render() }

    private fun addGot(e: Receiver.Event.Done) {
        val row = TextView(this).apply {
            text = "↓  ${e.name}   ·   відкрити"
            setTextColor(fg); textSize = 14f
            setPadding(dp(14), dp(12), dp(14), dp(12))
            background = round(chip, dp(14).toFloat())
            setOnClickListener {
                runCatching {
                    startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(e.uri, e.mime).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
                }.onFailure { toast("Немає програми, щоб відкрити — файл у Завантаженнях/Mg Share") }
            }
        }
        gotBox.addView(row, 0, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
        gotBox.visibility = View.VISIBLE
    }

    // ---------- плитка в шторці (Android 13+ вміє попросити сам — системне вікно) ----------
    private fun offerTile() {
        if (st.tileOffered || Build.VERSION.SDK_INT < 33) return
        st.tileOffered = true
        runCatching {
            getSystemService(StatusBarManager::class.java).requestAddTileService(
                ComponentName(this, ShareTile::class.java), "Mg Share",
                Icon.createWithResource(this, R.drawable.ic_tile), Executors.newSingleThreadExecutor()) { }
        }
    }

    // ---------- фоновий прийом ----------
    private fun setBg(on: Boolean) {
        st.bgMode = on
        Hub.sync(this)
        if (on) askBattery()
        render()
    }

    /** Samsung любить «приспати» застосунки — один раз просимо системне вікно «Не оптимізувати батарею». */
    private fun askBattery() {
        if (st.batteryAsked) return
        val pm = getSystemService(PowerManager::class.java)
        if (pm.isIgnoringBatteryOptimizations(packageName)) return
        st.batteryAsked = true
        AlertDialog.Builder(this, dialogTheme())
            .setTitle("Щоб не засинав")
            .setMessage("Samsung може приспати Mg Share у фоні, і файли з ПК не дійдуть. Дозволити працювати без обмежень батареї?")
            .setPositiveButton("Дозволити") { _, _ ->
                runCatching {
                    @Suppress("BatteryLife")
                    startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
                }
            }
            .setNegativeButton("Не зараз", null)
            .show()
    }

    /** Рідні системні діалоги (на Samsung — у стилі One UI), світлі або темні як система. */
    private fun dialogTheme() =
        if (night) android.R.style.Theme_DeviceDefault_Dialog_Alert else android.R.style.Theme_DeviceDefault_Light_Dialog_Alert

    // ---------- профіль: ім'я + аватарка ----------
    private fun squarePng(u: Uri): ByteArray {
        val src: Bitmap = if (Build.VERSION.SDK_INT >= 28) {
            ImageDecoder.decodeBitmap(ImageDecoder.createSource(contentResolver, u)) { dec, info, _ ->
                dec.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                val m = minOf(info.size.width, info.size.height)
                if (m > 1024) dec.setTargetSampleSize(m / 512)
            }
        } else contentResolver.openInputStream(u)!!.use { BitmapFactory.decodeStream(it) }
        val m = minOf(src.width, src.height)
        val sq = Bitmap.createBitmap(src, (src.width - m) / 2, (src.height - m) / 2, m, m)
        val out = Bitmap.createScaledBitmap(sq, 256, 256, true)
        return ByteArrayOutputStream().also { out.compress(Bitmap.CompressFormat.PNG, 100, it) }.toByteArray()
    }

    private fun avatar(size: Int, png: ByteArray?, name: String): FrameLayout = FrameLayout(this).apply {
        outlineProvider = object : ViewOutlineProvider() {
            override fun getOutline(v: View, o: Outline) { o.setOval(0, 0, v.width, v.height) }
        }
        clipToOutline = true
        background = round(accent, size / 2f)
        val bmp = png?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
        if (bmp != null) addView(ImageView(context).apply { setImageBitmap(bmp); scaleType = ImageView.ScaleType.CENTER_CROP }, FrameLayout.LayoutParams(-1, -1))
        else addView(TextView(context).apply {
            text = name.trim().take(1).uppercase().ifEmpty { "?" }
            setTextColor(Color.WHITE); typeface = Typeface.DEFAULT_BOLD; gravity = Gravity.CENTER
            textSize = size / resources.displayMetrics.density * 0.42f
        }, FrameLayout.LayoutParams(-1, -1))
    }

    private fun myAvatarPng() = if (st.avatarSha.isNotEmpty() && st.avatarFile.exists()) st.avatarFile.readBytes() else null

    private fun editProfile() {
        var png: ByteArray? = myAvatarPng()
        var changedPhoto = false
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(24), dp(16), dp(24), dp(4)); gravity = Gravity.CENTER_HORIZONTAL }
        val holder = FrameLayout(this)
        fun paintAvatar() {
            holder.removeAllViews()
            holder.addView(avatar(dp(96), png, st.myName), FrameLayout.LayoutParams(dp(96), dp(96)))
        }
        paintAvatar()
        box.addView(holder, LinearLayout.LayoutParams(dp(96), dp(96)))
        val btns = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        val choose = TextView(this).apply {
            text = "Змінити фото"; setTextColor(accent); textSize = 15f; typeface = Typeface.DEFAULT_BOLD
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }
        val drop = TextView(this).apply {
            text = "Прибрати"; setTextColor(Color.parseColor("#FF453A")); textSize = 15f
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }
        btns.addView(choose); btns.addView(drop)
        box.addView(btns)
        val nameEd = EditText(this).apply {
            setText(st.myName); hint = Build.MODEL ?: "Телефон"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
            setSingleLine(); setSelectAllOnFocus(true)
        }
        box.addView(TextView(this).apply { text = "Ім'я, яке бачать ваші ПК"; textSize = 13f; alpha = 0.7f },
            LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
        box.addView(nameEd, LinearLayout.LayoutParams(-1, -2))
        val pcCb = CheckBox(this).apply { text = "Також фото користувача на ПК\n(вхід SDDM, налаштування mgOS)"; isChecked = st.pcAvatar }
        box.addView(pcCb, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
        box.addView(TextView(this).apply {
            text = "Зміни розлетяться на всі з'єднані ПК — на ті, що зараз вимкнені, щойно вони з'являться."
            textSize = 12f; alpha = 0.6f
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(6) })

        choose.setOnClickListener {
            onPhoto = { b -> png = b; changedPhoto = true; paintAvatar() }
            pickPhoto.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
        }
        drop.setOnClickListener { png = null; changedPhoto = true; paintAvatar() }

        AlertDialog.Builder(this, dialogTheme())
            .setTitle("Профіль")
            .setView(ScrollView(this).apply { addView(box) })
            .setPositiveButton("Зберегти") { _, _ ->
                st.myName = nameEd.text.toString()
                st.pcAvatar = pcCb.isChecked
                if (changedPhoto) st.setAvatar(png)
                Hub.profileChanged()
                render()
                toast("Профіль оновлено — надсилаю на ПК")
            }
            .setNegativeButton("Скасувати", null)
            .setOnDismissListener { onPhoto = null }
            .show()
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
        background = round(if (primary) accent else chip2, dp(14).toFloat())
        setOnClickListener { onClick() }
    }

    private fun buildUi() {
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        // тап повз картку — закрити; передача (якщо йде) доїде у фоні з прогресом у шторці
        val root = FrameLayout(this).apply { setOnClickListener { finish() } }
        card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = round(cardColor(false), dp(28).toFloat(), top = true)
            setPadding(dp(20), dp(10), dp(20), dp(20))
            isClickable = true
        }
        root.setOnApplyWindowInsetsListener { _, ins ->
            @Suppress("DEPRECATION") card.setPadding(dp(20), dp(10), dp(20), dp(20) + ins.systemWindowInsetBottom)
            ins
        }
        card.addView(View(this).apply { background = round(dim, dp(3).toFloat()); alpha = 0.6f },
            LinearLayout.LayoutParams(dp(36), dp(5)).apply { gravity = Gravity.CENTER_HORIZONTAL; bottomMargin = dp(14) })

        // шапка: аватарка профілю (тап — профіль), назва, стан
        val head = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        avatarView = FrameLayout(this).apply { setOnClickListener { editProfile() } }
        head.addView(avatarView, LinearLayout.LayoutParams(dp(44), dp(44)))
        val tcol = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(12), 0, 0, 0); setOnClickListener { editProfile() } }
        tcol.addView(TextView(this).apply { tag = "title"; setTextColor(fg); textSize = 20f; typeface = Typeface.DEFAULT_BOLD })
        subtitle = TextView(this).apply { setTextColor(dim); textSize = 13f }
        tcol.addView(subtitle)
        head.addView(tcol, LinearLayout.LayoutParams(0, -2, 1f))
        card.addView(head)

        // фоновий прийом — те саме, що плитка в шторці
        val bgRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(14), dp(10), dp(10), dp(10))
            background = round(chip, dp(18).toFloat())
        }
        val bcol = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        bcol.addView(TextView(this).apply { text = "Чекати файли у фоні"; setTextColor(fg); textSize = 15f; typeface = Typeface.DEFAULT_BOLD })
        bgHint = TextView(this).apply { setTextColor(dim); textSize = 12f }
        bcol.addView(bgHint)
        bgRow.addView(bcol, LinearLayout.LayoutParams(0, -2, 1f))
        bgSwitch = Switch(this)
        bgRow.addView(bgSwitch)
        bgRow.setOnClickListener { bgSwitch.toggle() }
        bgSwitch.setOnCheckedChangeListener { _, on -> if (on != st.bgMode) setBg(on) }
        card.addView(bgRow, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(14) })

        banner = TextView(this).apply {
            setTextColor(fg); textSize = 14f
            setPadding(dp(14), dp(10), dp(14), dp(10))
            background = round((accent and 0x00FFFFFF) or 0x33000000, dp(12).toFloat())
            visibility = View.GONE
        }
        card.addView(banner, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(10) })

        listBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        card.addView(listBox, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(6) })

        progRow = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; visibility = View.GONE }
        progText = TextView(this).apply { setTextColor(fg); textSize = 13f; maxLines = 3 }
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

        // поява картки знизу
        card.translationY = dp(120).toFloat(); card.alpha = 0f
        card.animate().translationY(0f).alpha(1f).setDuration(280).setInterpolator(DecelerateInterpolator(2f)).start()
    }

    private var avatarSig = ""
    private fun render() {
        val sig = st.avatarSha + "|" + st.myName
        if (sig != avatarSig) {
            avatarSig = sig
            avatarView.removeAllViews()
            avatarView.addView(avatar(dp(44), myAvatarPng(), st.myName), FrameLayout.LayoutParams(-1, -1))
            card.findViewWithTag<TextView>("title")?.text = st.myName
        }
        bgSwitch.isChecked = st.bgMode
        bgHint.text = if (st.bgMode) "Увімкнено · ноут шле файли, навіть коли вікно закрите"
        else "Вимкнено · файли приходять, лише коли відкрите це вікно"

        if (pending.isNotEmpty()) {
            val first = runCatching { Hub.nameOf(pending[0]).first }.getOrDefault("файл")
            banner.text = if (pending.size == 1) "Надіслати: $first" else "Надіслати: $first і ще ${pending.size - 1}"
            banner.visibility = View.VISIBLE
        } else banner.visibility = View.GONE
        val devices = Hub.devices
        val anyOn = devices.any { it.online }
        status(when {
            devices.isEmpty() -> "Додайте ноут: топбар → годинник → «Додати»"
            anyOn && st.bgMode -> "Mg Share · на зв'язку, приймаю у фоні"
            anyOn -> "Mg Share · поки вікно відкрите, ноут може слати файли"
            else -> "Mg Share · шукаю свої пристрої…"
        })
        listBox.removeAllViews()
        for (d in devices) listBox.addView(deviceRow(d), LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
    }

    private fun deviceRow(d: Device): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(14), dp(12), dp(12), dp(12))
            background = round(chip, dp(18).toFloat())
            setOnLongClickListener {
                AlertDialog.Builder(this@MainActivity, dialogTheme())
                    .setTitle("Забути ${d.name}?")
                    .setMessage("Щоб знову обмінюватись файлами, треба буде з'єднатись через QR.")
                    .setPositiveButton("Забути") { _, _ -> Hub.forget(d.id); render() }
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
            setTextColor(if (d.online) green else dim); textSize = 12f
        })
        row.addView(col, LinearLayout.LayoutParams(0, -2, 1f))
        row.addView(button(if (pending.isNotEmpty()) "Надіслати" else "Файл…", true) {
            if (Hub.sending > 0) return@button toast("Зачекайте — ще надсилаю")
            if (pending.isNotEmpty()) doSend(d, pending.toList())
            else { target = d; pick.launch(arrayOf("*/*")) }
        })
        return row
    }
}
