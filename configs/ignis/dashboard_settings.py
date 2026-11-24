"""Налаштування → Дашборд: перемикач і керування віджетами дашборда робочого столу 1 (SYSui/dashboard).

Конфіг — ~/.config/megumin-os/dashboard.json; після кожної зміни дашборд перечитує його (SIGUSR1).
Класи інтерфейсу (SettingRow, MeguminSwitch, ...) передає settings.py — щоб не імпортувати його вдруге.
"""
import json
import os
import subprocess
from pathlib import Path

from PyQt6.QtCore import Qt
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
}
DEFAULTS = {"clock": {"seconds": False, "date": True},
            "system": {"cpu": True, "ram": True, "disk": True, "battery": True, "temp": False, "disk_path": "/"},
            "weather": {"city": "", "interval_min": 30}, "notes": {"height": 130},
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
                        ("clock", "system", "weather", "music", "network", "actions", "notes", "calendar")]}


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
                                                         cfg.get("columns", 3), lambda k, v: set_opt("columns", v))),
        SettingRow("", "", "Розташування", "", SegmentedSelector("dash_position", [("left", "Ліворуч"), ("center", "Центр"), ("right", "Праворуч")],
                                                              cfg.get("position", "center"), lambda k, v: set_opt("position", v))),
        SettingRow("", "", "Лише на порожньому столі", "Вимкніть, щоб дашборд лишався під вікнами робочого столу 1",
                   MeguminSwitch("dash_only_empty", cfg.get("only_empty", False), lambda k, v: set_opt("only_empty", v))),
    ]))

    win.section(lay, "Віджети")
    holder = QWidget(); hl = QVBoxLayout(holder); hl.setContentsMargins(0, 0, 0, 0)
    lay.addWidget(holder)
    state = {"card": None}

    def btn(text, tip, fn):
        b = QPushButton(text); b.setToolTip(tip); b.setFixedSize(30, 26); b.setFlat(True)
        b.setCursor(Qt.CursorShape.PointingHandCursor); b.clicked.connect(fn)
        return b

    def mutate(fn):
        c = load(); fn(c["widgets"]); save(c); rebuild()

    def edit(i):
        c = load()
        d = WidgetDialog(win, c["widgets"][i])
        if d.exec():
            c["widgets"][i] = d.result_widget(); save(c); rebuild()

    def add(t):
        mutate(lambda ws: ws.append(dict(type=t, **DEFAULTS.get(t, {}))))

    def rebuild():
        if state["card"] is not None:
            hl.removeWidget(state["card"]); state["card"].deleteLater()
        ws = load()["widgets"]
        rows = []
        for i, w in enumerate(ws):
            t = w.get("type", "")
            name, desc = TYPES.get(t, (t, "Невідомий тип"))[:2]
            ctl = QWidget(); cl = QHBoxLayout(ctl); cl.setContentsMargins(0, 0, 0, 0); cl.setSpacing(2)
            up = btn("↑", "Вище", lambda _=0, i=i: mutate(lambda ws: ws.insert(i - 1, ws.pop(i))))
            dn = btn("↓", "Нижче", lambda _=0, i=i: mutate(lambda ws: ws.insert(i + 1, ws.pop(i))))
            up.setEnabled(i > 0); dn.setEnabled(i < len(ws) - 1)
            cl.addWidget(up); cl.addWidget(dn)
            if t in TYPES: cl.addWidget(btn("⚙", "Налаштувати", lambda _=0, i=i: edit(i)))
            cl.addWidget(btn("✕", "Видалити", lambda _=0, i=i: mutate(lambda ws: ws.pop(i))))
            sw = MeguminSwitch(f"dash_w{i}", w.get("enabled", True),
                               lambda k, v, i=i: mutate(lambda ws: ws[i].__setitem__("enabled", bool(v))))
            cl.addSpacing(6); cl.addWidget(sw)
            title = w.get("title") or name
            rows.append(SettingRow("", "", f"{i + 1}. {title}", desc, ctl))
        add_btn = QPushButton("Додати віджет  ▾"); add_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        m = QMenu(add_btn)
        for t, (name, desc, _) in TYPES.items():
            m.addAction(name, lambda t=t: add(t))
        add_btn.setMenu(m)
        rows.append(SettingRow("", "", "Додати віджет", "Віджети розкладаються по колонках у цьому порядку", add_btn))
        state["card"] = GroupCard(rows)
        hl.addWidget(state["card"])

    rebuild()
    lay.addWidget(win.create_card([
        SettingRow("", "", "Перезапустити дашборд", "Якщо віджети «завмерли»",
                   on_click=lambda: subprocess.Popen(["bash", "-c", f"pkill -x dashboard; sleep 0.4; cfg={json.dumps(str(CFG))}; "
                                                     f"grep -q '\"enabled\": *false' \"$cfg\" || hyprctl dispatch exec {BIN}"],
                                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)),
    ]))
