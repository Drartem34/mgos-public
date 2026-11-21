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
 *  - POST /hello {"port":47812} — «я відкритий, шли файли»; POST /bye — закрився; PUT /file?name= — файл
 */
object Link {
    const val PC_PORT = 47811
    const val PHONE_PORT = 47812

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
        c.readTimeout = 20000
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

    /** «Я відкритий» — після цього ПК може надсилати файли на PHONE_PORT. */
    fun hello(st: Store, d: Device): Boolean = runCatching {
        val c = open(st, d, "POST", "/hello")
        c.doOutput = true
        c.setRequestProperty("Content-Type", "application/json")
        c.outputStream.use { it.write(JSONObject().put("port", PHONE_PORT).toString().toByteArray()) }
        val ok = c.responseCode == 200
        if (ok) JSONObject(c.inputStream.bufferedReader().readText()).optString("name").takeIf { it.isNotEmpty() }?.let { d.name = it }
        c.disconnect()
        ok
    }.getOrDefault(false)

    fun bye(st: Store, d: Device) {
        runCatching { val c = open(st, d, "POST", "/bye", 1500); c.doOutput = true; c.outputStream.close(); c.responseCode; c.disconnect() }
    }

    /** Надсилає файл на ПК. progress(0..100). */
    fun upload(st: Store, d: Device, name: String, size: Long, input: InputStream, progress: (Int) -> Unit): Boolean {
        val path = "/file?name=" + URLEncoder.encode(name, "UTF-8").replace("+", "%20")
        val c = open(st, d, "PUT", path, 6000)
        c.doOutput = true
        c.setFixedLengthStreamingMode(size)
        c.setRequestProperty("Content-Type", "application/octet-stream")
        var sent = 0L
        var last = -1
        c.outputStream.use { out ->
            input.use { inp ->
                val buf = ByteArray(1 shl 16)
                while (true) {
                    val n = inp.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    sent += n
                    val pct = if (size > 0) (sent * 100 / size).toInt() else 100
                    if (pct != last) { last = pct; progress(pct) }
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
