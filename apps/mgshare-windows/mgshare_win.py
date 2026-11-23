"""Mg Share для Windows — той самий протокол, що й служба mgOS (mgos-share), плюс трей, попап і «кишеня» для файлів.

Ядро (прийом файлів і посилань, з'єднання QR-кодом, надсилання на телефон) — mgshare_core.py,
це копія ~/.local/bin/mgos-share; тут лише підміняються Linux-речі (сповіщення, буфер обміну, теки, QR).

Як користуватись:
  • перетягніть файл на «кишеню» вгорі екрана (або на відкритий попап) → натисніть свій телефон;
  • ПКМ по файлу в Провіднику → «Надіслати» → «Mg Share»;
  • клік по іконці в треї — попап: пристрої, «Додати» (QR), що прийшло з телефона.
"""
import json, os, subprocess, sys, threading, time

from PyQt6.QtCore import Qt, QObject, QPoint, QPointF, QRect, QRectF, QSize, QTimer, QUrl, pyqtSignal
from PyQt6.QtGui import (QAction, QBrush, QColor, QCursor, QDesktopServices, QFont, QFontMetrics, QGuiApplication, QIcon,
                         QPainter, QPainterPath, QPen, QPixmap, QPolygonF)
from PyQt6.QtNetwork import QLocalServer, QLocalSocket
from PyQt6.QtWidgets import QApplication, QFileDialog, QMenu, QSystemTrayIcon, QWidget

import mgshare_core as core

APP = "Mg Share"
WIN = sys.platform == "win32"
GREEN = QColor("#30d158")
FONT = "Segoe UI" if WIN else "Inter"


# ---------------------------------------------------------------- шляхи й налаштування
def appdata():
    d = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~/.config"), "MgShare")
    os.makedirs(d, exist_ok=True)
    return d


def local_cache():
    d = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/.cache"), "MgShare")
    os.makedirs(d, exist_ok=True)
    return d


def downloads():
    d = os.path.join(os.path.expanduser("~"), "Downloads", "Mg Share")
    os.makedirs(d, exist_ok=True)
    return d


PREFS_F = os.path.join(appdata(), "prefs.json")


def prefs():
    try:
        return {"pocket": True, **json.load(open(PREFS_F, encoding="utf-8"))}
    except (OSError, ValueError):
        return {"pocket": True}


def set_pref(k, v):
    p = prefs(); p[k] = v
    json.dump(p, open(PREFS_F, "w", encoding="utf-8"))


# ---------------------------------------------------------------- міст ядро → GUI (ядро працює в потоках)
class Bridge(QObject):
    notify = pyqtSignal(str, str, str)      # заголовок, текст, що відкрити по кліку (шлях/URL або "")
    copy = pyqtSignal(str)
    line = pyqtSignal(str, str)             # ключ операції, рядок протоколу «WAIT|…»


B = None


class _Subprocess:
    """subprocess для ядра: qrencode → бібліотека qrcode, решта — як є (без чорних вікон консолі)."""
    def __getattr__(self, n):
        return getattr(subprocess, n)

    def run(self, args, *a, **k):
        if args and args[0] == "qrencode":
            try:
                import qrcode
            except ImportError:      # запуск із джерел на Linux — є системний qrencode
                return subprocess.run(args, *a, **k)
            url, dst = args[-1], args[args.index("-o") + 1]
            qr = qrcode.QRCode(border=1, box_size=8, error_correction=qrcode.constants.ERROR_CORRECT_M)
            qr.add_data(url); qr.make(fit=True)
            qr.make_image(fill_color="black", back_color="white").save(dst)
            return subprocess.CompletedProcess(args, 0)
        if WIN:
            k.setdefault("creationflags", 0x08000000)   # CREATE_NO_WINDOW
        return subprocess.run(args, *a, **k)


