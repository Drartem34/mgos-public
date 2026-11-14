import sys
import json
import subprocess
import os
import pwd
import glob
import shutil
from pathlib import Path
from PyQt6.QtWidgets import (QApplication, QWidget, QHBoxLayout, QVBoxLayout, QGridLayout,
                             QLabel, QPushButton, QFrame, QAbstractButton, QLineEdit,
                             QStackedWidget, QColorDialog, QSizeGrip,
                             QGraphicsOpacityEffect, QFileDialog, QDialog, QRubberBand,
                             QSlider, QScrollArea, QListView, QSizePolicy, QMenu,
                             QStyledItemDelegate, QStyle)
from PyQt6.QtCore import (Qt, QPropertyAnimation, QVariantAnimation, QEasingCurve, QRect, QRectF, QPoint, QPointF,
                          QSize, QTimer, QAbstractListModel, QModelIndex, QRunnable, QThreadPool, pyqtSignal, QObject, QUrl)
from PyQt6.QtGui import (QPainter, QColor, QBrush, QCursor, QIcon, QPixmap, QImage, QFont, QFontDatabase,
                         QPainterPath, QPen, QLinearGradient, QRadialGradient, QFontMetrics)
import hashlib
import shlex
import threading
import math
import time
import re

# ==========================================
# 0. ТЕМА «macOS 26» (Liquid Glass): токени кольорів
# ==========================================
DARK = dict(win=QColor(30, 30, 32, 238), sidebar=QColor(52, 52, 56, 170), side_border=QColor(255, 255, 255, 22),
            card=QColor(44, 44, 47), card_border=QColor(255, 255, 255, 14), sep=QColor(255, 255, 255, 22),
            text="#f5f5f7", text2="#98989f", ctrl=QColor(118, 118, 128, 70), seg_sel=QColor(99, 99, 102),
            hover=QColor(255, 255, 255, 14), field=QColor(118, 118, 128, 50), win_border=QColor(255, 255, 255, 30),
            knob_shadow=QColor(0, 0, 0, 90), track_off=QColor(120, 120, 128, 90))
LIGHT = dict(win=QColor(246, 246, 248, 242), sidebar=QColor(255, 255, 255, 150), side_border=QColor(0, 0, 0, 18),
             card=QColor(255, 255, 255), card_border=QColor(0, 0, 0, 16), sep=QColor(0, 0, 0, 22),
             text="#1d1d1f", text2="#86868b", ctrl=QColor(118, 118, 128, 36), seg_sel=QColor(255, 255, 255),
             hover=QColor(0, 0, 0, 10), field=QColor(0, 0, 0, 16), win_border=QColor(0, 0, 0, 34),
             knob_shadow=QColor(0, 0, 0, 55), track_off=QColor(120, 120, 128, 52))
T = dict(DARK)
ACCENT = ["#0a84ff"]
FONT_UI = "Inter"
ICON_FONT = "JetBrainsMono Nerd Font"


def accent():
    return QColor(ACCENT[0])


def tint(c, a):
    c = QColor(c)
    c.setAlpha(a)
    return c


def build_qss():
    t = T
    a = ACCENT[0]
    return f"""
    QWidget {{ font-family: "{FONT_UI}"; color: {t['text']}; font-size: 13px; }}
    QLabel {{ background: transparent; }}
    QLabel[role="h1"] {{ font-size: 15px; font-weight: 700; }}
    QLabel[role="hero"] {{ font-size: 21px; font-weight: 700; }}
    QLabel[role="herodesc"] {{ font-size: 12px; color: {t['text2']}; }}
    QLabel[role="row"] {{ font-size: 13px; }}
    QLabel[role="desc"] {{ font-size: 11px; color: {t['text2']}; }}
    QLabel[role="value"] {{ font-size: 12px; color: {t['text2']}; }}
    QLabel[role="section"] {{ font-size: 13px; font-weight: 700; padding-left: 6px; }}
    QLabel[role="side"] {{ font-size: 13px; }}
    QLabel[role="sidegroup"] {{ font-size: 11px; font-weight: 700; color: {t['text2']}; padding-left: 10px; }}
    QLabel[role="chev"] {{ font-family: "{ICON_FONT}"; font-size: 13px; color: {t['text2']}; }}
    QLabel[role="tick"] {{ font-size: 10px; color: {t['text2']}; }}
    QScrollArea, QScrollArea > QWidget > QWidget {{ background: transparent; border: none; }}
    QScrollBar:vertical {{ width: 9px; background: transparent; margin: 3px 2px; }}
    QScrollBar::handle:vertical {{ background: {tint(t['text'], 60).name(QColor.NameFormat.HexArgb)}; border-radius: 3px; min-height: 30px; }}
    QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page {{ height: 0; background: none; }}
    QLineEdit {{ background: {t['field'].name(QColor.NameFormat.HexArgb)}; border: 1px solid transparent; border-radius: 8px;
                 padding: 5px 9px; font-size: 13px; selection-background-color: {a}; }}
    QLineEdit:focus {{ border: 1px solid {a}; }}
    QLineEdit#search {{ padding-left: 28px; border-radius: 9px; }}
    QListView#wp {{ background: transparent; border: none; outline: none; color: {t['text']}; }}
    QListView#wp::item {{ padding: 6px; border-radius: 12px; }}
    QListView#wp::item:selected, QListView#wp::item:hover {{ background: {tint(a, 60).name(QColor.NameFormat.HexArgb)}; }}
    QMenu {{ background: {t['card'].name()}; border: 1px solid {t['card_border'].name(QColor.NameFormat.HexArgb)}; border-radius: 10px; padding: 5px; }}
    QMenu::item {{ padding: 5px 22px 5px 22px; border-radius: 6px; color: {t['text']}; }}
    QMenu::item:selected {{ background: {a}; color: white; }}
    QMenu::separator {{ height: 1px; background: {t['sep'].name(QColor.NameFormat.HexArgb)}; margin: 4px 8px; }}
    QToolTip {{ background: {t['card'].name()}; color: {t['text']}; border: 1px solid {t['card_border'].name(QColor.NameFormat.HexArgb)}; border-radius: 6px; padding: 4px 8px; }}
    QDialog {{ background: {'#2a2a2d' if T is not LIGHT and T['text'] == DARK['text'] else '#f5f5f7'}; }}
    """


def apply_palette():
    """Палітра Qt під тему: стандартні діалоги (вибір кольору, файлів) не лишаються білими з білим текстом."""
    from PyQt6.QtGui import QPalette
    app = QApplication.instance()
    app.setStyle("Fusion")
    dark = T["text"] == DARK["text"]
    pal = QPalette()
    bg, base, txt = (QColor(36, 36, 40), QColor(28, 28, 31), QColor(T["text"])) if dark else \
                    (QColor(246, 246, 248), QColor(255, 255, 255), QColor(T["text"]))
    R = QPalette.ColorRole
    for role, c in ((R.Window, bg), (R.Base, base), (R.AlternateBase, bg.lighter(110) if dark else bg.darker(103)),
                    (R.Button, bg.lighter(118) if dark else QColor(255, 255, 255)), (R.WindowText, txt), (R.Text, txt),
                    (R.ButtonText, txt), (R.ToolTipBase, bg), (R.ToolTipText, txt), (R.PlaceholderText, QColor(T["text2"])),
                    (R.Highlight, accent()), (R.HighlightedText, QColor("white")), (R.BrightText, QColor("white")),
                    (R.Link, accent())):
        pal.setColor(role, c)
    pal.setColor(QPalette.ColorGroup.Disabled, R.Text, QColor(T["text2"]))
    pal.setColor(QPalette.ColorGroup.Disabled, R.ButtonText, QColor(T["text2"]))
    app.setPalette(pal)


def pick_image_file(parent, title):
    """Системний вибір файлу (GTK4 через zenity — темна тема, як у всій системі); запасний — Qt-діалог."""
    try:
        r = subprocess.run(["zenity", "--file-selection", f"--title={title}", f"--filename={Path.home()}/Pictures/",
                            "--file-filter=Зображення | *.png *.jpg *.jpeg *.webp *.PNG *.JPG *.JPEG",
                            "--file-filter=Усі файли | *"], capture_output=True, text=True,
                           # без порталу: інакше діалог малює xdg-desktop-portal-gtk (GTK3, світлий Adwaita);
                           # власний діалог GTK4/libadwaita слідує темній темі системи
                           env=dict(os.environ, GDK_DEBUG="no-portals"))
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
        if r.returncode in (0, 1):   # 1 — користувач натиснув «Скасувати»
            return ""
    except FileNotFoundError:
        pass
    path, _ = QFileDialog.getOpenFileName(parent, title, str(Path.home()), "Images (*.png *.jpg *.jpeg *.webp)")
    return path


def rounded(p, rect, r):
    path = QPainterPath()
    path.addRoundedRect(QRectF(rect), r, r)
    return path


def draw_squircle_icon(p, rect, glyph, color, glyph_px=None):
    """Кольорова «сквіркл»-іконка як у сайдбарі System Settings (білий гліф на градієнті)."""
    rect = QRectF(rect)
    r = rect.width() * 0.26
    c = QColor(color)
    g = QLinearGradient(rect.topLeft(), rect.bottomLeft())
    g.setColorAt(0, c.lighter(122))
    g.setColorAt(1, c.darker(108))
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(QBrush(g))
    p.drawPath(rounded(p, rect, r))
    p.setPen(QPen(QColor(255, 255, 255, 40), 0.8))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawPath(rounded(p, rect.adjusted(0.4, 0.4, -0.4, -0.4), r))
    f = QFont(ICON_FONT)
    f.setPixelSize(int(glyph_px or rect.width() * 0.62))
    p.setFont(f)
    p.setPen(QColor("white"))
    p.drawText(rect, Qt.AlignmentFlag.AlignCenter, glyph)


# Гліфи Nerd Font (Material Design Icons)
G = dict(wifi="\U000F05A9", bt="\U000F00AF", palette="\U000F03D8", image="\U000F02E9", monitor="\U000F0379",
         sound="\U000F057E", cog="\U000F0493", search="\U000F0349", panels="\U000F056E", battery="\U000F0079",
         touch="\U000F0741", dock="\U000F10A9", account="\U000F0004", keyboard="\U000F030C", bell="\U000F009A",
         info="\U000F02FC", power="\U000F0425", update="\U000F06B0", shield="\U000F0498", chev="\U000F0142",
         left="\U000F0141", right="\U000F0142", laptop="\U000F0322")


# ==========================================
# 1. КОНТРОЛИ У СТИЛІ macOS 26
# ==========================================

class MeguminSwitch(QAbstractButton):
    """Перемикач macOS: акцентна доріжка, білий кноб із тінню, що «тягнеться» при натисканні (Liquid Glass)."""
    def __init__(self, key, current_state, save_callback, get_accent_cb=None, parent=None):
        super().__init__(parent)
        self.key, self.save_callback = key, save_callback
        self.setCheckable(True)
        self.setChecked(bool(current_state))
        self.setFixedSize(42, 24)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self._pos = 1.0 if self.isChecked() else 0.0
        self._press = 0.0
        self._anim = QVariantAnimation(self, duration=220, easingCurve=QEasingCurve.Type.OutCubic)
        self._anim.valueChanged.connect(self._set_pos)
        self._panim = QVariantAnimation(self, duration=140, easingCurve=QEasingCurve.Type.OutCubic)
        self._panim.valueChanged.connect(self._set_press)
        self.toggled.connect(self._animate)
        self.clicked.connect(self.on_toggle)

    def _set_pos(self, v): self._pos = v; self.update()
    def _set_press(self, v): self._press = v; self.update()

    def _animate(self, on):
        self._anim.stop()
        self._anim.setStartValue(self._pos)
        self._anim.setEndValue(1.0 if on else 0.0)
        self._anim.start()

    def on_toggle(self):
        if self.save_callback:
            self.save_callback(self.key, self.isChecked())

    def update_styles(self): self.update()

    def mousePressEvent(self, e):
        self._panim.stop(); self._panim.setStartValue(self._press); self._panim.setEndValue(1.0); self._panim.start()
        super().mousePressEvent(e)

    def mouseReleaseEvent(self, e):
        self._panim.stop(); self._panim.setStartValue(self._press); self._panim.setEndValue(0.0); self._panim.start()
        super().mouseReleaseEvent(e)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        off, on = T["track_off"], accent()
        c = QColor(int(off.red() + (on.red() - off.red()) * self._pos), int(off.green() + (on.green() - off.green()) * self._pos),
                   int(off.blue() + (on.blue() - off.blue()) * self._pos), int(off.alpha() + (255 - off.alpha()) * self._pos))
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(c)
        p.drawRoundedRect(QRectF(0, 0, w, h), h / 2, h / 2)
        kh = h - 4
        kw = kh + 6 * self._press
        x = 2 + (w - 4 - kw) * self._pos
        for i, a in ((2.0, 35), (1.0, 45)):
            p.setBrush(tint(T["knob_shadow"], a))
            p.drawRoundedRect(QRectF(x, 2 + i * 0.6, kw, kh), kh / 2, kh / 2)
        p.setBrush(QColor("white"))
        p.drawRoundedRect(QRectF(x, 2, kw, kh), kh / 2, kh / 2)


class MacSlider(QSlider):
    """Повзунок macOS 26: тонка доріжка, акцентне заповнення, кноб-пігулка з тінню, опційні мітки."""
    def __init__(self, ticks=0, parent=None):
        super().__init__(Qt.Orientation.Horizontal, parent)
        self.ticks = ticks
        self.setFixedHeight(24 if not ticks else 30)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))

    def _x_for(self, v):
        span = max(1, self.maximum() - self.minimum())
        return 12 + (self.width() - 24) * (v - self.minimum()) / span

    def mousePressEvent(self, e):
        self._drag(e.position().x()); super().mousePressEvent(e)

    def mouseMoveEvent(self, e):
        if e.buttons() & Qt.MouseButton.LeftButton: self._drag(e.position().x())

    def _drag(self, x):
        f = min(1, max(0, (x - 12) / max(1, self.width() - 24)))
        self.setValue(int(round(self.minimum() + f * (self.maximum() - self.minimum()))))

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        cy = 12
        x0, x1 = 12, self.width() - 12
        xv = self._x_for(self.value())
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(T["ctrl"])
        p.drawRoundedRect(QRectF(x0 - 2, cy - 2, x1 - x0 + 4, 4), 2, 2)
        p.setBrush(accent())
        p.drawRoundedRect(QRectF(x0 - 2, cy - 2, xv - x0 + 2, 4), 2, 2)
        if self.ticks:
            p.setBrush(QColor(T["text2"]))
            for i in range(self.ticks):
                tx = x0 + (x1 - x0) * i / (self.ticks - 1)
                p.drawRoundedRect(QRectF(tx - 1, cy + 9, 2, 5), 1, 1)
        kw, kh = 26, 16
        r = QRectF(xv - kw / 2, cy - kh / 2, kw, kh)
        p.setBrush(tint(T["knob_shadow"], 50))
        p.drawRoundedRect(r.translated(0, 1), kh / 2, kh / 2)
        p.setBrush(QColor("white"))
        p.setPen(QPen(QColor(0, 0, 0, 25), 0.6))
        p.drawRoundedRect(r, kh / 2, kh / 2)


class MeguminSlider(QWidget):
    """Повзунок зі значенням (або з підписами «Повільно … Швидко»). Зберігає з дебаунсом 250 мс."""
    def __init__(self, key, value, minimum, maximum, suffix, save_callback, get_accent_cb=None, parent=None,
                 labels=None, ticks=0, width=200, fmt=None):
        super().__init__(parent)
        self.key, self.suffix, self.fmt = key, suffix, fmt
        self.save_callback = save_callback
        lay = QVBoxLayout(self) if labels else QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0 if labels else 10)
        self.slider = MacSlider(ticks)
        self.slider.setRange(minimum, maximum)
        self.slider.setValue(int(value))
        self.slider.setFixedWidth(width)
        lay.addWidget(self.slider)
        self.label = None
        if labels:
            row = QHBoxLayout()
            row.setContentsMargins(4, 0, 4, 0)
            for i, t in enumerate(labels):
                l = QLabel(t); l.setProperty("role", "tick")
                row.addWidget(l)
                if i < len(labels) - 1: row.addStretch()
            lay.addLayout(row)
        else:
            self.label = QLabel(self._text(int(value)))
            self.label.setProperty("role", "value")
            self.label.setFixedWidth(52)
            self.label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            lay.addWidget(self.label)
        self.timer = QTimer(self, singleShot=True, interval=250)
        self.timer.timeout.connect(lambda: self.save_callback(self.key, self.slider.value()))
        self.slider.valueChanged.connect(self.on_change)

    def _text(self, v):
        return self.fmt(v) if self.fmt else f"{v}{self.suffix}"

    def on_change(self, v):
        if self.label: self.label.setText(self._text(v))
        self.slider.update()
        self.timer.start()

    def update_styles(self): self.slider.update()


class SegmentedSelector(QWidget):
    """Сегментований контрол macOS: капсула з «скляним» виділенням, що плавно їде до вибраного сегмента."""
    def __init__(self, key, options, current, save_callback, get_accent_cb=None, parent=None, seg_width=None):
        super().__init__(parent)
        self.key, self.options, self.save_callback = key, options, save_callback
        ids = [o for o, _ in options]
        self.current = current if current in ids else ids[0]
        f = QFont(FONT_UI); f.setPixelSize(12); f.setWeight(QFont.Weight.Medium)
        fm = QFontMetrics(f)
        self.seg_w = seg_width or max(fm.horizontalAdvance(n) for _, n in options) + 26
        self.setFixedSize(self.seg_w * len(options) + 4, 28)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self._x = float(ids.index(self.current))
        self._anim = QVariantAnimation(self, duration=260, easingCurve=QEasingCurve.Type.OutBack)
        self._anim.valueChanged.connect(lambda v: (setattr(self, "_x", v), self.update()))

    @property
    def buttons(self):  # сумісність зі старим кодом
        return {o: None for o, _ in self.options}

    def select(self, opt, save=True):
        ids = [o for o, _ in self.options]
        if opt not in ids: return
        self.current = opt
        self._anim.stop(); self._anim.setStartValue(self._x); self._anim.setEndValue(float(ids.index(opt))); self._anim.start()
        if save and self.save_callback: self.save_callback(self.key, opt)

    def set_profile(self, opt): self.select(opt)

    def mousePressEvent(self, e):
        i = int((e.position().x() - 2) // self.seg_w)
        if 0 <= i < len(self.options): self.select(self.options[i][0])

    def update_styles(self): self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(T["ctrl"])
        p.drawRoundedRect(QRectF(self.rect()), 14, 14)
        r = QRectF(2 + self._x * self.seg_w, 2, self.seg_w, 24)
        p.setBrush(tint(T["knob_shadow"], 40))
        p.drawRoundedRect(r.translated(0, 1), 12, 12)
        p.setBrush(T["seg_sel"])
        p.drawRoundedRect(r, 12, 12)
        f = QFont(FONT_UI); f.setPixelSize(12); f.setWeight(QFont.Weight.Medium)
        p.setFont(f)
        for i, (o, n) in enumerate(self.options):
            sel = o == self.current
            p.setPen(QColor(T["text"]) if sel else tint(T["text"], 200))
            p.drawText(QRectF(2 + i * self.seg_w, 2, self.seg_w, 24), Qt.AlignmentFlag.AlignCenter, n)


class PowerProfileSelector(SegmentedSelector):
    def __init__(self, current_profile, save_callback, get_accent_cb=None, parent=None):
        super().__init__("power_profile", [("power-saver", "Економ"), ("balanced", "Баланс"), ("performance", "Макс")],
                         current_profile, save_callback, parent=parent)


class DockWhitelistEditor(QWidget):
    def __init__(self, current_whitelist, save_callback, get_accent_cb=None, parent=None):
        super().__init__(parent)
        self.save_callback = save_callback
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.input_field = QLineEdit(current_whitelist)
        self.input_field.setPlaceholderText("nautilus, kitty, steam...")
        self.input_field.setFixedWidth(230)
        self.input_field.editingFinished.connect(lambda: self.save_callback("dock_whitelist", self.input_field.text()))
        layout.addWidget(self.input_field)

    def update_styles(self): pass


class LinkButton(QAbstractButton):
    """Кнопка macOS (сіра капсула) для дій типу «Відкрити…»."""
    def __init__(self, text, cb, parent=None):
        super().__init__(parent)
        self.setText(text)
        f = QFont(FONT_UI); f.setPixelSize(12); f.setWeight(QFont.Weight.Medium)
        self._f = f
        self.setFixedSize(QFontMetrics(f).horizontalAdvance(text) + 26, 26)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        # clicked(bool) не передаємо: інакше lambda e=ev отримала б False замість події → TypeError → краш
        self.clicked.connect(lambda *_: cb())

    def update_styles(self): self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(tint(T["ctrl"], 150 if self.isDown() else 110 if self.underMouse() else 80) if True else T["ctrl"])
        p.drawRoundedRect(QRectF(self.rect()), 13, 13)
        p.setFont(self._f)
        p.setPen(QColor(T["text"]))
        p.drawText(QRectF(self.rect()), Qt.AlignmentFlag.AlignCenter, self.text())


class MacPopup(QAbstractButton):
    """Спливне меню macOS (pop-up button): поточне значення + «⌃⌄», список у QMenu."""
    def __init__(self, key, options, current, save_callback, width=210):
        super().__init__()
        self.key, self.save_callback = key, save_callback
        self.set_options(options, current)
        self.setFixedSize(width, 26)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.clicked.connect(self.open_menu)

    def set_options(self, options, current=None):
        self.options = list(options)
        ids = [o for o, _ in self.options]
        self.current = current if current in ids else (current or (ids[0] if ids else ""))
        self.update()

    def label(self):
        for o, n in self.options:
            if o == self.current: return n
        return self.current or "—"

    def open_menu(self):
        m = QMenu(self)
        for o, n in self.options:
            act = m.addAction(("✓  " if o == self.current else "     ") + n)
            act.triggered.connect(lambda _=False, o=o: self.choose(o))
        m.exec(self.mapToGlobal(QPoint(0, self.height() + 2)))

    def choose(self, o):
        self.current = o
        self.update()
        if self.save_callback: self.save_callback(self.key, o)

    def update_styles(self): self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(QPen(T["card_border"], 1))
        p.setBrush(tint(T["ctrl"], 120 if self.underMouse() else 80))
        p.drawRoundedRect(QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7)
        f = QFont(FONT_UI); f.setPixelSize(12); p.setFont(f)
        p.setPen(QColor(T["text"]))
        el = QFontMetrics(f).elidedText(self.label(), Qt.TextElideMode.ElideRight, self.width() - 40)
        p.drawText(QRectF(10, 0, self.width() - 36, self.height()), Qt.AlignmentFlag.AlignVCenter, el)
        r = QRectF(self.width() - 22, 4, 18, 18)
        p.setPen(Qt.PenStyle.NoPen); p.setBrush(accent())
        p.drawRoundedRect(r, 5, 5)
        p.setPen(QPen(QColor("white"), 1.4))
        c = r.center()
        p.drawPolyline([QPointF(c.x() - 3, c.y() - 1.5), QPointF(c.x(), c.y() - 4.5), QPointF(c.x() + 3, c.y() - 1.5)])
        p.drawPolyline([QPointF(c.x() - 3, c.y() + 1.5), QPointF(c.x(), c.y() + 4.5), QPointF(c.x() + 3, c.y() + 1.5)])


class IconPreview(QWidget):
    """Ряд типових іконок вибраної теми (теки, термінал, браузер...)."""
    NAMES = ["folder", "user-home", "utilities-terminal", "firefox", "preferences-system", "text-x-generic",
             "image-x-generic", "audio-x-generic", "system-file-manager", "accessories-text-editor"]

    def __init__(self):
        super().__init__()
        self.theme = QIcon.themeName()
        self.setFixedHeight(64)

    def set_theme(self, t):
        self.theme = t
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        old = QIcon.themeName()
        QIcon.setThemeName(self.theme)
        n = len(self.NAMES)
        step = max(44, (self.width() - 28) / n)
        for i, name in enumerate(self.NAMES):
            ic = QIcon.fromTheme(name)
            if ic.isNull(): continue
            x = int(14 + i * step + (step - 40) / 2)
            if x + 40 > self.width(): break
            p.drawPixmap(x, 12, ic.pixmap(40, 40))
        QIcon.setThemeName(old)


def list_themes(kind):
    """kind='icons' — теми іконок, 'cursors' — теми курсорів (з /usr/share/icons, ~/.local/share/icons, ~/.icons)."""
    out = {}
    for base in ("/usr/share/icons", os.path.expanduser("~/.local/share/icons"), os.path.expanduser("~/.icons")):
        if not os.path.isdir(base): continue
        for d in sorted(os.listdir(base)):
            path = os.path.join(base, d)
            if d in ("default", "hicolor", "locolor") or not os.path.isdir(path): continue
            has_cur = os.path.isdir(os.path.join(path, "cursors"))
            idx = os.path.join(path, "index.theme")
            if kind == "cursors":
                if has_cur: out[d] = d
            elif os.path.isfile(idx):
                try:
                    txt = open(idx, errors="ignore").read()
                except Exception:
                    continue
                if "Directories=" in txt and any(l.startswith("Directories=") and len(l.strip()) > 13 for l in txt.splitlines()):
                    name = next((l.split("=", 1)[1].strip() for l in txt.splitlines() if l.startswith("Name=")), d)
                    out[d] = name if name.lower() == d.lower() else f"{name} ({d})"
    return sorted(out.items(), key=lambda kv: kv[1].lower())


def gsettings_get(key):
    try:
        return subprocess.run(["gsettings", "get", "org.gnome.desktop.interface", key], capture_output=True, text=True, timeout=1).stdout.strip().strip("'")
    except Exception:
        return ""


def set_gtk_ini(key, value):
    """Дублює значення в ~/.config/gtk-3.0|4.0/settings.ini (для застосунків, що не читають gsettings)."""
    for v in ("gtk-3.0", "gtk-4.0"):
        f = Path.home() / ".config" / v / "settings.ini"
        try:
            lines = f.read_text().splitlines() if f.exists() else ["[Settings]"]
            done = False
            for i, l in enumerate(lines):
                if l.split("=")[0].strip() == key:
                    lines[i] = f"{key}={value}"; done = True
            if not done: lines.append(f"{key}={value}")
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text("\n".join(lines) + "\n")
        except Exception:
            pass


# Набори іконок/курсорів для завантаження: (назва, опис, команда у терміналі)
_VL = "git clone --depth 1 https://github.com/vinceliuice/{r}.git /tmp/mgos-{r} && cd /tmp/mgos-{r} && ./install.sh {a}; rm -rf /tmp/mgos-{r}; echo; read -p 'Готово — Enter'"
ICON_PACKS = [
    ("MacTahoe", "Іконки macOS 26", _VL.format(r="MacTahoe-icon-theme", a="")),
    ("WhiteSur", "Іконки в стилі macOS Big Sur", _VL.format(r="WhiteSur-icon-theme", a="")),
    ("Tela", "Яскраві плоскі іконки", _VL.format(r="Tela-icon-theme", a="")),
    ("Colloid", "М'які заокруглені іконки", _VL.format(r="Colloid-icon-theme", a="")),
    ("Papirus", "Популярна тема (пакет pacman)", "sudo pacman -S --needed papirus-icon-theme; read -p 'Готово — Enter'"),
]
CURSOR_PACKS = [
    ("WhiteSur cursors", "Курсори macOS", _VL.format(r="WhiteSur-cursors", a="")),
    ("Bibata", "Сучасні курсори (AUR)", "yay -S --needed bibata-cursor-theme-bin; read -p 'Готово — Enter'"),
]


# Значення за замовчуванням для налаштувань панелей (ті самі, що в SYSui/sysui_common.h)
PANEL_DEFAULTS = {
    "accent_mode": "wal", "accent_color": "#9929CF", "animations": True,
    "topbar_style": "bubbles", "topbar_opacity": 80, "topbar_border": True, "topbar_lang": "flag",
    "dock_opacity": 80, "dock_border": True, "dock_border_style": "subtle", "dock_border_opacity": 100,
    "dock_icon_size": 38, "dock_magnify": True, "dock_magnify_pct": 160, "dock_reserve": False, "dock_minimize_drag": True, "dock_stash_layout": "grid",
    "cc_opacity": 78, "watchdog": True, "sys_popups": True,
    "dock_files": True, "dock_files_source": "both", "dock_files_count": 4, "dock_files_days": 3,
    "dock_q_stash": False, "dock_stash_zram": True, "dock_recent": True, "dock_recent_count": 15, "osd_position": "bottom", "notify_position": "bottom",
    "sddm_theme": "pixel", "sddm_bg_mode": "wallpaper", "sddm_pixel_size": 6, "sddm_dim": 25, "sddm_blur": False,
    "sddm_clock_style": "wave", "sddm_clock24": True, "sddm_seconds": True, "sddm_pixel_avatar": False,
    "topbar_power": True, "panel_font": "SF Pro Display", "pixel_style": "waves", "eco_fs_blur": True,
}

# Стилі піксельних шапок (SYSui/pixel_waves.h: about, блок годинника)
# Піксельні шпалери: ті самі стилі + «Чорна діра» (лінзує курсор і вікна; для маленьких шапок віджетів не годиться)
PIXEL_STYLES_WP = None  # заповнюється нижче
PIXEL_STYLES = [("waves", "Хвилі"), ("aurora", "Аврора"), ("horizon", "Захід сонця"), ("virus", "Вірус"), ("eq", "Еквалайзер"), ("rain", "Піксельний дощ"), ("life", "Гра «Життя»")]
PIXEL_STYLES_WP = PIXEL_STYLES + [("blackhole", "Чорна діра")]

# Тачпад: значення за замовчуванням = mgOS/kiosk (touchpad.conf + mgos-auto.conf)
TP_DEFAULTS = {
    "tp_speed": 62, "tp_accel": "adaptive", "tp_tap": True, "tp_secondary": "fingers", "tp_tap_drag": True,
    "tp_drag_lock": False, "tp_dwt": False, "tp_middle": False, "tp_natural": False, "tp_scroll": 80,
    "tp_swipe_invert": False, "tp_swipe_dist": 300, "tp_swipe_new": True, "tp_swipe_forever": True,
    "kb_delay": 250, "kb_rate": 35,
    # Миша (усі звичайні миші): значення = старий input-блок hyprland.conf (flat, 0.3)
    "ms_speed": 65, "ms_accel": "flat", "ms_natural": False, "ms_scroll": 100, "ms_left": False,
    # Apple Magic Mouse: Hyprland (курсор) + драйвер hid_magicmouse (прокрутка дотиком)
    "mm_speed": 70, "mm_accel": "adaptive", "mm_natural": True,
    "mm_scroll": True, "mm_scroll_speed": 32, "mm_scroll_accel": True, "mm_middle": False, "mm_left": False,
}
MM_PARAMS = Path("/sys/module/hid_magicmouse/parameters")

# Розмиття Hyprland: значення mgOS за замовчуванням (= hyprland.conf). Слайдери цілі → масштаб.
BLUR_FILE = Path.home() / ".config" / "hypr" / "blur-user.conf"
BLUR_SPEC = [  # ключ, опція Hyprland, мін, макс, множник (слайдер → значення), назва, опис
    ("blur_size", "size", 1, 20, 1, "Радіус", "Наскільки сильно розмите те, що під склом"),
    ("blur_passes", "passes", 1, 6, 1, "Проходи", "Більше — м'якше розмиття, але більше навантаження на GPU"),
    ("blur_noise", "noise", 0, 100, 0.001, "Зернистість", "Легкий шум, як матове скло (0 — ідеально гладко)"),
    ("blur_contrast", "contrast", 0, 200, 0.01, "Контраст", "Контраст розмитого фону"),
    ("blur_brightness", "brightness", 0, 200, 0.01, "Яскравість", "Темніше чи світліше скло"),
    ("blur_vibrancy", "vibrancy", 0, 100, 0.01, "Насиченість", "Кольори крізь скло, як vibrancy у macOS"),
    ("blur_vibrancy_dark", "vibrancy_darkness", 0, 100, 0.01, "Насиченість темних тонів", ""),
]
BLUR_BOOL = [  # ключ, опція, назва, опис, за замовчуванням
    ("blur_popups", "popups", "Розмиття меню і спливаючих вікон", "", True),
    ("blur_special", "special", "Розмиття на спеціальному робочому столі", "Scratchpad / special workspace", True),
    ("blur_xray", "xray", "Рентген", "Плаваючі вікна розмивають лише шпалери — швидше, але менш реалістично", False),
]
BLUR_DEFAULTS = {"blur_size": 5, "blur_passes": 4, "blur_noise": 12, "blur_contrast": 89, "blur_brightness": 100,
                 "blur_vibrancy": 17, "blur_vibrancy_dark": 0, "blur_popups": True, "blur_special": True, "blur_xray": False}


def blur_value(key, raw):
    for k, opt, lo, hi, mul, *_ in BLUR_SPEC:
        if k == key:
            v = raw * mul
            return str(int(v)) if mul == 1 else f"{v:.4f}"
    return "true" if raw else "false"


def blur_option(key):
    for k, opt, *_ in BLUR_SPEC:
        if k == key: return opt
    for k, opt, *_ in BLUR_BOOL:
        if k == key: return opt
    return None


def write_blur_conf(settings):
    """~/.config/hypr/blur-user.conf — підключається в кінці hyprland.conf, тому переважає його значення."""
    lines = ["# ━━ mgOS: розмиття з «Налаштувань» → Вигляд. Файл перезаписується. ━━", "decoration {", "    blur {",
             f"        enabled = {'true' if settings.get('hyprland_blur', True) else 'false'}"]
    for k, opt, *_ in BLUR_SPEC + [(b[0], b[1]) for b in BLUR_BOOL]:
        lines.append(f"        {opt} = {blur_value(k, settings.get(k, BLUR_DEFAULTS[k]))}")
    lines += ["    }", "}"]
    BLUR_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = str(BLUR_FILE) + ".tmp"
    with open(tmp, "w") as f: f.write("\n".join(lines) + "\n")
    os.replace(tmp, BLUR_FILE)
    hc = Path.home() / ".config" / "hypr" / "hyprland.conf"
    try:
        if "blur-user.conf" not in hc.read_text():
            with open(hc, "a") as fh: fh.write("\nsource = ~/.config/hypr/blur-user.conf    # розмиття з Налаштувань\n")
    except Exception:
        pass


# Масштаб інтерфейсу: лише «чисті» масштаби, за яких роздільність ділиться без залишку (як у mgos-display)
SCALE_CANDIDATES = [1, 1.2, 1.25, 4 / 3, 1.5, 1.6, 5 / 3, 2, 2.4, 2.5, 3]
DISPLAY_OVERRIDES = Path.home() / ".config" / "mgos" / "display.conf"


def fmt_scale(x):
    return ("%.6f" % x).rstrip("0").rstrip(".")


def scale_ok(w, h, x):
    n = round(x * 120)
    return n > 0 and (w * 120) % n == 0 and (h * 120) % n == 0


def hypr_monitors():
    try:
        return json.loads(subprocess.run(["hyprctl", "monitors", "-j"], capture_output=True, text=True, timeout=1).stdout)
    except Exception:
        return []


MGOS_FAN = str(Path.home() / ".local/bin/mgos-fan")


def fan_status():
    """rpm/temp/profile/mode/writable/split — напряму з sysfs, БЕЗ процесів (викликається раз на 2 с з UI-потоку).
    Кулер Samsung — лише профілі прошивки, без PWM; логіка режимів — у ~/.local/bin/mgos-fan."""
    def rd(path, default=""):
        try: return Path(path).read_text().strip()
        except Exception: return default
    st = {"rpm": "0", "temp": "0"}
    for h in Path("/sys/class/hwmon").glob("hwmon*"):
        name = rd(h / "name")
        if name == "acpi_fan": st["rpm"] = rd(h / "fan1_input", "0")
        elif name == "coretemp": st["temp"] = str(int(rd(h / "temp1_input", "0") or 0) // 1000)
    pp = "/sys/firmware/acpi/platform_profile"
    st["profile"] = rd(pp)
    st["mode"] = rd(Path.home() / ".config/mgos/fan.conf", "auto") or "auto"
    st["writable"] = "1" if os.access(pp, os.W_OK) else "0"
    st["split"] = "0" if Path("/etc/systemd/system/power-profiles-daemon.service.d/mgos-fan.conf").exists() else "1"   # 1 = PPD стоковий (старий блок прибрано — інакше гріється кіоск)
    return st


FAN_CURVE_FILE = Path.home() / ".config" / "mgos" / "fan-curve.conf"
FAN_CURVE_DEFAULT = [(40, 10), (55, 35), (70, 60), (85, 90)]
FAN_BANDS = [(0, 25, "Економ"), (25, 50, "Тихий"), (50, 75, "Баланс"), (75, 100, "Макс")]


def load_fan_curve():
    try:
        pts = sorted((int(a), int(b)) for a, b in (l.split()[:2] for l in FAN_CURVE_FILE.read_text().splitlines()
                                                    if l.strip() and l.strip()[0].isdigit()))
        return pts if len(pts) >= 2 else list(FAN_CURVE_DEFAULT)
    except Exception:
        return list(FAN_CURVE_DEFAULT)


def save_fan_curve(pts):
    FAN_CURVE_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = str(FAN_CURVE_FILE) + ".tmp"
    with open(tmp, "w") as f:
        f.write("# mgOS: крива кулера (°C  відсоток). 0–25 економ · 25–50 тихий · 50–75 баланс · 75–100 макс\n")
        f.write("".join(f"{t} {v}\n" for t, v in pts))
    os.replace(tmp, FAN_CURVE_FILE)


class FanCurveWidget(QWidget):
    """Сітка «температура × оберти»: крива з точками, які можна тягати. Смуги = профілі прошивки Samsung
    (кулер Galaxy Book керується лише ними). Жива точка — поточна температура CPU і реальні оберти."""
    T_MIN, T_MAX = 30, 100

    def __init__(self, on_change, parent=None):
        super().__init__(parent)
        self.on_change = on_change
        self.pts = load_fan_curve()
        self.drag = -1
        self.hover = -1
        self.live = None   # (°C, rpm)
        self.setMinimumHeight(230)
        self.setMouseTracking(True)
        self.save_timer = QTimer(self, singleShot=True, interval=350)
        self.save_timer.timeout.connect(lambda: (save_fan_curve(self.pts), self.on_change()))

    def plot(self):
        return QRectF(64, 12, self.width() - 64 - 14, self.height() - 12 - 30)

    def to_px(self, t, v):
        r = self.plot()
        return QPointF(r.left() + (t - self.T_MIN) / (self.T_MAX - self.T_MIN) * r.width(), r.bottom() - v / 100 * r.height())

    def from_px(self, pos):
        r = self.plot()
        t = self.T_MIN + (pos.x() - r.left()) / r.width() * (self.T_MAX - self.T_MIN)
        v = (r.bottom() - pos.y()) / r.height() * 100
        return t, v

    def value_at(self, t):
        p = self.pts
        if t <= p[0][0]: return p[0][1]
        if t >= p[-1][0]: return p[-1][1]
        for (x0, y0), (x1, y1) in zip(p, p[1:]):
            if t <= x1: return y0 + (y1 - y0) * (t - x0) / max(1, x1 - x0)
        return p[-1][1]

    def set_live(self, temp, rpm):
        self.live = (temp, rpm); self.update()

    def reset(self):
        self.pts = list(FAN_CURVE_DEFAULT); self.update(); self.save_timer.start()

    def _nearest(self, pos):
        best, bd = -1, 14.0
        for i, (t, v) in enumerate(self.pts):
            d = (self.to_px(t, v) - QPointF(pos)).manhattanLength()
            if d < bd: best, bd = i, d
        return best

    def mousePressEvent(self, e):
        self.drag = self._nearest(e.position())
        if self.drag < 0 and self.plot().contains(e.position()) and e.button() == Qt.MouseButton.LeftButton:
            t, v = self.from_px(e.position())          # клік по порожньому місцю — нова точка
            if len(self.pts) < 8:
                self.pts.append((int(round(t)), int(round(max(0, min(100, v))))))
                self.pts.sort(); self.drag = self.pts.index(min(self.pts, key=lambda q: abs(q[0] - t)))
                self.save_timer.start()
        elif self.drag >= 0 and e.button() == Qt.MouseButton.RightButton and len(self.pts) > 2:
            self.pts.pop(self.drag); self.drag = -1; self.save_timer.start()   # ПКМ — прибрати точку
        self.update()
        e.accept()   # не віддаємо вікну: інакше воно почне рухатися (startSystemMove)

    def mouseMoveEvent(self, e):
        if self.drag >= 0:
            t, v = self.from_px(e.position())
            lo = self.pts[self.drag - 1][0] + 2 if self.drag > 0 else self.T_MIN
            hi = self.pts[self.drag + 1][0] - 2 if self.drag < len(self.pts) - 1 else self.T_MAX
            self.pts[self.drag] = (int(round(max(lo, min(hi, t)))), int(round(max(0, min(100, v)))))
            self.save_timer.start()
        else:
            h = self._nearest(e.position())
            if h != self.hover:
                self.hover = h
                self.setCursor(QCursor(Qt.CursorShape.OpenHandCursor if h >= 0 else Qt.CursorShape.CrossCursor))
        self.update()

    def mouseReleaseEvent(self, e):
        self.drag = -1; self.update()

    def leaveEvent(self, e):
        self.hover = -1; self.update()

    def paintEvent(self, e):
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = self.plot()
        acc = QColor(ACCENT[0])
        txt2 = QColor(T["text2"])
        f = QFont(FONT_UI); f.setPixelSize(10); p.setFont(f)
        # смуги профілів
        for i, (lo, hi, name) in enumerate(FAN_BANDS):
            top, bot = self.to_px(0, hi).y(), self.to_px(0, lo).y()
            c = QColor(acc); c.setAlpha(10 + i * 9)
            p.fillRect(QRectF(r.left(), top, r.width(), bot - top), c)
            p.setPen(txt2)
            p.drawText(QRectF(0, top, r.left() - 8, bot - top), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, name)
        # сітка
        grid = QColor(T["text2"]); grid.setAlpha(40)
        p.setPen(QPen(grid, 1, Qt.PenStyle.DotLine))
        for t in range(self.T_MIN, self.T_MAX + 1, 10):
            x = self.to_px(t, 0).x(); p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
        for v in (25, 50, 75):
            y = self.to_px(0, v).y(); p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y))
        p.setPen(QPen(grid, 1)); p.setBrush(Qt.BrushStyle.NoBrush); p.drawRoundedRect(r, 6, 6)
        p.setPen(txt2)
        for t in range(self.T_MIN, self.T_MAX + 1, 10):
            x = self.to_px(t, 0).x()
            p.drawText(QRectF(x - 20, r.bottom() + 4, 40, 14), Qt.AlignmentFlag.AlignCenter, f"{t}°")
        p.drawText(QRectF(r.left(), r.bottom() + 16, r.width(), 14), Qt.AlignmentFlag.AlignCenter, "Температура CPU")
        # крива + заливка під нею
        path = QPainterPath(self.to_px(self.T_MIN, self.pts[0][1]))
        for t, v in self.pts: path.lineTo(self.to_px(t, v))
        path.lineTo(self.to_px(self.T_MAX, self.pts[-1][1]))
        fill = QPainterPath(path); fill.lineTo(r.bottomRight()); fill.lineTo(r.bottomLeft()); fill.closeSubpath()
        g = QLinearGradient(r.topLeft(), r.bottomLeft())
        c1 = QColor(acc); c1.setAlpha(70); c2 = QColor(acc); c2.setAlpha(0)
        g.setColorAt(0, c1); g.setColorAt(1, c2)
        p.fillPath(fill, QBrush(g))
        pen = QPen(acc, 2.4); pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin); pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        p.setPen(pen); p.setBrush(Qt.BrushStyle.NoBrush); p.drawPath(path)
        # точки
        for i, (t, v) in enumerate(self.pts):
            c = self.to_px(t, v); rad = 7 if i in (self.drag, self.hover) else 5.5
            p.setPen(QPen(QColor("white"), 2)); p.setBrush(acc); p.drawEllipse(c, rad, rad)
            if i in (self.drag, self.hover):
                p.setPen(QColor(T["text"]))
                lbl = f"{t}°C · {v}% · {next(n for lo, hi, n in FAN_BANDS if v < hi or hi == 100)}"
                p.drawText(QRectF(c.x() - 80, c.y() - 26, 160, 14), Qt.AlignmentFlag.AlignCenter, lbl)
        # жива точка
        if self.live:
            t, rpm = self.live
            tt = max(self.T_MIN, min(self.T_MAX, t))
            c = self.to_px(tt, self.value_at(tt))
            lc = QColor("#ff9f0a")
            p.setPen(QPen(lc, 1, Qt.PenStyle.DashLine)); p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()))
            p.setPen(QPen(QColor("white"), 2)); p.setBrush(lc); p.drawEllipse(c, 5, 5)
            p.setPen(lc)
            lbl = f"зараз {t}°C · " + (f"{rpm} об/хв" if rpm else "кулер стоїть")
            x = c.x() + 8 if c.x() < r.center().x() else c.x() - 168
            p.drawText(QRectF(x, r.top() + 4, 160, 14),
                       (Qt.AlignmentFlag.AlignLeft if c.x() < r.center().x() else Qt.AlignmentFlag.AlignRight) | Qt.AlignmentFlag.AlignVCenter, lbl)

