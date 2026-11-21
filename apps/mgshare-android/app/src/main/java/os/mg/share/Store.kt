package os.mg.share

import android.content.Context
import android.os.Build
import org.json.JSONObject
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

/** Ідентичність телефону й список своїх пристроїв (SharedPreferences, без мережі й фону). */
class Store(ctx: Context) {
    private val sp = ctx.getSharedPreferences("mg", Context.MODE_PRIVATE)

    val myId: String = sp.getString("id", null) ?: hex(8).also { sp.edit().putString("id", it).apply() }
    val myName: String get() = sp.getString("name", null) ?: (Build.MODEL ?: "Телефон")

    var tileOffered: Boolean
        get() = sp.getBoolean("tile_offered", false)
        set(v) { sp.edit().putBoolean("tile_offered", v).apply() }

    @Synchronized fun devices(): MutableList<Device> {
        val o = JSONObject(sp.getString("devices", "{}") ?: "{}")
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
     * Лише оновити IP / порт / ім'я тих, хто ВЖЕ є у сховищі — нікого не додає й не видаляє.
     * Раніше цикл присутності зберігав свій знімок списку цілком: якщо в цей час (≈1–5 с пошуку)
     * з'єднувався новий ПК, знімок без нього перезаписував сховище — ПК зникав зі списку, а його
     * файли телефон відкидав (403: невідомий пристрій).
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
    }
}
