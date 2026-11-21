"""Налаштування → Дашборд: перемикач і керування віджетами дашборда робочого столу 1 (SYSui/dashboard).

Конфіг — ~/.config/megumin-os/dashboard.json; після кожної зміни дашборд перечитує його (SIGUSR1).
Класи інтерфейсу (SettingRow, MeguminSwitch, ...) передає settings.py — щоб не імпортувати його вдруге.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

from PyQt6.QtCore import Qt, QRectF, QPointF, QSizeF
from PyQt6.QtGui import QPainter, QColor, QFont, QFontMetrics, QPen, QCursor
from PyQt6.QtWidgets import (QCheckBox, QDialog, QDialogButtonBox, QFormLayout, QHBoxLayout, QLabel, QLineEdit,
                             QMenu, QPlainTextEdit, QPushButton, QSpinBox, QVBoxLayout, QWidget)

CFG = Path.home() / ".config" / "megumin-os" / "dashboard.json"
BIN = Path.home() / ".config" / "SYSui" / "dashboard"

COMMON = [("title", "str", "Заголовок (порожньо — типовий)"), ("show_title", "bool", "Показувати заголовок")]
# тип: (назва, опис, поля); поле: (ключ, вид, підпис[, мін, макс])
TYPES = {
    "clock": ("Годинник", "Великий час і дата українською",
              [("seconds", "bool", "Секунди"), ("date", "bool", "Дата")]),
    "system": ("Система", "Процесор, памʼять, диск, батарея",
               [("cpu", "bool", "Процесор"), ("temp", "bool", "Температура"), ("ram", "bool", "Памʼять"),
                ("disk", "bool", "Диск"), ("disk_path", "str", "Шлях диска"), ("battery", "bool", "Батарея")]),
    "network": ("Мережа", "Підключення, IP і швидкість", []),
    "weather": ("Погода", "wttr.in, без ключа; кешується на диску",
                [("city", "str", "Місто (порожньо — за IP)"), ("interval_min", "int", "Оновлювати раз на, хв", 10, 240)]),
    "music": ("Музика", "Поточний трек і кнопки (playerctl)", []),
    "notes": ("Нотатки", "Пишіть просто на робочому столі — збереження автоматичне",
              [("height", "int", "Висота, px", 60, 500)]),
    "actions": ("Швидкі дії", "Кнопки запуску програм і команд", [("items", "actions", "Кнопки")]),
    "calendar": ("Календар", "Місяць із виділеним сьогоднішнім днем", []),
    "perf": ("Продуктивність", "CPU і GPU (температура, навантаження, VRAM), памʼять із zram, кулер і графік температур за годину", []),
    "games": ("Ігри", "Останні зіграні ігри Steam з обкладинками — клік запускає гру",
              [("count", "int", "Скільки ігор", 2, 8)]),
    "alerts": ("Повітряна тривога", "Статус тривоги у вашій області (публічні дані, без ключа)",
               [("region", "choice", "Область")]),
    "rates": ("Курс НБУ", "Офіційний курс гривні: долар, євро, злотий — зі зміною за день", []),
    "updates": ("Оновлення", "Скільки оновлень чекає (pacman і AUR) і кнопка «Оновити»", []),
}
REGIONS = ["", "м. Київ", "Вінницька область", "Волинська область", "Дніпропетровська область", "Донецька область",
           "Житомирська область", "Закарпатська область", "Запорізька область", "Івано-Франківська область", "Київська область",
           "Кіровоградська область", "Луганська область", "Львівська область", "Миколаївська область", "Одеська область",
           "Полтавська область", "Рівненська область", "Сумська область", "Тернопільська область", "Харківська область",
           "Херсонська область", "Хмельницька область", "Черкаська область", "Чернівецька область", "Чернігівська область"]
DEFAULTS = {"clock": {"seconds": False, "date": True},
            "system": {"cpu": True, "ram": True, "disk": True, "battery": True, "temp": False, "disk_path": "/"},
            "weather": {"city": "", "interval_min": 30}, "notes": {"height": 130}, "games": {"count": 4}, "alerts": {"region": ""},
            "actions": {"items": ["terminal", "files", "browser", "settings", "screenshot", "lock"]}}
ACTIONS = [("terminal", "Термінал"), ("files", "Файли"), ("browser", "Браузер"), ("settings", "Налаштування"),
           ("screenshot", "Знімок області"), ("lock", "Блокування"), ("wallpapers", "Шпалери"),
           ("music", "Пауза / відтворення"), ("monitor", "Монітор системи (btop)")]


def load():
    try:
        d = json.loads(CFG.read_text())
        if isinstance(d, dict):
            d.setdefault("widgets", [])
            return d
    except Exception:
        pass
    return {"enabled": True, "columns": 3, "position": "center", "only_empty": False,
            "widgets": [dict(type=t, **DEFAULTS.get(t, {})) for t in
                        ("clock", "perf", "weather", "music", "games", "alerts", "calendar", "actions", "rates",
                         "updates", "network", "notes")]}


def save(cfg):
    CFG.parent.mkdir(parents=True, exist_ok=True)
    tmp = str(CFG) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)
    os.replace(tmp, CFG)
    subprocess.Popen(["pkill", "-USR1", "-x", "dashboard"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def running():
    return subprocess.run(["pgrep", "-x", "dashboard"], stdout=subprocess.DEVNULL).returncode == 0


def set_enabled(on):
    cfg = load()
    cfg["enabled"] = bool(on)
    save(cfg)
    if on and not running() and BIN.exists():
        subprocess.Popen(["hyprctl", "dispatch", "exec", str(BIN)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif not on:
        subprocess.Popen(["pkill", "-x", "dashboard"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class WidgetDialog(QDialog):
    """Налаштування одного віджета: поля з TYPES + спільні."""
    def __init__(self, parent, w):
        super().__init__(parent)
        t = w.get("type", "")
        name = TYPES.get(t, (t,))[0]
        self.setWindowTitle(f"Віджет «{name}»")
        self.w, self.inputs = dict(w), {}
        v = QVBoxLayout(self)
        form = QFormLayout()
        v.addLayout(form)
        dflt = DEFAULTS.get(t, {})
        for f in TYPES.get(t, ("", "", []))[2] + COMMON:
            key, kind, lab = f[0], f[1], f[2]
            cur = w.get(key, dflt.get(key, True if kind == "bool" else ""))
            if kind == "bool":
                e = QCheckBox(); e.setChecked(bool(cur))
            elif kind == "int":
                e = QSpinBox(); e.setRange(f[3], f[4]); e.setValue(int(cur or f[3]))
            elif kind == "actions":
                e = self.actions_editor(cur or dflt.get("items", []))
            elif kind == "choice":
                from PyQt6.QtWidgets import QComboBox
                e = QComboBox()
                for r in REGIONS: e.addItem(r or "— уся Україна (кількість областей) —", r)
                i = e.findData(cur or ""); e.setCurrentIndex(max(0, i))
            else:
                e = QLineEdit(str(cur or ""))
            self.inputs[key] = (kind, e)
            form.addRow(lab, e)
        bb = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        bb.button(QDialogButtonBox.StandardButton.Ok).setText("Зберегти")
        bb.button(QDialogButtonBox.StandardButton.Cancel).setText("Скасувати")
        bb.accepted.connect(self.accept); bb.rejected.connect(self.reject)
        v.addWidget(bb)

    def actions_editor(self, items):
        box = QWidget(); l = QVBoxLayout(box); l.setContentsMargins(0, 0, 0, 0)
        on = [i for i in items if isinstance(i, str)]
        box.checks = []
        for k, lab in ACTIONS:
            c = QCheckBox(lab); c.setChecked(k in on); box.checks.append((k, c)); l.addWidget(c)
        l.addWidget(QLabel("Свої кнопки — рядок «Назва | команда»:"))
        box.custom = QPlainTextEdit()
        box.custom.setPlainText("\n".join(f"{i.get('label', '')} | {i.get('cmd', '')}" for i in items if isinstance(i, dict)))
        box.custom.setFixedHeight(70)
        l.addWidget(box.custom)
        box.order = on
        return box

    def result_widget(self):
        w = self.w
        for key, (kind, e) in self.inputs.items():
            if kind == "bool": w[key] = e.isChecked()
            elif kind == "int": w[key] = e.value()
            elif kind == "choice": w[key] = e.currentData() or ""
            elif kind == "actions":
                chosen = {k for k, c in e.checks if c.isChecked()}
                items = [k for k in e.order if k in chosen] + [k for k, _ in ACTIONS if k in chosen and k not in e.order]
                for line in e.custom.toPlainText().splitlines():
                    if "|" in line:
                        lab, cmd = (s.strip() for s in line.split("|", 1))
                        if cmd: items.append({"label": lab or "Дія", "cmd": cmd})
                w[key] = items
            else:
                s = e.text().strip()
                if s: w[key] = s
                else: w.pop(key, None)
        return w


# ---------- Розкладка перетягуванням ----------
# Приблизна висота віджета на дашборді (у «рядках») — щоб мініатюра була схожа на справжній стіл
HEIGHTS = {"clock": 1.0, "music": 1.5, "calendar": 2.1, "perf": 2.2, "games": 2.3, "actions": 1.4, "weather": 1.1,
           "alerts": 1.1, "rates": 1.0, "updates": 0.8, "network": 1.0, "notes": 1.3, "system": 1.3}
GLYPHS = {"clock": "\U000F0954", "music": "\U000F075A", "calendar": "\U000F00ED", "perf": "\U000F04C5", "games": "\U000F0297",
          "actions": "\U000F0241", "weather": "\U000F0590", "alerts": "\U000F0026", "rates": "\U000F0116", "updates": "\U000F06B0",
          "network": "\U000F05A9", "notes": "\U000F082E", "system": "\U000F035B"}


def _theme():
    m = sys.modules.get("__main__")
    T = getattr(m, "T", None) or {}
    acc = (getattr(m, "ACCENT", None) or ["#0a84ff"])[0]
    return T, acc, getattr(m, "FONT_UI", "Inter"), getattr(m, "ICON_FONT", "JetBrainsMono Nerd Font")


class LayoutBoard(QWidget):
    """Мініатюра дашборда: колонки з блоками віджетів. Тягніть блок у будь-яку колонку чи позицію,
    у «Приховані» — щоб сховати, звідти — щоб показати. Подвійний клік — налаштування віджета, ПКМ — меню."""
    ROW = 38          # px на «рядок» висоти віджета
    GAP = 8
    TRAY_H = 46

    def __init__(self, on_change, on_edit, on_delete, parent=None):
        super().__init__(parent)
        self.on_change, self.on_edit, self.on_delete = on_change, on_edit, on_delete
        self.cols, self.hidden, self.ncol = [], [], 3
        self.rects = []          # (QRectF, ("col", c, i) | ("tray", i))
        self.drag = None         # dict(src, w, pos, off)
        self.press = None
        self.drop = None
        self.setMouseTracking(True)
        self.setMinimumHeight(260)

    # дані: список віджетів (з полями enabled/col) ↔ колонки
    def set_widgets(self, ws, ncol):
        self.ncol = ncol
        self.cols = [[] for _ in range(ncol)]
        self.hidden = []
        i = 0
        for w in ws:
            if not w.get("enabled", True):
                self.hidden.append(w); continue
            c = w.get("col")
            c = min(max(int(c), 0), ncol - 1) if isinstance(c, int) else i % ncol
            self.cols[c].append(w); i += 1
        self.relayout(); self.update()

    def widgets(self):
        out = []
        for c, col in enumerate(self.cols):
            for w in col:
                w = dict(w); w["col"] = c; w["enabled"] = True; out.append(w)
        for w in self.hidden:
            w = dict(w); w["enabled"] = False; out.append(w)
        return out

    def col_w(self):
        return (self.width() - self.GAP * (self.ncol + 1)) / max(1, self.ncol)

    def relayout(self):
        self.rects = []
        cw = self.col_w()
        top = 26
        bottom = top
        for c, col in enumerate(self.cols):
            x = self.GAP + c * (cw + self.GAP)
            y = top
            for i, w in enumerate(col):
                h = HEIGHTS.get(w.get("type"), 1.0) * self.ROW
                self.rects.append((QRectF(x, y, cw, h), ("col", c, i)))
                y += h + self.GAP
            bottom = max(bottom, y)
        self.tray_y = bottom + 34
        x = self.GAP
        for i, w in enumerate(self.hidden):
            fm = QFontMetrics(QFont(_theme()[2], 11))
            wd = fm.horizontalAdvance(self.title(w)) + 46
            if x + wd > self.width() - self.GAP: x = self.GAP; self.tray_y += 0   # один рядок, далі обрізається
            self.rects.append((QRectF(x, self.tray_y + 8, wd, 30), ("tray", i)))
            x += wd + 6
        h = int(self.tray_y + self.TRAY_H + 6)
        if self.minimumHeight() != h: self.setMinimumHeight(h)

    def resizeEvent(self, e):
        self.relayout()

    def title(self, w):
        return w.get("title") or TYPES.get(w.get("type"), (w.get("type", "?"),))[0]

    def item(self, ref):
        return self.cols[ref[1]][ref[2]] if ref[0] == "col" else self.hidden[ref[1]]

    def hit(self, pos):
        for r, ref in self.rects:
            if r.contains(pos): return r, ref
        return None, None

    def drop_target(self, pos):
        """Куди впаде блок: ("col", c, index) або ("tray", index)."""
        if pos.y() > self.tray_y - 10: return ("tray", len(self.hidden))
        cw = self.col_w()
        c = int(min(self.ncol - 1, max(0, (pos.x() - self.GAP / 2) // (cw + self.GAP))))
        idx = 0
        for r, ref in self.rects:
            if ref[0] == "col" and ref[1] == c and not (self.drag and ref == self.drag["src"]):
                if pos.y() > r.center().y(): idx = ref[2] + 1
        if self.drag and self.drag["src"][0] == "col" and self.drag["src"][1] == c and self.drag["src"][2] < idx: idx -= 1
        return ("col", c, idx)

    def mousePressEvent(self, e):
        r, ref = self.hit(e.position())
        if ref is None: return
        if e.button() == Qt.MouseButton.RightButton:
            w = self.item(ref)
            m = QMenu(self)
            m.addAction("Налаштувати…", lambda: self.on_edit(w))
            m.addAction("Сховати" if ref[0] == "col" else "Показати", lambda: self.move(ref, ("tray", len(self.hidden)) if ref[0] == "col" else ("col", 0, len(self.cols[0]))))
            m.addAction("Видалити", lambda: self.on_delete(w))
            m.exec(e.globalPosition().toPoint()); return
        self.press = (e.position(), r, ref)
        e.accept()

    def mouseMoveEvent(self, e):
        if self.press and not self.drag and (e.position() - self.press[0]).manhattanLength() > 6:
            p0, r, ref = self.press
            self.drag = dict(src=ref, w=self.item(ref), size=r.size(), off=p0 - r.topLeft(), pos=e.position())
            self.setCursor(QCursor(Qt.CursorShape.ClosedHandCursor))
        if self.drag:
            self.drag["pos"] = e.position(); self.drop = self.drop_target(e.position())
        else:
            _, ref = self.hit(e.position())
            self.setCursor(QCursor(Qt.CursorShape.OpenHandCursor if ref else Qt.CursorShape.ArrowCursor))
        self.update()

    def mouseReleaseEvent(self, e):
        if self.drag and self.drop:
            self.move(self.drag["src"], self.drop)
        self.drag = self.press = self.drop = None
        self.setCursor(QCursor(Qt.CursorShape.ArrowCursor)); self.update()

    def mouseDoubleClickEvent(self, e):
        _, ref = self.hit(e.position())
        if ref: self.on_edit(self.item(ref))

    def move(self, src, dst):
        w = self.cols[src[1]].pop(src[2]) if src[0] == "col" else self.hidden.pop(src[1])
        if dst[0] == "tray": self.hidden.append(w)
        else: self.cols[dst[1]].insert(min(dst[2], len(self.cols[dst[1]])), w)
        self.relayout(); self.update()
        self.on_change(self.widgets())

    def paintEvent(self, e):
        T, acc, font, icon_font = _theme()
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        txt = QColor(T.get("text", "#f5f5f7")); txt2 = QColor(T.get("text2", "#98989f"))
        card = T.get("card", QColor(44, 44, 47)); border = T.get("card_border", QColor(255, 255, 255, 14))
        f = QFont(font); f.setPixelSize(11); f.setWeight(QFont.Weight.DemiBold); p.setFont(f)
        cw = self.col_w()
        # фон-«робочий стіл» і підписи колонок
        p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor(acc).darker(260)); p.drawRoundedRect(QRectF(0, 0, self.width(), self.tray_y - 14), 14, 14)
        for c in range(self.ncol):
            x = self.GAP + c * (cw + self.GAP)
            p.setPen(txt2); p.drawText(QRectF(x, 4, cw, 18), Qt.AlignmentFlag.AlignCenter, f"Колонка {c + 1}")
            if self.drag and self.drop and self.drop[0] == "col" and self.drop[1] == c:
                p.setPen(Qt.PenStyle.NoPen); c2 = QColor(acc); c2.setAlpha(36)
                p.setBrush(c2); p.drawRoundedRect(QRectF(x - 3, 22, cw + 6, self.tray_y - 40), 10, 10)
        # «Приховані»
        p.setPen(txt2); p.drawText(QRectF(self.GAP, self.tray_y - 12, 300, 16), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                                   "Приховані — перетягніть сюди, щоб сховати" if self.hidden or self.drag else "Приховані: немає")
        tr = QRectF(0, self.tray_y + 4, self.width(), self.TRAY_H - 6)
        pen = QPen(txt2, 1, Qt.PenStyle.DashLine); p.setPen(pen); p.setBrush(Qt.BrushStyle.NoBrush); p.drawRoundedRect(tr, 10, 10)
        # блоки
        for r, ref in self.rects:
            if self.drag and ref == self.drag["src"]:
                p.setPen(QPen(txt2, 1, Qt.PenStyle.DashLine)); p.setBrush(Qt.BrushStyle.NoBrush); p.drawRoundedRect(r, 10, 10); continue
            self._block(p, r, self.item(ref), ref[0] == "tray", T, acc, font, icon_font, txt, txt2, card, border)
        # лінія вставки
        if self.drag and self.drop and self.drop[0] == "col":
            c, idx = self.drop[1], self.drop[2]
            x = self.GAP + c * (cw + self.GAP)
            ys = [r for r, ref in self.rects if ref[0] == "col" and ref[1] == c and ref != self.drag["src"]]
            y = (ys[idx].top() - self.GAP / 2) if idx < len(ys) else ((ys[-1].bottom() + self.GAP / 2) if ys else 30)
            p.setPen(QPen(QColor(acc), 3, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)); p.drawLine(QPointF(x + 4, y), QPointF(x + cw - 4, y))
        # блок, що тягнеться
        if self.drag:
            r = QRectF(self.drag["pos"] - self.drag["off"], self.drag["size"])
            if self.drop and self.drop[0] == "tray": r.setSize(QSizeF(min(r.width(), 170), 30))
            p.setOpacity(0.92)
            self._block(p, r, self.drag["w"], False, T, acc, font, icon_font, txt, txt2, card, border, lifted=True)
            p.setOpacity(1)

    def _block(self, p, r, w, small, T, acc, font, icon_font, txt, txt2, card, border, lifted=False):
        if lifted:
            p.setPen(Qt.PenStyle.NoPen); p.setBrush(QColor(0, 0, 0, 70)); p.drawRoundedRect(r.translated(0, 4), 10, 10)
        p.setPen(QPen(QColor(acc) if lifted else border, 1.5 if lifted else 1)); p.setBrush(card); p.drawRoundedRect(r, 10, 10)
        fi = QFont(icon_font); fi.setPixelSize(14); p.setFont(fi); p.setPen(QColor(acc))
        p.drawText(QRectF(r.left() + 8, r.top() + (0 if small else 6), 20, r.height() if small else 18),
                   Qt.AlignmentFlag.AlignCenter, GLYPHS.get(w.get("type"), "\U000F0241"))
        f = QFont(font); f.setPixelSize(12); f.setWeight(QFont.Weight.DemiBold); p.setFont(f); p.setPen(txt)
        p.drawText(QRectF(r.left() + 32, r.top() + (0 if small else 6), r.width() - 40, r.height() if small else 18),
                   Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, self.title(w))
        if not small and r.height() > 44:
            p.save(); p.setClipRect(r.adjusted(4, 4, -4, -6))   # опис не вилазить за блок
            f.setPixelSize(10); f.setWeight(QFont.Weight.Normal); p.setFont(f); p.setPen(txt2)
            p.drawText(QRectF(r.left() + 10, r.top() + 28, r.width() - 20, r.height() - 32),
                       Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop | Qt.TextFlag.TextWordWrap,
                       TYPES.get(w.get("type"), ("", ""))[1])
            p.restore()


def build_page(win, lay, SettingRow, MeguminSwitch, SegmentedSelector, GroupCard):
    """Вміст сторінки «Дашборд». win — головне вікно Налаштувань (section/create_card)."""
    cfg = load()

    def set_opt(key, value):
        c = load(); c[key] = value; save(c)

    win.section(lay, "Дашборд")
    lay.addWidget(win.create_card([
        SettingRow("", "", "Увімкнути дашборд", "Віджети на робочому столі 1. Без вікон — видно, з вікнами — ховається і не оновлюється",
                   MeguminSwitch("dashboard_enabled", cfg.get("enabled", False), lambda k, v: set_enabled(v))),
        SettingRow("", "", "Колонки", "", SegmentedSelector("dash_columns", [(2, "2"), (3, "3"), (4, "4")],
                                                         cfg.get("columns", 3), lambda k, v: (set_opt("columns", v), getattr(win, "_dash_rebuild", lambda: None)()))),
        SettingRow("", "", "Розташування", "", SegmentedSelector("dash_position", [("left", "Ліворуч"), ("center", "Центр"), ("right", "Праворуч")],
                                                              cfg.get("position", "center"), lambda k, v: set_opt("position", v))),
        SettingRow("", "", "Лише на порожньому столі", "Вимкніть, щоб дашборд лишався під вікнами робочого столу 1",
                   MeguminSwitch("dash_only_empty", cfg.get("only_empty", False), lambda k, v: set_opt("only_empty", v))),
        SettingRow("", "", "Підкладка під віджетами", "Вимкнено — прозоре скло: лише розмиті шпалери без темного тону",
                   MeguminSwitch("dash_card_bg", cfg.get("card_bg", False), lambda k, v: set_opt("card_bg", v))),
    ]))

    win.section(lay, "Розкладка віджетів")
    hint = QLabel("Перетягуйте блоки між колонками та всередині них. У «Приховані» — сховати, звідти — показати. "
                  "Подвійний клік — налаштування віджета, правий клік — меню.")
    hint.setWordWrap(True); hint.setProperty("role", "desc")
    lay.addWidget(hint)
    holder = QWidget(); hl = QVBoxLayout(holder); hl.setContentsMargins(0, 0, 0, 0)
    lay.addWidget(holder)

    def same(a, b):   # той самий віджет у конфігу (без службових col/enabled)
        strip = lambda d: {k: v for k, v in d.items() if k not in ("col", "enabled")}
        return strip(a) == strip(b)

    def changed(ws):
        c = load(); c["widgets"] = ws; save(c)

    def edit(w):
        c = load()
        i = next((i for i, x in enumerate(c["widgets"]) if same(x, w)), None)
        if i is None: return
        d = WidgetDialog(win, c["widgets"][i])
        if d.exec():
            c["widgets"][i] = d.result_widget(); save(c); rebuild()

    def delete(w):
        c = load()
        c["widgets"] = [x for x in c["widgets"] if not same(x, w)]
        save(c); rebuild()

    board = LayoutBoard(changed, edit, delete)
    hl.addWidget(board)

    def add(t):
        c = load(); c["widgets"].append(dict(type=t, col=0, **DEFAULTS.get(t, {}))); save(c); rebuild()

    def rebuild():
        c = load()
        board.set_widgets(c["widgets"], int(c.get("columns", 3)))

    add_btn = QPushButton("Додати віджет  ▾"); add_btn.setCursor(Qt.CursorShape.PointingHandCursor)
    m = QMenu(add_btn)
    for t, (name, desc, _) in TYPES.items():
        m.addAction(f"{name} — {desc[:48]}{'…' if len(desc) > 48 else ''}", lambda t=t: add(t))
    add_btn.setMenu(m)
    lay.addWidget(win.create_card([SettingRow("", "", "Додати віджет", "Новий віджет зʼявиться в першій колонці", add_btn)]))
    win._dash_rebuild = rebuild   # кількість колонок змінилась — перемалювати дошку
    rebuild()
    lay.addWidget(win.create_card([
        SettingRow("", "", "Перезапустити дашборд", "Якщо віджети «завмерли»",
                   on_click=lambda: subprocess.Popen(["bash", "-c", f"pkill -x dashboard; sleep 0.4; cfg={json.dumps(str(CFG))}; "
                                                     f"grep -q '\"enabled\": *false' \"$cfg\" || hyprctl dispatch exec {BIN}"],
                                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)),
    ]))
