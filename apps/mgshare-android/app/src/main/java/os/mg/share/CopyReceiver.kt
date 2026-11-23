package os.mg.share

import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.widget.Toast

/** Кнопка «Копіювати» у сповіщенні з посиланням / текстом від ПК. */
class CopyReceiver : BroadcastReceiver() {
    companion object {
        const val TEXT = "text"
        const val NOTIF = "notif"
    }

    override fun onReceive(ctx: Context, i: Intent) {
        val t = i.getStringExtra(TEXT) ?: return
        ctx.getSystemService(ClipboardManager::class.java)?.setPrimaryClip(ClipData.newPlainText("Mg Share", t))
        Notif.cancel(ctx, i.getIntExtra(NOTIF, 0))
        // Android 13+ сам показує «Скопійовано» — свій тост лише на старіших
        if (android.os.Build.VERSION.SDK_INT < 33) Toast.makeText(ctx, "Скопійовано", Toast.LENGTH_SHORT).show()
    }
}