# MFG Unlocker (github.com/dashdogy/MFG-Unlocker): конфіг спільний з ~/.local/bin/mfg-unlocker-run
MFG_CFG = Path.home() / ".config" / "mfg-unlocker" / "config.json"
MFG_RUN = str(Path.home() / ".local" / "bin" / "mfg-unlocker-run")
MFG_DEFAULT_DLL = Path.home() / ".local" / "share" / "mfg-unlocker" / "MFGUnlocker.dll"
MFG_DLL_NAMES = ["version.dll", "winmm.dll", "dinput8.dll", "dxgi.dll", "d3d12.dll", "d3d11.dll", "dsound.dll",
                 "wininet.dll", "winhttp.dll", "binkw64.dll", "bink2w64.dll", "xinput1_3.dll", "xinput1_4.dll"]
STEAM_TOOLS = ("proton", "steam linux runtime", "steamworks", "steamvr")


def mfg_load():
    try: c = json.loads(MFG_CFG.read_text())
    except Exception: c = {}
    c.setdefault("mod_dll", str(MFG_DEFAULT_DLL)); c.setdefault("ask", True); c.setdefault("timeout_default", True)
    c.setdefault("games", {})
    return c


def mfg_save(c):
    MFG_CFG.parent.mkdir(parents=True, exist_ok=True)
    tmp = str(MFG_CFG) + ".tmp"
    with open(tmp, "w") as f: json.dump(c, f, ensure_ascii=False, indent=2)
    os.replace(tmp, MFG_CFG)


def steam_games():
    """[(appid, назва)] встановлених ігор Steam (без Proton/Runtime) з усіх бібліотек."""
    out, seen = [], set()
    for root in (Path.home() / ".local/share/Steam", Path.home() / ".steam/steam",
                 Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam"):
        vdf = root / "steamapps" / "libraryfolders.vdf"
        if not vdf.exists(): continue
        libs = [ln.strip().split('"')[3] for ln in vdf.read_text(errors="ignore").splitlines()
                if ln.strip().startswith('"path"') and len(ln.strip().split('"')) >= 4]
        for lib in libs:
            for acf in Path(lib, "steamapps").glob("appmanifest_*.acf"):
                kv = {}
                for ln in acf.read_text(errors="ignore").splitlines():
                    pr = ln.strip().split('"')
                    if len(pr) >= 4 and pr[1] in ("appid", "name"): kv[pr[1]] = pr[3]
                aid, name = kv.get("appid"), kv.get("name", "")
                if aid and aid not in seen and not name.lower().startswith(STEAM_TOOLS):
                    seen.add(aid); out.append((aid, name))
    return sorted(out, key=lambda g: g[1].lower())

# ---------- Довідка: усі бінди ----------
MODBITS = [(64, "Super"), (4, "Ctrl"), (8, "Alt"), (1, "Shift"), (2, "Caps"), (32, "Mod3"), (128, "Mod5")]
KEYNAMES = {"mouse:272": "ЛКМ", "mouse:273": "ПКМ", "mouse:274": "Коліщатко", "mouse_down": "Коліщатко ↓",
            "mouse_up": "Коліщатко ↑", "SPACE": "Space", "ESCAPE": "Esc", "RETURN": "Enter", "Alt_L": "Alt",
            "XF86AudioRaiseVolume": "Гучн +", "XF86AudioLowerVolume": "Гучн −", "XF86AudioMute": "Без звуку",
            "XF86AudioPlay": "▶︎❙❙", "XF86AudioNext": "⏭", "XF86AudioPrev": "⏮", "XF86MonBrightnessUp": "Яскр +",
            "XF86MonBrightnessDown": "Яскр −", "Print": "PrtSc", "left": "←", "right": "→", "up": "↑", "down": "↓",
            "l": "←", "r": "→", "u": "↑", "d": "↓", "fn": "Fn"}
EXEC_RULES = [   # (підрядок команди, опис, категорія)
    ("toggle-window launcher", "Запуск програм (Ignis)", "Застосунки"), ("runner.sock", "Пошук і запуск (Runner)", "Застосунки"),
    ("powermenu.sock", "Меню вимкнення", "Система"), ("clipboard.sock", "Історія буфера обміну", "Система"),
    ("cc.sock", "Центр керування", "Система"), ("about.sock", "Про систему (вікно)", "Система"),
    ("wallpapers.sock", "Шпалери", "Оформлення"), ("settings.py", "Налаштування", "Система"),
    ("dispatch exit", "Вийти з Hyprland", "Система"), ("mgos-close", "Сховати вікно в сховище доку (програма лишається відкритою)", "Вікна"), ("hyprctl kill", "Примусово закрити вікно (клікнути по ньому)", "Вікна"),
    ("togglefloating", "Плаваюче вікно 1000×600", "Вікна"), ("Toggled All Float", "Плаваючий режим для всіх", "Вікна"),
    ("resurrect.sh", "Відновити сесію Hyprland", "Система"), ("toggle-power-save", "Режим енергозбереження", "Система"),
    ("toggle-mode.sh", "Перемкнути режим mgOS", "Система"), ("dunstctl close-all", "Закрити всі сповіщення", "Система"),
    ("slurp", "Знімок області → буфер", "Знімки екрана"), ("grim - | wl-copy", "Знімок екрана → буфер", "Знімки екрана"),
    ("grim ~/Pictures", "Знімок екрана → ~/Pictures/screenshots", "Знімки екрана"), ("dunst/screenshot", "Сповіщення про знімок", "Знімки екрана"),
    ("flameshot", "Flameshot (редактор знімка)", "Знімки екрана"), ("hyprpicker -a", "Піпетка кольору", "Знімки екрана"),
    ("set-mute", "Вимкнути / увімкнути звук", "Звук і медіа"), ("5%+", "Гучніше", "Звук і медіа"), ("5%-", "Тихіше", "Звук і медіа"),
    ("play-pause", "Відтворення / пауза", "Звук і медіа"), ("media.sh next", "Наступний трек", "Звук і медіа"),
    ("media.sh previous", "Попередній трек", "Звук і медіа"), ("echo up", "Яскравість екрана +", "Звук і медіа"),
    ("echo down", "Яскравість екрана −", "Звук і медіа"), ('mpv "$(wl-paste)"', "Відкрити посилання з буфера в mpv", "Застосунки"),
    ("claude", "Claude (термінал)", "Застосунки"), ("fastfetch", "Термінал з fastfetch", "Застосунки"),
    ("btop", "Диспетчер процесів (btop)", "Застосунки"), ("bluetuith", "Bluetooth у терміналі", "Застосунки"),
    ("fish -c yy", "Файли (yazi)", "Застосунки"), ("nautilus", "Файли (Nautilus)", "Застосунки"),
    ("cool-retro-term", "Ретро-термінал", "Застосунки"), ("SLOWSHELL", "SlowShell", "Застосунки"), ("firefox", "Firefox", "Застосунки"),
    ("wezterm", "Термінал (WezTerm)", "Застосунки"), ("wofi", "Wofi", "Застосунки"), ("killall waybar", "Сховати Waybar", "Оформлення"),
    ("waybar", "Показати Waybar", "Оформлення"), ("timeWALL", "Шпалери за часом доби", "Оформлення"),
    ("switcher", "Перемикач вікон", "Вікна"), ("ignis", "Ignis", "Система"),
]
DIR_UA = {"l": "ліворуч", "r": "праворуч", "u": "вгору", "d": "вниз", "left": "ліворуч", "right": "праворуч", "up": "вгору", "down": "вниз"}


def describe_bind(b):
    d, a = b.get("dispatcher", ""), b.get("arg", "")
    if b.get("description"): return b["description"], "Інше"
    if d == "exec":
        for sub, text, cat in EXEC_RULES:
            if sub in a: return text, cat
        return a.split()[0].split("/")[-1] if a else "Команда", "Інше"
    if d == "workspace": return f"Робочий стіл {a}", "Робочі столи"
    if d in ("movetoworkspace", "movetoworkspacesilent"):
        return f"Перенести вікно на стіл {a}" + (" (без переходу)" if d.endswith("silent") else ""), "Робочі столи"
    if d == "togglespecialworkspace":
        return ("Сховище доку (сховані вікна)" if a == "mgos-dock" else "Схованка (scratchpad)"), "Робочі столи"
    if d == "workspaceopt": return f"Опція столу: {a}", "Робочі столи"
    m = {"killactive": "Закрити вікно", "fullscreen": "На весь екран", "togglefloating": "Плаваюче вікно",
         "togglegroup": "Згрупувати вікна (вкладки)", "moveoutofgroup": "Винести вікно з групи",
         "changegroupactive": "Наступна вкладка групи", "pin": "Закріпити поверх усіх"}
    if d in m: return m[d], "Вікна"
    if d == "layoutmsg": return ("Змінити напрям поділу" if "togglesplit" in a else f"Розкладка: {a}"), "Вікна"
    if d == "movefocus": return f"Фокус {DIR_UA.get(a, a)}", "Вікна"
    if d == "movewindow": return f"Посунути вікно {DIR_UA.get(a, a)}", "Вікна"
    if d == "swapwindow": return f"Поміняти вікно {DIR_UA.get(a, a)}", "Вікна"
    if d == "resizeactive": return f"Змінити розмір: {a}", "Вікна"
    if d == "mouse": return ("Перетягнути вікно" if "move" in a else "Змінити розмір мишею"), "Вікна"
    return f"{d} {a}".strip(), "Інше"


def bind_keys(b):
    keys = [n for bit, n in MODBITS if b.get("modmask", 0) & bit]
    k = b.get("key", "")
    keys.append(KEYNAMES.get(k, KEYNAMES.get(k.upper(), k.upper() if len(k) == 1 else k)))
    return keys


class KeyChips(QWidget):
    """Клавіші як «кейкапи» macOS: Super + Shift + S."""
    def __init__(self, keys):
        super().__init__()
        self.keys = keys
        f = QFont(FONT_UI); f.setPixelSize(11); f.setWeight(QFont.Weight.DemiBold); self.f = f
        fm = QFontMetrics(f)
        self.ws = [max(22, fm.horizontalAdvance(k) + 14) for k in keys]
        self.setFixedSize(sum(self.ws) + 14 * (len(keys) - 1) + 2, 24)

    def paintEvent(self, e):
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing); p.setFont(self.f)
        x = 1
        for i, (k, w) in enumerate(zip(self.keys, self.ws)):
            r = QRectF(x, 1, w, 21)
            p.setPen(QPen(T["card_border"], 1)); p.setBrush(T["field"]); p.drawRoundedRect(r, 5, 5)
            p.setPen(QColor(T["text"])); p.drawText(r, Qt.AlignmentFlag.AlignCenter, k)
            x += w
            if i < len(self.keys) - 1:
                p.setPen(QColor(T["text2"])); p.drawText(QRectF(x, 1, 14, 21), Qt.AlignmentFlag.AlignCenter, "+")
                x += 14

KBD_LED = next(iter(sorted(Path("/sys/class/leds").glob("*kbd_backlight*"))), None)

# Системні звуки: події freedesktop, які грає mgos-sound (або програми через libcanberra)
SOUND_EVENTS = [
    ("message-new-instant", "Сповіщення", "Нове повідомлення, нагадування"),
    ("desktop-login", "Вхід у систему", "Після входу в mgOS"),
    ("desktop-logout", "Вихід і вимкнення", "Перед вимкненням і перезапуском (вимкнено — без затримки)"),
    ("power-plug", "Зарядку підключено", ""),
    ("power-unplug", "Зарядку відключено", ""),
    ("audio-volume-change", "Зміна гучності", "Тихий «тік» клавішами гучності"),
    ("screen-capture", "Знімок екрана", "Shift+Print, Alt+F12, Super+Shift+S"),
    ("device-added", "Пристрій підключено", "USB, Bluetooth"),
    ("device-removed", "Пристрій відключено", ""),
    ("complete", "Готово", "Завершене завантаження, копіювання"),
    ("dialog-warning", "Попередження", ""),
    ("dialog-error", "Помилка", ""),
    ("trash-empty", "Кошик очищено", ""),
    ("battery-low", "Низький заряд", ""),
    ("message-sent-instant", "Надіслано", "Файл чи повідомлення пішли"),
]
SOUND_DEFAULTS = {"sounds_enabled": True, "sounds_volume": 70, "sound_theme": "mgos", "sounds_input": False}


def list_sound_themes():
    out = {}
    for base in (Path.home() / ".local/share/sounds", Path("/usr/share/sounds")):
        try:
            for d in sorted(base.iterdir()):
                idx = d / "index.theme"
                if not idx.exists(): continue
                name = d.name
                for l in idx.read_text(errors="ignore").splitlines():
                    if l.startswith("Name="): name = l[5:].strip(); break
                out.setdefault(d.name, name)
        except Exception:
            pass
    return [(k, v) for k, v in out.items()]

TP_FILE = Path.home() / ".config" / "hypr" / "touchpad-user.conf"


def tp_sensitivity(v):  # 0..100 → -1..1
    return round(v / 50.0 - 1.0, 2)


