package os.mg.share

import android.content.Context
import android.os.Build
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.security.SecureRandom

/** Свій пристрій (ПК), з яким один раз з'єднались через QR. */
data class Device(
    val id: String,
    var name: String,
    val kind: String,
    val key: String,
    var ip: String,
    var port: Int,
) {
    @Volatile var online = false
}

/** Ідентичність телефону, профіль (ім'я + аватарка) і список своїх пристроїв. */
class Store(ctx: Context) {
    private val sp = ctx.getSharedPreferences("mg", Context.MODE_PRIVATE)

    val myId: String = sp.getString("id", null) ?: hex(8).also { sp.edit().putString("id", it).apply() }

    /** Ім'я, яке бачать ПК. Порожнє → модель телефона. */
    var myName: String
        get() = sp.getString("name", null)?.takeIf { it.isNotBlank() } ?: (Build.MODEL ?: "Телефон")
        set(v) { sp.edit().putString("name", v.trim().take(40)).commit() }

    /** Аватарка профілю: квадратний PNG 256×256 у приватній теці застосунку. */
    val avatarFile = File(ctx.filesDir, "avatar.png")

    /** Короткий хеш аватарки ("" — немає): ПК порівнює його й просить файл лише коли змінився. */
    var avatarSha: String
        get() = sp.getString("avatar_sha", "") ?: ""
        private set(v) { sp.edit().putString("avatar_sha", v).commit() }

    fun setAvatar(png: ByteArray?) {
        if (png == null) { avatarFile.delete(); avatarSha = ""; return }
        avatarFile.writeBytes(png)
        avatarSha = sha(png)
    }

    /** Ставити цю аватарку ще й як фото користувача на ПК (SDDM, ~/.face, налаштування mgOS). */
    var pcAvatar: Boolean
        get() = sp.getBoolean("pc_avatar", true)
        set(v) { sp.edit().putBoolean("pc_avatar", v).commit() }

    /** Фоновий прийом (плитка в шторці): телефон завжди чекає файли від ПК. */
    var bgMode: Boolean
        get() = sp.getBoolean("bg", false)
        set(v) { sp.edit().putBoolean("bg", v).commit() }

    var tileOffered: Boolean
        get() = sp.getBoolean("tile_offered", false)
        set(v) { sp.edit().putBoolean("tile_offered", v).apply() }

    var batteryAsked: Boolean
        get() = sp.getBoolean("battery_asked", false)
        set(v) { sp.edit().putBoolean("battery_asked", v).apply() }

    @Synchronized fun devices(): MutableList<Device> {
        val o = stored()
        val out = mutableListOf<Device>()
        for (id in o.keys()) {
            val d = o.getJSONObject(id)
            out += Device(id, d.optString("name", "ПК"), d.optString("kind", "laptop"), d.getString("key"),
                d.optString("ip", ""), d.optInt("port", Link.PC_PORT))
        }
        return out
    }

    private fun json(d: Device) = JSONObject().apply {
        put("name", d.name); put("kind", d.kind); put("key", d.key); put("ip", d.ip); put("port", d.port)
    }

    private fun stored() = JSONObject(sp.getString("devices", "{}") ?: "{}")

    // commit(), а не apply(): Receiver читає сховище з іншого потоку одразу після з'єднання
    private fun write(o: JSONObject) { sp.edit().putString("devices", o.toString()).commit() }

    /** Додати / замінити один пристрій (з'єднання через QR). Інші записи не чіпає. */
    @Synchronized fun add(d: Device) { write(stored().put(d.id, json(d))) }

    /** Забути пристрій. */
    @Synchronized fun remove(id: String) { write(stored().apply { remove(id) }) }

    /**
     * Лише оновити IP / порт / ім'я тих, хто ВЖЕ є у сховищі — нікого не додає й не видаляє
     * (інакше паралельне з'єднання нового ПК губилось би: знімок без нього перезаписував сховище).
     */
    @Synchronized fun updateKnown(list: List<Device>) {
        val o = stored()
        for (d in list) o.optJSONObject(d.id)?.apply { put("name", d.name); put("ip", d.ip); put("port", d.port) }
        write(o)
    }

    companion object {
        fun hex(n: Int): String {
            val b = ByteArray(n); SecureRandom().nextBytes(b)
            return b.joinToString("") { "%02x".format(it) }
        }

        fun sha(b: ByteArray): String =
            MessageDigest.getInstance("SHA-256").digest(b).joinToString("") { "%02x".format(it) }.take(16)
    }
}
