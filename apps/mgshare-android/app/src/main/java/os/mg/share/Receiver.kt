package os.mg.share

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.provider.MediaStore
import android.webkit.MimeTypeMap
import java.io.BufferedInputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.URLDecoder
import kotlin.concurrent.thread

/**
 * Мінімальний HTTP-сервер на :47812 — працює ЛИШЕ поки вікно Mg Share відкрите (start у onStart, stop у onStop).
 * Приймає тільки підписані PUT /file від своїх пристроїв і кладе файл у Завантаження/Mg Share.
 */
class Receiver(
    private val ctx: Context,
    private val st: Store,
    private val onEvent: (Event) -> Unit,
) {
    sealed class Event {
        data class Start(val from: String, val name: String) : Event()
        data class Progress(val name: String, val pct: Int) : Event()
        data class Done(val from: String, val name: String, val uri: Uri, val mime: String) : Event()
        data class Failed(val name: String) : Event()
    }

    @Volatile private var server: ServerSocket? = null

    fun start() {
        if (server != null) return
        val ss = try {
            ServerSocket().apply { reuseAddress = true; bind(InetSocketAddress(Link.PHONE_PORT)) }
        } catch (_: Exception) { return }
        server = ss
        thread(name = "mg-recv", isDaemon = true) {
            while (!ss.isClosed) {
                val s = try { ss.accept() } catch (_: Exception) { break }
                thread(isDaemon = true) { runCatching { handle(s) }; runCatching { s.close() } }
            }
        }
    }

    fun stop() {
        runCatching { server?.close() }
        server = null
    }

    private fun readLine(inp: InputStream): String? {
        val sb = StringBuilder()
        while (true) {
            val c = inp.read()
            if (c < 0) return if (sb.isEmpty()) null else sb.toString()
            if (c == '\n'.code) return sb.toString().trimEnd('\r')
            sb.append(c.toChar())
            if (sb.length > 8192) return null
        }
    }

    private fun respond(out: OutputStream, code: Int, body: String) {
        val b = body.toByteArray()
        out.write("HTTP/1.1 $code X\r\nContent-Type: application/json\r\nContent-Length: ${b.size}\r\nConnection: close\r\n\r\n".toByteArray())
        out.write(b)
        out.flush()
    }

    private fun handle(s: Socket) {
        s.soTimeout = 20000
        val inp = BufferedInputStream(s.getInputStream(), 1 shl 16)
        val out = s.getOutputStream()
        val req = readLine(inp) ?: return
        val parts = req.split(" ")
        if (parts.size < 2) return respond(out, 400, "{}")
        val method = parts[0]
        val path = parts[1]
        val h = HashMap<String, String>()
        while (true) {
            val l = readLine(inp) ?: return
            if (l.isEmpty()) break
            val i = l.indexOf(':')
            if (i > 0) h[l.substring(0, i).trim().lowercase()] = l.substring(i + 1).trim()
        }
        // хто це? лише свій пристрій із правильним підписом і свіжим часом
        val from = h["x-mg-from"] ?: ""
        val t = h["x-mg-time"] ?: ""
        val dev = st.devices().firstOrNull { it.id == from }
        val fresh = t.toLongOrNull()?.let { kotlin.math.abs(System.currentTimeMillis() / 1000 - it) < 300 } ?: false
        if (dev == null || !fresh || Link.sign(dev.key, from, t, method, path) != (h["x-mg-sig"] ?: "")) {
            return respond(out, 403, """{"error":"auth"}""")
        }
        if (method == "GET" && path == "/ping") return respond(out, 200, """{"name":"${st.myName.replace("\"", "")}"}""")
        if (method != "PUT" || !path.startsWith("/file?")) return respond(out, 404, "{}")
        val name = path.substringAfter("name=", "file").substringBefore('&').let { URLDecoder.decode(it, "UTF-8") }
            .substringAfterLast('/').ifBlank { "file" }
        val size = h["content-length"]?.toLongOrNull() ?: return respond(out, 411, "{}")
        val ext = name.substringAfterLast('.', "").lowercase()
        val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
        val cr = ctx.contentResolver
        val uri = cr.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, name)
            put(MediaStore.Downloads.MIME_TYPE, mime)
            put(MediaStore.Downloads.RELATIVE_PATH, "Download/Mg Share")
            put(MediaStore.Downloads.IS_PENDING, 1)
        }) ?: return respond(out, 500, """{"error":"disk"}""")
        onEvent(Event.Start(dev.name, name))
        var got = 0L
        var last = -1
        try {
            cr.openOutputStream(uri)!!.use { o ->
                val buf = ByteArray(1 shl 16)
                while (got < size) {
                    val n = inp.read(buf, 0, minOf(buf.size.toLong(), size - got).toInt())
                    if (n < 0) break
                    o.write(buf, 0, n)
                    got += n
                    val pct = if (size > 0) (got * 100 / size).toInt() else 100
                    if (pct != last) { last = pct; onEvent(Event.Progress(name, pct)) }
                }
            }
        } catch (_: Exception) { }
        if (got != size) {
            runCatching { cr.delete(uri, null, null) }
            onEvent(Event.Failed(name))
            return
        }
        cr.update(uri, ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }, null, null)
        respond(out, 200, """{"saved":true}""")
        onEvent(Event.Done(dev.name, name, uri, mime))
    }
}