class TouchpadBackend:
    """Пише ~/.config/hypr/touchpad-user.conf (підключається після touchpad.conf / mgos-auto.conf)
    і застосовує зміни наживо через hyprctl --batch — без перезавантаження Hyprland."""
    def __init__(self, settings):
        self.s = settings

    def g(self, k):
        return self.s.get(k, TP_DEFAULTS[k])

    @staticmethod
    def magic_mice():
        """Apple Magic Mouse (1/2/USB-C) — Hyprland називає її за Bluetooth-ім'ям, напр. «apple-magic-mouse»."""
        try:
            out = subprocess.run(["hyprctl", "devices", "-j"], capture_output=True, text=True, timeout=1).stdout
            return [m["name"] for m in json.loads(out).get("mice", []) if "magic" in m["name"].lower()]
        except Exception:
            return []

    @staticmethod
    def touchpads():
        try:
            out = subprocess.run(["hyprctl", "devices", "-j"], capture_output=True, text=True, timeout=1).stdout
            return [m["name"] for m in json.loads(out).get("mice", []) if "touchpad" in m["name"].lower()]
        except Exception:
            return []

    def _b(self, k): return "true" if self.g(k) else "false"

    def render(self, pads, magic=()):
        s = [
            "# ━━ mgOS: тачпад, клавіатура, курсор з «Налаштувань» (settings.py). Файл перезаписується. ━━",
            "input {",
            f"    repeat_delay = {int(self.g('kb_delay'))}",
            f"    repeat_rate = {int(self.g('kb_rate'))}",
            f"    sensitivity = {tp_sensitivity(self.g('ms_speed'))}",
            f"    accel_profile = {self.g('ms_accel')}",
            f"    natural_scroll = {self._b('ms_natural')}",
            f"    scroll_factor = {self.g('ms_scroll') / 100:.2f}",
            f"    left_handed = {self._b('ms_left')}",
            "    touchpad {",
            f"        natural_scroll = {self._b('tp_natural')}",
            f"        tap-to-click = {self._b('tp_tap')}",
            f"        clickfinger_behavior = {'true' if self.g('tp_secondary') == 'fingers' else 'false'}",
            f"        tap-and-drag = {self._b('tp_tap_drag')}",
            f"        drag_lock = {self._b('tp_drag_lock')}",
            f"        disable_while_typing = {self._b('tp_dwt')}",
            f"        middle_button_emulation = {self._b('tp_middle')}",
            f"        scroll_factor = {self.g('tp_scroll') / 100:.2f}",
            "    }", "}", "gestures {",
            f"    workspace_swipe_invert = {self._b('tp_swipe_invert')}",
            f"    workspace_swipe_distance = {int(self.g('tp_swipe_dist'))}",
            f"    workspace_swipe_create_new = {self._b('tp_swipe_new')}",
            f"    workspace_swipe_forever = {self._b('tp_swipe_forever')}",
            "}",
        ]
        ct, cs = self.s.get("cursor_theme"), self.s.get("cursor_size")
        if ct and cs:
            s += [f"env = XCURSOR_THEME,{ct}", f"env = XCURSOR_SIZE,{int(cs)}", f"env = HYPRCURSOR_SIZE,{int(cs)}",
                  f"exec-once = hyprctl setcursor {ct} {int(cs)}"]
        for name in pads:
            s += ["device {", f"    name = {name}", f"    sensitivity = {tp_sensitivity(self.g('tp_speed'))}",
                  f"    accel_profile = {self.g('tp_accel')}", "}"]
        for name in magic:
            s += ["device {", f"    name = {name}", f"    sensitivity = {tp_sensitivity(self.g('mm_speed'))}",
                  f"    accel_profile = {self.g('mm_accel')}", f"    natural_scroll = {self._b('mm_natural')}",
                  f"    left_handed = {self._b('mm_left')}", "}"]
        return "\n".join(s) + "\n"

    def ensure_source(self):
        """Гарантує, що файл існує і підключений (Hyprland падає з помилкою на source неіснуючого файлу)."""
        TP_FILE.parent.mkdir(parents=True, exist_ok=True)
        if not TP_FILE.exists():
            TP_FILE.write_text(self.render(self.touchpads()))
        line = "source = ~/.config/hypr/touchpad-user.conf"
        hypr = Path.home() / ".config" / "hypr"
        for f in ("mgos-auto.conf", "hyprland.conf"):
            p = hypr / f
            try:
                if p.exists() and "touchpad-user.conf" in p.read_text():
                    return
            except Exception:
                pass
        try:
            with open(hypr / "hyprland.conf", "a") as fh:
                fh.write(f"\n{line}    # тачпад із Налаштувань\n")
        except Exception:
            pass

    def apply(self):
        pads, magic = self.touchpads(), self.magic_mice()
        try:
            self.ensure_source()
            tmp = str(TP_FILE) + ".tmp"
            with open(tmp, "w") as f:
                f.write(self.render(pads, magic))
            os.replace(tmp, TP_FILE)
        except Exception:
            pass
        kw = [
            f"keyword input:touchpad:natural_scroll {self._b('tp_natural')}",
            f"keyword input:touchpad:tap-to-click {self._b('tp_tap')}",
            f"keyword input:touchpad:clickfinger_behavior {'true' if self.g('tp_secondary') == 'fingers' else 'false'}",
            f"keyword input:touchpad:tap-and-drag {self._b('tp_tap_drag')}",
            f"keyword input:touchpad:drag_lock {self._b('tp_drag_lock')}",
            f"keyword input:touchpad:disable_while_typing {self._b('tp_dwt')}",
            f"keyword input:touchpad:middle_button_emulation {self._b('tp_middle')}",
            f"keyword input:touchpad:scroll_factor {self.g('tp_scroll') / 100:.2f}",
            f"keyword gestures:workspace_swipe_invert {self._b('tp_swipe_invert')}",
            f"keyword gestures:workspace_swipe_distance {int(self.g('tp_swipe_dist'))}",
            f"keyword gestures:workspace_swipe_create_new {self._b('tp_swipe_new')}",
            f"keyword gestures:workspace_swipe_forever {self._b('tp_swipe_forever')}",
            f"keyword input:repeat_delay {int(self.g('kb_delay'))}",
            f"keyword input:repeat_rate {int(self.g('kb_rate'))}",
            f"keyword input:sensitivity {tp_sensitivity(self.g('ms_speed'))}",
            f"keyword input:accel_profile {self.g('ms_accel')}",
            f"keyword input:natural_scroll {self._b('ms_natural')}",
            f"keyword input:scroll_factor {self.g('ms_scroll') / 100:.2f}",
            f"keyword input:left_handed {self._b('ms_left')}",
        ]
        for name in magic:
            kw += [f"keyword device[{name}]:sensitivity {tp_sensitivity(self.g('mm_speed'))}",
                   f"keyword device[{name}]:accel_profile {self.g('mm_accel')}",
                   f"keyword device[{name}]:natural_scroll {self._b('mm_natural')}",
                   f"keyword device[{name}]:left_handed {self._b('mm_left')}"]
        for name in pads:
            kw += [f"keyword device[{name}]:sensitivity {tp_sensitivity(self.g('tp_speed'))}",
                   f"keyword device[{name}]:accel_profile {self.g('tp_accel')}"]
        subprocess.Popen(["hyprctl", "--batch", " ; ".join(kw)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class TrackpadPreview(QWidget):
    """Анімоване прев'ю як у панелі «Трекпад» macOS: показує жест рядка, над яким курсор."""
    MODES = {
        "pointer": "Курсор рухається за пальцем", "tap": "Торкніться одним пальцем", "secondary": "Клік двома пальцями",
        "drag": "Подвійний дотик і тягніть", "scroll": "Прокручуйте двома пальцями", "swipe": "Змахніть трьома пальцями",
        "runner": "Три пальці вгору — пошук", "typing": "Тачпад під час набору тексту",
    }

    def __init__(self, backend, parent=None):
        super().__init__(parent)
        self.backend = backend
        self.mode = "pointer"
        self.t0 = time.monotonic()
        self.setFixedHeight(190)
        self.timer = QTimer(self, interval=16)
        self.timer.timeout.connect(self.update)

    def showEvent(self, e): self.timer.start(); super().showEvent(e)
    def hideEvent(self, e): self.timer.stop(); super().hideEvent(e)

    def set_mode(self, m):
        if m and m != self.mode:
            self.mode = m
            self.t0 = time.monotonic()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        t = time.monotonic() - self.t0
        W, H = self.width(), self.height()
        pw_, ph = 250, 150
        pad = QRectF((W - pw_) / 2, 12, pw_, ph)
        # «екран» з курсором ліворуч і трекпад по центру
        g = QLinearGradient(pad.topLeft(), pad.bottomLeft())
        base = QColor(T["card"])
        g.setColorAt(0, base.lighter(118) if T is not LIGHT else QColor(236, 236, 240))
        g.setColorAt(1, base.lighter(104) if T is not LIGHT else QColor(222, 222, 228))
        p.setPen(QPen(T["card_border"], 1))
        p.setBrush(QBrush(g))
        p.drawRoundedRect(pad, 16, 16)
        a = accent()
        dot = lambda x, y, s=1.0, al=230: (p.setBrush(tint(a, al)), p.setPen(QPen(QColor(255, 255, 255, 160), 1.5)),
                                           p.drawEllipse(QPointF(pad.left() + x, pad.top() + y), 11 * s, 11 * s))
        m = self.mode
        cx, cy = pw_ / 2, ph / 2
        speed = 0.5 + self.backend.g("tp_speed") / 100
        nat = -1 if self.backend.g("tp_natural") else 1
        if m == "pointer":
            ang = t * 1.6 * speed
            dot(cx + 70 * math.cos(ang), cy + 38 * math.sin(2 * ang) / 1.2)
        elif m in ("tap", "secondary"):
            k = (t % 1.3) / 1.3
            s = 1.0 - 0.25 * math.sin(min(1, k * 3) * math.pi)
            al = 230 if k < 0.6 else int(230 * (1 - (k - 0.6) / 0.4))
            if m == "tap": dot(cx, cy, s, al)
            else: dot(cx - 16, cy, s, al); dot(cx + 16, cy, s, al)
        elif m == "drag":
            k = (t % 2.2) / 2.2
            x = cx - 60 + 120 * min(1, max(0, (k - 0.25) / 0.6))
            s = 0.8 if (k < 0.12 or 0.16 < k < 0.25) else 1.0
            dot(x, cy, s)
        elif m == "scroll":
            k = (t % 1.4) / 1.4
            y = cy + nat * (-40 + 80 * k)
            al = int(230 * math.sin(k * math.pi))
            dot(cx - 15, y, 1, al); dot(cx + 15, y, 1, al)
        elif m == "swipe":
            k = (t % 1.5) / 1.5
            d = -1 if self.backend.g("tp_swipe_invert") else 1
            x = cx + d * (-60 + 120 * k)
            al = int(230 * math.sin(k * math.pi))
            for o in (-26, 0, 26): dot(x + o, cy - abs(o) * 0.3, 1, al)
        elif m == "runner":
            k = (t % 1.5) / 1.5
            y = cy + 40 - 80 * k
            al = int(230 * math.sin(k * math.pi))
            for o in (-26, 0, 26): dot(cx + o, y - abs(o) * 0.3, 1, al)
        elif m == "typing":
            f = QFont(ICON_FONT); f.setPixelSize(44); p.setFont(f)
            p.setPen(tint(T["text"], 90 + int(60 * (0.5 + 0.5 * math.sin(t * 3)))))
            p.drawText(pad, Qt.AlignmentFlag.AlignCenter, G["keyboard"])
        f = QFont(FONT_UI); f.setPixelSize(11); p.setFont(f)
        p.setPen(QColor(T["text2"]))
        p.drawText(QRectF(0, pad.bottom() + 6, W, 18), Qt.AlignmentFlag.AlignCenter, self.MODES.get(m, ""))


class MagicMousePreview(QWidget):
    """Прев'ю як у панелі «Миша» macOS: Magic Mouse згори з пальцями/зонами кліку і міні-екран з результатом жесту."""
    MODES = {
        "point": "Рухайте мишею — курсор іде слідом",
        "secondary": "Натисніть правою стороною — контекстне меню",
        "secondary_left": "Натисніть лівою стороною — контекстне меню",
        "middle": "Натисніть посередині — середня кнопка (посилання в новій вкладці)",
        "scroll": "Проведіть одним пальцем угору чи вниз — прокрутка",
        "scroll_h": "Проведіть пальцем ліворуч чи праворуч — прокрутка вбік",
        "inertia": "Швидко змахніть — сторінка прокручується далі за інерцією",
        "off": "Прокрутку дотиком вимкнено — поверхня працює лише як кнопка",
    }

    def __init__(self, backend, parent=None):
        super().__init__(parent)
        self.backend = backend
        self.mode = "point"
        self.t0 = time.monotonic()
        self.setFixedHeight(238)
        self.timer = QTimer(self, interval=16)
        self.timer.timeout.connect(self.update)

    def showEvent(self, e): self.timer.start(); super().showEvent(e)
    def hideEvent(self, e): self.timer.stop(); super().hideEvent(e)

    def set_mode(self, m):
        if m == "secondary" and self.backend.g("mm_left"): m = "secondary_left"
        if m in ("scroll", "scroll_h", "inertia") and not self.backend.g("mm_scroll"): m = "off"
        if m and m != self.mode:
            self.mode = m
            self.t0 = time.monotonic()

    @staticmethod
    def mouse_path(cx, top, w, h):
        """Силует Magic Mouse згори: вужчий заокруглений перед, ширша спинка."""
        path = QPainterPath()
        path.moveTo(cx, top)
        path.cubicTo(cx + w * 0.50, top, cx + w * 0.5, top + h * 0.22, cx + w * 0.5, top + h * 0.52)
        path.cubicTo(cx + w * 0.5, top + h * 0.84, cx + w * 0.40, top + h, cx, top + h)
        path.cubicTo(cx - w * 0.40, top + h, cx - w * 0.5, top + h * 0.84, cx - w * 0.5, top + h * 0.52)
        path.cubicTo(cx - w * 0.5, top + h * 0.22, cx - w * 0.50, top, cx, top)
        path.closeSubpath()
        return path

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        t = time.monotonic() - self.t0
        W, H = self.width(), self.height()
        m, a = self.mode, accent()
        nat = -1 if self.backend.g("mm_natural") else 1

        # ---------- миша ----------
        mw, mh = 92, 170
        mcx = W * 0.5 - 150
        mtop = 16
        # рух усієї миші в режимі «курсор»
        speed = 0.6 + self.backend.g("mm_speed") / 100
        ox = oy = 0.0
        if m == "point":
            ang = t * 1.5
            ox, oy = 10 * math.cos(ang), 6 * math.sin(2 * ang)
        mcx += ox; mtop += oy
        # тінь
        for i in range(6):
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(0, 0, 0, 14 if T is not LIGHT else 9))
            p.drawEllipse(QPointF(mcx + 4, mtop + mh * 0.56 + 8), mw * 0.5 + 6 - i * 1.5, mh * 0.5 + 4 - i * 1.5)
        # алюмінієва основа
        base = self.mouse_path(mcx, mtop + 3, mw + 6, mh + 4)
        g = QLinearGradient(mcx - mw / 2, 0, mcx + mw / 2, 0)
        g.setColorAt(0, QColor("#9ea1a7")); g.setColorAt(0.5, QColor("#d9dbdf")); g.setColorAt(1, QColor("#8e9197"))
        p.setBrush(QBrush(g)); p.setPen(QPen(QColor(0, 0, 0, 40), 1))
        p.drawPath(base)
        # глянцева сенсорна поверхня
        top = self.mouse_path(mcx, mtop, mw, mh)
        g = QLinearGradient(0, mtop, 0, mtop + mh)
        g.setColorAt(0, QColor("#ffffff")); g.setColorAt(0.55, QColor("#f3f4f6")); g.setColorAt(1, QColor("#e3e5e9"))
        p.setBrush(QBrush(g)); p.setPen(QPen(QColor(0, 0, 0, 28), 1))
        p.drawPath(top)
        hl = QRadialGradient(QPointF(mcx - mw * 0.18, mtop + mh * 0.22), mw * 0.6)
        hl.setColorAt(0, QColor(255, 255, 255, 170)); hl.setColorAt(1, QColor(255, 255, 255, 0))
        p.setBrush(QBrush(hl)); p.setPen(Qt.PenStyle.NoPen)
        p.drawPath(top)

        p.save()
        p.setClipPath(top)
        zone = lambda x, w_, al: (p.setBrush(tint(a, al)), p.drawRect(QRectF(x, mtop, w_, mh * 0.55)))
        def finger(x, y, al=230, s=1.0):
            p.setBrush(tint(a, al)); p.setPen(QPen(QColor(255, 255, 255, min(255, al)), 2))
            p.drawEllipse(QPointF(x, y), 12 * s, 12 * s); p.setPen(Qt.PenStyle.NoPen)
        k = (t % 1.6) / 1.6
        press = math.sin(min(1, k * 3) * math.pi) if k < 0.34 else 0
        if m in ("secondary", "secondary_left"):
            right = m == "secondary"
            zone(mcx if right else mcx - mw / 2, mw / 2, int(40 + 70 * press))
            finger(mcx + (22 if right else -22), mtop + mh * 0.28, 230, 1 - 0.2 * press)
        elif m == "middle":
            p.setBrush(tint(a, int(40 + 70 * press))); p.drawRect(QRectF(mcx - 14, mtop, 28, mh * 0.55))
            finger(mcx, mtop + mh * 0.28, 230, 1 - 0.2 * press)
        elif m in ("scroll", "inertia"):
            kk = (t % (1.0 if m == "inertia" else 1.5)) / (1.0 if m == "inertia" else 1.5)
            span = 34 if m == "inertia" else 56
            y = mtop + mh * 0.34 + (-span / 2 + span * min(1, kk * (2.2 if m == "inertia" else 1)))
            al = int(230 * math.sin(min(1, kk * (2.2 if m == "inertia" else 1)) * math.pi))
            for j in range(5):   # слід пальця
                p.setBrush(tint(a, max(0, al // (3 + j * 2)))); p.drawEllipse(QPointF(mcx, y - (j + 1) * 6), 9 - j, 9 - j)
            finger(mcx, y, al)
        elif m == "scroll_h":
            kk = (t % 1.5) / 1.5
            x = mcx - 26 + 52 * kk
            al = int(230 * math.sin(kk * math.pi))
            finger(x, mtop + mh * 0.32, al)
        elif m == "point":
            pass
        p.restore()

        # ---------- міні-екран ----------
        sw_, sh_ = 236, 162
        scr = QRectF(W * 0.5 - 20, 22, sw_, sh_)
        p.setPen(QPen(T["card_border"], 1))
        g = QLinearGradient(scr.topLeft(), scr.bottomLeft())
        base_c = QColor(T["card"])
        g.setColorAt(0, base_c.lighter(122) if T is not LIGHT else QColor(246, 246, 248))
        g.setColorAt(1, base_c.lighter(108) if T is not LIGHT else QColor(232, 232, 236))
        p.setBrush(QBrush(g)); p.drawRoundedRect(scr, 12, 12)
        for i, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor(c)); p.drawEllipse(QPointF(scr.left() + 14 + i * 12, scr.top() + 12), 4, 4)
        body = QRectF(scr.left() + 10, scr.top() + 26, sw_ - 20, sh_ - 34)
        p.save(); p.setClipRect(body)
        line_c = tint(T["text"], 38)
        def page_lines(dy, dx=0.0):
            for i in range(-12, 24):
                y = body.top() + 6 + i * 14 + (dy % 14 if dy else 0) + (dy - dy % 14 if dy else 0)
                if y < body.top() - 14 or y > body.bottom() + 14: continue
                w_ = body.width() * (0.92 if i % 4 else 0.55) - (i * 17 % 40)
                p.setBrush(tint(a, 160) if i % 7 == 0 else line_c); p.setPen(Qt.PenStyle.NoPen)
                p.drawRoundedRect(QRectF(body.left() + 6 + dx, y, w_, 6), 3, 3)
        def cursor(x, y):
            path = QPainterPath(); path.moveTo(x, y); path.lineTo(x, y + 16); path.lineTo(x + 4.5, y + 12)
            path.lineTo(x + 8, y + 18); path.lineTo(x + 10.5, y + 17); path.lineTo(x + 7, y + 11); path.lineTo(x + 12, y + 11); path.closeSubpath()
            p.setBrush(QColor("black")); p.setPen(QPen(QColor("white"), 1.4)); p.drawPath(path)
        if m in ("scroll", "inertia", "off"):
            if m == "scroll":
                kk = (t % 1.5) / 1.5
                dy = -nat * 60 * kk
            elif m == "inertia":
                kk = (t % 2.2) / 2.2
                dy = -nat * 160 * (1 - (1 - kk) ** 3)
            else:
                dy = 0
            page_lines(dy)
        elif m == "scroll_h":
            kk = (t % 1.5) / 1.5
            dx = -nat * 70 * kk
            for i in range(-2, 6):
                x = body.left() + 8 + i * 70 + dx % 70
                p.setBrush(tint(a, 70 + (i % 3) * 40)); p.setPen(Qt.PenStyle.NoPen)
                p.drawRoundedRect(QRectF(x, body.top() + 14, 60, body.height() - 28), 8, 8)
        else:
            page_lines(0)
        p.restore()
        if m == "point":
            ang = t * 1.5
            cursor(scr.center().x() + 60 * speed * math.cos(ang), scr.center().y() + 30 * speed * math.sin(2 * ang) / 1.2)
        elif m in ("secondary", "secondary_left", "middle"):
            cx_, cy_ = scr.center().x() - 30, scr.center().y() - 10
            cursor(cx_, cy_)
            k = (t % 1.6) / 1.6
            if k > 0.18:
                al = 255 if k < 0.85 else int(255 * (1 - (k - 0.85) / 0.15))
                if m == "middle":   # нова вкладка
                    tab = QRectF(scr.left() + 60, scr.top() + 4, 70, 16)
                    p.setBrush(tint(a, al)); p.setPen(Qt.PenStyle.NoPen); p.drawRoundedRect(tab, 5, 5)
                    p.setPen(tint(QColor("white"), al)); f = QFont(FONT_UI); f.setPixelSize(10); f.setBold(True); p.setFont(f)
                    p.drawText(tab, Qt.AlignmentFlag.AlignCenter, "+ вкладка")
                else:               # контекстне меню
                    menu = QRectF(cx_ + 10, cy_ + 8, 92, 70)
                    p.setBrush(tint(QColor(T["card"]).lighter(140) if T is not LIGHT else QColor("white"), al))
                    p.setPen(QPen(tint(T["text"], al // 6), 1)); p.drawRoundedRect(menu, 7, 7)
                    for i in range(4):
                        r = QRectF(menu.left() + 6, menu.top() + 6 + i * 15, menu.width() - 12, 12)
                        if i == 1: p.setBrush(tint(a, al)); p.setPen(Qt.PenStyle.NoPen); p.drawRoundedRect(r, 3, 3)
                        p.setBrush(tint(T["text"], al // 3) if i != 1 else tint(QColor("white"), al)); p.setPen(Qt.PenStyle.NoPen)
                        p.drawRoundedRect(QRectF(r.left() + 5, r.top() + 4, r.width() * (0.5 + 0.1 * i), 4), 2, 2)
        f = QFont(FONT_UI); f.setPixelSize(12); p.setFont(f)
        p.setPen(QColor(T["text2"]))
        p.drawText(QRectF(0, H - 26, W, 20), Qt.AlignmentFlag.AlignCenter, self.MODES.get(m, ""))


class AppearancePicker(QWidget):
    """Мініатюри «Світла / Темна» як у «Вигляд» macOS."""
    def __init__(self, is_dark, cb, parent=None):
        super().__init__(parent)
        self.dark, self.cb = is_dark, cb
        self.setFixedSize(2 * 96 + 22, 92)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))

    def mousePressEvent(self, e):
        d = e.position().x() > self.width() / 2
        if d != self.dark:
            self.dark = d
            self.update()
            self.cb("dark_theme", d)

    def update_styles(self): self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        f = QFont(FONT_UI); f.setPixelSize(11)
        for i, (name, dark) in enumerate((("Світла", False), ("Темна", True))):
            r = QRectF(3 + i * 108, 3, 90, 58)
            sel = dark == self.dark
            if sel:
                p.setPen(QPen(accent(), 3)); p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), 10, 10)
            g = QLinearGradient(r.topLeft(), r.bottomRight())
            g.setColorAt(0, QColor("#7aa7ff") if not dark else QColor("#2b3a67"))
            g.setColorAt(1, QColor("#ffb8d9") if not dark else QColor("#5a2d5e"))
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(QBrush(g))
            p.drawRoundedRect(r, 8, 8)
            win = QRectF(r.left() + 12, r.top() + 12, 62, 38)
            p.setBrush(QColor(250, 250, 252, 235) if not dark else QColor(36, 36, 40, 235))
            p.drawRoundedRect(win, 5, 5)
            p.setBrush(QColor(225, 225, 230) if not dark else QColor(60, 60, 66))
            p.drawRoundedRect(QRectF(win.left() + 4, win.top() + 9, 16, 25), 3, 3)
            p.setBrush(accent())
            p.drawRoundedRect(QRectF(win.left() + 24, win.top() + 10, 30, 5), 2, 2)
            for k, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
                p.setBrush(QColor(c)); p.drawEllipse(QPointF(win.left() + 5 + k * 5, win.top() + 4), 1.6, 1.6)
            p.setFont(f)
            p.setPen(QColor(T["text"]) if sel else QColor(T["text2"]))
            p.drawText(QRectF(r.left(), r.bottom() + 8, r.width(), 18), Qt.AlignmentFlag.AlignCenter, name)


class AccentPicker(QWidget):
    """Ряд кольорових кружечків акценту як у macOS. Перший — «з шпалер» (pywal)."""
    COLORS = ["#0a84ff", "#a550a7", "#f74f9e", "#ff5257", "#f7821b", "#ffc600", "#62ba46", "#8c8c8c"]

    def __init__(self, owner, parent=None):
        super().__init__(parent)
        self.owner = owner
        self.setFixedSize(26 * (len(self.COLORS) + 2) + 4, 30)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.setToolTip("Перший — колір із шпалер (pywal), останній — свій колір")

    def update_styles(self): self.update()

    def mousePressEvent(self, e):
        i = int(e.position().x() // 26)
        if i == 0: self.owner.set_accent_from_wal("accent_mode", True)
        elif 1 <= i <= len(self.COLORS): self.owner.set_custom_accent(self.COLORS[i - 1])
        elif i == len(self.COLORS) + 1: self.owner.pick_accent_color()
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        mode = self.owner.settings.get("accent_mode", "wal")
        cur = self.owner.accent_color.lower()
        for i in range(len(self.COLORS) + 2):
            c = QPointF(13 + i * 26, 15)
            if i == 0:
                g = QLinearGradient(c - QPointF(8, 8), c + QPointF(8, 8))
                g.setColorAt(0, QColor("#ff5f9e")); g.setColorAt(0.5, QColor("#a56bff")); g.setColorAt(1, QColor("#3fb6ff"))
                p.setBrush(QBrush(g)); sel = mode != "custom"
            elif i <= len(self.COLORS):
                col = self.COLORS[i - 1]
                p.setBrush(QColor(col)); sel = mode == "custom" and cur == col.lower()
            else:
                p.setBrush(QColor(self.owner.accent_color) if mode == "custom" and cur not in [x.lower() for x in self.COLORS] else T["ctrl"])
                sel = mode == "custom" and cur not in [x.lower() for x in self.COLORS]
            p.setPen(QPen(QColor(0, 0, 0, 40), 0.8))
            p.drawEllipse(c, 9, 9)
            if i == len(self.COLORS) + 1 and not sel:
                p.setPen(QColor(T["text"])); f = QFont(FONT_UI); f.setPixelSize(13); f.setBold(True); p.setFont(f)
                p.drawText(QRectF(c.x() - 9, c.y() - 10, 18, 18), Qt.AlignmentFlag.AlignCenter, "+")
            if sel:
                p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor("white"))
                p.drawEllipse(c, 3, 3)


# ==========================================
# 2. РЯДКИ, ГРУПИ, САЙДБАР
# ==========================================

class DockPreview(QWidget):
    """Живе прев'ю доку (як ілюстрації в «Робочий стіл і Док» macOS): міні-екран зі шпалерами,
    вікном і доком, що повторює налаштування. Наведення на рядок підсвічує частину доку,
    на яку він впливає; вимкнене показується «привидом» — видно, що саме прибирається."""
    MODES = {
        "size": "Розмір іконок у доку",
        "magnify": "Іконка під курсором збільшується",
        "bg": "Підкладка доку: 0% — іконки «висять» просто над шпалерами",
        "border": "Контур навколо доку",
        "reserve": "Вікна закінчуються над доком / док лежить поверх вікон",
        "drag": "Перетягніть вікно на док — програма сховається, іконка лишиться",
        "recent": "Закриті програми — після роздільника, клік запускає знову",
        "files": "Недавні файли — завантаження і скопійоване",
        "stash": "Сховані програми живуть у сховищі (Super+A)",
    }
    APPS = ["#0a84ff", "#ff9f0a", "#30d158", "#bf5af2", "#ff375f"]
    RECENT = ["#64d2ff", "#ffd60a", "#ac8e68", "#5e5ce6", "#ff453a", "#66d4cf"]

    def __init__(self, settings, parent=None):
        super().__init__(parent)
        self.s = settings
        self.mode = None
        self.t0 = time.monotonic()
        self.cur = {}                 # плавні (інтерпольовані) значення
        self.wall = None
        self.setFixedHeight(230)
        self.timer = QTimer(self, interval=16)
        self.timer.timeout.connect(self.update)

    def g(self, k):
        return self.s.get(k, PANEL_DEFAULTS.get(k))

    def showEvent(self, e):
        self.timer.start(); self._load_wall(); super().showEvent(e)

    def hideEvent(self, e):
        self.timer.stop(); super().hideEvent(e)

    def set_mode(self, m):
        if m != self.mode:
            self.mode = m
            self.t0 = time.monotonic()

    def leaveEvent(self, e):
        super().leaveEvent(e)

    def _load_wall(self):
        if self.wall is not None:
            return
        home = Path.home()
        path = None
        try:
            last = (home / ".cache/ignis/last_wallpaper.txt").read_text().split("\n")[0].strip()
            if last.lower().endswith((".mp4", ".webm", ".gif", ".mkv", ".mov")):
                poster = home / ".cache/megumin-os/wp_poster.jpg"
                path = str(poster) if poster.is_file() else None
            elif last and os.path.isfile(last):
                path = last
        except Exception:
            pass
        if not path:
            try:
                path = json.load(open(home / ".cache/wal/colors.json")).get("wallpaper")
            except Exception:
                path = None
        img = QImage(path) if path and os.path.isfile(path) else QImage()
        self.wall = QPixmap.fromImage(img.scaled(640, 360, Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                                                 Qt.TransformationMode.SmoothTransformation)) if not img.isNull() else QPixmap()

    def _ease(self, key, target, k=0.18):
        v = self.cur.get(key, target)
        v += (target - v) * k
        if abs(target - v) < 0.002: v = target
        self.cur[key] = v
        return v

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        t = time.monotonic() - self.t0
        W, H = self.width(), self.height()
        a = accent()
        m = self.mode

        # ---------- «екран» ----------
        sw_ = min(W - 40, 340)
        sh = sw_ * 9 / 16
        scr = QRectF((W - sw_) / 2, 10, sw_, sh)
        clip = QPainterPath(); clip.addRoundedRect(scr, 10, 10)
        p.save(); p.setClipPath(clip)
        if self.wall and not self.wall.isNull():
            src = QRectF(self.wall.rect())
            sc = max(scr.width() / src.width(), scr.height() / src.height())
            tw, th = scr.width() / sc, scr.height() / sc
            p.drawPixmap(scr, self.wall, QRectF((src.width() - tw) / 2, (src.height() - th) / 2, tw, th))
        else:
            gr = QLinearGradient(scr.topLeft(), scr.bottomRight())
            gr.setColorAt(0, QColor("#2b5876")); gr.setColorAt(1, QColor("#4e4376"))
            p.fillRect(scr, QBrush(gr))

        # ---------- розміри доку (у масштабі міні-екрана) ----------
        k = scr.width() / 1920 * 2.3                      # іконки в прев'ю трохи більші за реальні, щоб читалось
        isz = self._ease("isz", self.g("dock_icon_size") * k)
        gap = isz * 0.28
        pad = isz * 0.32
        n_apps = len(self.APPS)
        rec_on = self._ease("rec", 1.0 if self.g("dock_recent") else 0.0)
        rec_n = min(int(self.g("dock_recent_count") or 1), 4)
        rec_cnt = self._ease("recn", rec_n)
        fil_on = self._ease("fil", 1.0 if self.g("dock_files") else 0.0)
        fil_cnt = self._ease("filn", min(int(self.g("dock_files_count") or 1), 3))
        sec_rec = (gap * 2 + 1.5 + rec_cnt * (isz + gap)) * rec_on
        sec_fil = (gap * 2 + 1.5 + fil_cnt * (isz + gap)) * fil_on
        dw = pad * 2 + n_apps * isz + (n_apps - 1) * gap + sec_rec + sec_fil
        dh = isz + pad * 1.6
        reserve = self._ease("res", 1.0 if self.g("dock_reserve") else 0.0)
        dx = scr.center().x() - dw / 2
        dy = scr.bottom() - dh - 6
        dock = QRectF(dx, dy, dw, dh)

        # ---------- вікно програми: над доком або під ним ----------
        win_bottom = scr.bottom() - 10 - (scr.bottom() - dy + 4) * reserve
        win = QRectF(scr.left() + scr.width() * 0.16, scr.top() + 14, scr.width() * 0.68, win_bottom - scr.top() - 14)
        p.setPen(QPen(QColor(255, 255, 255, 40), 1))
        p.setBrush(QColor(28, 28, 30, 225))
        p.drawRoundedRect(win, 7, 7)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(255, 255, 255, 18))
        p.drawRoundedRect(QRectF(win.left(), win.top(), win.width(), 14), 7, 7)
        for i, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
            p.setBrush(QColor(c)); p.drawEllipse(QPointF(win.left() + 9 + i * 9, win.top() + 7), 2.6, 2.6)
        p.setBrush(QColor(255, 255, 255, 22))
        for i in range(4):
            p.drawRoundedRect(QRectF(win.left() + 14, win.top() + 24 + i * 11, win.width() * (0.7 - i * 0.12), 5), 2.5, 2.5)

        # ---------- підкладка й рамка ----------
        op = self._ease("op", self.g("dock_opacity") / 100)
        p.setBrush(QColor(40, 40, 44, int(225 * op)))
        bo = self.g("dock_border")
        bop = self._ease("bop", (self.g("dock_border_opacity") / 100) if bo else 0)
        if self.g("dock_border_style") == "accent":
            pen = QPen(tint(a, int(255 * bop)), 2)
        else:
            pen = QPen(QColor(255, 255, 255, int(70 * bop)), 1)
        p.setPen(pen if bop > 0.01 else Qt.PenStyle.NoPen)
        p.drawRoundedRect(dock, dh * 0.32, dh * 0.32)

        # ---------- іконки ----------
        cx_mag = None
        magnify = self.g("dock_magnify")
        mag = (self.g("dock_magnify_pct") or 160) / 100 if magnify else 1.0
        if m == "magnify":   # курсор їздить по доку
            u = (math.sin(t * 1.6) + 1) / 2
            cx_mag = dock.left() + pad + u * (dw - pad * 2)

        def icon(x, color, alpha=255, dot=False, shape="app"):
            s = isz
            scale = 1.0
            if cx_mag is not None and mag > 1:
                d = abs(x + s / 2 - cx_mag) / (s * 1.6)
                scale = 1 + (mag - 1) * math.exp(-d * d)
            s2 = s * scale
            r = QRectF(x + (s - s2) / 2, dock.bottom() - pad * 0.8 - s2, s2, s2)
            c = QColor(color); c.setAlpha(alpha)
            if shape == "file":
                path = QPainterPath()
                path.moveTo(r.left() + s2 * 0.18, r.top()); path.lineTo(r.right() - s2 * 0.38, r.top())
                path.lineTo(r.right() - s2 * 0.12, r.top() + s2 * 0.26); path.lineTo(r.right() - s2 * 0.12, r.bottom())
                path.lineTo(r.left() + s2 * 0.18, r.bottom()); path.closeSubpath()
                p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor(245, 245, 247, alpha)); p.drawPath(path)
                p.setBrush(c)
                for j in range(3):
                    p.drawRoundedRect(QRectF(r.left() + s2 * 0.3, r.top() + s2 * (0.42 + j * 0.15), s2 * 0.42, s2 * 0.07), 1, 1)
            else:
                g2 = QLinearGradient(r.topLeft(), r.bottomLeft())
                g2.setColorAt(0, c.lighter(125)); g2.setColorAt(1, c)
                p.setPen(Qt.PenStyle.NoPen); p.setBrush(QBrush(g2))
                p.drawRoundedRect(r, s2 * 0.24, s2 * 0.24)
            if dot:
                p.setBrush(QColor(255, 255, 255, int(alpha * 0.85)))
                p.drawEllipse(QPointF(r.center().x(), dock.bottom() - pad * 0.38), 1.4, 1.4)
            return r

        x = dock.left() + pad
        app_rects = []
        for i, c in enumerate(self.APPS):
            app_rects.append(icon(x, c, dot=True))
            x += isz + gap
        x -= gap

        def separator(x, alpha):
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(255, 255, 255, int(70 * alpha)))
            p.drawRoundedRect(QRectF(x + gap, dock.top() + pad * 0.6, 1.5, dh - pad * 1.2), 0.75, 0.75)
            return x + gap * 2 + 1.5

        rec_rect = QRectF(x, dock.top(), sec_rec, dh)
        if rec_on > 0.02:
            p.save(); p.setClipRect(QRectF(x, dock.top() - isz, sec_rec, dh + isz * 2))
            xr = separator(x, rec_on)
            for i in range(int(math.ceil(rec_cnt))):
                icon(xr, self.RECENT[i % len(self.RECENT)], alpha=int(150 * rec_on))
                xr += isz + gap
            p.restore()
        x += sec_rec
        fil_rect = QRectF(x, dock.top(), sec_fil, dh)
        if fil_on > 0.02:
            p.save(); p.setClipRect(QRectF(x, dock.top() - isz, sec_fil, dh + isz * 2))
            xf = separator(x, fil_on)
            for i in range(int(math.ceil(fil_cnt))):
                icon(xf, ["#0a84ff", "#ff9f0a", "#30d158"][i % 3], alpha=int(255 * fil_on), shape="file")
                xf += isz + gap
            p.restore()

        # курсор у режимі збільшення
        if cx_mag is not None:
            self._cursor(p, QPointF(cx_mag, dock.bottom() - pad * 0.6))

        # ---------- підсвітка того, на що впливає рядок ----------
        pulse = 0.55 + 0.45 * math.sin(t * 4)
        hl = None
        ghost = None
        if m in ("size", "magnify", "bg", "border"):
            hl = dock
        elif m == "reserve":
            hl = win
        elif m == "recent":
            if self.g("dock_recent"): hl = rec_rect
            else: ghost = QRectF(rec_rect.left() - (gap * 2 + rec_n * (isz + gap)) / 2, dock.top() - dh - 8, gap * 2 + rec_n * (isz + gap), dh)
        elif m == "files":
            if self.g("dock_files"): hl = fil_rect
            else:
                gw_ = gap * 2 + min(int(self.g("dock_files_count") or 1), 3) * (isz + gap)
                ghost = QRectF(dock.right() - gw_ / 2 - pad, dock.top() - dh - 8, gw_, dh)
        elif m in ("drag", "stash"):
            hl = app_rects[1].adjusted(-3, -3, 3, 3)
            # анімація: мініатюра вікна «падає» в іконку
            u = (t * 0.7) % 1.0
            tr = QRectF(win.center().x() - 30 + (app_rects[1].center().x() - win.center().x() + 30) * u,
                        win.center().y() - 18 + (app_rects[1].center().y() - win.center().y() + 18) * u,
                        60 * (1 - u * 0.85), 36 * (1 - u * 0.85))
            p.setPen(QPen(QColor(255, 255, 255, int(160 * (1 - u))), 1))
            p.setBrush(QColor(60, 60, 64, int(220 * (1 - u))))
            p.drawRoundedRect(tr, 4, 4)
        if hl is not None and hl.width() > 1:
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(tint(a, int(255 * pulse)), 2))
            p.drawRoundedRect(hl.adjusted(-3, -3, 3, 3), 9, 9)
        if ghost is not None:
            gh = QRectF(ghost)
            if gh.right() > scr.right() - 6: gh.moveRight(scr.right() - 6)
            # пунктирна стрілка: звідси секцію прибрано
            p.setPen(QPen(QColor(255, 255, 255, int(120 * pulse)), 1, Qt.PenStyle.DotLine))
            p.drawLine(QPointF(gh.center().x(), gh.bottom()), QPointF(gh.center().x(), dock.top() + 2))
            pen = QPen(QColor(255, 255, 255, int(150 * pulse)), 1.5, Qt.PenStyle.DashLine)
            p.setPen(pen); p.setBrush(QColor(255, 255, 255, 18))
            p.drawRoundedRect(gh, 8, 8)
            # що саме прибрано — напівпрозорі іконки секції
            cols = self.RECENT if m == "recent" else ["#f5f5f7"] * 3
            nn = max(1, int((gh.width() - gap * 2) // (isz + gap)))
            p.setPen(Qt.PenStyle.NoPen)
            for j in range(nn):
                p.setBrush(tint(QColor(cols[j % len(cols)]), 90))
                p.drawRoundedRect(QRectF(gh.left() + gap + j * (isz + gap), gh.center().y() - isz / 2, isz, isz), isz * 0.24, isz * 0.24)
            p.setPen(QPen(QColor(255, 90, 90, 220), 2))
            p.drawLine(gh.topLeft() + QPointF(6, 6), gh.bottomRight() - QPointF(6, 6))
        p.restore()

        # рамка «екрана»
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(T["card_border"], 1))
        p.drawRoundedRect(scr, 10, 10)

        # ---------- підпис ----------
        txt = self.MODES.get(m, "Наведіть на налаштування — тут видно, що воно змінює")
        if m == "recent" and not self.g("dock_recent"): txt = "Вимкнено: закриті програми зникають із доку"
        if m == "files" and not self.g("dock_files"): txt = "Вимкнено: секції недавніх файлів у доку немає"
        if m == "magnify" and not magnify: txt = "Збільшення вимкнено — іконки не ростуть"
        if m == "border" and not bo: txt = "Рамку вимкнено — доку без контуру"
        if m == "reserve": txt = ("Док завжди видно, вікна закінчуються над ним" if self.g("dock_reserve")
                                  else "Док ховається і виїжджає поверх вікон")
        f = QFont(FONT_UI); f.setPixelSize(12)
        p.setFont(f)
        p.setPen(QColor(T["text2"]))
        p.drawText(QRectF(0, scr.bottom() + 6, W, H - scr.bottom() - 6), Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop, txt)

    @staticmethod
    def _cursor(p, pt):
        path = QPainterPath()
        path.moveTo(pt); path.lineTo(pt + QPointF(0, 12)); path.lineTo(pt + QPointF(3.2, 9))
        path.lineTo(pt + QPointF(5.6, 13.6)); path.lineTo(pt + QPointF(7.4, 12.8)); path.lineTo(pt + QPointF(5, 8.4))
        path.lineTo(pt + QPointF(9, 8.4)); path.closeSubpath()
        p.setPen(QPen(QColor(255, 255, 255), 1)); p.setBrush(QColor(0, 0, 0))
        p.drawPath(path)


class OsdPreview(QWidget):
    """Міні-екран: де з'являться плашки OSD (гучність/яскравість) і сповіщення. Наведення — підсвічує плашку."""
    def __init__(self, settings, parent=None):
        super().__init__(parent)
        self.s = settings
        self.mode = None
        self.t0 = time.monotonic()
        self.cur = {}
        self.setFixedHeight(220)
        self.timer = QTimer(self, interval=16)
        self.timer.timeout.connect(self.update)
        self.wall = None

    def showEvent(self, e):
        self.timer.start()
        if self.wall is None:
            dp = DockPreview(self.s); dp._load_wall(); self.wall = dp.wall; dp.deleteLater()
        super().showEvent(e)

    def hideEvent(self, e): self.timer.stop(); super().hideEvent(e)

    def set_mode(self, m):
        if m != self.mode: self.mode = m; self.t0 = time.monotonic()

    def _ease(self, k, v):
        c = self.cur.get(k, v); c += (v - c) * 0.18
        if abs(v - c) < 0.05: c = v
        self.cur[k] = c; return c

    @staticmethod
    def _pos(pos, scr, w, h):
        cx = scr.center().x() - w / 2
        top, bottom = scr.top() + 18, scr.bottom() - h - scr.height() * 0.13
        right = scr.right() - w - 8
        return {"top": (cx, top), "center": (cx, scr.center().y() - h / 2), "top-right": (right, top),
                "bottom-right": (right, scr.bottom() - h - 8)}.get(pos, (cx, bottom))

    def paintEvent(self, e):
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        W, H = self.width(), self.height()
        t = time.monotonic() - self.t0
        sw_ = min(W - 40, 300); sh = sw_ * 9 / 16
        scr = QRectF((W - sw_) / 2, 10, sw_, sh)
        clip = QPainterPath(); clip.addRoundedRect(scr, 10, 10)
        p.save(); p.setClipPath(clip)
        if self.wall and not self.wall.isNull():
            src = QRectF(self.wall.rect()); sc = max(scr.width() / src.width(), scr.height() / src.height())
            tw, th = scr.width() / sc, scr.height() / sc
            p.drawPixmap(scr, self.wall, QRectF((src.width() - tw) / 2, (src.height() - th) / 2, tw, th))
        else:
            p.fillRect(scr, QColor("#3a3f58"))
        # топбар і док
        p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor(20, 20, 22, 150))
        p.drawRect(QRectF(scr.left(), scr.top(), scr.width(), 9))
        p.drawRoundedRect(QRectF(scr.center().x() - 60, scr.bottom() - 18, 120, 14), 5, 5)
        pulse = 0.55 + 0.45 * math.sin(t * 4)
        a = accent()
        items = [("osd", self.s.get("osd_position", "bottom"), 70, 16), ("notify", self.s.get("notify_position", "bottom"), 92, 22)]
        same = items[0][1] == items[1][1]
        for i, (kind, pos, w, h) in enumerate(items):
            x, y = self._pos(pos, scr, w, h)
            if same and kind == "notify":   # однакова позиція — на екрані вони підуть одна над одною
                y += -(h + 4) if pos not in ("top", "top-right") else (items[0][3] + 4)
            x = self._ease(kind + "x", x); y = self._ease(kind + "y", y)
            r = QRectF(x, y, w, h)
            dim = self.mode is not None and self.mode != kind
            p.setBrush(QColor(28, 28, 30, 120 if dim else 225))
            p.setPen(QPen(QColor(255, 255, 255, 50), 1))
            p.drawRoundedRect(r, h / 2 if kind == "osd" else 6, h / 2 if kind == "osd" else 6)
            p.setPen(Qt.PenStyle.NoPen)
            if kind == "osd":   # іконка гучності + смужка
                p.setBrush(QColor(255, 255, 255, 120 if dim else 230))
                p.drawEllipse(QPointF(r.left() + 9, r.center().y()), 3, 3)
                p.setBrush(QColor(255, 255, 255, 60)); p.drawRoundedRect(QRectF(r.left() + 17, r.center().y() - 2, w - 26, 4), 2, 2)
                p.setBrush(QColor(255, 255, 255, 120 if dim else 230)); p.drawRoundedRect(QRectF(r.left() + 17, r.center().y() - 2, (w - 26) * 0.6, 4), 2, 2)
            else:               # іконка програми + два рядки тексту
                p.setBrush(QColor("#0a84ff") if not dim else QColor(10, 132, 255, 110))
                p.drawRoundedRect(QRectF(r.left() + 4, r.top() + 4, h - 8, h - 8), 3, 3)
                p.setBrush(QColor(255, 255, 255, 110 if dim else 220)); p.drawRoundedRect(QRectF(r.left() + h + 1, r.top() + 6, w * 0.35, 3), 1.5, 1.5)
                p.setBrush(QColor(255, 255, 255, 60 if dim else 130)); p.drawRoundedRect(QRectF(r.left() + h + 1, r.top() + 12, w * 0.55, 3), 1.5, 1.5)
            if self.mode == kind:
                p.setBrush(Qt.BrushStyle.NoBrush); p.setPen(QPen(tint(a, int(255 * pulse)), 2))
                p.drawRoundedRect(r.adjusted(-3, -3, 3, 3), h / 2 + 3 if kind == "osd" else 8, h / 2 + 3 if kind == "osd" else 8)
        p.restore()
        p.setBrush(Qt.BrushStyle.NoBrush); p.setPen(QPen(T["card_border"], 1)); p.drawRoundedRect(scr, 10, 10)
        txt = {"osd": "Гучність, яскравість, підсвітка клавіатури і музика",
               "notify": "Сповіщення: іконка програми або сайту, від кого і текст"}.get(self.mode, "Наведіть на налаштування — підсвітиться плашка")
        f = QFont(FONT_UI); f.setPixelSize(12); p.setFont(f); p.setPen(QColor(T["text2"]))
        p.drawText(QRectF(0, scr.bottom() + 6, W, H - scr.bottom() - 6), Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop, txt)


