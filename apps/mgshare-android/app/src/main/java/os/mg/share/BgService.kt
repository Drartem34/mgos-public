package os.mg.share

import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Фоновий прийом: служба переднього плану, яку вмикає плитка в шторці (або перемикач у вікні).
 * Сама нічого не робить — лише тримає процес живим і показує сповіщення; роботу робить Hub.
 * Також живе, поки доїжджає надсилання, навіть якщо вікно вже закрили.
 */
class BgService : Service() {
    companion object { const val STOP = "os.mg.share.STOP" }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        Hub.init(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val n = Notif.service(this)
        if (Build.VERSION.SDK_INT >= 29) startForeground(Notif.ID_SERVICE, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        else startForeground(Notif.ID_SERVICE, n)
        Hub.serviceUp = true
        if (intent?.action == STOP) {
            Hub.st.bgMode = false
            Hub.sync(this)
            if (Hub.sending == 0) stopSelf()
            return START_NOT_STICKY
        }
        if (!Hub.st.bgMode && Hub.sending == 0) { stopSelf(); return START_NOT_STICKY }
        if (Hub.st.bgMode) Hub.acquire("bg")
        return if (Hub.st.bgMode) START_STICKY else START_NOT_STICKY
    }

    override fun onDestroy() {
        Hub.serviceUp = false
        if (!Hub.st.bgMode) Hub.release("bg")
        super.onDestroy()
    }
}

/** Після перезавантаження / оновлення застосунку — повертаємо фоновий прийом, якщо його було ввімкнено. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED && intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        Hub.init(ctx)
        if (Hub.st.bgMode) Hub.sync(ctx)
    }
}