def patch_core():
    core.subprocess = _Subprocess()
    core.conf_dir = appdata
    core.LINK_PORT = int(os.environ.get("MGSHARE_PORT", core.LINK_PORT))
    core.cache_dir = local_cache
    core.downloads_dir = downloads
    core.set_user_avatar = lambda src: None
    core.APP_APK = os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), "MgShare.apk")
    core.copy_text = lambda t: B.copy.emit(t)

    def notify_link(dev_name, url, text, icon=None):
        if not url:
            B.copy.emit(text)
            B.notify.emit(f"Текст від {dev_name} — скопійовано", text[:300], "")
        else:
            title = text.replace(url, "").strip() if text else ""
            B.notify.emit(f"Посилання від {dev_name}", (title[:200] + "\n" if title else "") + url + "\n(клік — відкрити)", url)

    def notify_received(dev_name, path, icon=None):
        B.notify.emit(f"Від {dev_name}", os.path.basename(path) + "\n(клік — відкрити)", path)

    core.notify_link = notify_link
    core.notify_received = notify_received


def start_core():
    """Те саме, що core.daemon(), але без unix-сокета: керуємо Link напряму."""
    link = core.Link()
    with core._dev_lock:
        devs = core.load_devices()
        for d in devs.values():
            d["seen"] = 0
        core.save_devices(devs)
    import http.server
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", core.LINK_PORT), link.handler())
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    threading.Thread(target=link.udp_loop, daemon=True).start()
    return link


def run_op(key, fn, *args):
    """ctl_pair / ctl_push / ctl_link у потоці; кожен рядок протоколу → сигнал line(key, рядок)."""
    st = {"gone": False}
    def say(*a):
        B.line.emit(key, "|".join(a))
    def go():
        try:
            fn(*args, say, lambda: st["gone"])
        except Exception as e:   # noqa — показати, а не впасти
            say("ERROR", str(e)[:80])
    threading.Thread(target=go, daemon=True).start()
    return st


def devices():
    out = []
    for i, d in core.load_devices().items():
        av = core.avatar_path(i)
        out.append((i, d.get("name", "?"), core.is_online(d), av if d.get("avatar") and os.path.exists(av) else ""))
    return out


# ---------------------------------------------------------------- малювання
def app_icon(size=64, dot=False):
    pm = QPixmap(size, size); pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm); p.setRenderHint(QPainter.RenderHint.Antialiasing)
    s = size / 64
    p.setPen(Qt.PenStyle.NoPen); p.setBrush(GREEN); p.drawRoundedRect(QRectF(2 * s, 2 * s, 60 * s, 60 * s), 15 * s, 15 * s)
    p.setBrush(QColor(255, 255, 255)); p.drawRoundedRect(QRectF(21 * s, 11 * s, 22 * s, 42 * s), 5 * s, 5 * s)
    p.setBrush(GREEN); p.drawRoundedRect(QRectF(24 * s, 16 * s, 16 * s, 30 * s), 2 * s, 2 * s)
    p.setBrush(QColor(255, 255, 255)); p.drawEllipse(QPointF(32 * s, 49.5 * s), 1.8 * s, 1.8 * s)
    if dot:
        p.setBrush(QColor("#0a84ff")); p.setPen(QPen(QColor(255, 255, 255), 3 * s)); p.drawEllipse(QPointF(50 * s, 14 * s), 9 * s, 9 * s)
    p.end()
    return QIcon(pm)


def round_pm(path, d):
    src = QPixmap(path)
    if src.isNull():
        return None
    side = min(src.width(), src.height())
    src = src.copy((src.width() - side) // 2, (src.height() - side) // 2, side, side).scaled(
        d * 4, d * 4, Qt.AspectRatioMode.IgnoreAspectRatio, Qt.TransformationMode.SmoothTransformation)
    pm = QPixmap(d * 4, d * 4); pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm); p.setRenderHint(QPainter.RenderHint.Antialiasing)
    path_ = QPainterPath(); path_.addEllipse(0, 0, d * 4, d * 4); p.setClipPath(path_); p.drawPixmap(0, 0, src); p.end()
    pm.setDevicePixelRatio(2)
    return pm


