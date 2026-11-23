package os.mg.share

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle

/**
 * Системні сповіщення — на Samsung (One UI) вони показуються як рідні спливаючі вікна:
 *  - «Отримані файли» — важливий канал → спливає зверху (heads-up) з кнопками «Відкрити» / «Поділитися»;
 *  - «Передача» — тихий прогрес зі швидкістю (на Android 16 просимо «живе» сповіщення — Now Bar на One UI 8);
 *  - «Фоновий прийом» — мінімальне постійне сповіщення служби з кнопкою «Вимкнути».
 */
object Notif {
    const val ID_SERVICE = 1
    const val ID_RX = 2
    const val ID_TX = 3
    private var nextId = 100

    private const val CH_BG = "bg"
    private const val CH_XFER = "xfer"
    private const val CH_GOT = "got"

    fun channels(ctx: Context) {
        if (Build.VERSION.SDK_INT < 26) return
        val nm = ctx.getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CH_BG, "Фоновий прийом", NotificationManager.IMPORTANCE_MIN).apply {
            description = "Постійне сповіщення, поки Mg Share чекає файли у фоні"
            setShowBadge(false)
        })
        nm.createNotificationChannel(NotificationChannel(CH_XFER, "Передача файлів", NotificationManager.IMPORTANCE_LOW).apply {
            description = "Прогрес і швидкість передачі"
            setShowBadge(false)
        })
        nm.createNotificationChannel(NotificationChannel(CH_GOT, "Отримані файли", NotificationManager.IMPORTANCE_HIGH).apply {
            description = "Спливаюче сповіщення, коли ПК надіслав файл"
            enableVibration(true)
        })
    }

    private fun builder(ctx: Context, ch: String) =
        (if (Build.VERSION.SDK_INT >= 26) Notification.Builder(ctx, ch) else @Suppress("DEPRECATION") Notification.Builder(ctx))
            .setSmallIcon(R.drawable.ic_tile)
            .setColor(Color.parseColor("#0A84FF"))

    private fun openApp(ctx: Context) = PendingIntent.getActivity(ctx, 0,
        Intent(ctx, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), PendingIntent.FLAG_IMMUTABLE)

    fun service(ctx: Context): Notification {
        val on = Hub.devices.filter { it.online }.joinToString(", ") { it.name }
        val stop = PendingIntent.getService(ctx, 1, Intent(ctx, BgService::class.java).setAction(BgService.STOP), PendingIntent.FLAG_IMMUTABLE)
        val text = when {
            Hub.sending > 0 && !Hub.st.bgMode -> "Надсилаю файли…"
            on.isNotEmpty() -> "Чекаю файли · на зв'язку: $on"
            Hub.devices.isEmpty() -> "Чекаю файли · додайте ноут через QR"
            else -> "Чекаю файли від своїх ПК"
        }
        return builder(ctx, CH_BG)
            .setContentTitle("Mg Share у фоні")
            .setContentText(text)
            .setOngoing(true)
            .setShowWhen(false)
            .setContentIntent(openApp(ctx))
            .apply { if (Hub.st.bgMode) addAction(Notification.Action.Builder(null, "Вимкнути", stop).build()) }
            .apply { if (Build.VERSION.SDK_INT >= 31) setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE) }
            .build()
    }

    fun progress(ctx: Context, title: String, text: String, pct: Int, indeterminate: Boolean): Notification =
        builder(ctx, CH_XFER)
            .setContentTitle(title)
            .setContentText(text.replace('\n', ' '))
            .setStyle(Notification.BigTextStyle().bigText(text))
            .setProgress(100, pct, indeterminate)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setContentIntent(openApp(ctx))
            .setCategory(Notification.CATEGORY_PROGRESS)
            // Android 16: просимо «живе» сповіщення (Live Update / Now Bar). Старі системи ключ просто ігнорують.
            .addExtras(Bundle().apply { putBoolean("android.requestPromotedOngoing", true) })
            .build()

    fun received(ctx: Context, from: String, name: String, uri: Uri, mime: String) {
        val view = Intent(Intent.ACTION_VIEW).setDataAndType(uri, mime)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        val share = Intent.createChooser(Intent(Intent.ACTION_SEND).setType(mime).putExtra(Intent.EXTRA_STREAM, uri)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), name).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        val id = nextId++
        val n = builder(ctx, CH_GOT)
            .setContentTitle("Від $from")
            .setContentText(name)
            .setCategory(Notification.CATEGORY_STATUS)
            .setAutoCancel(true)
            .setContentIntent(PendingIntent.getActivity(ctx, id, view, PendingIntent.FLAG_IMMUTABLE))
            .addAction(Notification.Action.Builder(null, "Відкрити", PendingIntent.getActivity(ctx, id, view, PendingIntent.FLAG_IMMUTABLE)).build())
            .addAction(Notification.Action.Builder(null, "Поділитися", PendingIntent.getActivity(ctx, id + 50_000, share, PendingIntent.FLAG_IMMUTABLE)).build())
            .apply { if (Build.VERSION.SDK_INT < 26) @Suppress("DEPRECATION") setPriority(Notification.PRIORITY_HIGH).setDefaults(Notification.DEFAULT_ALL) }
            .build()
        post(ctx, id, n)
    }

    fun simple(ctx: Context, title: String, text: String) {
        post(ctx, nextId++, builder(ctx, CH_GOT).setContentTitle(title).setContentText(text).setAutoCancel(true)
            .setContentIntent(openApp(ctx)).build())
    }

    fun post(ctx: Context, id: Int, n: Notification) {
        runCatching { ctx.getSystemService(NotificationManager::class.java).notify(id, n) }
    }

    fun cancel(ctx: Context, id: Int) {
        runCatching { ctx.getSystemService(NotificationManager::class.java).cancel(id) }
    }
}
