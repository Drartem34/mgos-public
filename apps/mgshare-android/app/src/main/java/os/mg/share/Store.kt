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

    fun devices(): MutableList<Device> {
        val o = JSONObject(sp.getString("devices", "{}") ?: "{}")
        val out = mutableListOf<Device>()
        for (id in o.keys()) {
            val d = o.getJSONObject(id)
            out += Device(id, d.optString("name", "ПК"), d.optString("kind", "laptop"), d.getString("key"),
                d.optString("ip", ""), d.optInt("port", Link.PC_PORT))
        }
        return out
    }

    fun save(list: List<Device>) {
        val o = JSONObject()
        for (d in list) o.put(d.id, JSONObject().apply {
            put("name", d.name); put("kind", d.kind); put("key", d.key); put("ip", d.ip); put("port", d.port)
        })
        sp.edit().putString("devices", o.toString()).apply()
    }

    companion object {
        fun hex(n: Int): String {
            val b = ByteArray(n); SecureRandom().nextBytes(b)
            return b.joinToString("") { "%02x".format(it) }
        }
    }
}
