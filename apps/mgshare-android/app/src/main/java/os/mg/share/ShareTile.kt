package os.mg.share

import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.Intent
import android.os.Build
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService

/**
 * Плитка в шторці = перемикач фонового прийому.
 *  Увімкнена — телефон завжди чекає файли від своїх ПК (служба зі сповіщенням).
 *  Вимкнена — файли приходять, лише коли відкрите вікно Mg Share.
 * Довге натискання на плитку відкриває саме вікно.
 */
class ShareTile : TileService() {
    override fun onStartListening() {
        Hub.init(this)
        paint()
    }

    private fun paint() {
        val on = Hub.st.bgMode
        qsTile?.apply {
            state = if (on) Tile.STATE_ACTIVE else Tile.STATE_INACTIVE
            label = "Mg Share"
            if (Build.VERSION.SDK_INT >= 29) subtitle = if (on) "Чекає файли" else "Вимкнено"
            if (Build.VERSION.SDK_INT >= 30) stateDescription = if (on) "Фоновий прийом увімкнено" else "Фоновий прийом вимкнено"
            updateTile()
        }
    }

    override fun onClick() {
        Hub.init(this)
        val on = !Hub.st.bgMode
        Hub.st.bgMode = on
        val ok = Hub.sync(this)
        paint()
        // система не дала стартувати службу з плитки — відкриваємо вікно, воно запустить її вже «на передньому плані»
        if (on && !ok) open(Intent(this, MainActivity::class.java).putExtra(MainActivity.EXTRA_BG_START, true))
    }

    @SuppressLint("StartActivityAndCollapseDeprecated")
    private fun open(i: Intent) {
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= 34) {
            startActivityAndCollapse(PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        } else {
            @Suppress("DEPRECATION") startActivityAndCollapse(i)
        }
    }
}
