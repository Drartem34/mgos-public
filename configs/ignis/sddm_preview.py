"""Живе прев'ю теми входу mgOS Pixel для «Налаштувань» → Екран входу.

Тема (QML) рендериться прямо в процесі налаштувань у НЕвидимому QQuickView (grabWindow
малює без показу вікна), з макетом API SDDM: sddm / config / userModel / sessionModel / keyboard.
"""
import os
import pwd
from pathlib import Path

from PyQt6.QtCore import (QObject, pyqtSignal, pyqtSlot, pyqtProperty, QUrl, QTimer, QAbstractListModel,
                          QModelIndex, QByteArray, Qt, QSize)
from PyQt6.QtGui import QPixmap, QPainter, QPainterPath, QColor
from PyQt6.QtWidgets import QLabel, QSizePolicy

THEME_DIR = Path.home() / ".config" / "megumin-os" / "sddm-pixel"
PREVIEW_W, PREVIEW_H = 1920, 1080


class _Sddm(QObject):
    loginFailed = pyqtSignal()
    loginSucceeded = pyqtSignal()

    @pyqtProperty(bool, constant=True)
    def canSuspend(self): return True
    @pyqtProperty(bool, constant=True)
    def canReboot(self): return True
    @pyqtProperty(bool, constant=True)
    def canPowerOff(self): return True
    @pyqtProperty(bool, constant=True)
    def canHibernate(self): return False
    @pyqtSlot(str, str, int)
    def login(self, u, p, s): pass
    @pyqtSlot()
    def powerOff(self): pass
    @pyqtSlot()
    def reboot(self): pass
    @pyqtSlot()
    def suspend(self): pass


class _Model(QAbstractListModel):
    def __init__(self, rows, names):
        super().__init__()
        self.rows = rows
        self.roles = {Qt.ItemDataRole.UserRole + 1 + i: n for i, n in enumerate(names)}

    def rowCount(self, parent=QModelIndex()): return len(self.rows)
    def data(self, idx, role):
        k = self.roles.get(role)
        return self.rows[idx.row()].get(k) if k else None
    def roleNames(self): return {r: QByteArray(n.encode()) for r, n in self.roles.items()}
    @pyqtProperty(int, constant=True)
    def lastIndex(self): return 0
    @pyqtProperty(str, constant=True)
    def lastUser(self): return self.rows[0].get("name", "") if self.rows else ""


class _Keyboard(QObject):
    @pyqtProperty(bool, constant=True)
    def capsLock(self): return False
    @pyqtProperty(list, constant=True)
    def layouts(self): return []
    @pyqtProperty(int, constant=True)
    def currentLayout(self): return 0


def _avatar_path():
    for p in (Path("/usr/share/sddm/faces") / f"{os.environ.get('USER', '')}.face.icon", Path.home() / ".face.icon"):
        if p.is_file():
            return str(p)
    return ""


class SddmPreview(QLabel):
    """Картинка екрана входу; update_config(dict) — перемалювати з новими значеннями theme.conf."""

    def __init__(self, width=560, parent=None):
        super().__init__(parent)
        self.w = width
        self.h = int(width * PREVIEW_H / PREVIEW_W)
        self.setFixedSize(self.w, self.h)
        self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        self.view = None
        self.conf = {}
        self._frames = 0
        self._tick = QTimer(self)
        self._tick.timeout.connect(self._frame)
        self._debounce = QTimer(self)
        self._debounce.setSingleShot(True)
        self._debounce.timeout.connect(self._start)

    def _ensure_view(self):
        if self.view is not None:
            return True
        if not (THEME_DIR / "Main.qml").is_file():
            self.setText("Тему mgOS Pixel не знайдено")
            return False
        from PyQt6.QtQuick import QQuickView
        from PyQt6.QtQml import QQmlPropertyMap
        self.view = QQuickView()
        self.view.setResizeMode(QQuickView.ResizeMode.SizeRootObjectToView)
        ctx = self.view.rootContext()
        self._objs = dict(
            sddm=_Sddm(),
            config=QQmlPropertyMap(),
            userModel=_Model([{"name": os.environ.get("USER", "user"), "realName": (pwd.getpwuid(os.getuid()).pw_gecos.split(",")[0] or os.environ.get("USER", "")),
                               "icon": _avatar_path()}], ["name", "realName", "icon"]),
            sessionModel=_Model([{"name": "mgOS"}], ["name"]),
            keyboard=_Keyboard(),
        )
        for k, v in self._objs.items():
            ctx.setContextProperty(k, v)
        ctx.setContextProperty("primaryScreen", True)
        ctx.setContextProperty("mgosPreview", True)
        return True

    def update_config(self, conf):
        self.conf = dict(conf)
        self._dirty = True
        if self.isVisible():
            self._debounce.start(120)

    def showEvent(self, e):
        super().showEvent(e)
        if getattr(self, "_dirty", False):
            self._debounce.start(60)

    def _start(self):
        self._dirty = False
        if not self._ensure_view():
            return
        cm = self._objs["config"]
        for k, v in self.conf.items():
            cm.insert(k, str(v))
        # перезавантажуємо QML, щоб усі значення взялися з нуля (і фон, і годинник)
        self.view.setSource(QUrl())
        self.view.engine().clearComponentCache()
        self.view.setSource(QUrl.fromLocalFile(str(THEME_DIR / "Main.qml")))
        self.view.resize(PREVIEW_W, PREVIEW_H)
        self._frames = 0
        self._tick.start(40)

    def _frame(self):
        img = self.view.grabWindow()      # невидиме вікно: малюється в offscreen-поверхню
        self._frames += 1
        if self._frames < 8:               # кілька «кадрів», щоб відмалювались полотна й ефекти
            return
        self._tick.stop()
        dpr = max(1, round(self.devicePixelRatioF()))
        pm = QPixmap.fromImage(img).scaled(self.w * dpr, self.h * dpr,
                                           Qt.AspectRatioMode.IgnoreAspectRatio, Qt.TransformationMode.SmoothTransformation)
        out = QPixmap(pm.size())
        out.fill(Qt.GlobalColor.transparent)
        p = QPainter(out)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        path = QPainterPath()
        path.addRoundedRect(0, 0, pm.width(), pm.height(), 14 * pm.width() / self.w, 14 * pm.width() / self.w)
        p.setClipPath(path)
        p.drawPixmap(0, 0, pm)
        p.end()
        out.setDevicePixelRatio(pm.width() / self.w)
        self.setPixmap(out)