def font(px, bold=False, spacing=0):
    f = QFont(FONT); f.setPixelSize(px)
    if bold: f.setWeight(QFont.Weight.Bold)
    if spacing: f.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, spacing)
    return f


# ---------------------------------------------------------------- попап (1-в-1 з блоком «Надіслати» топбару mgOS)
class Popup(QWidget):
    W, H = 330, 300
    CHIP = QSize(78, 58)

    def __init__(self, link):
        super().__init__(None, Qt.WindowType.FramelessWindowHint | Qt.WindowType.Tool | Qt.WindowType.WindowStaysOnTopHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAcceptDrops(True)
        self.setFixedSize(self.W, self.H)
        self.setMouseTracking(True)
        self.link = link
        self.files = []
        self.state = ("idle", "", "")      # (вид, заголовок, підпис)
        self.qr = None                     # QPixmap під час з'єднання
        self.op = None
        self.hot = False                   # файл над попапом
        self.hover = None
        self.devs = []
        self.t0 = time.monotonic()
        self.tick = QTimer(self, interval=33); self.tick.timeout.connect(self.update)
        self.poll = QTimer(self, interval=2000); self.poll.timeout.connect(self.refresh)
        B.line.connect(self.on_line)

    # --- показ ---
    def open_at(self, anchor=None, files=None):
        if files is not None:
            self.files = files
            self.state = ("pick", "ОБЕРІТЬ ПРИСТРІЙ", self.files_text())
        elif self.state[0] in ("done", "error"):
            self.state = ("idle", "", "")
        self.refresh()
        scr = QGuiApplication.screenAt(anchor or QCursor.pos()) or QGuiApplication.primaryScreen()
        g = scr.availableGeometry()
        if anchor is None:   # біля трею: правий нижній кут над панеллю задач (або там, де панель)
            x, y = g.right() - self.W - 12, g.bottom() - self.H - 12
            if g.top() > scr.geometry().top(): y = g.top() + 12
        else:
            x = min(max(g.left() + 8, anchor.x() - self.W // 2), g.right() - self.W - 8)
            y = min(anchor.y(), g.bottom() - self.H - 8)
        self.move(x, y)
        self.show(); self.raise_(); self.activateWindow()
        self.tick.start(); self.poll.start()

    def hideEvent(self, e):
        self.tick.stop(); self.poll.stop()
        if self.op and self.state[0] == "pair":
            self.op["gone"] = True; self.op = None; self.qr = None; self.state = ("idle", "", "")
        super().hideEvent(e)

    def changeEvent(self, e):
        # клік повз попап ховає його (як у топбарі), але не посеред надсилання
        if e.type() == e.Type.ActivationChange and not self.isActiveWindow() and self.state[0] not in ("send", "pair"):
            QTimer.singleShot(120, lambda: (not self.isActiveWindow()) and self.hide())
        super().changeEvent(e)

    def keyPressEvent(self, e):
        if e.key() == Qt.Key.Key_Escape: self.hide()

    def refresh(self):
        self.devs = devices()
        self.update()

    def files_text(self):
        n = len(self.files)
        if n == 1:
            return os.path.basename(self.files[0])
        return f"{n} файли" if 2 <= n % 10 <= 4 and not 12 <= n % 100 <= 14 else f"{n} файлів"

    # --- геометрія ---
    def zone(self):
        return QRectF(18, 18, self.W - 36, self.H - 36 - self.CHIP.height() - 16)

    def chips(self):
        items = [(d[0], d) for d in self.devs] + [("+add", None)]
        gap = 10
        w = len(items) * self.CHIP.width() + (len(items) - 1) * gap
        if w > self.W - 36:   # багато пристроїв — стискаємо
            cw = (self.W - 36 - (len(items) - 1) * gap) / len(items)
        else:
            cw = self.CHIP.width()
        x = (self.W - (len(items) * cw + (len(items) - 1) * gap)) / 2
        y = self.H - 18 - self.CHIP.height()
        out = []
        for key, d in items:
            out.append((QRectF(x, y, cw, self.CHIP.height()), key, d)); x += cw + gap
        return out

    # --- малювання ---
    def paintEvent(self, e):
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        t = time.monotonic() - self.t0
        card = QRectF(0.5, 0.5, self.W - 1, self.H - 1)
        p.setPen(QPen(QColor(255, 255, 255, 30), 1)); p.setBrush(QColor(24, 24, 28, 246)); p.drawRoundedRect(card, 22, 22)
        z = self.zone()
        kind, title, sub = self.state
        hot = self.hot
        p.setPen(QPen(QColor(255, 255, 255, 150 if hot else 70), 1.6, Qt.PenStyle.DashLine)); p.setBrush(QColor(255, 255, 255, 18 if hot else 8))
        p.drawRoundedRect(z, 16, 16)
        cx = z.center().x()
        if kind == "pair" and self.qr is not None:
            q = 112
            r = QRectF(cx - q / 2, z.top() + 14, q, q)
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor(255, 255, 255)); p.drawRoundedRect(r.adjusted(-6, -6, 6, 6), 10, 10)
            p.drawPixmap(r.toRect(), self.qr)
            title, sub = "ДОДАТИ ПРИСТРІЙ", "Mg Share → «Додати пристрій» → на код"
            ty = r.bottom() + 12
        else:
            # іконка: стрілка вгору / галочка / хрестик / крутилка
            ic = QPointF(cx, z.top() + 52 + (3 * __import__("math").sin(t * 3) if kind == "idle" and hot else 0))
            p.setPen(Qt.PenStyle.NoPen)
            if kind == "done":
                p.setBrush(GREEN); p.drawEllipse(ic, 22, 22)
                p.setPen(QPen(QColor(255, 255, 255), 4, cap=Qt.PenCapStyle.RoundCap, join=Qt.PenJoinStyle.RoundJoin))
                p.drawPolyline(QPolygonF([QPointF(ic.x() - 10, ic.y()), QPointF(ic.x() - 3, ic.y() + 8), QPointF(ic.x() + 11, ic.y() - 8)]))
            elif kind == "error":
                p.setBrush(QColor("#ff453a")); p.drawEllipse(ic, 22, 22)
                p.setPen(QPen(QColor(255, 255, 255), 4, cap=Qt.PenCapStyle.RoundCap))
                p.drawLine(QPointF(ic.x() - 8, ic.y() - 8), QPointF(ic.x() + 8, ic.y() + 8))
                p.drawLine(QPointF(ic.x() + 8, ic.y() - 8), QPointF(ic.x() - 8, ic.y() + 8))
            elif kind == "send":
                p.setPen(QPen(GREEN, 4, cap=Qt.PenCapStyle.RoundCap)); p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawArc(QRectF(ic.x() - 20, ic.y() - 20, 40, 40), int(-t * 360 * 16) % (360 * 16), 100 * 16)
            else:
                p.setBrush(QColor(255, 255, 255, 240))
                p.drawPolygon(QPolygonF([QPointF(ic.x(), ic.y() - 22), QPointF(ic.x() + 19, ic.y() - 2), QPointF(ic.x() + 8, ic.y() - 2),
                                         QPointF(ic.x() + 8, ic.y() + 12), QPointF(ic.x() - 8, ic.y() + 12), QPointF(ic.x() - 8, ic.y() - 2),
                                         QPointF(ic.x() - 19, ic.y() - 2)]))
                p.drawRect(QRectF(ic.x() - 18, ic.y() + 17, 36, 4))
            if kind == "idle":
                title = "КИНЬТЕ ФАЙЛ СЮДИ"
                sub = "або натисніть свій пристрій нижче — виберете файл"
            ty = ic.y() + 42
        p.setPen(QColor(255, 255, 255)); p.setFont(font(13, True, 1.2))
        p.drawText(QRectF(z.left() + 8, ty, z.width() - 16, 20), Qt.AlignmentFlag.AlignCenter, title)
        p.setPen(QColor(255, 255, 255, 150)); p.setFont(font(11))
        p.drawText(QRectF(z.left() + 14, ty + 22, z.width() - 28, 34),
                   Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop | Qt.TextFlag.TextWordWrap, sub)
        # чипи пристроїв
        for r, key, d in self.chips():
            hov = self.hover == key
            online = d is not None and d[2]
            pick = kind == "pick" and d is not None
            p.setPen(QPen(GREEN, 1.6) if online else QPen(QColor(255, 255, 255, 40), 1, Qt.PenStyle.DashLine if d is None else Qt.PenStyle.SolidLine))
            p.setBrush(QColor(255, 255, 255, 34 if hov else 22 if pick else 14))
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 14, 14)
            c = QPointF(r.center().x(), r.top() + 20)
            if d is None:
                p.setPen(QPen(QColor(255, 255, 255, 190), 2)); p.drawLine(QPointF(c.x() - 7, c.y()), QPointF(c.x() + 7, c.y()))
                p.drawLine(QPointF(c.x(), c.y() - 7), QPointF(c.x(), c.y() + 7)); name = "Додати"
            else:
                pm = round_pm(d[3], 14) if d[3] else None
                if pm: p.drawPixmap(QPointF(c.x() - 14, c.y() - 14), pm)
                else:
                    p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor("#ff9fc8")); p.drawEllipse(c, 14, 14)
                    p.setPen(QColor(255, 255, 255)); p.setFont(font(13, True))
                    p.drawText(QRectF(c.x() - 14, c.y() - 14, 28, 28), Qt.AlignmentFlag.AlignCenter, d[1][:1].upper())
                name = d[1]
            p.setPen(QColor(255, 255, 255, 220)); p.setFont(font(11))
            p.drawText(QRectF(r.left() + 3, r.bottom() - 20, r.width() - 6, 16), Qt.AlignmentFlag.AlignCenter,
                       QFontMetrics(font(11)).elidedText(name, Qt.TextElideMode.ElideRight, int(r.width() - 8)))

    # --- миша ---
    def hit(self, pos):
        for r, key, d in self.chips():
            if r.contains(QPointF(pos)): return key, d
        return None, None

    def mouseMoveEvent(self, e):
        k, _ = self.hit(e.position())
        if k != self.hover:
            self.hover = k
            self.setCursor(Qt.CursorShape.PointingHandCursor if k else Qt.CursorShape.ArrowCursor); self.update()

    def leaveEvent(self, e):
        self.hover = None; self.update()

    def mousePressEvent(self, e):
        key, d = self.hit(e.position())
        if key is None:
            if self.zone().contains(e.position()) and self.state[0] in ("idle", "done", "error"):
                self.pick_files(None)
            return
        if key == "+add":
            return self.start_pair()
        if e.button() == Qt.MouseButton.RightButton:
            return self.chip_menu(d, e.globalPosition().toPoint())
        if self.state[0] == "send":
            return
        if self.files:
            self.send(d)
        else:
            self.pick_files(d)

    def chip_menu(self, d, at):
        m = QMenu(self)
        m.addAction("Надіслати файли…", lambda: self.pick_files(d))
        m.addAction("Надіслати текст / посилання з буфера", lambda: self.send_clip(d))
        m.addSeparator()
        m.addAction(f"Забути «{d[1]}»", lambda: (core.forget(d[0]), self.refresh()))
        m.exec(at)

    def pick_files(self, d):
        files, _ = QFileDialog.getOpenFileNames(self, "Що надіслати на телефон", os.path.expanduser("~"))
        if not files: return
        self.files = files
        if d is not None:
            self.send(d)
        else:
            self.state = ("pick", "ОБЕРІТЬ ПРИСТРІЙ", self.files_text()); self.update()
        if not self.isVisible(): self.open_at()

    # --- дії ---
    def send(self, d):
        self.state = ("send", f"НАДСИЛАННЯ → {d[1].upper()}", self.files_text())
        self.op = run_op("push", self.link.ctl_push, d[0], list(self.files))
        self.update()

    def send_clip(self, d):
        text = QGuiApplication.clipboard().text().strip()
        if not text:
            self.state = ("error", "БУФЕР ПОРОЖНІЙ", "Скопіюйте посилання або текст і спробуйте ще раз"); return self.update()
        self.state = ("send", f"НАДСИЛАННЯ → {d[1].upper()}", text[:80])
        self.op = run_op("link", self.link.ctl_link, [d[0]], text)
        self.update()

    def start_pair(self):
        if self.op and self.state[0] == "pair": return
        self.state = ("pair", "", ""); self.qr = None
        self.op = run_op("pair", self.link.ctl_pair)
        self.update()

    ERR = {"offline": "Телефон не на зв'язку: відкрийте Mg Share на ньому, та сама Wi-Fi",
           "nonet": "Немає мережі: під'єднайтесь до Wi-Fi", "auth": "Телефон не впізнав ПК — з'єднайте заново (Додати)",
           "busy": "Телефон зайнятий іншою передачею", "fail": "Не вдалося — перевірте Wi-Fi і брандмауер Windows"}

    def on_line(self, key, line):
        tag, _, rest = line.partition("|")
        if key == "pair":
            if tag == "QR": self.qr = QPixmap(rest)
            elif tag == "PAIRED":
                self.qr = None; self.op = None
                self.state = ("done", "З'ЄДНАНО", rest.split("|")[-1]); self.refresh()
            elif tag in ("EXPIRED", "ERROR"):
                self.qr = None; self.op = None
                self.state = ("error", "КОД ЗАСТАРІВ" if tag == "EXPIRED" else "ПОМИЛКА", self.ERR.get(rest, rest))
        else:
            if tag == "SENDING": self.state = ("send", f"НАДСИЛАННЯ → {rest.upper()}", self.state[2])
            elif tag == "GOT": self.state = ("send", "НАДСИЛАННЯ…", f"{rest} ✓")
            elif tag == "DONE":
                self.state = ("done", "НАДІСЛАНО", self.files_text() if key == "push" else "Посилання на телефоні")
                self.files = []; self.op = None
                QTimer.singleShot(2500, lambda: self.state[0] == "done" and self.isVisible() and self.hide())
            elif tag == "ERROR":
                self.state = ("error", "НЕ НАДІСЛАНО", self.ERR.get(rest, rest)); self.op = None
        self.update()

    # --- перетягування ---
    def dragEnterEvent(self, e):
        if e.mimeData().hasUrls(): e.acceptProposedAction(); self.hot = True; self.update()

    def dragLeaveEvent(self, e):
        self.hot = False; self.update()

    def dropEvent(self, e):
        self.hot = False
        files = [u.toLocalFile() for u in e.mimeData().urls() if u.isLocalFile() and os.path.isfile(u.toLocalFile())]
        if not files:
            self.state = ("error", "ЦЕ НЕ ФАЙЛ", "Перетягніть файл із Провідника"); return self.update()
        self.files = files
        online = [d for d in self.devs if d[2]]
        k, d = self.hit(e.position())
        if d is not None: self.send(d)                   # кинули прямо на чип телефона
        elif len(self.devs) == 1: self.send(self.devs[0])   # єдиний пристрій — одразу на нього
        else:
            self.state = ("pick", "ОБЕРІТЬ ПРИСТРІЙ", self.files_text() + (" · жоден телефон не на зв'язку" if not online else ""))
        self.update()