class SettingRow(QWidget):
    def __init__(self, icon_name, fallback_emoji, title, description="", control=None, is_last=False, on_click=None,
                 preview=None, preview_mode=None):
        super().__init__()
        self.icon_name, self.on_click_callback, self.control = icon_name, on_click, control
        self.title, self.description = title, description
        self.preview, self.preview_mode = preview, preview_mode
        self._hover = False
        self.setMinimumHeight(52 if description else 44)
        if on_click or isinstance(control, MeguminSwitch):
            self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        lay = QHBoxLayout(self)
        lay.setContentsMargins(14, 7, 14, 7)
        lay.setSpacing(12)
        tl = QVBoxLayout()
        tl.setSpacing(1)
        tl.setAlignment(Qt.AlignmentFlag.AlignVCenter)
        self.title_lbl = QLabel(title)
        self.title_lbl.setProperty("role", "row")
        tl.addWidget(self.title_lbl)
        if description:
            self.desc_lbl = QLabel(description)
            self.desc_lbl.setProperty("role", "desc")
            self.desc_lbl.setWordWrap(True)
            tl.addWidget(self.desc_lbl)
        lay.addLayout(tl, 1)
        if control is not None:
            lay.addWidget(control, 0, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight)
        elif on_click:
            ch = QLabel(G["chev"]); ch.setProperty("role", "chev")
            lay.addWidget(ch)

    def update_theme(self, is_dark):
        if self.control is not None and hasattr(self.control, "update_styles"):
            self.control.update_styles()
        self.update()

    def enterEvent(self, e):
        self._hover = True
        if self.preview and self.preview_mode: self.preview.set_mode(self.preview_mode)
        self.update()

    def leaveEvent(self, e):
        self._hover = False
        self.update()

    def paintEvent(self, e):
        if self._hover and self.on_click_callback:
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.Antialiasing)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(T["hover"])
            p.drawRoundedRect(QRectF(self.rect()).adjusted(4, 2, -4, -2), 8, 8)

    def mousePressEvent(self, event):
        # Клікабельний рядок мусить «забрати» натискання: інакше воно доходить до вікна,
        # яке починає startSystemMove — композитор захоплює курсор, і release (а з ним
        # і дія рядка) вже не приходить. Через це не працювали всі рядки-посилання.
        if self.on_click_callback or isinstance(self.control, MeguminSwitch):
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self.rect().contains(event.position().toPoint()):
            if self.on_click_callback:
                self.on_click_callback()
            elif isinstance(self.control, MeguminSwitch):
                self.control.click()
        super().mouseReleaseEvent(event)


class PixelColorPicker(QWidget):
    """Колір піксельних шпалер: перший кружечок — акцент системи, далі — свої кольори."""
    COLORS = ["#0a84ff", "#5e5ce6", "#a550a7", "#f74f9e", "#ff5257", "#f7821b", "#ffc600", "#62ba46", "#30b0c7"]

    def __init__(self, current, on_pick):
        super().__init__()
        self.current, self.on_pick = (current or "").lower(), on_pick
        self.setFixedSize(26 * (len(self.COLORS) + 1) + 4, 30)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.setToolTip("Перший — як акцент системи")

    def update_styles(self): self.update()

    def mousePressEvent(self, e):
        i = int(e.position().x() // 26)
        if 0 <= i <= len(self.COLORS):
            self.current = "" if i == 0 else self.COLORS[i - 1].lower()
            self.update(); self.on_pick(self.current)

    def paintEvent(self, e):
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        for i in range(len(self.COLORS) + 1):
            c = QPointF(13 + i * 26, 15)
            if i == 0:
                g = QLinearGradient(c - QPointF(8, 8), c + QPointF(8, 8))
                g.setColorAt(0, QColor("#ff5f9e")); g.setColorAt(0.5, QColor("#a56bff")); g.setColorAt(1, QColor("#3fb6ff"))
                p.setBrush(QBrush(g)); sel = self.current == ""
            else:
                p.setBrush(QColor(self.COLORS[i - 1])); sel = self.current == self.COLORS[i - 1].lower()
            p.setPen(QPen(QColor(0, 0, 0, 40), 0.8)); p.drawEllipse(c, 9, 9)
            if sel:
                p.setBrush(Qt.BrushStyle.NoBrush); p.setPen(QPen(QColor(T["text"]), 2)); p.drawEllipse(c, 12, 12)


class PixelPreviewCard(QAbstractButton):
    """Мініатюра піксельних шпалер (кадр, намальований самим pixel_wallpaper) + назва."""
    def __init__(self, sid, name, on_click):
        super().__init__()
        self.sid, self.name, self.pix, self.active = sid, name, None, False
        self.setFixedSize(200, 140)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.clicked.connect(lambda: on_click(sid))

    def set_image(self, path):
        pm = QPixmap(path)
        self.pix = pm if not pm.isNull() else None
        self.update()

    def paintEvent(self, e):
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(2, 2, self.width() - 4, 112)
        path = QPainterPath(); path.addRoundedRect(r, 12, 12)
        p.setClipPath(path)
        if self.pix: p.drawPixmap(r.toRect(), self.pix)
        else: p.fillRect(r, QColor(T["ctrl"]))
        p.setClipping(False)
        p.setPen(QPen(QColor(ACCENT[0]), 3) if self.active else QPen(QColor(0, 0, 0, 50), 1))
        p.setBrush(Qt.BrushStyle.NoBrush); p.drawRoundedRect(r, 12, 12)
        p.setPen(QColor(T["text"])); f = QFont(FONT_UI); f.setPixelSize(12); f.setBold(self.active); p.setFont(f)
        p.drawText(QRectF(0, 118, self.width(), 20), Qt.AlignmentFlag.AlignCenter, ("✓ " if self.active else "") + self.name)


class PixelPreviewSignals(QObject):
    ready = pyqtSignal(str, str)   # sid, path


class GroupCard(QWidget):
    """Згрупована «inset»-картка macOS: заокруглений фон, тонкі роздільники між рядками."""
    def __init__(self, rows):
        super().__init__()
        self.rows = rows
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 2, 0, 2)
        lay.setSpacing(0)
        for r in rows:
            lay.addWidget(r)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(QPen(T["card_border"], 1))
        p.setBrush(T["card"])
        p.drawRoundedRect(QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12)
        p.setPen(QPen(T["sep"], 1))
        vis = [r for r in self.rows if r.isVisible()]
        for r in vis[:-1]:
            y = r.geometry().bottom() + 0.5
            p.drawLine(QPointF(14, y), QPointF(self.width() - 14, y))


class HeroCard(QWidget):
    """Шапка панелі: велика кольорова іконка, назва, опис (як у Wi-Fi / Bluetooth у macOS)."""
    def __init__(self, glyph, color, title, desc, control=None):
        super().__init__()
        self.glyph, self.color = glyph, color
        lay = QHBoxLayout(self)
        lay.setContentsMargins(84, 14, 16, 14)
        tl = QVBoxLayout(); tl.setSpacing(3)
        t = QLabel(title); t.setProperty("role", "hero")
        d = QLabel(desc); d.setProperty("role", "herodesc"); d.setWordWrap(True)
        tl.addWidget(t); tl.addWidget(d)
        lay.addLayout(tl, 1)
        if control is not None:
            lay.addWidget(control, 0, Qt.AlignmentFlag.AlignVCenter)
        self.setMinimumHeight(86)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(QPen(T["card_border"], 1))
        p.setBrush(T["card"])
        p.drawRoundedRect(QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12)
        s = 54
        draw_squircle_icon(p, QRectF(16, (self.height() - s) / 2, s, s), self.glyph, self.color)


class SidebarItem(QWidget):
    clicked = pyqtSignal(int)

    def __init__(self, index, glyph, color, text):
        super().__init__()
        self.index, self.glyph, self.color, self.text = index, glyph, color, text
        self.selected = False
        self._hover = False
        self.setFixedHeight(32)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))

    def enterEvent(self, e): self._hover = True; self.update()
    def leaveEvent(self, e): self._hover = False; self.update()

    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton: self.clicked.emit(self.index)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(8, 1, -8, -1)
        if self.selected:
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(accent())
            p.drawRoundedRect(r, 9, 9)
        elif self._hover:
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(T["hover"])
            p.drawRoundedRect(r, 9, 9)
        draw_squircle_icon(p, QRectF(r.left() + 7, r.top() + 4, 22, 22), self.glyph, self.color, 13)
        f = QFont(FONT_UI); f.setPixelSize(13)
        p.setFont(f)
        p.setPen(QColor("white") if self.selected else QColor(T["text"]))
        p.drawText(QRectF(r.left() + 38, r.top(), r.width() - 40, r.height()),
                   Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, self.text)


class TrafficLights(QWidget):
    """Кнопки вікна macOS: закрити / згорнути / на весь екран; символи з'являються при наведенні."""
    def __init__(self, win):
        super().__init__()
        self.win = win
        self.setFixedSize(16, 16)
        self._hover = False
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))

    def enterEvent(self, e): self._hover = True; self.update()
    def leaveEvent(self, e): self._hover = False; self.update()

    def mousePressEvent(self, e):
        self.win.close()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        for i, (c, sym) in enumerate((("#ff5f57", "×"),)):
            ctr = QPointF(7 + i * 20, 8)
            p.setPen(QPen(QColor(0, 0, 0, 40), 0.6))
            p.setBrush(QColor(c))
            p.drawEllipse(ctr, 6.5, 6.5)
            if self._hover:
                f = QFont(FONT_UI); f.setPixelSize(11); f.setBold(True); p.setFont(f)
                p.setPen(QColor(0, 0, 0, 150))
                p.drawText(QRectF(ctr.x() - 7, ctr.y() - 8, 14, 15), Qt.AlignmentFlag.AlignCenter, sym)


class NavButton(QAbstractButton):
    def __init__(self, glyph, cb):
        super().__init__()
        self.glyph = glyph
        self.setFixedSize(30, 28)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.clicked.connect(cb)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self.underMouse() and self.isEnabled():
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(T["hover"]); p.drawRoundedRect(QRectF(self.rect()), 8, 8)
        f = QFont(ICON_FONT); f.setPixelSize(18); p.setFont(f)
        p.setPen(QColor(T["text"]) if self.isEnabled() else tint(T["text2"], 110))
        p.drawText(QRectF(self.rect()), Qt.AlignmentFlag.AlignCenter, self.glyph)


class AccountCard(QWidget):
    def __init__(self, on_click):
        super().__init__()
        self.on_click = on_click
        self.setFixedHeight(54)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        try:
            pw = pwd.getpwuid(os.getuid())
            self.name = (pw.pw_gecos.split(",")[0] or pw.pw_name).strip() or pw.pw_name
        except Exception:
            self.name = os.environ.get("USER", "user")
        self.reload()

    def reload(self):
        self.pix = QPixmap(str(Path.home() / ".face.icon"))
        self.update()

    def mousePressEvent(self, e): self.on_click()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        r = QRectF(14, 9, 36, 36)
        path = QPainterPath(); path.addEllipse(r)
        if not self.pix.isNull():
            p.save(); p.setClipPath(path)
            p.drawPixmap(r.toRect(), self.pix.scaled(72, 72, Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                                                     Qt.TransformationMode.SmoothTransformation))
            p.restore()
        else:
            g = QLinearGradient(r.topLeft(), r.bottomLeft())
            g.setColorAt(0, QColor("#a1a1a6")); g.setColorAt(1, QColor("#6e6e73"))
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(QBrush(g)); p.drawEllipse(r)
            f = QFont(FONT_UI); f.setPixelSize(16); f.setBold(True); p.setFont(f); p.setPen(QColor("white"))
            p.drawText(r, Qt.AlignmentFlag.AlignCenter, self.name[:1].upper())
        f = QFont(FONT_UI); f.setPixelSize(13); f.setWeight(QFont.Weight.DemiBold); p.setFont(f)
        p.setPen(QColor(T["text"]))
        p.drawText(QRectF(60, 9, self.width() - 64, 19), Qt.AlignmentFlag.AlignVCenter, self.name)
        f.setPixelSize(11); f.setWeight(QFont.Weight.Normal); p.setFont(f)
        p.setPen(QColor(T["text2"]))
        p.drawText(QRectF(60, 27, self.width() - 64, 17), Qt.AlignmentFlag.AlignVCenter, "Обліковий запис mgOS")


class FadeStackedWidget(QStackedWidget):
    def setCurrentIndex(self, index):
        if index == self.currentIndex(): return
        super().setCurrentIndex(index)
        widget = self.currentWidget()
        effect = QGraphicsOpacityEffect(widget)
        widget.setGraphicsEffect(effect)
        self.anim = QPropertyAnimation(effect, b"opacity")
        self.anim.setDuration(180)
        self.anim.setStartValue(0.0)
        self.anim.setEndValue(1.0)
        self.anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.anim.finished.connect(lambda: widget.setGraphicsEffect(None))
        self.anim.start()


# ==========================================
# 3. МІНІАТЮРИ ШПАЛЕР (lazy load)
# ==========================================

class WorkerSignals(QObject):
    thumb_ready = pyqtSignal(int, str) # row_index, thumb_path

WIDE_W, WIDE_H = 400, 250   # мініатюри шпалер 16:10 (обрізані по центру, без сплющення)


def thumb_name(path, wide):
    return hashlib.md5((path + ("|wide" if wide else "")).encode()).hexdigest() + ".png"


