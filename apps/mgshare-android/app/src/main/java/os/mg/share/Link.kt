package os.mg.share

import org.json.JSONObject
import java.io.InputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import java.net.URL
import java.net.URLEncoder
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Протокол Mg Share (як у mgos-share на ПК):
 *  - пошук: UDP {"t":"hello","id":<мій id>} на :47811 (широкомовно) → ПК відповідає лише своїм {"t":"here",...}
 *  - запити підписані: X-Mg-From / X-Mg-Time / X-Mg-Sig = HMAC-SHA256(key, "from\ntime\nMETHOD\npath")
 *  - POST /hello {"port","name","avatar","pc_avatar"} — «я відкритий» + профіль; ПК відповідає need_avatar, якщо
 *    в нього інша аватарка → PUT /avatar?sha=&pc= (PNG). POST /bye — закрився; PUT /file?name= — файл
 */
object Link {
    const val PC_PORT = 47811
    const val PHONE_PORT = 47812
    /** Великі шматки: менше системних викликів і записів у FUSE (Завантаження) — помітно швидше на великих файлах. */
    const val CHUNK = 1 shl 20

    fun sign(key: String, from: String, t: String, method: String, path: String): String {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(unhex(key), "HmacSHA256"))
        return mac.doFinal("$from\n$t\n$method\n$path".toByteArray()).joinToString("") { "%02x".format(it) }
    }

    private fun unhex(s: String) = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

    private fun now() = (System.currentTimeMillis() / 1000).toString()

    private fun open(st: Store, d: Device, method: String, path: String, timeout: Int = 4000): HttpURLConnection {
        val c = URL("http://${d.ip}:${d.port}$path").openConnection() as HttpURLConnection
        val t = now()
        c.requestMethod = method
        c.connectTimeout = timeout
        c.readTimeout = 30000
        c.useCaches = false
        c.setRequestProperty("X-Mg-From", st.myId)
        c.setRequestProperty("X-Mg-Time", t)
        c.setRequestProperty("X-Mg-Sig", sign(d.key, st.myId, t, method, path))
        return c
    }

    /** Широкомовний «привіт» на всі мережі телефону (Wi-Fi, власна роздача) + останні відомі IP. id → IP. */
    fun discover(st: Store, devs: List<Device>, waitMs: Int = 1200): Map<String, String> {
        val found = HashMap<String, String>()
        val known = devs.map { it.id }.toSet()
        try {
            DatagramSocket().use { s ->
                s.broadcast = true
                s.soTimeout = 250
                val msg = JSONObject().put("t", "hello").put("id", st.myId).toString().toByteArray()
                val targets = LinkedHashSet<InetAddress>()
                for (ni in NetworkInterface.getNetworkInterfaces()) {
                    if (!ni.isUp || ni.isLoopback) continue
                    for (ia in ni.interfaceAddresses) if (ia.address is Inet4Address) ia.broadcast?.let { targets += it }
                }
                targets += InetAddress.getByName("255.255.255.255")
                for (d in devs) if (d.ip.isNotEmpty()) runCatching { targets += InetAddress.getByName(d.ip) }
                for (a in targets) runCatching { s.send(DatagramPacket(msg, msg.size, a, PC_PORT)) }
                val buf = ByteArray(2048)
                val end = System.currentTimeMillis() + waitMs
                while (System.currentTimeMillis() < end && found.size < known.size) {
                    val p = DatagramPacket(buf, buf.size)
                    try { s.receive(p) } catch (_: SocketTimeoutException) { continue }
                    val j = runCatching { JSONObject(String(p.data, 0, p.length)) }.getOrNull() ?: continue
                    val id = j.optString("id")
                    if (j.optString("t") == "here" && id in known) found[id] = p.address.hostAddress ?: continue
                }
            }
        } catch (_: Exception) { }
        return found
    }

    /**
     * «Я відкритий» + мій профіль. Якщо ПК каже, що в нього стара аватарка, — одразу шлемо нову.
     * Так аватарка доходить і до тих ПК, що були вимкнені в момент зміни.
     */
    fun hello(st: Store, d: Device): Boolean = runCatching {
        val c = open(st, d, "POST", "/hello")
        c.doOutput = true
        c.setRequestProperty("Content-Type", "application/json")
        val body = JSONObject().put("port", PHONE_PORT).put("name", st.myName)
            .put("avatar", st.avatarSha).put("pc_avatar", st.pcAvatar).put("bg", st.bgMode)
        c.outputStream.use { it.write(body.toString().toByteArray()) }
        val ok = c.responseCode == 200
        var needAvatar = false
        if (ok) {
            val j = JSONObject(c.inputStream.bufferedReader().readText())
            j.optString("name").takeIf { it.isNotEmpty() }?.let { d.name = it }
            needAvatar = j.optBoolean("need_avatar", false)
        }
        c.disconnect()
        if (ok && needAvatar) pushAvatar(st, d)
        ok
    }.getOrDefault(false)

    fun pushAvatar(st: Store, d: Device): Boolean = runCatching {
        val png = if (st.avatarSha.isNotEmpty() && st.avatarFile.exists()) st.avatarFile.readBytes() else ByteArray(0)
        val c = open(st, d, "PUT", "/avatar?sha=${st.avatarSha}&pc=${if (st.pcAvatar) 1 else 0}", 4000)
        c.doOutput = true
        c.setFixedLengthStreamingMode(png.size)
        c.setRequestProperty("Content-Type", "image/png")
        c.outputStream.use { it.write(png) }
        val ok = c.responseCode == 200
        c.disconnect()
        ok
    }.getOrDefault(false)

    fun bye(st: Store, d: Device) {
        runCatching { val c = open(st, d, "POST", "/bye", 1500); c.doOutput = true; c.outputStream.close(); c.responseCode; c.disconnect() }
    }

    /** Посилання (url) або просто текст → ПК. ПК показує сповіщення «Відкрити» / копіює текст у буфер. */
    fun sendLink(st: Store, d: Device, url: String, text: String): Boolean = runCatching {
        val c = open(st, d, "POST", "/link", 4000)
        c.doOutput = true
        c.setRequestProperty("Content-Type", "application/json")
        val body = JSONObject().put("url", url).put("text", text).toString().toByteArray()
        c.setFixedLengthStreamingMode(body.size)
        c.outputStream.use { it.write(body) }
        val ok = c.responseCode == 200
        c.disconnect()
        ok
    }.getOrDefault(false)

    /** Перше http(s)-посилання в тексті (браузери шлють «Заголовок\nhttps://…»). */
    fun findUrl(s: String): String? =
        Regex("""https?://[^\s<>"']+""", RegexOption.IGNORE_CASE).find(s)?.value?.trimEnd('.', ',', ')', ';', '!', '?')

    /** Надсилає файл на ПК. progress(байти надіслано). */
    fun upload(st: Store, d: Device, name: String, size: Long, input: InputStream, progress: (Long) -> Unit): Boolean {
        val path = "/file?name=" + URLEncoder.encode(name, "UTF-8").replace("+", "%20")
        val c = open(st, d, "PUT", path, 6000)
        c.doOutput = true
        c.setFixedLengthStreamingMode(size)
        c.setRequestProperty("Content-Type", "application/octet-stream")
        var sent = 0L
        c.outputStream.use { out ->
            input.use { inp ->
                val buf = ByteArray(CHUNK)
                while (true) {
                    val n = inp.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    sent += n
                    progress(sent)
                }
            }
        }
        val ok = c.responseCode == 200
        c.disconnect()
        return ok && sent == size
    }

    /** З'єднання: QR з ПК містить http://<ip>:47811/pair/<token>. Повертає новий пристрій. */
    fun pair(st: Store, url: String): Device {
        val u = URL(url)
        require(u.path.startsWith("/pair/")) { "Це не QR з'єднання Mg Share" }
        val c = u.openConnection() as HttpURLConnection
        c.requestMethod = "POST"
        c.connectTimeout = 5000
        c.readTimeout = 8000
        c.doOutput = true
        c.setRequestProperty("Content-Type", "application/json")
        val body = JSONObject().put("id", st.myId).put("name", st.myName).put("kind", "phone").put("port", PHONE_PORT)
        c.outputStream.use { it.write(body.toString().toByteArray()) }
        val code = c.responseCode
        if (code == 410) throw IllegalStateException("QR застарів — натисніть «Додати» на ПК ще раз")
        if (code != 200) throw IllegalStateException("ПК відповів $code")
        val j = JSONObject(c.inputStream.bufferedReader().readText())
        c.disconnect()
        return Device(j.getString("id"), j.optString("name", "ПК"), j.optString("kind", "laptop"), j.getString("key"),
            u.host, if (u.port > 0) u.port else PC_PORT)
    }
}