# ---------------------------------------------------------------- «кишеня» вгорі екрана (аналог годинника в топбарі mgOS)
class Pocket(QWidget):
    """Тонка смужка вгорі по центру. Наводите на неї файл — розкривається, відпустили — попап із вибором телефона."""
    def __init__(self, popup):
        super().__init__(None, Qt.WindowType.FramelessWindowHint | Qt.WindowType.Tool | Qt.WindowType.WindowStaysOnTopHint
                         | Qt.WindowType.WindowDoesNotAcceptFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.setAcceptDrops(True)
        self.popup = popup
        self.open = 0.0; self.target = 0.0
        self.anim = QTimer(self, interval=16); self.anim.timeout.connect(self.step)
        self.setFixedSize(260, 64)
        self.place()
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setToolTip("Mg Share — перетягніть сюди файл, щоб надіслати на телефон")

    def place(self):
        g = QGuiApplication.primaryScreen().geometry()
        self.move(g.center().x() - self.width() // 2, g.top())
        # поки згорнута — вікно ловить мишу лише смужкою 150×8 зверху
        self.setMask(self.region())

    def region(self):
        from PyQt6.QtGui import QRegion
        h = int(8 + 52 * self.open)
        w = int(150 + 110 * self.open)
        return QRegion((self.width() - w) // 2, 0, w, max(8, h))

    def step(self):
        self.open += (self.target - self.open) * 0.25
        if abs(self.target - self.open) < 0.01:
            self.open = self.target; self.anim.stop()
        self.setMask(self.region()); self.update()

    def go(self, v):
        self.target = v; self.anim.start()

    def paintEvent(self, e):
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        o = self.open
        w, h = 150 + 110 * o, 8 + 52 * o
        r = QRectF((self.width() - w) / 2, -12, w, h + 12)
        p.setPen(QPen(GREEN if o > 0.05 else QColor(255, 255, 255, 60), 1.5))
        p.setBrush(QColor(24, 24, 28, int(150 + 96 * o)))
        p.drawRoundedRect(r, 12, 12)
        if o < 0.3:
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor(48, 209, 88, 200)); p.drawRoundedRect(QRectF(self.width() / 2 - 16, 2, 32, 3), 1.5, 1.5)
        if o > 0.6:
            p.setOpacity((o - 0.6) / 0.4)
            p.setPen(QColor(255, 255, 255)); p.setFont(font(12, True, 1))
            p.drawText(QRectF(r.left(), 6, r.width(), h - 8), Qt.AlignmentFlag.AlignCenter, "⬆  ВІДПУСТІТЬ — НА ТЕЛЕФОН")

    def enterEvent(self, e): self.go(1.0)
    def leaveEvent(self, e): self.go(0.0)
    def mousePressEvent(self, e): self.popup.open_at(QPoint(self.geometry().center().x(), self.geometry().top() + 70))

    def dragEnterEvent(self, e):
        if e.mimeData().hasUrls(): e.acceptProposedAction(); self.go(1.0)

    def dragLeaveEvent(self, e): self.go(0.0)

    def dropEvent(self, e):
        self.go(0.0)
        files = [u.toLocalFile() for u in e.mimeData().urls() if u.isLocalFile() and os.path.isfile(u.toLocalFile())]
        self.popup.open_at(QPoint(self.geometry().center().x(), self.geometry().top() + 70), files or None)


# ---------------------------------------------------------------- інтеграція з Windows
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"


def exe_cmd():
    return f'"{sys.executable}"' if getattr(sys, "frozen", False) else f'"{sys.executable}" "{os.path.abspath(__file__)}"'


def autostart(on=None):
    if not WIN: return False
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_READ | winreg.KEY_WRITE) as k:
        if on is None:
            try: winreg.QueryValueEx(k, "MgShare"); return True
            except OSError: return False
        if on: winreg.SetValueEx(k, "MgShare", 0, winreg.REG_SZ, exe_cmd() + " --tray")
        else:
            try: winreg.DeleteValue(k, "MgShare")
            except OSError: pass
        return on


def install_sendto():
    """ПКМ по файлу → «Надіслати» → «Mg Share» (ярлик у теці SendTo)."""
    if not WIN or not getattr(sys, "frozen", False): return
    lnk = os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\SendTo", "Mg Share.lnk")
    if os.path.exists(lnk): return
    ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{lnk}');$s.TargetPath='{sys.executable}';"
          f"$s.IconLocation='{sys.executable},0';$s.Save()")
    subprocess.Popen(["powershell", "-NoProfile", "-WindowStyle", "Hidden", "-Command", ps], creationflags=0x08000000)