class ThumbnailWorker(QRunnable):
    def __init__(self, index, original_path, wide=False):
        super().__init__()
        self.index = index
        self.original_path = original_path
        self.wide = wide
        self.signals = WorkerSignals()
        self.cache_dir = os.path.expanduser("~/.cache/ignis/wallpaper_thumbs")
        os.makedirs(self.cache_dir, exist_ok=True)

    def run(self):
        thumb_path = os.path.join(self.cache_dir, thumb_name(self.original_path, self.wide))
        if self.wide:
            self.run_wide(thumb_path)
        else:
            self.run_square(thumb_path)
        self.signals.thumb_ready.emit(self.index, thumb_path)

    def _source(self):
        source_path = self.original_path
        if os.path.isdir(self.original_path):
            h = time.localtime().tm_hour
            per = "morning" if 6 <= h < 12 else "day" if 12 <= h < 18 else "evening" if 18 <= h < 22 else "night"
            for name in (per, "day"):
                for ext in ['jpg', 'png', 'jpeg', 'webp', 'mp4', 'webm', 'gif']:
                    p = os.path.join(self.original_path, f"{name}.{ext}")
                    if os.path.exists(p):
                        return p
        return source_path

    def run_wide(self, thumb_path):
        if os.path.exists(thumb_path):
            return
        src = self._source()
        if src.lower().split('.')[-1] in ('mp4', 'webm', 'gif', 'mkv', 'mov'):
            vf = f"scale={WIDE_W}:{WIDE_H}:force_original_aspect_ratio=increase,crop={WIDE_W}:{WIDE_H}"
            subprocess.run(["ffmpeg", "-loglevel", "error", "-ss", "1", "-i", src, "-frames:v", "1", "-vf", vf, thumb_path, "-y"],
                           stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
            if not os.path.exists(thumb_path):   # ролик коротший за 1 с
                subprocess.run(["ffmpeg", "-loglevel", "error", "-i", src, "-frames:v", "1", "-vf", vf, thumb_path, "-y"],
                               stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
            return
        img = QImage(src)
        if img.isNull():
            return
        img = img.scaled(WIDE_W, WIDE_H, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation)
        img.copy((img.width() - WIDE_W) // 2, (img.height() - WIDE_H) // 2, WIDE_W, WIDE_H).save(thumb_path, "PNG")

    def run_square(self, thumb_path):
        if not os.path.exists(thumb_path):
            source_path = self.original_path
            if os.path.isdir(self.original_path):
                for ext in ['jpg', 'png', 'jpeg', 'webp', 'mp4', 'gif']:
                    p = os.path.join(self.original_path, f"day.{ext}")
                    if os.path.exists(p):
                        source_path = p
                        break
            
            ext = source_path.lower().split('.')[-1]
            if ext in ['mp4', 'webm', 'gif']:
                subprocess.run(f"ffmpeg -i '{source_path}' -ss 00:00:00.000 -vframes 1 -s 300x300 '{thumb_path}' -y", shell=True, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
            else:
                # QImage безпечний для фонових потоків, на відміну від QPixmap
                img = QImage(source_path)
                if not img.isNull():
                    img.scaled(300, 300, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation).save(thumb_path, "PNG")


class WallpaperModel(QAbstractListModel):
    wide = False

    def __init__(self, items):
        super().__init__()
        self.items = items
        self.thumbnails = {}
        self.loading = set()
        self.pool = QThreadPool.globalInstance()
        self.fallback_icon = QIcon.fromTheme("image-loading")

    def rowCount(self, parent=QModelIndex()):
        return len(self.items)

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid(): return None
        row = index.row()
        path = self.items[row]

        if role == Qt.ItemDataRole.DisplayRole:
            return os.path.basename(path)
            
        elif role == Qt.ItemDataRole.DecorationRole:
            if row in self.thumbnails:
                return self.thumbnails[row]
            
            if row not in self.loading:
                self.loading.add(row)
                worker = ThumbnailWorker(row, path, self.wide)
                worker.signals.thumb_ready.connect(self.on_thumb_ready)
                self.pool.start(worker)
                
            return self.fallback_icon
        return None

    def on_thumb_ready(self, row, thumb_path):
        # після зміни теки старі фонові завдання ще завершуються — не чіпляємо чужу мініатюру
        if row >= len(self.items) or os.path.basename(thumb_path) != thumb_name(self.items[row], self.wide):
            return
        if os.path.exists(thumb_path):
            self.thumbnails[row] = QIcon(thumb_path)
            idx = self.index(row, 0)
            self.dataChanged.emit(idx, idx, [Qt.ItemDataRole.DecorationRole])

    def update_items(self, new_items):
        self.beginResetModel()
        self.items = new_items
        self.thumbnails.clear()
        self.loading.clear()
        self.endResetModel()


class PhotoModel(WallpaperModel):
    """Вміст теки для вибору фото: спершу теки (з підписом), потім фото (мініатюри, без підпису)."""
    def __init__(self, items):
        super().__init__(items)
        self.folder_icon = QIcon.fromTheme("folder")
        if self.folder_icon.isNull():   # тема іконок без «folder» — малюємо свою (гліф теки у стилі macOS)
            pm = QPixmap(128, 88); pm.fill(Qt.GlobalColor.transparent)
            p = QPainter(pm); p.setRenderHint(QPainter.RenderHint.Antialiasing)
            f = QFont(ICON_FONT); f.setPixelSize(84); p.setFont(f); p.setPen(QColor("#5ac8fa"))
            p.drawText(QRectF(0, 0, 128, 88), Qt.AlignmentFlag.AlignCenter, "\U000F024B")
            p.end()
            self.folder_icon = QIcon(pm)

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid(): return None
        path = self.items[index.row()]
        is_dir = os.path.isdir(path)
        if role == Qt.ItemDataRole.DisplayRole:
            return os.path.basename(path) if is_dir else None
        if role == Qt.ItemDataRole.ToolTipRole:
            return os.path.basename(path)
        if role == Qt.ItemDataRole.DecorationRole and is_dir:
            return self.folder_icon
        return super().data(index, role)


class WallGridModel(WallpaperModel):
    """Шпалери в «Налаштуваннях»: широкі мініатюри, назва — у підказці."""
    wide = True

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid(): return None
        if role == Qt.ItemDataRole.DisplayRole: return None
        if role == Qt.ItemDataRole.ToolTipRole:
            n = os.path.basename(self.items[index.row()])
            return n.rsplit(".", 1)[0] if "." in n else n
        if role == Qt.ItemDataRole.UserRole: return self.items[index.row()]
        if role == Qt.ItemDataRole.DecorationRole and index.row() not in self.thumbnails:
            super().data(index, role)   # запускає фонову генерацію
            return None
        return super().data(index, role)


class WallpaperDelegate(QStyledItemDelegate):
    """Картка шпалер у стилі macOS 26: заокруглена мініатюра, кільце акценту на поточних,
    тонка рамка при наведенні, значок «відео» / «динамічні»."""
    CELL = QSize(198, 132)

    def __init__(self, owner):
        super().__init__(owner)
        self.owner = owner
        self.badge_font = QFont(ICON_FONT); self.badge_font.setPixelSize(13)

    def sizeHint(self, option, index):
        return self.CELL

    def paint(self, p, option, index):
        p.save()
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        path_str = index.data(Qt.ItemDataRole.UserRole) or ""
        r = QRectF(option.rect).adjusted(7, 7, -7, -7)
        hover = bool(option.state & QStyle.StateFlag.State_MouseOver)
        current = path_str == self.owner.current_wallpaper
        clip = QPainterPath(); clip.addRoundedRect(r, 12, 12)
        icon = index.data(Qt.ItemDataRole.DecorationRole)
        if isinstance(icon, QIcon) and not icon.isNull():
            pm = icon.pixmap(QSize(WIDE_W, WIDE_H))
            p.setClipPath(clip)
            src = QRectF(pm.rect())
            # cover: обрізаємо під пропорції картки
            ar, sr = r.width() / r.height(), src.width() / max(1, src.height())
            if sr > ar:
                w = src.height() * ar; src = QRectF(src.x() + (src.width() - w) / 2, src.y(), w, src.height())
            else:
                h = src.width() / ar; src = QRectF(src.x(), src.y() + (src.height() - h) / 2, src.width(), h)
            p.drawPixmap(r, pm, src)
            p.setClipping(False)
        else:
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(T["field"]); p.drawPath(clip)
        # тонкий внутрішній контур, як у macOS
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(QPen(QColor(255, 255, 255, 34) if T["text"] == DARK["text"] else QColor(0, 0, 0, 30), 1))
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 12, 12)
        # значок типу
        ext = path_str.lower().rsplit(".", 1)[-1] if "." in os.path.basename(path_str) else ""
        glyph = "\U000F040A" if ext in ("mp4", "webm", "gif", "mkv", "mov") else ("\U000F0150" if os.path.isdir(path_str) else "")
        if glyph:
            b = QRectF(r.left() + 7, r.top() + 7, 24, 20)
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor(0, 0, 0, 120)); p.drawRoundedRect(b, 10, 10)
            p.setFont(self.badge_font); p.setPen(QColor(255, 255, 255, 235))
            p.drawText(b, Qt.AlignmentFlag.AlignCenter, glyph)
        if current:
            p.setPen(QPen(QColor(ACCENT[0]), 3)); p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(r.adjusted(-3.5, -3.5, 3.5, 3.5), 15, 15)
            c = QRectF(r.right() - 27, r.bottom() - 27, 20, 20)
            p.setPen(QPen(QColor(255, 255, 255), 1.5)); p.setBrush(QColor(ACCENT[0])); p.drawEllipse(c)
            p.setFont(self.badge_font); p.setPen(QColor(255, 255, 255))
            p.drawText(c, Qt.AlignmentFlag.AlignCenter, "\U000F012C")
        elif hover:
            p.setPen(QPen(QColor(T["text2"]), 2)); p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(r.adjusted(-3, -3, 3, 3), 15, 15)
        p.restore()


class AvatarPicker(QDialog):
    """Провідник для вибору фото з будь-якого місця на ПК — з мініатюрами (системний GTK-діалог прев'ю не має)
    і круглим прев'ю аватарки. Теки відкриваються подвійним кліком; не блокує програму."""
    EXT = ('png', 'jpg', 'jpeg', 'webp', 'bmp', 'gif')

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Вибрати фото")
        self.resize(940, 600)
        self.chosen, self.cwd, self.back = "", "", []
        root = QHBoxLayout(self)
        root.setContentsMargins(12, 12, 12, 12)
        root.setSpacing(12)

        side = QVBoxLayout(); side.setSpacing(2)
        t = QLabel("Місця"); t.setProperty("role", "sidegroup"); side.addWidget(t)
        self.places = []
        home = Path.home()
        places = [("Домівка", "\U000F02DC", "#8e8e93", home), ("Зображення", G["image"], "#32ade6", home / "Pictures"),
                  ("Завантаження", "\U000F01DA", "#0a84ff", home / "Downloads"), ("Робочий стіл", G["monitor"], "#8e8e93", home / "Desktop"),
                  ("Документи", "\U000F0219", "#ff9f0a", home / "Documents"), ("Диск /", "\U000F02CA", "#636366", Path("/"))]
        for base in (Path("/run/media") / os.environ.get("USER", ""), Path("/mnt"), Path("/media")):
            try:
                for d in sorted(base.iterdir()):
                    if d.is_dir() and os.access(d, os.R_OK): places.append((d.name, "\U000F02CA", "#30d158", d))
            except Exception:
                pass
        for name, glyph, color, path in places:
            if not path.is_dir(): continue
            it = SidebarItem(len(self.places), glyph, color, name)
            it.clicked.connect(self.open_place)
            self.places.append((it, str(path)))
            side.addWidget(it)
        side.addStretch()
        sw = QWidget(); sw.setLayout(side); sw.setFixedWidth(200)
        root.addWidget(sw)

        mid = QVBoxLayout(); mid.setSpacing(6)
        bar = QHBoxLayout(); bar.setSpacing(2)
        self.back_btn = NavButton(G["left"], self.go_back)
        self.up_btn = NavButton("\U000F005D", self.go_up)   # стрілка вгору
        self.path_lbl = QLabel(""); self.path_lbl.setProperty("role", "h1")
        self.count_lbl = QLabel(""); self.count_lbl.setProperty("role", "desc")
        bar.addWidget(self.back_btn); bar.addWidget(self.up_btn); bar.addSpacing(6)
        bar.addWidget(self.path_lbl, 1); bar.addWidget(self.count_lbl)
        mid.addLayout(bar)
        self.grid = QListView(); self.grid.setObjectName("wp")
        self.grid.setViewMode(QListView.ViewMode.IconMode)
        self.grid.setIconSize(QSize(128, 88)); self.grid.setGridSize(QSize(146, 128)); self.grid.setSpacing(4)
        self.grid.setResizeMode(QListView.ResizeMode.Adjust); self.grid.setUniformItemSizes(True)
        self.grid.setMovement(QListView.Movement.Static); self.grid.setWordWrap(True)
        self.grid.setTextElideMode(Qt.TextElideMode.ElideMiddle)
        self.model = PhotoModel([])
        self.grid.setModel(self.model)
        self.grid.selectionModel().currentChanged.connect(lambda cur, prev: self.select(cur))
        self.grid.doubleClicked.connect(self.activate)
        mid.addWidget(self.grid, 1)
        root.addLayout(mid, 1)

        right = QVBoxLayout(); right.setSpacing(10)
        self.preview = QLabel(); self.preview.setFixedSize(180, 180)
        self.preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        right.addSpacing(20); right.addWidget(self.preview, 0, Qt.AlignmentFlag.AlignHCenter)
        self.name_lbl = QLabel("Оберіть фото"); self.name_lbl.setProperty("role", "desc")
        self.name_lbl.setWordWrap(True); self.name_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        right.addWidget(self.name_lbl)
        right.addStretch()
        btns = QHBoxLayout()
        btns.addWidget(LinkButton("Скасувати", self.reject))
        self.ok_btn = QPushButton("Вибрати")
        self.ok_btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.ok_btn.setStyleSheet(f"QPushButton {{ background: {ACCENT[0]}; color: white; border: none; border-radius: 13px; padding: 5px 16px; font-weight: 600; }}"
                                  "QPushButton:disabled { background: rgba(128,128,128,0.35); color: rgba(255,255,255,0.5); }")
        self.ok_btn.setEnabled(False)
        self.ok_btn.clicked.connect(self.accept_choice)
        btns.addWidget(self.ok_btn)
        right.addLayout(btns)
        rw = QWidget(); rw.setLayout(right); rw.setFixedWidth(210)
        root.addWidget(rw)
        self.draw_preview(None)
        start = home / "Pictures"
        self.open_folder(str(start if start.is_dir() else home), record=False)

    # ---- навігація ----
    def open_place(self, i):
        if 0 <= i < len(self.places): self.open_folder(self.places[i][1])

    def go_up(self):
        parent = os.path.dirname(self.cwd.rstrip("/")) or "/"
        if parent != self.cwd: self.open_folder(parent)

    def go_back(self):
        if self.back: self.open_folder(self.back.pop(), record=False)

    def activate(self, idx):
        if not idx.isValid(): return
        path = self.model.items[idx.row()]
        if os.path.isdir(path): self.open_folder(path)
        else: self.select(idx); self.accept_choice()

    def open_folder(self, path, record=True):
        try:
            names = sorted(os.listdir(path), key=str.lower)
        except Exception:
            self.count_lbl.setText("немає доступу")
            return
        if record and self.cwd and self.cwd != path: self.back.append(self.cwd)
        self.cwd = path
        dirs = [os.path.join(path, n) for n in names if not n.startswith(".") and os.path.isdir(os.path.join(path, n))]
        imgs = [os.path.join(path, n) for n in names if not n.startswith(".") and n.lower().rsplit(".", 1)[-1] in self.EXT
                and os.path.isfile(os.path.join(path, n))]
        self.model.update_items(dirs + imgs)
        self.grid.scrollToTop()
        shown = path.replace(str(Path.home()), "~", 1) if path.startswith(str(Path.home())) else path
        self.path_lbl.setText(shown)
        self.count_lbl.setText(f"{len(imgs)} фото" + (f" · {len(dirs)} тек" if dirs else ""))
        self.back_btn.setEnabled(bool(self.back)); self.up_btn.setEnabled(path != "/")
        self.back_btn.update(); self.up_btn.update()
        for it, p in self.places:
            it.selected = os.path.normpath(p) == os.path.normpath(path); it.update()
        self.chosen = ""; self.ok_btn.setEnabled(False); self.name_lbl.setText("Оберіть фото"); self.draw_preview(None)

    def select(self, idx):
        if not idx.isValid(): return
        path = self.model.items[idx.row()]
        if os.path.isdir(path):
            self.chosen = ""; self.ok_btn.setEnabled(False)
            self.name_lbl.setText(os.path.basename(path) + " — подвійний клік, щоб відкрити"); self.draw_preview(None)
            return
        self.chosen = path
        self.ok_btn.setEnabled(True)
        self.name_lbl.setText(os.path.basename(path))
        self.draw_preview(path)

    def draw_preview(self, path):
        S = 180
        img = QPixmap(S, S); img.fill(Qt.GlobalColor.transparent)
        p = QPainter(img)
        p.setRenderHint(QPainter.RenderHint.Antialiasing); p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        circle = QPainterPath(); circle.addEllipse(QRectF(4, 4, S - 8, S - 8))
        src = QPixmap(path) if path else QPixmap()
        if not src.isNull():
            p.setClipPath(circle)
            sc = src.scaled(S, S, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation)
            p.drawPixmap((S - sc.width()) // 2, (S - sc.height()) // 2, sc)
            p.setClipping(False)
        else:
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(T["ctrl"]); p.drawPath(circle)
            f = QFont(ICON_FONT); f.setPixelSize(64); p.setFont(f); p.setPen(QColor(T["text2"]))
            p.drawText(QRectF(0, 0, S, S), Qt.AlignmentFlag.AlignCenter, G["account"])
        p.setPen(QPen(QColor(255, 255, 255, 150), 3)); p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(QRectF(5, 5, S - 10, S - 10))
        p.end()
        self.preview.setPixmap(img)

    def accept_choice(self):
        if self.chosen: self.accept()


# ==========================================
# 4. ВІКНО ОБРІЗАННЯ ФОТО (CROPPER)
# ==========================================

class ImageCropperDialog(QDialog):
    def __init__(self, image_path, accent_color, is_dark, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Обрізати аватарку")
        self.setFixedSize(540, 600)
        
        bg_col = "#242424" if is_dark else "#F5F5F5"
        text_col = "white" if is_dark else "black"
        self.setStyleSheet(f"QDialog {{ background-color: {bg_col}; color: {text_col}; }}")
        
        layout = QVBoxLayout(self)
        
        lbl = QLabel("Натисни і тягни мишкою, щоб виділити квадрат:")
        lbl.setStyleSheet("font-weight: bold; font-size: 14px;")
        layout.addWidget(lbl)
        
        self.original_pixmap = QPixmap(image_path)
        self.display_pixmap = self.original_pixmap.scaled(500, 500, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
        
        self.image_label = QLabel()
        self.image_label.setPixmap(self.display_pixmap)
        self.image_label.setFixedSize(self.display_pixmap.size())
        
        img_layout = QHBoxLayout()
        img_layout.addStretch()
        img_layout.addWidget(self.image_label)
        img_layout.addStretch()
        layout.addLayout(img_layout)
        
        btn_layout = QHBoxLayout()
        btn_layout.addStretch()
        
        cancel_btn = QPushButton("Скасувати")
        save_btn = QPushButton("Зберегти")
        
        cancel_btn.setStyleSheet(f"QPushButton {{ background-color: rgba(100,100,100,0.3); color: {text_col}; border-radius: 8px; padding: 8px 16px; border: none; font-weight: bold; }}")
        save_btn.setStyleSheet(f"QPushButton {{ background-color: {accent_color}; color: white; border-radius: 8px; padding: 8px 16px; border: none; font-weight: bold; }}")
        cancel_btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        save_btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        
        btn_layout.addWidget(cancel_btn)
        btn_layout.addWidget(save_btn)
        layout.addLayout(btn_layout)
        
        cancel_btn.clicked.connect(self.reject)
        save_btn.clicked.connect(self.accept)
        
        # Логіка виділення
        self.rubberBand = QRubberBand(QRubberBand.Shape.Rectangle, self.image_label)
        s = min(self.display_pixmap.width(), self.display_pixmap.height())
        self.rubberBand.setGeometry((self.display_pixmap.width()-s)//2, (self.display_pixmap.height()-s)//2, s, s)
        self.rubberBand.show()
        
        self.origin = QPoint()
        self.dragging = False
        
        self.image_label.mousePressEvent = self.img_press
        self.image_label.mouseMoveEvent = self.img_move
        self.image_label.mouseReleaseEvent = self.img_release

    def img_press(self, event):
        self.origin = event.pos()
        self.rubberBand.setGeometry(QRect(self.origin, QSize()))
        self.dragging = True

    def img_move(self, event):
        if not self.dragging: return
        diff = event.pos() - self.origin
        side = min(abs(diff.x()), abs(diff.y()))
        x = self.origin.x() if diff.x() > 0 else self.origin.x() - side
        y = self.origin.y() if diff.y() > 0 else self.origin.y() - side
        rect = QRect(x, y, side, side).intersected(self.image_label.rect())
        self.rubberBand.setGeometry(rect)

    def img_release(self, event):
        self.dragging = False

    def get_cropped_image(self):
        rect = self.rubberBand.geometry()
        ratio_x = self.original_pixmap.width() / self.display_pixmap.width()
        ratio_y = self.original_pixmap.height() / self.display_pixmap.height()
        
        orig_rect = QRect(
            int(rect.x() * ratio_x), int(rect.y() * ratio_y),
            int(rect.width() * ratio_x), int(rect.height() * ratio_y)
        )
        return self.original_pixmap.copy(orig_rect).scaled(256, 256, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)


# ==========================================
# 3. ГОЛОВНЕ ВІКНО & ЛОГІКА СИСТЕМИ


# ==========================================
# 5. ГОЛОВНЕ ВІКНО (macOS 26 System Settings)
# ==========================================

# (індекс сторінки, назва для --page, заголовок, гліф, колір іконки, група сайдбару)
PAGES = [
    ("wifi", "Wi-Fi", G["wifi"], "#0a84ff", 0),
    ("bluetooth", "Bluetooth", G["bt"], "#0a84ff", 0),
    ("appearance", "Вигляд", G["palette"], "#5e5ce6", 1),
    ("wallpapers", "Шпалери", G["image"], "#32ade6", 1),
    ("pixelwp", "Піксельні шпалери", "\U000F0D74", "#bf5af2", 1),
    ("login", "Екран входу", "\U000F033E", "#5856d6", 1),
    ("look", "Іконки і курсор", "\U000F01BF", "#bf5af2", 1),
    ("dock", "Док", G["dock"], "#1c1c1e", 1),
    ("panels", "Панелі та віджети", G["panels"], "#ff9f0a", 1),
    ("osd", "OSD і сповіщення", "\U000F0E1E", "#30b0c7", 1),
    ("display", "Екран", G["monitor"], "#0a84ff", 2),
    ("sound", "Звук", G["sound"], "#ff375f", 2),
    ("sounds", "Системні звуки", G["bell"], "#ff2d55", 2),
    ("trackpad", "Трекпад", G["touch"], "#8e8e93", 3),
    ("mouse", "Миша", "\U000F037D", "#8e8e93", 3),
    ("magicmouse", "Magic Mouse", "\U000F037D", "#c7c7cc", 3),
    ("keyboard", "Клавіатура", G["keyboard"], "#8e8e93", 3),
    ("games", "Ігри", "\U000F0297", "#ff453a", 5),
    ("power", "Батарея", G["battery"], "#30d158", 4),
    ("system", "Система", G["cog"], "#8e8e93", 4),
    ("about", "Про систему", G["info"], "#8e8e93", 4),
    ("help", "Довідка", "\U000F02D7", "#0a84ff", 4),
    ("developer", "Розробник", "\U000F0169", "#636366", 4),
]
PAGE_GROUPS = ["Мережа", "Оформлення", "Екран і звук", "Пристрої введення", "Система", "Ігри"]


class SideGroup(QWidget):
    """Блок сайдбару: маленький заголовок і заокруглена «скляна» підкладка під пунктами групи."""
    def __init__(self, title):
        super().__init__()
        v = QVBoxLayout(self); v.setContentsMargins(0, 0, 0, 0); v.setSpacing(3)
        self.header = QLabel(title.upper()); self.header.setProperty("role", "sidehdr")
        self.header.setStyleSheet(f"color: {T['text2']}; font-size: 10px; font-weight: 600; letter-spacing: 0.6px; padding-left: 12px;")
        v.addWidget(self.header)
        self.box = QWidget(); self.box.paintEvent = self._paint_box
        self.il = QVBoxLayout(self.box); self.il.setContentsMargins(3, 3, 3, 3); self.il.setSpacing(1)
        v.addWidget(self.box)
        self.items = []

    def add(self, it):
        self.items.append(it); self.il.addWidget(it)

    def _paint_box(self, e):
        p = QPainter(self.box); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        bg = QColor(T["text"]); bg.setAlpha(9)
        p.setPen(QPen(T["side_border"], 1)); p.setBrush(bg)
        p.drawRoundedRect(QRectF(self.box.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10)


class MeguminSettings(QWidget):
    def __init__(self):
        super().__init__()
        self.config_dir = Path.home() / ".config" / "megumin-os"
        self.config_file = self.config_dir / "settings.json"
        self.setup_icon_theme()
        self.accent_color = self.get_wal_accent_color()
        ACCENT[0] = self.accent_color
        self.settings = self.get_real_system_states()
        self.tp = TouchpadBackend(self.settings)
        self.history, self.hist_pos = [], -1
        self.page_words = {}
        self.all_rows = []
        T.clear(); T.update(DARK if self.settings.get("dark_theme", True) else LIGHT)
        apply_palette()
        QApplication.instance().setStyleSheet(build_qss())
        self.initUI()

    # ---------------- стан системи ----------------
    def setup_icon_theme(self):
        try:
            theme = subprocess.run("gsettings get org.gnome.desktop.interface icon-theme", shell=True, capture_output=True, text=True).stdout.strip().replace("'", "")
            QIcon.setThemeName(theme if theme else "WhiteSur")
        except Exception:
            QIcon.setThemeName("WhiteSur")
        paths = QIcon.themeSearchPaths()
        for p in ["/usr/share/icons", os.path.expanduser("~/.local/share/icons"), os.path.expanduser("~/.icons")]:
            if p not in paths: paths.append(p)
        QIcon.setThemeSearchPaths(paths)

    def get_wal_accent_color(self):
        # Той самий акцент, що й у всіх модулях SYSui: власний колір або розумний вибір зі шпалер (mgos-accent)
        try:
            with open(self.config_file, "r") as f:
                saved = json.load(f)
            if saved.get("accent_mode") == "custom" and str(saved.get("accent_color", "")).startswith("#"):
                return saved["accent_color"]
        except Exception: pass
        # акцент зі шпалер — той самий розумний вибір, що й у SYSui (не сирий pywal color5)
        try:
            r = subprocess.run([str(Path.home() / ".local/bin/mgos-accent")], capture_output=True, text=True, timeout=3)
            if re.fullmatch(r"#[0-9A-Fa-f]{6}", r.stdout.strip()): return r.stdout.strip()
        except Exception: pass
        return "#C86FA3"

    def get_real_system_states(self):
        state = dict(PANEL_DEFAULTS)
        state.update(TP_DEFAULTS)
        state["video_optimization"] = False
        state["wp_anime4k"] = False
        state["dock_whitelist"] = "nautilus, kitty, wezterm, ignis, code, modrinth-app, steam, spotify"
        try:
            if self.config_file.exists():
                with open(self.config_file, "r") as f:
                    state.update(json.load(f))
        except Exception: pass

        # усі запити — паралельно: час запуску = найповільніший з них, а не сума (~0.8 с → ~0.2 с)
        cmds = {"wifi": "nmcli -t -f WIFI general", "bt": "bluetoothctl show | grep 'Powered: yes'",
                "blur": "hyprctl getoption decoration:blur:enabled -j",
                "theme": "gsettings get org.gnome.desktop.interface color-scheme",
                "night": "pgrep hyprsunset", "pp": "powerprofilesctl get"}
        procs = {k: subprocess.Popen(c, shell=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
                 for k, c in cmds.items()}
        out = {}
        for k, pr in procs.items():
            try: out[k] = pr.communicate(timeout=1.5)[0].strip()
            except Exception:
                pr.kill(); out[k] = ""
        state["wifi_enabled"] = out["wifi"] == "enabled"
        state["bluetooth_enabled"] = bool(out["bt"])
        try: state["hyprland_blur"] = bool(json.loads(out["blur"]).get("int", 1))
        except Exception: state["hyprland_blur"] = True
        state["dark_theme"] = "prefer-dark" in out["theme"] if out["theme"] else True
        state["night_light"] = bool(out["night"])
        state["power_profile"] = out["pp"] or "balanced"
        return state

    def get_current_accent(self):
        return self.accent_color

    def save_setting(self, key, value):
        self.settings[key] = value
        self.config_dir.mkdir(parents=True, exist_ok=True)
        # Перечитуємо файл і змінюємо ЛИШЕ один ключ: модулі SYSui (Control Center)
        # теж пишуть у settings.json, і повний перезапис затирав би їхні зміни.
        data = {}
        try:
            with open(self.config_file, "r") as f:
                data = json.load(f)
        except Exception:
            data = {}
        data[key] = value
        tmp = str(self.config_file) + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
        os.replace(tmp, self.config_file)
        self.apply_system_setting(key, value)

    def apply_system_setting(self, key, value):
        DEVNULL = subprocess.DEVNULL
        try:
            if key.startswith("sddm_") or key in ("accent_mode", "accent_color"):
                self.sddm_changed()
            if key == "dock_q_stash":
                self.write_close_binds(bool(value))
            if key.startswith(("tp_", "kb_", "ms_", "mm_")):
                self.tp.apply()
                if hasattr(self, "tp_preview"): self.tp_preview.update()
                if key.startswith("mm_"):
                    self.refresh_mm_status()
                    if hasattr(self, "mm_preview"):   # бік кліку / прокрутка змінились — перемалювати жест
                        P = self.mm_preview; cur = {"secondary_left": "secondary", "off": "scroll"}.get(P.mode, P.mode)
                        P.mode = None; P.set_mode(cur)
            elif key == "wifi_enabled":
                subprocess.Popen(["nmcli", "radio", "wifi", "on" if value else "off"], stdout=DEVNULL, stderr=DEVNULL)
            elif key == "bluetooth_enabled":
                subprocess.Popen(["bluetoothctl", "power", "on" if value else "off"], stdout=DEVNULL, stderr=DEVNULL)
            elif key == "hyprland_blur":
                subprocess.Popen(["hyprctl", "keyword", "decoration:blur:enabled", str(value).lower()], stdout=DEVNULL, stderr=DEVNULL)
                write_blur_conf(self.settings)
            elif key.startswith("blur_"):
                subprocess.Popen(["hyprctl", "keyword", f"decoration:blur:{blur_option(key)}", blur_value(key, value)], stdout=DEVNULL, stderr=DEVNULL)
                write_blur_conf(self.settings)
            elif key == "kbd_light" and KBD_LED:
                subprocess.Popen(["brightnessctl", "-q", "-d", KBD_LED.name, "set", str(int(value))], stdout=DEVNULL, stderr=DEVNULL)
            elif key.startswith("ui_scale:"):
                self.apply_scale(key.split(":", 1)[1], value)
            elif key in ("sounds_enabled", "sound_theme", "sounds_volume", "sounds_input"):
                self.apply_sounds()
            elif key == "dark_theme":
                subprocess.Popen(["gsettings", "set", "org.gnome.desktop.interface", "color-scheme", "prefer-dark" if value else "default"],
                                 stdout=DEVNULL, stderr=DEVNULL)
                # GTK3 (і діалоги xdg-desktop-portal-gtk у Firefox тощо) color-scheme не читає — лише цей ключ
                set_gtk_ini("gtk-application-prefer-dark-theme", 1 if value else 0)
                self.apply_ui_theme()
            elif key == "night_light":
                if value: subprocess.Popen(["hyprsunset", "-t", "4000"], start_new_session=True, stdout=DEVNULL, stderr=DEVNULL)
                else: subprocess.Popen(["killall", "hyprsunset"], stdout=DEVNULL, stderr=DEVNULL)
            elif key == "power_profile":
                # лише процесор; кулер у «Авто» підхопить зміну сам (mgos-fan слухає D-Bus), «Крива» лишається кривою
                subprocess.Popen(["powerprofilesctl", "set", value], stdout=DEVNULL, stderr=DEVNULL)
            elif key == "fan_mode":
                subprocess.Popen([MGOS_FAN, "set", value], stdout=DEVNULL, stderr=DEVNULL)
                if getattr(self, "fan_curve_card", None): self.fan_curve_card.setVisible(value == "curve")
                QTimer.singleShot(600, self.refresh_fan)
            elif key == "volume":
                subprocess.Popen(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", f"{value}%"], stdout=DEVNULL, stderr=DEVNULL)
            elif key == "screen_brightness":
                subprocess.Popen(["brightnessctl", "-q", "set", f"{max(1, value)}%"], stdout=DEVNULL, stderr=DEVNULL)
        except Exception: pass

    def launch_app(self, cmd, close_ui=False):
        subprocess.Popen(cmd, shell=True, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if close_ui: self.close()

    def launch_local_installer(self):
        """Локальний графічний інсталятор. Якщо його бінарник уже зібраний — відкриваємо одразу;
        якщо треба збирати / доставляти пакети (sudo) — у терміналі, щоб було видно пароль і помилки.
        Раніше скрипт мовчки падав без терміналу і вікно просто не з'являлося."""
        d = Path.home() / ".local" / "share" / "mgos" / "installer"
        script = d / "install-gui.sh"
        if not script.exists():
            subprocess.Popen(["notify-send", "-a", "Налаштування", "Локальний інсталятор не знайдено",
                              f"{d} порожня. Скористайтеся «Відновити систему» — вона завантажить свіжу копію."])
            return
        binp, src = d / "gui" / "mgos-installer", d / "gui" / "mgos_installer.cpp"
        ready = binp.exists() and os.access(binp, os.X_OK) and (not src.exists() or binp.stat().st_mtime >= src.stat().st_mtime)
        if ready:
            subprocess.Popen([str(script)], cwd=str(d), start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            cmd = f"cd {json.dumps(str(d))} && ./install-gui.sh || {{ echo; read -rp 'Інсталятор завершився з помилкою — Enter'; }}"
            subprocess.Popen(["kitty", "--class", "mgos-recovery", "-e", "bash", "-c", cmd], start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.Popen(["notify-send", "-a", "Налаштування", "-t", "2500", "Відкриваю інсталятор mgOS…"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def set_custom_accent(self, hex_color):
        self.accent_color = hex_color
        subprocess.Popen(f"hyprctl keyword general:col.active_border rgba({hex_color.replace('#','')}ff)", shell=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Застосовується до ВСІХ модулів SYSui наживо
        self.save_setting("accent_color", hex_color)
        self.save_setting("accent_mode", "custom")
        if hasattr(self, "accent_wal_switch"): self.accent_wal_switch.setChecked(False)
        self.apply_ui_theme()

    def pick_accent_color(self):
        color = QColorDialog.getColor(QColor(self.accent_color), self, "Свій акцентний колір")
        if color.isValid():
            self.set_custom_accent(color.name())

    def set_accent_from_wal(self, key, value):
        self.save_setting("accent_mode", "wal" if value else "custom")
        self.accent_color = self.get_wal_accent_color()
        if hasattr(self, "accent_wal_switch"): self.accent_wal_switch.setChecked(bool(value))
        self.apply_ui_theme()

    def set_sddm_avatar(self):
        picker = AvatarPicker(self)
        if picker.exec() != QDialog.DialogCode.Accepted: return
        file_path = picker.chosen
        if not file_path: return
        dialog = ImageCropperDialog(file_path, self.accent_color, self.settings.get("dark_theme", True), self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            try:
                cropped = dialog.get_cropped_image()
                if not cropped.save(str(Path.home() / ".face.icon"), "PNG"):
                    raise Exception("Немає прав для запису у файл .face.icon")
                cropped.save(str(Path.home() / ".face"), "PNG")
                self.install_sddm_avatar(cropped)
                self.account.reload()
                subprocess.Popen(["notify-send", "Аватарку встановлено", "Наступного разу в SDDM буде нове фото"])
            except Exception as e:
                subprocess.Popen(["notify-send", "-u", "critical", "Помилка", f"Деталі: {e}"])

    def install_sddm_avatar(self, pix):
        """Кругла копія для теми SDDM (Assets/avatar.png) + /usr/share/sddm/faces через sync_sddm.sh."""
        img = QImage(256, 256, QImage.Format.Format_ARGB32_Premultiplied)
        img.fill(Qt.GlobalColor.transparent)
        p = QPainter(img)
        p.setRenderHint(QPainter.RenderHint.Antialiasing); p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        path = QPainterPath(); path.addEllipse(QRectF(2, 2, 252, 252))
        p.setClipPath(path)
        p.drawPixmap(0, 0, pix.scaled(256, 256, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation))
        p.setClipping(False)
        p.setPen(QPen(QColor(255, 255, 255, 170), 3)); p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(QRectF(3, 3, 250, 250))
        p.end()
        cache = Path.home() / ".cache" / "megumin-os"
        cache.mkdir(parents=True, exist_ok=True)
        img.save(str(cache / "avatar_round.png"), "PNG")
        theme_assets = Path("/usr/share/sddm/themes/sddm-astronaut-theme/Assets")
        try:
            if os.access(theme_assets, os.W_OK):
                img.save(str(theme_assets / "avatar.png"), "PNG")
        except Exception:
            pass
        sync = Path.home() / ".config" / "megumin-os" / "sync_sddm.sh"
        if sync.exists():
            subprocess.Popen(["sudo", "-n", str(sync)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def apply_ui_theme(self):
        ACCENT[0] = self.accent_color
        T.clear(); T.update(DARK if self.settings.get("dark_theme", True) else LIGHT)
        apply_palette()
        QApplication.instance().setStyleSheet(build_qss())
        for w in self.findChildren(QWidget):
            w.update()
        self.update()

    # ---------------- каркас ----------------
    def initUI(self):
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setWindowTitle("Налаштування")
        self.resize(980, 660)
        self.setMinimumSize(820, 520)

        root = QHBoxLayout(self)
        root.setContentsMargins(9, 9, 0, 0)
        root.setSpacing(0)

        # --- сайдбар: «скляна» панель, що плаває всередині вікна ---
        self.sidebar = QWidget()
        self.sidebar.setFixedWidth(250)
        self.sidebar.paintEvent = self._paint_sidebar
        sl = QVBoxLayout(self.sidebar)
        sl.setContentsMargins(6, 14, 6, 10)
        sl.setSpacing(2)
        tl = QHBoxLayout(); tl.setContentsMargins(12, 0, 0, 6)
        tl.addWidget(TrafficLights(self)); tl.addStretch()
        sl.addLayout(tl)

        sw = QWidget(); swl = QHBoxLayout(sw); swl.setContentsMargins(8, 4, 8, 6)
        self.search = QLineEdit(); self.search.setObjectName("search"); self.search.setPlaceholderText("Пошук")
        self.search.textChanged.connect(self.filter_sidebar)
        self.search.setFocusPolicy(Qt.FocusPolicy.ClickFocus)   # як у macOS: пошук не фокусується сам
        swl.addWidget(self.search)
        ico = QLabel(G["search"], self.search); ico.setProperty("role", "chev"); ico.move(9, 6)
        sw.setFixedHeight(42)   # не даємо layout'у стискати пошук і аватарку, коли пунктів багато
        sl.addWidget(sw)

        self.account = AccountCard(self.set_sddm_avatar)
        sl.addWidget(self.account)
        sl.addSpacing(4)

        # пункти — у власній прокрутці: їх більше, ніж влазить у мінімальну висоту вікна
        side_scroll = QScrollArea(); side_scroll.setWidgetResizable(True); side_scroll.setFrameShape(QFrame.Shape.NoFrame)
        side_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        side_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        side_scroll.setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; border: none; }")
        items_w = QWidget(); il = QVBoxLayout(items_w); il.setContentsMargins(4, 0, 6, 6); il.setSpacing(10)
        self.side_items, self.side_gaps, self.side_groups = [], [], {}
        for i, (_, title, glyph, color, grp) in enumerate(PAGES):
            if grp not in self.side_groups:
                sg = SideGroup(PAGE_GROUPS[grp] if grp < len(PAGE_GROUPS) else "")
                self.side_groups[grp] = sg; il.addWidget(sg)
            it = SidebarItem(i, glyph, color, title)
            it.clicked.connect(self.go)
            self.side_groups[grp].add(it)
            self.side_items.append(it)
        il.addStretch()
        side_scroll.setWidget(items_w)
        self.side_scroll = side_scroll
        sl.addWidget(side_scroll, 1)
        root.addWidget(self.sidebar)

        # --- вміст ---
        right = QWidget()
        rl = QVBoxLayout(right)
        rl.setContentsMargins(18, 6, 12, 0)
        rl.setSpacing(4)
        tb = QHBoxLayout(); tb.setSpacing(2)
        self.back_btn = NavButton(G["left"], lambda: self.nav_history(-1))
        self.fwd_btn = NavButton(G["right"], lambda: self.nav_history(+1))
        self.page_title = QLabel(""); self.page_title.setProperty("role", "h1")
        tb.addWidget(self.back_btn); tb.addWidget(self.fwd_btn); tb.addSpacing(8); tb.addWidget(self.page_title); tb.addStretch()
        self.toolbar = QWidget(); self.toolbar.setLayout(tb); self.toolbar.setFixedHeight(44)
        rl.addWidget(self.toolbar)

        self.pages = FadeStackedWidget()
        builders = dict(wifi=self.create_wifi_page, bluetooth=self.create_bluetooth_page, appearance=self.create_appearance_page,
                        wallpapers=self.create_wallpapers_page, pixelwp=self.create_pixelwp_page, display=self.create_display_page, sound=self.create_audio_page,
                        trackpad=self.create_trackpad_page, mouse=self.create_mouse_page, magicmouse=self.create_magicmouse_page, look=self.create_look_page, keyboard=self.create_keyboard_page, dock=self.create_dock_page, panels=self.create_panels_page,
                        power=self.create_power_page, system=self.create_system_page,
                        sounds=self.create_sounds_page, login=self.create_login_page, about=self.create_about_page, games=self.create_games_page, help=self.create_help_page, osd=self.create_osd_page,
                        developer=self.create_developer_page)
        for i, (pid, *_r) in enumerate(PAGES):
            self._building = i
            self.page_words[i] = [PAGES[i][1].lower()]
            self.pages.addWidget(builders[pid]())
        rl.addWidget(self.pages, 1)
        grip = QHBoxLayout(); grip.addStretch(); grip.addWidget(QSizeGrip(self))
        rl.addLayout(grip)
        root.addWidget(right, 1)
        self.go(2, record=True)

    def _paint_sidebar(self, e):
        p = QPainter(self.sidebar)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.sidebar.rect()).adjusted(0.5, 0.5, -0.5, -9.5)
        p.setPen(QPen(T["side_border"], 1))
        p.setBrush(T["sidebar"])
        p.drawRoundedRect(r, 18, 18)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        rad = 0 if self.isMaximized() else 24
        p.setPen(QPen(T["win_border"], 1))
        p.setBrush(T["win"])
        p.drawRoundedRect(r, rad, rad)

    # ---------------- навігація ----------------
    def go(self, index, record=True):
        index = max(0, min(len(PAGES) - 1, int(index)))
        for it in self.side_items:
            it.selected = it.index == index
            it.update()
            if it.selected and getattr(self, "side_scroll", None):
                QTimer.singleShot(0, lambda w=it: self.side_scroll.ensureWidgetVisible(w, 0, 8))
        self.pages.setCurrentIndex(index)
        self.page_title.setText(PAGES[index][1])
        if record:
            self.history = self.history[:self.hist_pos + 1] + [index]
            self.hist_pos = len(self.history) - 1
        self.back_btn.setEnabled(self.hist_pos > 0)
        self.fwd_btn.setEnabled(self.hist_pos < len(self.history) - 1)
        self.back_btn.update(); self.fwd_btn.update()

    def nav_history(self, d):
        np_ = self.hist_pos + d
        if 0 <= np_ < len(self.history):
            self.hist_pos = np_
            self.go(self.history[np_], record=False)

    def open_page(self, spec):
        ids = [p[0] for p in PAGES]
        if str(spec) in ids: self.go(ids.index(str(spec)))
        else:
            try: self.go(int(spec))
            except ValueError: pass

    def filter_sidebar(self, text):
        q = text.strip().lower()
        first = None
        for it in self.side_items:
            ok = not q or any(q in w for w in self.page_words.get(it.index, []))
            it.setVisible(ok)
            if ok and first is None: first = it.index
        for g in self.side_gaps: g.setVisible(not q)
        for sg in getattr(self, "side_groups", {}).values():
            sg.setVisible(any(not it.isHidden() for it in sg.items))
        if q and first is not None: self.go(first)

    # перетягування вікна за тулбар / порожнє місце (Wayland — системний move)
    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self.windowHandle():
            self.windowHandle().startSystemMove()

    # ---------------- будівельники сторінок ----------------
    def page_base(self, pid):
        idx = [p[0] for p in PAGES].index(pid)
        page = QScrollArea()
        page.setWidgetResizable(True)
        page.setFrameShape(QFrame.Shape.NoFrame)
        page.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        holder = QWidget()
        hl = QHBoxLayout(holder); hl.setContentsMargins(0, 0, 6, 24)
        col = QWidget(); col.setMaximumWidth(660)
        lay = QVBoxLayout(col); lay.setContentsMargins(0, 4, 0, 0); lay.setSpacing(10)
        lay.setAlignment(Qt.AlignmentFlag.AlignTop)
        hl.addStretch(); hl.addWidget(col, 100); hl.addStretch()
        page.setWidget(holder)
        return page, lay

    def hero(self, lay, pid, desc, control=None):
        _, title, glyph, color, _g = PAGES[[p[0] for p in PAGES].index(pid)]
        lay.addWidget(HeroCard(glyph, color, title, desc, control))
        self.page_words[self._building].append(desc.lower())

    def section(self, lay, title):
        l = QLabel(title); l.setProperty("role", "section")
        lay.addSpacing(6)
        lay.addWidget(l)

    def create_card(self, rows):
        for r in rows:
            self.all_rows.append(r)
            self.page_words[self._building] += [r.title.lower(), r.description.lower()]
        return GroupCard(rows)

    def sw(self, k, default=False):
        return MeguminSwitch(k, bool(self.settings.get(k, default)), self.save_setting)

    def sl(self, k, lo, hi, suf="%", **kw):
        return MeguminSlider(k, self.settings.get(k, PANEL_DEFAULTS.get(k, TP_DEFAULTS.get(k, lo))), lo, hi, suf, self.save_setting, **kw)

    def seg(self, k, opts, **kw):
        return SegmentedSelector(k, opts, self.settings.get(k, PANEL_DEFAULTS.get(k, TP_DEFAULTS.get(k))), self.save_setting, **kw)

    # --- Wi-Fi / Bluetooth ---
    def create_wifi_page(self):
        page, lay = self.page_base("wifi")
        self.hero(lay, "wifi", "Бездротова мережа. Список мереж і паролі — у менеджері мереж.",
                  self.sw("wifi_enabled"))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Мережі та підключення", "Вибір мережі, пароль, VPN (nmtui)",
                       on_click=lambda: self.launch_app("kitty -e nmtui")),
            SettingRow("", "", "Control Center", "Швидке перемикання мереж у панелі SYSui",
                       on_click=lambda: self.launch_app(f"echo 'page|network' | socat - UNIX-CLIENT:{Path.home()}/.config/SYSui/cc.sock")),
        ]))
        return page

    def create_bluetooth_page(self):
        page, lay = self.page_base("bluetooth")
        self.hero(lay, "bluetooth", "Навушники, миші, геймпади та інші бездротові пристрої.", self.sw("bluetooth_enabled"))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Пристрої", "Пошук і підключення (bluetuith)", on_click=lambda: self.launch_app("kitty -e bluetuith")),
            SettingRow("", "", "Менеджер Blueman", "Розширені налаштування та передача файлів", on_click=lambda: self.launch_app("blueman-manager")),
        ]))
        return page

    # --- Вигляд ---
    def create_appearance_page(self):
        page, lay = self.page_base("appearance")
        self.hero(lay, "appearance", "Тема системи, колір акценту, ефекти вікон.")
        self.appearance_picker = AppearancePicker(self.settings.get("dark_theme", True), self.save_setting)
        lay.addWidget(self.create_card([
            SettingRow("", "", "Вигляд", "Світле чи темне оформлення всієї системи", self.appearance_picker),
            SettingRow("", "", "Колір акценту", "Перший кружечок — колір із шпалер (pywal)", AccentPicker(self)),
        ]))
        self.section(lay, "Ефекти")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Розмиття і тіні вікон", "Скляні ефекти Hyprland", self.sw("hyprland_blur", True)),
            SettingRow("", "", "Живі анімації", "Пружинні наведення, поява панелей SYSui", self.sw("animations", True)),
            SettingRow("", "", "Стиль піксельних шапок", "Анімація у вікні «Про систему» і в блоці годинника",
                       MacPopup("pixel_style", PIXEL_STYLES, self.settings.get("pixel_style", "waves"), self.save_setting, width=200)),
        ]))
        self.section(lay, "Розмиття — детально")
        self.blur_widgets = {}
        rows = []
        for k, opt, lo, hi, mul, title, desc in BLUR_SPEC:
            fmt = (lambda v: str(v)) if mul == 1 else (lambda v, m=mul: f"{v * m:.2f}".rstrip("0").rstrip(".") if m < 0.01 else f"{v}%")
            w = MeguminSlider(k, self.settings.get(k, BLUR_DEFAULTS[k]), lo, hi, "", self.save_setting, width=200, fmt=fmt)
            self.blur_widgets[k] = w
            rows.append(SettingRow("", "", title, desc, w))
        for k, opt, title, desc, d in BLUR_BOOL:
            w = self.sw(k, d)
            self.blur_widgets[k] = w
            rows.append(SettingRow("", "", title, desc, w))
        rows.append(SettingRow("", "", "Скинути розмиття", "Повернути налаштування mgOS за замовчуванням",
                               LinkButton("За замовчуванням", self.reset_blur)))
        lay.addWidget(self.create_card(rows))
        self.section(lay, "Обліковий запис")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Аватарка екрана входу", "Фото в SDDM і в цьому вікні", on_click=self.set_sddm_avatar),
        ]))
        return page

    # --- Шпалери ---
    def create_wallpapers_page(self):
        page, lay = self.page_base("wallpapers")
        self.hero(lay, "wallpapers", "Картинки, відео та динамічні шпалери, що змінюються протягом дня.")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Пауза відео-шпалер", "Коли відкрите вікно на весь екран", self.sw("video_optimization")),
            SettingRow("", "", "Шейдери Anime4K", "Деблюр і апскейл «мильних» відео-шпалер", self.sw("wp_anime4k")),
            SettingRow("", "", "Швидкий вибір", "Super + W — панель шпалер поверх усіх вікон",
                       on_click=lambda: self.launch_app(f"echo show | socat - UNIX-CONNECT:{Path.home()}/.config/SYSui/wallpapers.sock")),
        ]))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Піксельні живі шпалери", "Аврора, захід сонця, хвилі… у кольорі акценту — окрема вкладка",
                       on_click=lambda: self.go([p[0] for p in PAGES].index("pixelwp"))),
        ]))
        fl = QHBoxLayout()
        self.wp_seg = SegmentedSelector("wp_filter", [("all", "Всі"), ("images", "Картинки"), ("videos", "Відео"), ("dynamic", "Динамічні")],
                                        "all", lambda k, v: self.load_wallpapers(v))
        fl.addWidget(self.wp_seg); fl.addStretch()
        fl.addWidget(LinkButton("Оновити", lambda: self.load_wallpapers(self.current_wp_filter)))
        lay.addSpacing(6)
        lay.addLayout(fl)
        self.current_wallpaper = self.read_current_wallpaper()
        self.wp_list = QListView(); self.wp_list.setObjectName("wp")
        self.wp_list.setViewMode(QListView.ViewMode.IconMode)
        self.wp_list.setResizeMode(QListView.ResizeMode.Adjust)
        self.wp_list.setUniformItemSizes(True)
        self.wp_list.setMovement(QListView.Movement.Static)
        self.wp_list.setSelectionMode(QListView.SelectionMode.NoSelection)
        self.wp_list.setMouseTracking(True)
        self.wp_list.setFrameShape(QFrame.Shape.NoFrame)
        self.wp_list.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.wp_list.setVerticalScrollMode(QListView.ScrollMode.ScrollPerPixel)
        self.wp_list.verticalScrollBar().setSingleStep(24)
        self.wp_list.setStyleSheet("QListView#wp { background: transparent; border: none; }")
        self.wp_list.viewport().setAutoFillBackground(False)
        self.wp_list.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.wp_list.setItemDelegate(WallpaperDelegate(self))
        self.wp_list.setMinimumHeight(520)
        self.wp_model = WallGridModel([])
        self.wp_list.setModel(self.wp_model)
        self.wp_list.clicked.connect(self.on_wallpaper_selected)
        lay.addWidget(self.wp_list)
        self.current_wp_filter = "all"
        self.load_wallpapers("all")
        return page

    # --- Піксельні шпалери ---
    PIXEL_BIN = Path.home() / ".config/SYSui/pixel_wallpaper"

    def create_pixelwp_page(self):
        page, lay = self.page_base("pixelwp")
        self.hero(lay, "pixelwp", "Живі піксельні шпалери. Можуть задавати акцент і світлу/темну тему системи. "
                                  "Поки вікно на весь екран, анімація на паузі.")
        g = lambda k, d: self.settings.get(k, d)
        lay.addWidget(self.create_card([
            SettingRow("", "", "Колір шпалер", "Перший — як акцент системи",
                       PixelColorPicker(g("wp_pixel_color", ""), lambda c: self.pixel_option("wp_pixel_color", c))),
            SettingRow("", "", "Тема шпалер", "Авто — як оформлення системи",
                       SegmentedSelector("wp_pixel_theme", [("auto", "Авто"), ("light", "Світла"), ("dark", "Темна")],
                                         g("wp_pixel_theme", "auto"), lambda k, v: self.pixel_option(k, v))),
            SettingRow("", "", "Кадрів за секунду", "Плавніше — більше навантаження: 12 к/с ≈ 8–15% одного ядра, 30 к/с ≈ 20–40%",
                       MeguminSlider("wp_pixel_fps", g("wp_pixel_fps", 12), 2, 30, " к/с", self.save_setting)),
            SettingRow("", "", "Пауза під вікнами", "Як у відео-шпалер: поки вікна закривають майже весь екран, анімація стоїть",
                       MeguminSwitch("wp_pixel_pause_covered", bool(g("wp_pixel_pause_covered", True)), self.save_setting)),
            SettingRow("", "", "Від батареї", "Повільно — половина вибраних кадрів/с. На весь екран анімація завжди на паузі",
                       SegmentedSelector("wp_pixel_battery", [("slow", "Повільно"), ("full", "Як від мережі"), ("freeze", "Пауза")],
                                         g("wp_pixel_battery", "slow"), self.save_setting)),
            SettingRow("", "", "Змінювати акцент системи", "Свій колір шпалер стає акцентом усіх панелей",
                       MeguminSwitch("wp_pixel_sync_accent", bool(g("wp_pixel_sync_accent", True)), self.save_setting)),
            SettingRow("", "", "Змінювати тему системи", "Світла/темна тема шпалер вмикає таку ж тему системи",
                       MeguminSwitch("wp_pixel_sync_theme", bool(g("wp_pixel_sync_theme", True)), self.save_setting)),
        ]))
        self.section(lay, "Стилі")
        grid_w = QWidget(); grid = QGridLayout(grid_w); grid.setContentsMargins(0, 0, 0, 0); grid.setSpacing(12)
        self.pixel_cards = {}
        cur = self.read_current_wallpaper()
        cur = cur[6:] if cur.startswith("pixel:") else ""
        for i, (sid, name) in enumerate(PIXEL_STYLES_WP):
            card = PixelPreviewCard(sid, name, self.apply_pixel_wallpaper)
            card.active = sid == cur
            self.pixel_cards[sid] = card
            grid.addWidget(card, i // 3, i % 3)
        lay.addWidget(grid_w)
        self.page_words[self._building] += [n.lower() for _, n in PIXEL_STYLES_WP]
        self.pixel_sig = PixelPreviewSignals()
        self.pixel_sig.ready.connect(lambda sid, path: sid in self.pixel_cards and self.pixel_cards[sid].set_image(path))
        self.render_pixel_previews()
        return page

    def pixel_theme_now(self):
        th = self.settings.get("wp_pixel_theme", "auto")
        if th in ("light", "dark"): return th
        return "dark" if self.settings.get("dark_theme", True) else "light"

    def render_pixel_previews(self):
        """Прев'ю малює сам pixel_wallpaper --poster (той самий код, що й шпалери) — у фоні, з кешем."""
        col = self.settings.get("wp_pixel_color", "") or self.get_wal_accent_color()
        th = self.pixel_theme_now()
        cache = Path.home() / ".cache/megumin-os/pixel_previews"; cache.mkdir(parents=True, exist_ok=True)
        sig = self.pixel_sig
        def work():
            for sid, _ in PIXEL_STYLES:
                out = cache / f"{sid}_{col.lstrip('#')}_{th}.png"
                if not out.exists():
                    subprocess.run([str(self.PIXEL_BIN), "--poster", str(out), "400x224", "--style", sid,
                                    "--color", col, "--theme", th], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if out.exists(): sig.ready.emit(sid, str(out))
        threading.Thread(target=work, daemon=True).start()

    def pixel_active(self):
        return self.read_current_wallpaper().startswith("pixel:")

    def pixel_option(self, key, value):
        self.save_setting(key, value)
        self.render_pixel_previews()
        if self.pixel_active(): self.pixel_sync_system()   # шпалери вже стоять — оновлюються наживо

    def apply_pixel_wallpaper(self, sid):
        subprocess.Popen([str(self.PIXEL_BIN), "--set", sid], start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for k, c in getattr(self, "pixel_cards", {}).items():
            c.active = k == sid; c.update()
        self.current_wallpaper = "pixel:" + sid
        QTimer.singleShot(300, self.pixel_sync_system)

    def pixel_sync_system(self):
        """Шпалери задають систему: свій колір → акцент, своя тема → тема; потім постер, pywal і SDDM."""
        col = self.settings.get("wp_pixel_color", "")
        if self.settings.get("wp_pixel_sync_accent", True) and col:
            if self.settings.get("accent_mode") != "custom" or self.accent_color.lower() != col.lower():
                self.set_custom_accent(col)
        th = self.settings.get("wp_pixel_theme", "auto")
        if self.settings.get("wp_pixel_sync_theme", True) and th in ("light", "dark"):
            if bool(self.settings.get("dark_theme", True)) != (th == "dark"):
                self.save_setting("dark_theme", th == "dark")
                if hasattr(self, "appearance_picker"):
                    self.appearance_picker.dark = th == "dark"; self.appearance_picker.update()
        home = Path.home()
        def work():
            poster = subprocess.run([str(home / ".local/bin/mgos-wp-poster")], capture_output=True, text=True).stdout.strip()
            if poster:   # палітра pywal (термінал, рамки вікон…) — з кадру піксельних шпалер
                subprocess.run(["wal", "-q", "-t", "-i", poster], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                subprocess.run(["killall", "-SIGUSR2", "waybar"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["sudo", "-n", str(home / ".config/megumin-os/sync_sddm.sh")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        threading.Thread(target=work, daemon=True).start()

    def read_current_wallpaper(self):
        try:
            return Path(os.path.expanduser("~/.cache/ignis/last_wallpaper.txt")).read_text().strip()
        except Exception:
            return ""

    def load_wallpapers(self, category):
        self.current_wp_filter = category
        wp_dir = os.path.expanduser("~/Pictures/wallpapers")
        if not os.path.exists(wp_dir): return
        valid_img, valid_vid = ('png', 'jpg', 'jpeg', 'webp'), ('mp4', 'webm', 'gif')
        filtered = []
        self.current_wallpaper = self.read_current_wallpaper()
        for f in sorted(os.listdir(wp_dir), key=str.lower):
            path = os.path.join(wp_dir, f)
            is_dir = os.path.isdir(path)
            ext = path.lower().split('.')[-1] if not is_dir else ""
            if not is_dir and ext not in valid_img + valid_vid: continue
            if category == "all": filtered.append(path)
            elif category == "dynamic" and is_dir: filtered.append(path)
            elif category == "videos" and ext in valid_vid: filtered.append(path)
            elif category == "images" and ext in valid_img: filtered.append(path)
        self.wp_model.update_items(filtered)

    # --- Екран ---
    def create_display_page(self):
        page, lay = self.page_base("display")
        self.hero(lay, "display", "Яскравість, нічне світло та розташування моніторів.")
        rows = []
        try:
            cur = int(subprocess.run(["brightnessctl", "-m"], capture_output=True, text=True, timeout=1).stdout.split(",")[3].rstrip("%"))
            rows.append(SettingRow("", "", "Яскравість", "Також Fn+F2 / Fn+F3",
                                   MeguminSlider("screen_brightness", cur, 1, 100, "%", self.save_setting, width=220)))
        except Exception:
            pass
        rows += [
            SettingRow("", "", "Нічне світло", "Теплий фільтр (hyprsunset, 4000K)", self.sw("night_light")),
        ]
        kc = self.kbd_light_control()
        if kc: rows.append(SettingRow("", "", "Підсвітка клавіатури", "Fn + F9 · 3 рівні + вимкнено", kc))
        lay.addWidget(self.create_card(rows))
        self.section(lay, "Масштаб інтерфейсу")
        srows = []
        for m in hypr_monitors():
            w, h, cur = m.get("width", 0), m.get("height", 0), float(m.get("scale", 1))
            opts = []
            for x in SCALE_CANDIDATES:
                if scale_ok(w, h, x) and round(h / x) >= 600:
                    opts.append((fmt_scale(x), f"{round(x * 100)}%  ·  як {round(w / x)}×{round(h / x)}"))
            curs = fmt_scale(min((float(o[0]) for o in opts), key=lambda v: abs(v - cur))) if opts else "1"
            name = m.get("name", "")
            desc = f"{m.get('description', '').split(' 0x')[0] or name} · {w}×{h}. Більше — крупніші вікна, текст і панелі"
            srows.append(SettingRow("", "", "Вбудований екран" if name.startswith(("eDP", "LVDS", "DSI")) else name, desc,
                                    MacPopup(f"ui_scale:{name}", opts, curs, self.save_setting, width=240)))
        srows.append(SettingRow("", "", "Автоматично", "Підібрати масштаб за щільністю пікселів (як після встановлення)",
                                LinkButton("За замовчуванням", self.reset_scale)))
        srows.append(SettingRow("", "", "Монітори", "Роздільність і розташування (wdisplays)", on_click=lambda: self.launch_app("wdisplays")))
        lay.addWidget(self.create_card(srows))
        return page

    def apply_scale(self, name, value):
        """Пише ~/.config/mgos/display.conf (його читає mgos-display при кожному вході й підключенні) і застосовує зараз."""
        lines = []
        try: lines = DISPLAY_OVERRIDES.read_text().splitlines()
        except Exception: pass
        lines = [l for l in lines if not (l.split() and l.split()[0] == name)]
        if value is not None: lines.append(f"{name}  {value}")
        DISPLAY_OVERRIDES.parent.mkdir(parents=True, exist_ok=True)
        DISPLAY_OVERRIDES.write_text("\n".join(lines) + ("\n" if lines else ""))
        subprocess.Popen(["/usr/local/bin/mgos-display", "apply", "--profile", "mgos"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def reset_scale(self):
        for m in hypr_monitors():
            self.apply_scale(m.get("name", ""), None)
            self.settings.pop(f"ui_scale:{m.get('name', '')}", None)
        QTimer.singleShot(900, lambda: self.rebuild_page("display"))

    def rebuild_page(self, pid):
        ids = [p[0] for p in PAGES]
        i = ids.index(pid)
        old = self.pages.widget(i)
        self._building = i
        new = getattr(self, f"create_{'audio' if pid == 'sound' else pid}_page")()
        self.pages.insertWidget(i, new)
        self.pages.removeWidget(old); old.deleteLater()
        self.pages.setCurrentIndex(i)

    def reset_blur(self):
        for k, d in BLUR_DEFAULTS.items():
            self.settings[k] = d
            w = self.blur_widgets.get(k)
            if isinstance(w, MeguminSlider):
                w.slider.blockSignals(True); w.slider.setValue(int(d)); w.slider.blockSignals(False)
                if w.label: w.label.setText(w._text(int(d)))
                w.slider.update()
            elif w is not None and hasattr(w, "setChecked"):
                w.blockSignals(True); w.setChecked(bool(d)); w._animate(bool(d)) if hasattr(w, "_animate") else None; w.blockSignals(False)
            self.save_setting(k, d)

    # --- Звук ---
    def create_audio_page(self):
        page, lay = self.page_base("sound")
        self.hero(lay, "sound", "Гучність виходу, пристрої та мікшер застосунків.")
        rows = []
        try:
            out = subprocess.run(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"], capture_output=True, text=True, timeout=1).stdout
            vol = int(round(float(out.split()[1].replace(",", ".")) * 100))
            rows.append(SettingRow("", "", "Гучність виходу", "", MeguminSlider("volume", min(vol, 150), 0, 150, "%", self.save_setting, width=220)))
        except Exception:
            pass
        rows += [
            SettingRow("", "", "Мікшер", "Гучність застосунків і вибір пристроїв (pavucontrol)", on_click=lambda: self.launch_app("pavucontrol")),
            SettingRow("", "", "Звук у Control Center", "", on_click=lambda: self.launch_app(f"echo 'page|sound' | socat - UNIX-CLIENT:{Path.home()}/.config/SYSui/cc.sock")),
        ]
        lay.addWidget(self.create_card(rows))
        return page

    # --- Іконки, курсор, шрифти ---
    def apply_look(self, key, value):
        DN = subprocess.DEVNULL
        self.settings[key] = value
        if key == "icon_theme":
            subprocess.Popen(["gsettings", "set", "org.gnome.desktop.interface", "icon-theme", value], stdout=DN, stderr=DN)
            set_gtk_ini("gtk-icon-theme-name", value)
            QIcon.setThemeName(value)
            self.icon_preview.set_theme(value)
        elif key in ("cursor_theme", "cursor_size"):
            ct = self.settings.get("cursor_theme") or gsettings_get("cursor-theme") or "Adwaita"
            cs = int(self.settings.get("cursor_size") or 24)
            subprocess.Popen(["gsettings", "set", "org.gnome.desktop.interface", "cursor-theme", ct], stdout=DN, stderr=DN)
            subprocess.Popen(["gsettings", "set", "org.gnome.desktop.interface", "cursor-size", str(cs)], stdout=DN, stderr=DN)
            subprocess.Popen(["hyprctl", "setcursor", ct, str(cs)], stdout=DN, stderr=DN)
            set_gtk_ini("gtk-cursor-theme-name", ct); set_gtk_ini("gtk-cursor-theme-size", cs)
            d = Path.home() / ".local" / "share" / "icons" / "default"
            try:
                d.mkdir(parents=True, exist_ok=True)
                (d / "index.theme").write_text(f"[Icon Theme]\nName=Default\nInherits={ct}\n")
            except Exception: pass
            self.save_setting("cursor_theme", ct)
            self.save_setting("cursor_size", cs)
            self.tp.apply()   # env XCURSOR_* у згенерованому конфігу Hyprland
            return
        elif key in ("ui_font", "ui_font_size"):
            fam = self.settings.get("ui_font") or "Inter"
            size = int(self.settings.get("ui_font_size") or 11)
            subprocess.Popen(["gsettings", "set", "org.gnome.desktop.interface", "font-name", f"{fam} {size}"], stdout=DN, stderr=DN)
            set_gtk_ini("gtk-font-name", f"{fam} {size}")
        self.save_setting(key, value)

    def refresh_theme_lists(self):
        self.icon_popup.set_options(list_themes("icons"), gsettings_get("icon-theme"))
        self.cursor_popup.set_options(list_themes("cursors"), gsettings_get("cursor-theme"))

    def create_look_page(self):
        page, lay = self.page_base("look")
        self.hero(lay, "look", "Тема іконок, курсор миші, шрифт інтерфейсу. Нові набори можна завантажити нижче.")
        cur_icons = gsettings_get("icon-theme")
        self.icon_popup = MacPopup("icon_theme", list_themes("icons"), cur_icons, self.apply_look, width=240)
        self.icon_preview = IconPreview(); self.icon_preview.set_theme(cur_icons or QIcon.themeName())
        lay.addWidget(self.create_card([
            SettingRow("", "", "Тема іконок", "Застосунки підхоплять після перезапуску", self.icon_popup),
        ]))
        lay.addWidget(GroupCard([self.icon_preview]))
        self.section(lay, "Курсор миші")
        try: cs = int(gsettings_get("cursor-size") or 24)
        except ValueError: cs = 24
        self.cursor_popup = MacPopup("cursor_theme", list_themes("cursors"), gsettings_get("cursor-theme"), self.apply_look, width=240)
        lay.addWidget(self.create_card([
            SettingRow("", "", "Стиль курсора", "", self.cursor_popup),
            SettingRow("", "", "Розмір курсора", "", MeguminSlider("cursor_size", cs, 16, 64, " px", self.apply_look, width=200)),
        ]))
        self.section(lay, "Шрифти")
        fn = gsettings_get("font-name") or "Inter 11"
        parts = fn.rsplit(" ", 1)
        fam, size = (parts[0], int(parts[1])) if len(parts) == 2 and parts[1].isdigit() else (fn, 11)
        fams = sorted(set(f for f in QFontDatabase.families() if not f.startswith(".")
                          and any(k in f for k in ("SF Pro", "Inter", "Noto Sans", "Cantarell", "Roboto", "Ubuntu", "JetBrains", "Fira", "Open Sans", "DejaVu Sans"))))
        if fam not in fams: fams.insert(0, fam)
        lay.addWidget(self.create_card([
            SettingRow("", "", "Шрифт інтерфейсу", "GTK-застосунки та діалоги", MacPopup("ui_font", [(f, f) for f in fams], fam, self.apply_look, width=240)),
            SettingRow("", "", "Розмір тексту", "", MeguminSlider("ui_font_size", size, 8, 16, " pt", self.apply_look, width=200)),
            SettingRow("", "", "Шрифт панелей", "Топбар, док, Control Center, OSD. Значки — окремим легким шрифтом",
                       MacPopup("panel_font", [(f, f) for f in fams if "Nerd Font Mono" not in f],
                                self.settings.get("panel_font", "SF Pro Display"), self.save_setting, width=240)),
        ]))
        self.section(lay, "Завантажити іконки")
        run = lambda cmd: (lambda: self.launch_app("kitty -e bash -c " + json.dumps(cmd)))
        lay.addWidget(self.create_card([SettingRow("", "", n, d, LinkButton("Встановити", run(c))) for n, d, c in ICON_PACKS]))
        self.section(lay, "Завантажити курсори")
        lay.addWidget(self.create_card([SettingRow("", "", n, d, LinkButton("Встановити", run(c))) for n, d, c in CURSOR_PACKS] + [
            SettingRow("", "", "Оновити списки", "Після встановлення нових тем", on_click=self.refresh_theme_lists)]))
        return page

    # --- Клавіатура ---
    def create_keyboard_page(self):
        page, lay = self.page_base("keyboard")
        try:
            lays = json.loads(subprocess.run(["hyprctl", "getoption", "input:kb_layout", "-j"], capture_output=True, text=True, timeout=1).stdout).get("str", "")
        except Exception:
            lays = ""
        self.hero(lay, "keyboard", f"Розкладки: {lays.upper().replace(',', ' · ')}" if lays else "Повтор клавіш і розкладки.")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Швидкість повтору", "Скільки символів за секунду, коли клавішу затиснуто",
                       self.sl("kb_rate", 10, 60, labels=("Повільно", "Швидко"), ticks=8, width=240)),
            SettingRow("", "", "Затримка перед повтором", "", self.sl("kb_delay", 150, 800, labels=("Коротка", "Довга"), ticks=8, width=240)),
        ]))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Перемикання розкладки", "Alt + Shift", None),
            SettingRow("", "", "Яскравість екрана", "Fn + F2 / Fn + F3", None),
        ]))
        if KBD_LED:
            self.section(lay, "Підсвітка клавіатури")
            lay.addWidget(self.create_card([
                SettingRow("", "", "Яскравість підсвітки",
                           "Fn + F9. Контролер ноутбука вміє лише 3 рівні + вимкнено — більше рівнів залізо не підтримує",
                           self.kbd_light_control()),
            ]))
        t = QLineEdit(); t.setPlaceholderText("Перевірте повтор клавіш тут…")
        lay.addWidget(t)
        return page

    def kbd_light_control(self):
        if not KBD_LED: return None
        try:
            mx = int((KBD_LED / "max_brightness").read_text()); cur = int((KBD_LED / "brightness").read_text())
        except Exception:
            return None
        names = ["Вимк.", "Слабка", "Середня", "Яскрава"] if mx == 3 else [str(i) for i in range(mx + 1)]
        return SegmentedSelector("kbd_light", [(i, names[i]) for i in range(mx + 1)], cur, self.save_setting)

    # --- Трекпад ---
    def create_trackpad_page(self):
        page, lay = self.page_base("trackpad")
        pads = self.tp.touchpads()
        self.hero(lay, "trackpad", ("Тачпад: " + ", ".join(pads)) if pads else "Тачпад не знайдено — налаштування застосуються, щойно він з'явиться.")
        self.tp_preview = TrackpadPreview(self.tp)
        prev_card = GroupCard([self.tp_preview])
        lay.addWidget(prev_card)
        tabs = SegmentedSelector("tp_tab", [("point", "Наведення і клік"), ("scroll", "Прокрутка"), ("gest", "Інші жести")], "point",
                                 lambda k, v: (self.tp_stack.setCurrentIndex(["point", "scroll", "gest"].index(v)),
                                               self.tp_preview.set_mode({"point": "pointer", "scroll": "scroll", "gest": "swipe"}[v])),
                                 seg_width=150)
        th = QHBoxLayout(); th.addStretch(); th.addWidget(tabs); th.addStretch()
        lay.addLayout(th)
        P = self.tp_preview
        R = lambda title, desc, ctl, mode: SettingRow("", "", title, desc, ctl, preview=P, preview_mode=mode)

        point = self.create_card([
            R("Швидкість курсора", "", self.sl("tp_speed", 0, 100, labels=("Повільно", "Швидко"), ticks=10, width=240), "pointer"),
            R("Прискорення", "Адаптивне — точніше на повільних рухах", self.seg("tp_accel", [("adaptive", "Адаптивне"), ("flat", "Рівне")]), "pointer"),
            R("Клік дотиком", "Торкання одним пальцем = клік", self.sw("tp_tap", True), "tap"),
            R("Додатковий клік", "Правий клік", self.seg("tp_secondary", [("fingers", "Двома пальцями"), ("corner", "У правому куті")]), "secondary"),
            R("Перетягування дотиком", "Подвійний дотик і рух — перетягнути вікно чи файл", self.sw("tp_tap_drag", True), "drag"),
            R("Фіксація перетягування", "Можна відпустити палець і продовжити тягнути", self.sw("tp_drag_lock"), "drag"),
            R("Середній клік", "Одночасне натискання лівої та правої кнопки", self.sw("tp_middle"), "secondary"),
            R("Вимикати під час набору", "Ігнорувати випадкові дотики, коли друкуєте", self.sw("tp_dwt"), "typing"),
        ])
        scroll = self.create_card([
            R("Природний напрямок", "Вміст рухається за пальцями (як на телефоні)", self.sw("tp_natural"), "scroll"),
            R("Швидкість прокрутки", "", self.sl("tp_scroll", 20, 200, "%", width=220), "scroll"),
        ])
        gest = self.create_card([
            R("Змах між робочими столами", "Трьома пальцями вліво / вправо", None, "swipe"),
            R("Інвертувати змах", "Стіл їде в бік руху пальців", self.sw("tp_swipe_invert"), "swipe"),
            R("Довжина змаху", "Скільки треба провести, щоб перемкнути стіл", self.sl("tp_swipe_dist", 150, 600, " px", width=220), "swipe"),
            R("Створювати новий стіл", "Змах за останній стіл відкриває новий", self.sw("tp_swipe_new", True), "swipe"),
            R("Кілька столів одним змахом", "Не зупинятися на сусідньому столі", self.sw("tp_swipe_forever", True), "swipe"),
            R("Пошук (Runner)", "Три пальці вгору — відкрити, вниз — сховати", None, "runner"),
        ])
        self.tp_stack = QStackedWidget()
        for c in (point, scroll, gest):
            w = QWidget(); l = QVBoxLayout(w); l.setContentsMargins(0, 0, 0, 0); l.addWidget(c); l.addStretch()
            self.tp_stack.addWidget(w)
        lay.addWidget(self.tp_stack)
        self.tp.ensure_source()
        return page

    # --- Миша ---
    def mm_driver_wanted(self):
        g = self.tp.g
        return {"emulate_scroll_wheel": "Y" if g("mm_scroll") else "N",
                "scroll_speed": str(max(0, min(63, int(g("mm_scroll_speed"))))),
                "scroll_acceleration": "Y" if g("mm_scroll_accel") else "N",
                "emulate_3button": "Y" if g("mm_middle") else "N"}

    def mm_driver_status(self):
        want = self.mm_driver_wanted()
        conf = Path("/etc/modprobe.d/mgos-magicmouse.conf")
        line = "options hid_magicmouse " + " ".join(f"{k}={v}" for k, v in want.items())
        try:
            saved = conf.read_text().strip() == line
        except Exception:
            saved = False
        if not MM_PARAMS.exists():
            return saved, "Збережено — спрацює, щойно миша підключиться" if saved else "Ще не застосовано (потрібен пароль)"
        try:
            live = all((MM_PARAMS / k).read_text().strip() == v for k, v in want.items())
        except Exception:
            live = False
        ok = saved and live
        return ok, "Драйвер налаштовано" if ok else "Є незастосовані зміни (потрібен пароль)"

    def refresh_mm_status(self, *_):
        if hasattr(self, "mm_status_row"):
            ok, text = self.mm_driver_status()
            self.mm_status_row.desc_lbl.setText(("✓ " if ok else "• ") + text)

    def apply_mm_driver(self):
        """Параметри модуля hid_magicmouse: назавжди (/etc/modprobe.d) і наживо (/sys/module/...)."""
        want = self.mm_driver_wanted()
        opts = " ".join(f"{k}={v}" for k, v in want.items())
        live = "; ".join(f"echo {v} > /sys/module/hid_magicmouse/parameters/{k}" for k, v in want.items())
        root = (f"echo 'options hid_magicmouse {opts}' > /etc/modprobe.d/mgos-magicmouse.conf && "
                f"if [ -d /sys/module/hid_magicmouse ]; then {live}; else modprobe hid_magicmouse 2>/dev/null; true; fi")
        cmd = (f"echo 'Apple Magic Mouse: застосовую параметри драйвера (потрібен пароль)'; "
               f"sudo sh -c {json.dumps(root)} && echo '✓ Готово' || echo '✗ Не вдалося'; sleep 1.2")
        subprocess.Popen(["kitty", "--class", "mgos-recovery", "-e", "bash", "-c", cmd], start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        QTimer.singleShot(4000, self.refresh_mm_status)
        QTimer.singleShot(15000, self.refresh_mm_status)

    def create_mouse_page(self):
        page, lay = self.page_base("mouse")
        magic = self.tp.magic_mice()
        self.hero(lay, "mouse", "Швидкість, прокрутка й кнопки миші. Окремі налаштування для Apple Magic Mouse.")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Швидкість курсора", "", self.sl("ms_speed", 0, 100, labels=("Повільно", "Швидко"), ticks=10, width=240)),
            SettingRow("", "", "Прискорення", "Рівне — для ігор і точної роботи", self.seg("ms_accel", [("flat", "Рівне"), ("adaptive", "Адаптивне")])),
            SettingRow("", "", "Природна прокрутка", "Вміст рухається за коліщатком, як на трекпаді", self.sw("ms_natural")),
            SettingRow("", "", "Швидкість прокрутки", "", self.sl("ms_scroll", 20, 300, "%", width=220)),
            SettingRow("", "", "Миша для лівої руки", "Поміняти ліву й праву кнопки", self.sw("ms_left")),
        ]))

        self.section(lay, "Apple Magic Mouse")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Apple Magic Mouse", "Окрема сторінка: жести на поверхні, вторинний клік, прокрутка дотиком",
                       on_click=lambda: self.open_page("magicmouse")),
        ]))
        return page

    def mm_battery(self):
        """Заряд Magic Mouse з upower (None — не підключена / невідомо)."""
        try:
            for dev in subprocess.run(["upower", "-e"], capture_output=True, text=True, timeout=2).stdout.split():
                if "mouse" not in dev: continue
                info = subprocess.run(["upower", "-i", dev], capture_output=True, text=True, timeout=2).stdout
                if "magic" not in info.lower(): continue
                m = re.search(r"percentage:\s*(\d+)", info)
                if m: return int(m.group(1))
        except Exception:
            pass
        return None

    def create_magicmouse_page(self):
        page, lay = self.page_base("magicmouse")
        magic = self.tp.magic_mice()
        if magic:
            bat = self.mm_battery()
            state = "Підключено" + (f" · заряд {bat}%" if bat is not None else "")
        else:
            state = "Не підключена — увімкніть мишу (зелений індикатор) і підключіть у Bluetooth"
        self.hero(lay, "magicmouse", "Apple Magic Mouse: курсор, клік правою чи лівою стороною, прокрутка дотиком. "
                  "Наведіть на пункт — картинка покаже, що він робить.",
                  LinkButton("Bluetooth", lambda: self.launch_app("blueman-manager")))
        P = self.mm_preview = MagicMousePreview(self.tp)
        lay.addWidget(GroupCard([P]))
        row = lambda title, desc, ctl, mode: SettingRow("", "", title, desc, ctl, preview=P, preview_mode=mode)
        lay.addWidget(self.create_card([
            row("Стан", state, None, "point"),
        ]))
        self.section(lay, "Наведення і клік")
        lay.addWidget(self.create_card([
            row("Швидкість курсора", "Magic Mouse зазвичай повільніша за звичайну мишу",
                self.sl("mm_speed", 0, 100, labels=("Повільно", "Швидко"), ticks=10, width=240), "point"),
            row("Прискорення", "Рівне — для точної роботи", self.seg("mm_accel", [("adaptive", "Адаптивне"), ("flat", "Рівне")]), "point"),
            row("Вторинний клік", "Яким боком миші відкривати контекстне меню",
                self.seg("mm_left", [(False, "Правий бік"), (True, "Лівий бік")]), "secondary"),
            row("Середній клік", "Натискання посередині миші = середня кнопка (вкладка, вставлення)", self.sw("mm_middle"), "middle"),
        ]))
        self.section(lay, "Прокрутка дотиком")
        lay.addWidget(self.create_card([
            row("Прокрутка дотиком", "Проведіть пальцем по поверхні миші — прокрутка в усі боки", self.sw("mm_scroll", True), "scroll"),
            row("Природна прокрутка", "Як у macOS: сторінка їде за пальцем", self.sw("mm_natural", True), "scroll"),
            row("Швидкість прокрутки", "", self.sl("mm_scroll_speed", 0, 63, "", labels=("Повільно", "Швидко"), ticks=8, width=240), "scroll_h"),
            row("Інерція прокрутки", "Швидкий змах прокручує далі", self.sw("mm_scroll_accel", True), "inertia"),
        ]))
        self.section(lay, "Драйвер")
        ok, text = self.mm_driver_status()
        self.mm_status_row = SettingRow("", "", "Застосувати до драйвера", ("✓ " if ok else "• ") + text,
                                        LinkButton("Застосувати", self.apply_mm_driver))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Що це", "Прокрутка дотиком, інерція і середній клік — параметри драйвера hid_magicmouse "
                       "(потрібен пароль один раз); швидкість, прискорення й бік кліку застосовуються одразу"),
            self.mm_status_row,
        ]))
        return page

    # --- Док ---
    def zram_status_text(self):
        try:
            z = json.loads(subprocess.run([str(Path.home() / ".local/bin/mgos-zram"), "json"], capture_output=True, text=True, timeout=3).stdout)
        except Exception:
            z = {}
        if not z.get("enabled"):
            return "zram ще не налаштовано — натисніть «Налаштувати zram» нижче"
        t = f"zram {z['size_mb'] / 1024:.1f} ГБ · зараз {z['orig_mb']} МБ даних займають {z['used_mb']} МБ"
        return t if z.get("pageout") else t + " · стискання схованих — після «Налаштувати zram»"

    def write_close_binds(self, stash):
        """Super+Q: закрити (звично) або сховати в сховище доку; Shift+Super+Q — закрити назовсім."""
        conf = Path.home() / ".config/hypr/mgos-close.conf"
        if stash:
            body = ("# Генерується «Налаштуваннями» (Док → «Super+Q ховає програму в сховище»). Режим сховища:\n"
                    "bind = SUPER, Q, exec, ~/.local/bin/mgos-close\n"
                    "bind = SUPER SHIFT, Q, killactive\n")
            live = ["keyword bind SUPER,Q,exec,~/.local/bin/mgos-close", "keyword bind SUPER SHIFT,Q,killactive"]
        else:
            body = ("# Генерується «Налаштуваннями» (Док → «Super+Q ховає програму в сховище»). Звичайний режим:\n"
                    "bind = SUPER, Q, killactive\n"
                    "bind = SUPER SHIFT, Q, exec, hyprctl kill\n")
            live = ["keyword bind SUPER,Q,killactive", "keyword bind SUPER SHIFT,Q,exec,hyprctl kill"]
        try:
            conf.write_text(body)
        except Exception as e:
            print("mgos-close.conf:", e)
        subprocess.Popen(["hyprctl", "--batch", " ; ".join(["keyword unbind SUPER,Q", "keyword unbind SUPER SHIFT,Q"] + live)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # --- Екран входу (SDDM) ---
    def sddm_conf(self):
        """Значення для theme.conf теми mgOS Pixel — ті самі, що пише sync_sddm.sh."""
        g = lambda k: self.settings.get(k, PANEL_DEFAULTS.get(k))
        acc = self.settings.get("accent_color", "") if self.settings.get("accent_mode") == "custom" else ""
        if not re.fullmatch(r"#[0-9A-Fa-f]{6}", str(acc)):
            acc = "#9929CF"
            try:
                r = subprocess.run([str(Path.home() / ".local/bin/mgos-accent")], capture_output=True, text=True, timeout=3)
                if re.fullmatch(r"#[0-9A-Fa-f]{6}", r.stdout.strip()): acc = r.stdout.strip()
            except Exception:
                pass
        poster = subprocess.run([str(Path.home() / ".local/bin/mgos-wp-poster"), "--cached"], capture_output=True, text=True).stdout.strip()
        tf = lambda v: "true" if v else "false"
        return {"Background": QUrl.fromLocalFile(poster).toString() if poster else "Backgrounds/wallpaper.jpg",
                "BackgroundMode": g("sddm_bg_mode"), "PixelSize": g("sddm_pixel_size"), "Dim": g("sddm_dim"),
                "Blur": tf(g("sddm_blur")), "Accent": acc, "ClockStyle": g("sddm_clock_style"),
                "Clock24": tf(g("sddm_clock24")), "ShowSeconds": tf(g("sddm_seconds")), "PixelAvatar": tf(g("sddm_pixel_avatar"))}

    def sddm_changed(self, sync=True):
        if getattr(self, "sddm_preview", None) is not None:
            self.sddm_preview.update_config(self.sddm_conf())
            classic = self.settings.get("sddm_theme", "pixel") == "classic"
            self.sddm_preview.setVisible(not classic)
            if hasattr(self, "sddm_pixel_card"): self.sddm_pixel_card.setEnabled(not classic)
        if not sync:
            return
        # застосувати в системі (root-скрипт через sudo -n), з дебаунсом — повзунки шлють багато змін
        if not hasattr(self, "_sddm_sync"):
            self._sddm_sync = QTimer(self); self._sddm_sync.setSingleShot(True)
            self._sddm_sync.timeout.connect(lambda: subprocess.Popen(
                ["sudo", "-n", str(Path.home() / ".config/megumin-os/sync_sddm.sh")],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        self._sddm_sync.start(900)

    def create_login_page(self):
        page, lay = self.page_base("login")
        self.hero(lay, "login", "Екран входу в систему (SDDM): мінімалістична тема mgOS у стилі macOS, фон — ваші шпалери, плавний перехід із заставки.")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Тема", "mgOS — мінімалістична, як на macOS; класична — попередня тема",
                       self.seg("sddm_theme", [("pixel", "mgOS"), ("classic", "Класична")])),
        ]))
        try:
            from sddm_preview import SddmPreview
            self.sddm_preview = SddmPreview(560)
            box = QWidget(); bl = QHBoxLayout(box); bl.setContentsMargins(0, 6, 0, 6)
            bl.addStretch(); bl.addWidget(self.sddm_preview); bl.addStretch()
            lay.addWidget(box)
        except Exception as e:
            self.sddm_preview = None
            print("sddm preview:", e)
        self.section(lay, "Фон")
        self.sddm_pixel_card = self.create_card([
            SettingRow("", "", "Фон", "Шпалери — ті самі, що на робочому столі (для відео — перший кадр, тож вхід без «стрибка»)",
                       self.seg("sddm_bg_mode", [("wallpaper", "Шпалери"), ("pixel", "Піксельні"), ("waves", "Хвилі")])),
            SettingRow("", "", "Розмір пікселя", "Для піксельних шпалер і хвиль", self.sl("sddm_pixel_size", 3, 16, " px")),
            SettingRow("", "", "Затемнення", "", self.sl("sddm_dim", 0, 80)),
            SettingRow("", "", "Розмиття фону", "", self.sw("sddm_blur", False)),
        ])
        lay.addWidget(self.sddm_pixel_card)
        self.section(lay, "Годинник")
        lay.addWidget(self.create_card([
            SettingRow("", "", "24-годинний формат", "", self.sw("sddm_clock24", True)),
            SettingRow("", "", "Секунди", "", self.sw("sddm_seconds", True)),
        ]))
        self.section(lay, "Застосування")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Застосувати зараз", "Зміни застосовуються самі; кнопка — оновити шпалери, акцент і аватарку на екрані входу",
                       on_click=lambda: self.sddm_changed()),
        ]))
        if self.sddm_preview is not None:   # прев'ю малюється лише коли сторінку відкрито
            self.sddm_preview.update_config(self.sddm_conf())
            self.sddm_preview.setVisible(self.settings.get("sddm_theme", "pixel") != "classic")
        return page

    def create_dock_page(self):
        page, lay = self.page_base("dock")
        self.hero(lay, "dock", "Вигляд доку, збільшення іконок, закриті програми й недавні файли.")
        self.dock_preview = DockPreview(self.settings)
        lay.addWidget(GroupCard([self.dock_preview]))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Розмір іконок", "", self.sl("dock_icon_size", 28, 56, " px")),
            SettingRow("", "", "Збільшення", "Іконки ростуть під курсором", self.sw("dock_magnify", True)),
            SettingRow("", "", "Сила збільшення", "", self.sl("dock_magnify_pct", 110, 220)),
            SettingRow("", "", "Прозорість фону", "0% — повністю прозорий", self.sl("dock_opacity", 0, 100)),
        ]))
        self.section(lay, "Поведінка")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Док відсуває вікна", "Увімкнено — док завжди видно, вікна закінчуються над ним. "
                       "Вимкнено — док ховається і виїжджає поверх вікон", self.sw("dock_reserve", False)),
            SettingRow("", "", "Ховати програму перетягуванням на док",
                       "Перетягніть вікно на док — програма лишиться відкритою, але без вікна. Клік по іконці — повернути",
                       self.sw("dock_minimize_drag", True)),
        ]))
        self.section(lay, "Сховище (Super+A)")
        self.zram_row = SettingRow("", "", "Стискати сховані програми (zram)", self.zram_status_text(),
                                   self.sw("dock_stash_zram", True))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Super+Q ховає програму в сховище",
                       "Вікно зникає, програма лишається відкритою — повернути кліком по іконці в доку або Super+A. "
                       "Закрити назовсім — Shift+Super+Q", self.sw("dock_q_stash", False)),
            self.zram_row,
            SettingRow("", "", "Налаштувати zram", "Стиснена пам'ять у RAM замість swap на диску (потрібен пароль адміністратора)",
                       on_click=lambda: self.launch_app(f"kitty --class mgos-zram-setup -e sudo {Path.home()}/.local/bin/mgos-zram setup")),
        ]))
        self.section(lay, "Рамка")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Рамка", "Контур доку", self.sw("dock_border", True)),
            SettingRow("", "", "Стиль рамки", "Тонка — як у топбару; акцентна — кольорова",
                       self.seg("dock_border_style", [("subtle", "Тонка"), ("accent", "Акцентна")])),
            SettingRow("", "", "Прозорість рамки", "", self.sl("dock_border_opacity", 0, 100)),
        ]))
        self.section(lay, "Закриті програми")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Показувати закриті програми", "Недавно закриті програми лишаються в доку після роздільника — клік запускає знову, СКМ прибирає",
                       self.sw("dock_recent", True)),
            SettingRow("", "", "Скільки закритих програм", "", self.sl("dock_recent_count", 1, 15, "")),
        ]))
        self.section(lay, "Недавні файли")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Показувати недавні файли", "ЛКМ — відкрити, СКМ — у теці, можна перетягнути", self.sw("dock_files", True)),
            SettingRow("", "", "Джерело", "", self.seg("dock_files_source", [("downloads", "Завантаження"), ("clipboard", "Скопійовані"), ("both", "Обидва")])),
            SettingRow("", "", "Кількість", "", self.sl("dock_files_count", 1, 8, "")),
            SettingRow("", "", "Не старші за", "Старіші файли з ~/Downloads не показуються", self.sl("dock_files_days", 1, 30, " дн")),
        ]))
        self.section(lay, "Поведінка")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Не ховати док для", "Док лишається, якщо ці застосунки активні",
                       DockWhitelistEditor(self.settings.get("dock_whitelist", ""), self.save_setting)),
            SettingRow("", "", "Перезапустити док", "Застосувати налаштування", on_click=lambda: self.launch_app("bash ~/.config/SYSui/restart_dock.sh")),
        ]))
        # наведення на рядок → прев'ю показує, на що він впливає
        modes = {"Розмір іконок": "size", "Збільшення": "magnify", "Сила збільшення": "magnify", "Прозорість фону": "bg",
                 "Док відсуває вікна": "reserve", "Ховати програму перетягуванням на док": "drag",
                 "Стискати сховані програми (zram)": "stash", "Super+Q ховає програму в сховище": "stash",
                 "Рамка": "border", "Стиль рамки": "border", "Прозорість рамки": "border",
                 "Показувати закриті програми": "recent", "Скільки закритих програм": "recent",
                 "Показувати недавні файли": "files", "Джерело": "files", "Кількість": "files", "Не старші за": "files"}
        for row in page.findChildren(SettingRow):
            if row.title in modes:
                row.preview, row.preview_mode = self.dock_preview, modes[row.title]
        return page

    # --- OSD і сповіщення ---
    def create_osd_page(self):
        page, lay = self.page_base("osd")
        self.hero(lay, "osd", "Де з'являються плашки гучності, яскравості й музики, і окремо — плашка сповіщень з іконками програм і сайтів.")
        P = OsdPreview(self.settings)
        self.osd_preview = P
        lay.addWidget(GroupCard([P]))
        opts = [("bottom", "Знизу"), ("top", "Зверху"), ("center", "Центр"), ("top-right", "Праворуч ↑"), ("bottom-right", "Праворуч ↓")]
        self.section(lay, "Плашки OSD")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Розташування", "Гучність, яскравість, підсвітка клавіатури, музика",
                       self.seg("osd_position", opts, seg_width=78), preview=P, preview_mode="osd"),
        ]))
        self.section(lay, "Сповіщення")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Розташування", "Окремо від плашок OSD — щоб вони не накладались",
                       self.seg("notify_position", opts, seg_width=78), preview=P, preview_mode="notify"),
            SettingRow("", "", "Іконки програм і сайтів",
                       "Програма — за її .desktop; сповіщення сайтів у браузері показують іконку сайту, браузер — значком у куті", None,
                       preview=P, preview_mode="notify"),
            SettingRow("", "", "Показати тестове сповіщення", "",
                       on_click=lambda: subprocess.Popen(["notify-send", "-a", "mgOS", "-h", "string:desktop-entry:megumin-settings",
                                                          "Тестове сповіщення", "Так виглядатиме плашка сповіщень"]),
                       preview=P, preview_mode="notify"),
            SettingRow("", "", "Показати плашку гучності", "",
                       on_click=lambda: subprocess.Popen(["bash", "-c", "echo show | socat - UNIX-CONNECT:$HOME/.config/SYSui/volume.sock"]),
                       preview=P, preview_mode="osd"),
        ]))
        return page

    # --- Панелі та віджети ---
    def create_panels_page(self):
        page, lay = self.page_base("panels")
        self.hero(lay, "panels", "Топбар, Control Center і поведінка модулів SYSui.")
        self.accent_wal_switch = MeguminSwitch("accent_mode", self.settings.get("accent_mode") != "custom", self.set_accent_from_wal)
        self.section(lay, "Топбар")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Стиль", "Бульбашки, крапки замість цифр або суцільна плашка як у macOS",
                       self.seg("topbar_style", [("bubbles", "Бульбашки"), ("dots", "Крапки"), ("macos", "macOS")])),
            SettingRow("", "", "Мова", "Як показувати розкладку", self.seg("topbar_lang", [("flag", "Прапорець"), ("text", "Букви"), ("icon", "Іконка")])),
            SettingRow("", "", "Прозорість фону", "0% — повністю прозорий", self.sl("topbar_opacity", 0, 100)),
            SettingRow("", "", "Рамки", "Тонкий контур бульбашок / нижній край плашки", self.sw("topbar_border", True)),
            SettingRow("", "", "Кнопка вимкнення", "⏻ ліворуч у топбарі. Меню живлення лишається в Control Center і на Super + O", self.sw("topbar_power", True)),
            SettingRow("", "", "Блок годинника", "Клік по годиннику: календар, музика, фокус-таймер",
                       on_click=lambda: self.launch_app("pkill -USR1 -x topbar")),
        ]))
        self.section(lay, "Control Center")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Прозорість панелі", "", self.sl("cc_opacity", 30, 100)),
            SettingRow("", "", "Акцент із шпалер", "Колір усіх панелей береться з pywal", self.accent_wal_switch),
            SettingRow("", "", "Сторож модулів", "Перезапускати модулі SYSui, якщо вони впали", self.sw("watchdog", True)),
            SettingRow("", "", "Системні сповіщення", "Спливаючі попередження про збої", self.sw("sys_popups", True)),
        ]))
        return page

    # --- Батарея ---
    def create_power_page(self):
        page, lay = self.page_base("power")
        bat = ""
        for d in glob.glob("/sys/class/power_supply/BAT*"):
            try:
                cap = open(d + "/capacity").read().strip()
                st = open(d + "/status").read().strip()
                bat = f"Заряд {cap}% · " + {"Charging": "заряджається", "Discharging": "від батареї", "Full": "заряджена"}.get(st, "від мережі")
            except Exception: pass
        self.hero(lay, "power", bat or "Режим енергоспоживання процесора.")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Режим живлення", "Економ — довше від батареї, Макс — для ігор",
                       PowerProfileSelector(self.settings.get("power_profile", "balanced"), self.save_setting)),
            SettingRow("", "", "Споживання енергії", "Що з'їдає батарею (btop)", on_click=lambda: self.launch_app("kitty -e btop")),
        ]))
        self.section(lay, "Економія та оптимізація")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Запечений блюр на весь екран",
                       "Поки вікно на весь екран, відео-шпалери на паузі — розмиття не перераховується і майже нічого не коштує, "
                       "а після виходу перехід непомітний",
                       self.sw("eco_fs_blur", True)),
        ]))
        self.section(lay, "Кулер")
        self.fan_selector = SegmentedSelector("fan_mode", [("auto", "Авто"), ("quiet", "Тихий"), ("balanced", "Баланс"),
                                                           ("performance", "Макс"), ("curve", "Крива")],
                                              fan_status().get("mode", "auto"), self.save_setting)
        self.fan_row = SettingRow("", "", "Режим кулера", "Зараз: …", self.fan_selector)
        rows = [self.fan_row]
        fs = fan_status()
        if fs.get("writable") != "1" or fs.get("split") != "1":
            rows.append(SettingRow("", "", "Дозволити керування кулером",
                                   "Один раз потрібен пароль: права на профіль прошивки кулера",
                                   LinkButton("Дозволити", self.install_fan_rule)))
        lay.addWidget(self.create_card(rows))
        # графік кривої: видно завжди, працює в режимі «Крива»
        self.fan_curve_card = QWidget(); cl = QVBoxLayout(self.fan_curve_card); cl.setContentsMargins(0, 0, 0, 0); cl.setSpacing(6)
        self.fan_curve = FanCurveWidget(lambda: self.settings.get("fan_mode") == "curve" and
                                        subprocess.Popen([MGOS_FAN, "set", "curve"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        hint = QLabel("Тягніть точки: при якій температурі які оберти. Клік по порожньому місцю — нова точка, правий клік — прибрати. "
                      "Прошивка Samsung вміє лише 4 рівні, тому реальні оберти змінюються сходинками за смугами.")
        hint.setProperty("role", "desc"); hint.setWordWrap(True)
        box = QWidget(); bl = QVBoxLayout(box); bl.setContentsMargins(14, 12, 14, 10); bl.setSpacing(6)
        bl.addWidget(self.fan_curve); bl.addWidget(hint)
        rl2 = QHBoxLayout(); rl2.addStretch(); rl2.addWidget(LinkButton("За замовчуванням", self.fan_curve.reset)); bl.addLayout(rl2)
        cl.addWidget(GroupCard([box]))
        lay.addWidget(self.fan_curve_card)
        self.settings["fan_mode"] = fan_status().get("mode", "auto")   # правда — у mgos-fan, не в settings.json
        self.fan_curve_card.setVisible(self.settings["fan_mode"] == "curve")
        self.refresh_fan()
        if not getattr(self, "fan_timer", None):
            self.fan_timer = QTimer(self, interval=2000)
            self.fan_timer.timeout.connect(self.refresh_fan)
            self.fan_timer.start()
        return page

    def refresh_fan(self):
        row = getattr(self, "fan_row", None)
        if row is None: return
        # оновлюємо лише видиму сторінку «Батарея» і лише коли вікно не згорнуте
        if getattr(self, "_fan_once", False) and not (row.isVisible() and self.isVisible()): return
        self._fan_once = True
        st = fan_status()
        rpm = int(st.get("rpm", "0") or 0)
        spin = f"{rpm} об/хв" if rpm else "зупинений"
        names = {"low-power": "економ", "quiet": "тихий", "balanced": "баланс", "performance": "макс"}
        txt = f"Зараз: {spin} · CPU {st.get('temp', '?')}°C · крива прошивки «{names.get(st.get('profile', ''), st.get('profile', '?'))}»"
        try: row.desc_lbl.setText(txt)
        except Exception: pass
        fc = getattr(self, "fan_curve", None)
        if fc is not None:
            try: fc.set_live(int(st.get("temp", "0") or 0), rpm)
            except Exception: pass

    def install_fan_rule(self):
        cmd = (f"echo 'mgOS: дозволяю керування кулером (потрібен пароль один раз)'; "
               f"sudo {MGOS_FAN} install && echo '✓ Готово' || echo '✗ Не вдалося'; sleep 1.2")
        subprocess.Popen(["kitty", "--class", "mgos-recovery", "-e", "bash", "-c", cmd], start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        def check(n=0):
            st = fan_status()
            if st.get("writable") == "1" and st.get("split") == "1":
                subprocess.Popen([MGOS_FAN, "set", self.settings.get("fan_mode", "auto")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.rebuild_page("power")
            elif n < 40: QTimer.singleShot(1500, lambda: check(n + 1))
        QTimer.singleShot(3000, check)

    # --- Система ---
    def create_system_page(self):
        page, lay = self.page_base("system")
        self.hero(lay, "system", "Оновлення, моніторинг і перезапуск оболонки mgOS.")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Про систему", "Залізо, версія mgOS", on_click=lambda: self.launch_app(f"echo show | socat - UNIX-CLIENT:{Path.home()}/.config/SYSui/about.sock")),
            SettingRow("", "", "Оновлення", "Пакети системи (yay)", on_click=lambda: self.launch_app("kitty -e yay")),
            SettingRow("", "", "Монітор ресурсів", "btop", on_click=lambda: self.launch_app("kitty -e btop")),
        ]))
        self.section(lay, "Оболонка")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Перезапустити Ignis", "Підняти оболонку після збою", on_click=lambda: self.launch_app("ignis quit; sleep 0.6; hyprctl dispatch exec 'ignis init'")),
            SettingRow("", "", "Перезапустити модулі SYSui", "Топбар, док, Control Center та OSD", on_click=lambda: self.launch_app("bash ~/.config/SYSui/restart_all.sh")),
        ]))
        self.section(lay, "Відновлення")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Відновити систему", "Свіжий інсталятор mgOS із GitHub: повернути конфіги, панелі й тему або перевстановити",
                       LinkButton("Відкрити інсталятор", lambda: self.launch_app("kitty --class mgos-recovery -e ~/.local/bin/mgos-recovery"))),
            SettingRow("", "", "Локальний інсталятор", "Копія, з якої систему було встановлено (~/.local/share/mgos/installer)",
                       LinkButton("Відкрити", self.launch_local_installer)),
        ]))
        return page

    # --- Системні звуки ---
    def create_sounds_page(self):
        page, lay = self.page_base("sounds")
        self.hero(lay, "sounds", "Звуки сповіщень, зарядки, гучності та знімків екрана. Тема mgOS — м'які «скляні» тони.")
        g = lambda k: self.settings.get(k, SOUND_DEFAULTS[k])
        self.snd_widgets = {
            "sounds_enabled": MeguminSwitch("sounds_enabled", bool(g("sounds_enabled")), self.save_setting),
            "sounds_volume": MeguminSlider("sounds_volume", g("sounds_volume"), 0, 100, "%", self.save_setting, width=200),
        }
        themes = list_sound_themes() or [("mgos", "mgOS")]
        self.snd_theme = MacPopup("sound_theme", themes, g("sound_theme"), self.save_setting, width=220)
        lay.addWidget(self.create_card([
            SettingRow("", "", "Системні звуки", "Увімкнено за замовчуванням", self.snd_widgets["sounds_enabled"]),
            SettingRow("", "", "Гучність системних звуків", "Окремо від гучності музики й відео", self.snd_widgets["sounds_volume"]),
            SettingRow("", "", "Звукова тема", "Застосунки GTK і Firefox теж її використовують", self.snd_theme),
            SettingRow("", "", "Звук натискань у меню", "Клацання кнопок у застосунках GTK", self.sw("sounds_input", False)),
            SettingRow("", "", "Вібрація", "У цьому ноутбуці й у Magic Mouse немає вібромотора — вмикати нічого", None),
        ]))
        self.section(lay, "Звуки подій")
        off = set(self.settings.get("sounds_off", []))
        rows = []
        for ev, title, desc in SOUND_EVENTS:
            box = QWidget(); bl = QHBoxLayout(box); bl.setContentsMargins(0, 0, 0, 0); bl.setSpacing(12)
            bl.addWidget(LinkButton("▶  Прослухати", lambda e=ev: subprocess.Popen([str(Path.home() / ".local/bin/mgos-sound"), e, "--force"])))
            bl.addWidget(MeguminSwitch(f"snd:{ev}", ev not in off, self.toggle_sound_event))
            rows.append(SettingRow("", "", title, desc, box))
        lay.addWidget(self.create_card(rows))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Скинути звуки", "Тема mgOS, гучність 70%, усі події увімкнено", LinkButton("За замовчуванням", self.reset_sounds)),
        ]))
        return page

    def toggle_sound_event(self, key, on):
        ev = key.split(":", 1)[1]
        off = set(self.settings.get("sounds_off", []))
        (off.discard if on else off.add)(ev)
        self.save_setting("sounds_off", sorted(off))

    def reset_sounds(self):
        for k, v in SOUND_DEFAULTS.items(): self.save_setting(k, v)
        self.save_setting("sounds_off", [])
        self.rebuild_page("sounds")

    def apply_sounds(self):
        on = bool(self.settings.get("sounds_enabled", True))
        th = self.settings.get("sound_theme", "mgos") or "mgos"
        cmds = [["gsettings", "set", "org.gnome.desktop.sound", "event-sounds", "true" if on else "false"],
                ["gsettings", "set", "org.gnome.desktop.sound", "theme-name", th],
                ["gsettings", "set", "org.gnome.desktop.sound", "input-feedback-sounds",
                 "true" if on and self.settings.get("sounds_input", False) else "false"]]
        for c in cmds: subprocess.Popen(c, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        set_gtk_ini("gtk-enable-event-sounds", 1 if on else 0)
        set_gtk_ini("gtk-sound-theme-name", th)

    # --- Ігри: MFG Unlocker ---
    def create_games_page(self):
        page, lay = self.page_base("games")
        self.hero(lay, "games", "MFG Unlocker — DLSS / XeSS Multi Frame Generation у Windows-іграх через Proton.")
        cfg = mfg_load()
        self.mfg_cfg = cfg
        mod = Path(os.path.expanduser(cfg["mod_dll"]))
        launch = f"{MFG_RUN} %command%"

        self.mfg_mod_row = SettingRow("", "", "Файл мода",
                                      (f"✓ {mod}" if mod.exists() else f"• Немає: {mod}. Завантажте MFGUnlocker.dll з GitHub Releases"),
                                      LinkButton("Вибрати…", self.mfg_pick_dll))
        ask = MeguminSwitch("mfg_ask", bool(cfg.get("ask", True)), lambda k, v: self.mfg_set(ask=bool(v)))
        lay.addWidget(self.create_card([
            self.mfg_mod_row,
            SettingRow("", "", "Питати перед запуском", "Вікно «З модом / Без мода» (через 15 с — з модом)", ask),
            SettingRow("", "", "Параметри запуску Steam", f"Steam → Властивості гри → Параметри запуску:  {launch}",
                       LinkButton("Скопіювати", lambda: (subprocess.run(["wl-copy", launch]),
                                                        subprocess.Popen(["notify-send", "-a", "Налаштування", "Скопійовано", launch])))),
            SettingRow("", "", "Відновити файли ігор", "Прибрати мод і повернути оригінальні DLL (після краша гри)",
                       LinkButton("Відновити", lambda: subprocess.Popen([MFG_RUN, "--restore-all"]))),
            SettingRow("", "", "Сумісність", "Потрібна гра з DLSS Frame Generation (RTX 20–50) або XeMFG; не для ігор з анти-читом. "
                       "Назву DLL для кожної гри — у docs/compatibility.md", LinkButton("Відкрити",
                       lambda: self.launch_app("xdg-open https://github.com/dashdogy/MFG-Unlocker/blob/main/docs/compatibility.md"))),
        ]))

        self.section(lay, "Ігри Steam")
        games = steam_games()
        rows = []
        for aid, name in games:
            g = cfg["games"].get(aid, {})
            ctl = QWidget(); h = QHBoxLayout(ctl); h.setContentsMargins(0, 0, 0, 0); h.setSpacing(10)
            pop = MacPopup(f"mfg_dll:{aid}", [(d, d) for d in MFG_DLL_NAMES], g.get("dll", "version.dll"),
                           lambda k, v, a=aid, n=name: self.mfg_game(a, n, dll=v), width=130)
            sw = MeguminSwitch(f"mfg_on:{aid}", bool(g.get("enabled")), lambda k, v, a=aid, n=name: self.mfg_game(a, n, enabled=bool(v)))
            h.addWidget(pop); h.addWidget(sw)
            rows.append(SettingRow("", "", name, f"AppID {aid}" + (f" · тека: {g['exe_dir']}" if g.get("exe_dir") else ""), ctl))
        if not rows:
            rows.append(SettingRow("", "", "Ігор Steam не знайдено", "Встановіть гру в Steam і відкрийте цю сторінку знову"))
        lay.addWidget(self.create_card(rows))
        return page

    def mfg_set(self, **kw):
        c = mfg_load(); c.update(kw); mfg_save(c); self.mfg_cfg = c

    def mfg_game(self, appid, name, **kw):
        c = mfg_load()
        g = c["games"].setdefault(appid, {"name": name, "enabled": False, "dll": "version.dll", "exe_dir": ""})
        g["name"] = name; g.update(kw)
        mfg_save(c); self.mfg_cfg = c

    def mfg_pick_dll(self):
        path, _ = QFileDialog.getOpenFileName(self, "MFGUnlocker.dll", str(Path.home() / "Downloads"), "DLL (*.dll)")
        if not path: return
        MFG_DEFAULT_DLL.parent.mkdir(parents=True, exist_ok=True)
        try:
            if Path(path).resolve() != MFG_DEFAULT_DLL.resolve(): shutil.copy2(path, MFG_DEFAULT_DLL)
            self.mfg_set(mod_dll=str(MFG_DEFAULT_DLL))
        except Exception:
            self.mfg_set(mod_dll=path)
        self.rebuild_page("games")

    # --- Довідка: усі бінди ---
    # --- Розробник: перегляд привітання та інших «одноразових» екранів, щоб поклацати ---
    def create_developer_page(self):
        page, lay = self.page_base("developer")
        self.hero(lay, "developer", "Інструменти для перевірки: привітання першого входу, OSD, звуки. Нічого не змінюють у системі, "
                                    "крім налаштувань, які ви самі виберете в привітанні.")
        W = str(Path.home() / ".config/SYSui/welcome")
        S = Path.home() / ".config/SYSui"
        def run(*cmd):
            return lambda: subprocess.Popen(list(cmd), start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        def sh(cmd):
            return lambda: subprocess.Popen(cmd, shell=True, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.section(lay, "Привітання mgOS")
        lay.addWidget(self.create_card([
            SettingRow("", "", "Повністю, з інтро", "Темніння → сяйво → «Привіт» мовами → налаштування. Esc — вийти", LinkButton("Запустити", run(W, "--preview"))),
            SettingRow("", "", "Вигляд", "Тема і акцентний колір", LinkButton("Відкрити", run(W, "--preview", "--page", "0"))),
            SettingRow("", "", "Wi-Fi", "Список мереж і підключення", LinkButton("Відкрити", run(W, "--preview", "--page", "1"))),
            SettingRow("", "", "Шпалери", "Піксельні та зображення, прев'ю у фоні", LinkButton("Відкрити", run(W, "--preview", "--page", "2"))),
            SettingRow("", "", "Робочий стіл", "Блюр, звуки, док, живлення, шрифт", LinkButton("Відкрити", run(W, "--preview", "--page", "3"))),
            SettingRow("", "", "Усе готово", "Гарячі клавіші; «Почати роботу» — згасання і виїзд панелей", LinkButton("Відкрити", run(W, "--preview", "--page", "4"))),
            SettingRow("", "", "Як після встановлення", "Те саме, що бачить новий користувач (позначку «показано» не ставить)",
                       LinkButton("Запустити", run(W, "--preview"))),
        ]))
        self.section(lay, "Інше")
        lay.addWidget(self.create_card([
            SettingRow("", "", "OSD гучності", "", LinkButton("Показати", sh(f"echo show | socat - UNIX-CLIENT:{S}/volume.sock"))),
            SettingRow("", "", "OSD яскравості", "", LinkButton("Показати", sh(f"echo show | socat - UNIX-CLIENT:{S}/brightness.sock"))),
            SettingRow("", "", "Тестове сповіщення", "", LinkButton("Надіслати", run("notify-send", "-a", "mgOS", "Тестове сповіщення", "Так виглядає плашка повідомлення"))),
            SettingRow("", "", "Звук входу", "", LinkButton("Програти", run(str(Path.home() / ".local/bin/mgos-sound"), "desktop-login"))),
            SettingRow("", "", "Меню живлення", "Super + O", LinkButton("Показати", sh(f"echo show | socat - UNIX-CLIENT:{S}/powermenu.sock"))),
            SettingRow("", "", "Перезапустити SYSui", "Док, топбар, OSD, Control Center", LinkButton("Перезапустити", run(str(S / "restart_all.sh")))),
        ]))
        lay.addStretch()
        return page

    def create_help_page(self):
        page, lay = self.page_base("help")
        try:
            binds = json.loads(subprocess.run(["hyprctl", "binds", "-j"], capture_output=True, text=True, timeout=2).stdout)
        except Exception:
            binds = []
        self.hero(lay, "help", f"Усі сполучення клавіш mgOS — {len(binds)} біндів Hyprland, наживо з поточного конфігу. "
                               "Пошук зліва шукає і тут.")
        order = ["Застосунки", "Вікна", "Робочі столи", "Знімки екрана", "Звук і медіа", "Оформлення", "Система", "Інше"]
        groups = {c: [] for c in order}
        seen = set()
        for b in binds:
            text, cat = describe_bind(b)
            keys = bind_keys(b)
            sig = (tuple(keys), text)
            if sig in seen: continue           # однакові бінди з різних файлів — показуємо раз
            seen.add(sig)
            flags = [n for f, n in (("locked", "працює на заблокованому екрані"), ("repeat", "повторюється при утриманні"),
                                    ("release", "на відпускання")) if b.get(f)]
            raw = b.get("arg", "") if b.get("dispatcher") == "exec" else f"{b.get('dispatcher', '')} {b.get('arg', '')}".strip()
            desc = (raw[:110] + "…" if len(raw) > 110 else raw) + (" · " + ", ".join(flags) if flags else "")
            groups.setdefault(cat, []).append((text, desc, keys))
        for cat in order:
            items = sorted(groups.get(cat, []), key=lambda t: t[0])
            if not items: continue
            self.section(lay, f"{cat} · {len(items)}")
            lay.addWidget(self.create_card([SettingRow("", "", t, d, KeyChips(k)) for t, d, k in items]))

        self.section(lay, "Поза Hyprland")
        extra = [("Перемикання розкладки", "XKB: grp:alt_shift_toggle", ["Alt", "Shift"]),
                 ("Підсвітка клавіатури", "Прошивка ноутбука (3 рівні + вимк.)", ["Fn", "F9"]),
                 ("Яскравість екрана", "Fn-клавіші → XF86MonBrightness", ["Fn", "F2 / F3"]),
                 ("Робочі столи", "Жест тачпада", ["3 пальці", "← →"]),
                 ("Пошук і запуск (Runner)", "Жест тачпада", ["3 пальці", "↑"]),
                 ("Сховати Runner", "Жест тачпада", ["3 пальці", "↓"]),
                 ("Меню MFG Unlocker у грі", "Лише в іграх з MFG Unlocker", ["Backspace"])]
        lay.addWidget(self.create_card([SettingRow("", "", t, d, KeyChips(k)) for t, d, k in extra]))
        return page

    # --- Про систему ---
    def create_about_page(self):
        page, lay = self.page_base("about")
        rd = lambda f: (Path(f).read_text(errors="ignore").strip() if Path(f).exists() else "")
        sh = lambda c: (subprocess.run(c, shell=True, capture_output=True, text=True, timeout=2).stdout.strip())
        vendor = rd("/sys/class/dmi/id/sys_vendor").replace("SAMSUNG ELECTRONICS CO., LTD.", "Samsung")
        model = (rd("/sys/class/dmi/id/product_family") or "") + " " + rd("/sys/class/dmi/id/product_name")
        title = f"{vendor} {model.strip()}".strip() or "Комп'ютер"
        cpu = next((l.split(":", 1)[1].strip() for l in rd("/proc/cpuinfo").splitlines() if l.startswith("model name")), "")
        cpu = cpu.replace("(R)", "").replace("(TM)", "").replace(" CPU", "").replace("11th Gen ", "").replace("12th Gen ", "").replace("13th Gen ", "")
        cores = os.cpu_count() or 0
        mem = next((int(l.split()[1]) for l in rd("/proc/meminfo").splitlines() if l.startswith("MemTotal")), 0)
        gpu = ""
        try:
            for l in sh("lspci -mm").splitlines():
                if "VGA" in l or "3D controller" in l or "Display controller" in l:
                    parts = [x.strip('"') for x in shlex.split(l)]
                    name = parts[3].split("[")[-1].rstrip("]") if "[" in parts[3] else parts[3]
                    gpu = f"{gpu} · {name}" if gpu else name
        except Exception: pass
        try:
            st = os.statvfs("/")
            disk = f"{st.f_bavail * st.f_frsize / 1e9:.0f} ГБ вільно з {st.f_blocks * st.f_frsize / 1e9:.0f} ГБ"
        except Exception: disk = ""
        mons = hypr_monitors()
        disp = " · ".join(f"{m.get('width')}×{m.get('height')} @ {round(m.get('refreshRate', 60))} Гц, масштаб {fmt_scale(float(m.get('scale', 1)))}" for m in mons)
        inst = Path.home() / ".local/share/mgos/installer"
        mg = sh(f"git -C '{inst}' log -1 --format='%cd · %h' --date=format:'%d.%m.%Y'") if inst.exists() else ""
        try: hv = json.loads(sh("hyprctl version -j")).get("tag", "").lstrip("v")
        except Exception: hv = ""
        try:
            up = float(rd("/proc/uptime").split()[0]); upt = f"{int(up // 3600)} год {int(up % 3600 // 60)} хв"
        except Exception: upt = ""
        bat = ""
        for b in Path("/sys/class/power_supply").glob("BAT*"):
            try:
                full, design = int(rd(b / "energy_full") or rd(b / "charge_full")), int(rd(b / "energy_full_design") or rd(b / "charge_full_design"))
                bat = f"{rd(b / 'capacity')}% · здоров'я {round(full * 100 / design)}% · циклів {rd(b / 'cycle_count') or '—'}"
            except Exception: pass
        self.hero(lay, "about", title)
        val = lambda t: (lambda l: (l.setProperty("role", "value"), l.setWordWrap(True), l.setMaximumWidth(330),
                                     l.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter),
                                     l.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse), l)[-1])(QLabel(t))
        info = [("Процесор", f"{cpu} · {cores} потоків" if cpu else ""), ("Графіка", gpu), ("Пам'ять", f"{mem / 1048576:.0f} ГБ" if mem else ""),
                ("Накопичувач", disk), ("Екран", disp), ("Батарея", bat)]
        lay.addWidget(self.create_card([SettingRow("", "", k, "", val(v)) for k, v in info if v]))
        self.section(lay, "Програмне забезпечення")
        sw_rows = [("mgOS", mg or "—"), ("Ядро Linux", os.uname().release), ("Hyprland", hv), ("Ім'я комп'ютера", os.uname().nodename),
                   ("Час роботи", upt)]
        lay.addWidget(self.create_card([SettingRow("", "", k, "", val(v)) for k, v in sw_rows if v]))
        lay.addWidget(self.create_card([
            SettingRow("", "", "Детальніше", "Навантаження, температури, оновлення", LinkButton("Відкрити", lambda: self.launch_app(
                f"echo show | socat - UNIX-CLIENT:{Path.home()}/.config/SYSui/about.sock"))),
            SettingRow("", "", "Привітання mgOS", "Показати знову екран першого входу (Esc — закрити)",
                       LinkButton("Показати", lambda: subprocess.Popen([str(Path.home() / ".config/SYSui/welcome"), "--preview"],
                                                                       start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))),
            SettingRow("", "", "Скопіювати звіт", "Усе з цієї сторінки — для підтримки чи форуму", LinkButton("Скопіювати", lambda: QApplication.clipboard().setText(
                "\n".join([title] + [f"{k}: {v}" for k, v in info + sw_rows if v])))),
        ]))
        return page

    # ---------------- застосування шпалер (як було) ----------------
    def on_wallpaper_selected(self, index):
        path = self.wp_model.items[index.row()]
        self.current_wallpaper = path
        self.wp_list.viewport().update()
        # Запускаємо зміну шпалер у фоні, щоб не фрізити UI на час роботи ffmpeg/wal
        threading.Thread(target=self.apply_actual_wallpaper, args=(path,), daemon=True).start()

    def apply_actual_wallpaper(self, path):
        import datetime
        LAST_WALLPAPER_FILE = os.path.expanduser("~/.cache/ignis/last_wallpaper.txt")
        
        # Логіка для динамічних
        target_path = path
        if os.path.isdir(path):
            hour = datetime.datetime.now().hour
            period = "morning" if 6 <= hour < 12 else "day" if 12 <= hour < 18 else "evening" if 18 <= hour < 22 else "night"
            for ext in ['jpg', 'png', 'jpeg', 'webp', 'mp4', 'webm', 'gif']:
                test = os.path.join(path, f"{period}.{ext}")
                if os.path.exists(test): 
                    target_path = test
                    break

        with open(LAST_WALLPAPER_FILE, "w") as f: f.write(path)
        subprocess.run(["pkill", "-x", "pixel_wallpaper"], stderr=subprocess.DEVNULL)
        
        ext = target_path.lower().split('.')[-1]
        is_video = ext in ['mp4', 'webm', 'gif']
        
        if is_video:
            subprocess.run("pkill -9 mpvpaper; killall awww-daemon; rm -f /tmp/mpv-socket", shell=True, stderr=subprocess.DEVNULL)
            anime4k_enabled = self.settings.get("wp_anime4k", False)
            # Оптимізована команда для мінімального споживання RAM:
            # - profile=wallpaper: легші M-шейдери, 30fps, cache=no, idle priority
            # - config=yes: дозволяємо mpv.conf, але задаємо профіль явно
            # - nvdec: вся декодування на GPU, CPU не навантажений
            # - demuxer-max-bytes=512KiB: буфер в RAM мінімальний
            if anime4k_enabled:
                mpv_opts = (
                    "profile=wallpaper "
                    "input-ipc-server=/tmp/mpv-socket "
                    "loop-file=inf "
                    "hwdec=auto-safe "
                    "no-audio "
                    "panscan=1.0 "
                    "cache=no "
                    "demuxer-max-bytes=512KiB "
                    "demuxer-max-back-bytes=128KiB"
                )
            else:
                mpv_opts = (
                    "config=no "
                    "glsl-shaders= "
                    "input-ipc-server=/tmp/mpv-socket "
                    "loop-file=inf "
                    "hwdec=auto-safe "
                    "no-audio "
                    "panscan=1.0 "
                    "cache=no "
                    "demuxer-max-bytes=512KiB "
                    "demuxer-max-back-bytes=128KiB"
                )
            # nice -n 19: знижуємо пріоритет CPU до мінімального (ігри/робота мають перевагу)
            os.system(f"nohup nice -n 19 mpvpaper -o '{mpv_opts}' '*' '{target_path}' >/dev/null 2>&1 &")
            safe_name = os.path.basename(target_path).replace(" ", "_")
            frame_target = os.path.expanduser(f"~/.cache/ignis/sddm_frame_{safe_name}.png")
            subprocess.run(f"ffmpeg -i '{target_path}' -ss 00:00:00.000 -vframes 1 '{frame_target}' -y", shell=True, stderr=subprocess.DEVNULL)
            wal_target = frame_target
        else:
            subprocess.run("pkill -9 mpvpaper; pgrep awww-daemon || (awww-daemon & sleep 0.5)", shell=True, stderr=subprocess.DEVNULL)
            subprocess.Popen(f"awww img '{target_path}' --resize crop --transition-type fade --transition-angle 30 --transition-fps 60 --transition-duration 2", shell=True)
            wal_target = target_path

        # Генеруємо кольори та рестартимо Waybar
        subprocess.run(f"wal -q -t -i '{wal_target}'", shell=True)
        subprocess.run("killall -SIGUSR2 waybar", shell=True)
        subprocess.run(f"{Path.home()}/.local/bin/mgos-wp-poster", shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # постер для входу/SDDM
        subprocess.Popen(f"sudo -n {Path.home()}/.config/megumin-os/sync_sddm.sh", shell=True)
        subprocess.Popen("ignis reload", shell=True) # Перезапуск Ignis для підтягування кольорів    


def pick_ui_font():
    fams = set(QFontDatabase.families())
    for f in ("SF Pro Text", "SF Pro Display", "Inter", "Noto Sans"):
        if f in fams:
            return f
    return QApplication.font().family()


def headless_apply(keys):
    """settings.py --apply ключ… — застосувати збережені в settings.json значення без вікна
    (привітання mgOS пише settings.json і просить застосувати). Той самий код, що й у вікні."""
    cfg = Path.home() / ".config" / "megumin-os" / "settings.json"
    try: data = json.loads(cfg.read_text())
    except Exception: data = {}
    DN = subprocess.DEVNULL
    for key in keys:
        v = data.get(key)
        try:
            if key == "hyprland_blur":
                subprocess.run(["hyprctl", "keyword", "decoration:blur:enabled", str(bool(v)).lower()], stdout=DN, stderr=DN)
                write_blur_conf(data)
            elif key in ("sounds_enabled", "sound_theme", "sounds_input"):
                on = bool(data.get("sounds_enabled", True)); th = data.get("sound_theme", "mgos") or "mgos"
                for c in (["gsettings", "set", "org.gnome.desktop.sound", "event-sounds", "true" if on else "false"],
                          ["gsettings", "set", "org.gnome.desktop.sound", "theme-name", th],
                          ["gsettings", "set", "org.gnome.desktop.sound", "input-feedback-sounds",
                           "true" if on and data.get("sounds_input", False) else "false"]):
                    subprocess.run(c, stdout=DN, stderr=DN)
                set_gtk_ini("gtk-enable-event-sounds", 1 if on else 0)
                set_gtk_ini("gtk-sound-theme-name", th)
            elif key == "power_profile" and v:
                subprocess.run(["powerprofilesctl", "set", v], stdout=DN, stderr=DN)
            elif key in ("ui_font", "ui_font_size"):
                fam = data.get("ui_font") or "Inter"; size = int(data.get("ui_font_size") or 11)
                subprocess.run(["gsettings", "set", "org.gnome.desktop.interface", "font-name", f"{fam} {size}"], stdout=DN, stderr=DN)
                set_gtk_ini("gtk-font-name", f"{fam} {size}")
        except Exception as e:
            print(f"--apply {key}: {e}", file=sys.stderr)


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == "--apply":
        headless_apply(sys.argv[2:])
        sys.exit(0)
    app = QApplication(sys.argv)
    app.setDesktopFileName("megumin-settings.desktop")
    app.setApplicationName("megumin-settings")
    FONT_UI = pick_ui_font()
    ex = MeguminSettings()
    # settings.py --page trackpad | --page 6  — відкрити одразу на потрібній сторінці
    if "--page" in sys.argv:
        try: ex.open_page(sys.argv[sys.argv.index("--page") + 1])
        except IndexError: pass
    ex.show()
    sys.exit(app.exec())
