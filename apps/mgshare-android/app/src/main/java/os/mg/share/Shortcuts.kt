package os.mg.share

import android.content.Context
import android.content.Intent
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.drawable.Icon

/**
 * Свої ПК прямо в системному меню «Поділитися» (Direct Share) і в довгому натисканні на іконку:
 * вибрали «Ноут» у шторці поділитися — файл летить одразу, без зайвих кроків.
 */
object Shortcuts {
    const val CATEGORY = "os.mg.share.category.PC"
    const val PREFIX = "dev_"

    fun publish(ctx: Context, devs: List<Device>) = runCatching {
        val sm = ctx.getSystemService(ShortcutManager::class.java) ?: return@runCatching
        val max = sm.maxShortcutCountPerActivity.coerceAtMost(4)
        val list = devs.take(max).mapIndexed { i, d ->
            ShortcutInfo.Builder(ctx, PREFIX + d.id)
                .setShortLabel(d.name)
                .setLongLabel("Надіслати на ${d.name}")
                .setIcon(icon(ctx))
                .setCategories(setOf(CATEGORY))
                .setLongLived(true)
                .setRank(i)
                .setIntent(Intent(ctx, MainActivity::class.java).setAction(Intent.ACTION_VIEW)
                    .putExtra(MainActivity.EXTRA_DEV, d.id).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP))
                .build()
        }
        sm.dynamicShortcuts = list
    }

    private var cached: Icon? = null
    private fun icon(ctx: Context): Icon = cached ?: run {
        // адаптивна іконка: 108dp-полотно, малюнок у безпечній зоні 72dp
        val s = 216
        val bmp = Bitmap.createBitmap(s, s, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.drawColor(Color.parseColor("#0A84FF"))
        ctx.getDrawable(R.drawable.ic_laptop)!!.mutate().apply {
            setTint(Color.WHITE)
            val pad = (s * 0.32f).toInt()
            setBounds(pad, pad, s - pad, s - pad)
            draw(c)
        }
        Icon.createWithAdaptiveBitmap(bmp).also { cached = it }
    }
}