def open_path(p):
    if WIN and os.path.exists(p): os.startfile(p)
    else: QDesktopServices.openUrl(QUrl(p) if "://" in p else QUrl.fromLocalFile(p))


# ---------------------------------------------------------------- старт
SOCK = "MgShare-" + (os.environ.get("USERNAME") or os.environ.get("USER") or "u")


def main():
    global B
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    files = [os.path.abspath(a) for a in args if os.path.isfile(a)]
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    app.setApplicationName(APP)
    # одна копія: друга (з «Надіслати → Mg Share») лише передає файли першій
    s = QLocalSocket(); s.connectToServer(SOCK)
    if s.waitForConnected(300):
        s.write((json.dumps(files) + "\n").encode()); s.flush(); s.waitForBytesWritten(1000); s.disconnectFromServer()
        return 0
    QLocalServer.removeServer(SOCK)
    server = QLocalServer(); server.listen(SOCK)

    B = Bridge()
    patch_core()
    tray = QSystemTrayIcon(app_icon()); tray.setToolTip(APP)
    try:
        link = start_core()
    except OSError as e:
        tray.show(); tray.showMessage(APP, f"Порт {core.LINK_PORT} зайнятий — Mg Share уже запущено? ({e})")
        QTimer.singleShot(6000, app.quit); return app.exec()

    popup = Popup(link)
    pocket = Pocket(popup)
    if prefs()["pocket"]: pocket.show()

    pending = {"open": ""}
    B.notify.connect(lambda title, body, what: (pending.__setitem__("open", what),
                                                tray.showMessage(title, body, app_icon(), 8000)))
    tray.messageClicked.connect(lambda: pending["open"] and open_path(pending["open"]))
    B.copy.connect(lambda t: QGuiApplication.clipboard().setText(t))

    def incoming():
        c = server.nextPendingConnection()
        def read():
            for raw in bytes(c.readAll()).decode(errors="ignore").splitlines():
                try: fl = json.loads(raw)
                except ValueError: continue
                popup.open_at(None, fl or None)
        c.readyRead.connect(read)
    server.newConnection.connect(incoming)

    menu = QMenu()
    menu.addAction("Відкрити Mg Share", lambda: popup.open_at())
    menu.addAction("Додати пристрій (QR)…", lambda: (popup.open_at(), popup.start_pair()))
    menu.addAction("Отримані файли", lambda: open_path(downloads()))
    menu.addSeparator()
    a_pocket = QAction("Кишеня для файлів угорі екрана", menu, checkable=True, checked=prefs()["pocket"])
    a_pocket.toggled.connect(lambda v: (set_pref("pocket", v), pocket.setVisible(v)))
    menu.addAction(a_pocket)
    if WIN:
        a_auto = QAction("Запускати разом із Windows", menu, checkable=True, checked=autostart())
        a_auto.toggled.connect(lambda v: autostart(v))
        menu.addAction(a_auto)
    menu.addSeparator()
    menu.addAction("Вийти", lambda: (tray.hide(), os._exit(0)))
    tray.setContextMenu(menu)
    tray.activated.connect(lambda r: r in (QSystemTrayIcon.ActivationReason.Trigger, QSystemTrayIcon.ActivationReason.DoubleClick)
                           and (popup.hide() if popup.isVisible() else popup.open_at()))
    tray.show()

    # перший запуск: автозапуск і пункт «Надіслати → Mg Share», підказка де шукати
    if WIN and not prefs().get("first_done"):
        autostart(True); set_pref("first_done", True)
        a_auto.setChecked(True)
        tray.showMessage(APP, "Працює в треї. Перетягніть файл на зелену смужку вгорі екрана або ПКМ по файлу → Надіслати → Mg Share.",
                         app_icon(), 10000)
    install_sendto()
    if files or "--tray" not in sys.argv:
        QTimer.singleShot(300, lambda: popup.open_at(None, files or None))
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
