"""Звичайна (не Apple) клавіатура для «Налаштувань» → Клавіатура.

  • detect_keyboards() — фізичні клавіатури з /proc/bus/input/devices (читається без root): приймач USB чи
    Bluetooth, що вони заявляють — цифровий блок, ISO-клавіша < > (102nd), японські/корейські, F13–F24,
    медіа- і спецклавіші (калькулятор, пошта, браузер, макроси…). Приймачі часто заявляють «усе підряд»,
    тому це лише підказка; точну відповідь дає перевірка клавіш.
  • xkb_layout() — підписи клавіш для БУДЬ-ЯКОЇ розкладки системи прямо з /usr/share/X11/xkb/symbols
    (з include), символи — з X11/keysymdef.h. Відповідність «клавіша xkb ↔ код ядра» — xkb/keycodes/evdev.
  • KeyboardPreview — схема клавіатури (ANSI або ISO, з цифровим блоком чи без) у двох розкладках;
    у режимі перевірки підсвічує натиснуті клавіші, а клавіші поза стандартною схемою (медіа, макроси,
    Fn-клавіші виробника) показує списком: код ядра, назва і чи призначена на неї дія в Hyprland.
    На час перевірки Hyprland у порожньому submap «mgos-keytest» — гарячі клавіші не перехоплюють натискання.
"""
import json
import os
import re
import subprocess
from pathlib import Path

from PyQt6.QtCore import Qt, QRectF, QTimer, QEvent, pyqtSignal
from PyQt6.QtGui import QPainter, QColor, QFont, QPen, QPainterPath
from PyQt6.QtWidgets import QWidget

STORE = Path.home() / ".config" / "megumin-os" / "keyboards.json"

# ---------------- довідники з системних файлів ----------------
_KC, _EV, _KS, _KSV = {}, {}, {}, {}


def keycodes():
    """xkb-ім'я → xkb-код (= код ядра + 8)."""
    if not _KC:
        try:
            for m in re.finditer(r"<(\w+)>\s*=\s*(\d+)\s*;", Path("/usr/share/X11/xkb/keycodes/evdev").read_text(errors="ignore")):
                _KC.setdefault(m.group(1), int(m.group(2)))
        except OSError:
            pass
    return _KC


def evnames():
    """код ядра → 'KEY_CALC'."""
    if not _EV:
        try:
            for m in re.finditer(r"#define\s+(KEY_\w+)\s+(0x[0-9a-fA-F]+|\d+)", Path("/usr/include/linux/input-event-codes.h").read_text()):
                _EV.setdefault(int(m.group(2), 0), m.group(1))
        except OSError:
            pass
    return _EV


def keysyms():
    """keysym-ім'я → символ (з коментаря U+XXXX у keysymdef.h); keysym-код → ім'я (і XF86)."""
    if not _KS:
        for f in ("/usr/include/X11/keysymdef.h", "/usr/include/X11/XF86keysym.h"):
            try:
                for m in re.finditer(r"#define\s+(?:XK|XF86XK)_(\w+)\s+(0x[0-9a-fA-F]+)(?:\s*/\*\s*U\+([0-9A-F]{4,6}))?", Path(f).read_text(errors="ignore")):
                    name = ("XF86" if "XF86" in f else "") + m.group(1)
                    if m.group(3): _KS.setdefault(m.group(1), chr(int(m.group(3), 16)))
                    _KSV.setdefault(int(m.group(2), 16), name)
            except OSError:
                pass
    return _KS


def sym_char(sym):
    ks = keysyms()
    if re.fullmatch(r"U[0-9A-Fa-f]{4,6}", sym): return chr(int(sym[1:], 16))
    if len(sym) == 1: return sym
    return ks.get(sym, "")


# ---------------- розкладки xkb ----------------
def _sections(text):
    """{ім'я секції: тіло}, ім'я default-секції."""
    out, default = {}, None
    for m in re.finditer(r'((?:\w+\s+)*)xkb_symbols\s+"([^"]+)"\s*\{', text):
        i, depth = m.end(), 1
        while i < len(text) and depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0); i += 1
        out[m.group(2)] = text[m.end():i - 1]
        if "default" in m.group(1).split() and default is None: default = m.group(2)
    return out, default or (next(iter(out)) if out else None)


def xkb_layout(layout, variant="", _depth=0):
    """{xkb-ім'я клавіші: [рівень1, рівень2]} у символах."""
    if _depth > 8: return {}
    p = Path("/usr/share/X11/xkb/symbols") / layout
    try: secs, default = _sections(re.sub(r"//[^\n]*", "", p.read_text(errors="ignore")))
    except OSError: return {}
    body = secs.get(variant or default) or secs.get(default, "")
    keys = {}
    for st in re.finditer(r'include\s+"([^"]+)"|key\s*<(\w+)>\s*\{([^}]*)\}', body):
        if st.group(1):
            for part in re.split(r"[+|]", st.group(1)):
                mm = re.fullmatch(r"([\w-]+)(?:\(([\w-]+)\))?", part.strip())
                if mm: keys.update(xkb_layout(mm.group(1), mm.group(2) or "", _depth + 1))
        else:
            lv = re.search(r"\[([^\]]*)\]", st.group(3))
            if lv:
                syms = [s.strip() for s in lv.group(1).split(",")]
                keys[st.group(2)] = [sym_char(s) for s in syms[:2]]
    return keys


def system_layouts():
    """[(розкладка, варіант)] з Hyprland і активна мапа головної клавіатури."""
    def opt(o):
        try: return json.loads(subprocess.run(["hyprctl", "getoption", o, "-j"], capture_output=True, text=True, timeout=1).stdout).get("str", "")
        except Exception: return ""
    lays = [l.strip() for l in (opt("input:kb_layout") or "us").split(",")]
    vars_ = [v.strip() for v in opt("input:kb_variant").replace("[[EMPTY]]", "").split(",")] + [""] * len(lays)
    active, main = "", ""
    try:
        for k in json.loads(subprocess.run(["hyprctl", "devices", "-j"], capture_output=True, text=True, timeout=1).stdout)["keyboards"]:
            if k.get("main"): active, main = k.get("active_keymap", ""), k.get("name", "")
    except Exception:
        pass
    return list(zip(lays, vars_)), active, main


# ---------------- пристрої ----------------
NUMPAD = {71, 72, 73, 75, 76, 77, 79, 80, 81, 82, 83, 96, 98, 55, 74, 78, 69}
SPECIAL = {   # код ядра → як назвати людською мовою
    113: "Вимкнути звук", 114: "Гучність −", 115: "Гучність +", 163: "Наступний трек", 164: "Пуск/пауза",
    165: "Попередній трек", 166: "Стоп", 140: "Калькулятор", 155: "Пошта", 150: "Браузер", 172: "Домашня сторінка",
    158: "Назад", 159: "Вперед", 217: "Пошук", 156: "Закладки", 157: "Мій комп'ютер", 224: "Яскравість −",
    225: "Яскравість +", 228: "Підсвітка вкл/викл", 229: "Підсвітка −", 230: "Підсвітка +", 142: "Сон",
    143: "Пробудження", 116: "Живлення", 248: "Мікрофон вкл/викл", 171: "Медіаплеєр", 226: "Медіа", 464: "Fn",
    152: "Блокування екрана", 0x249: "Емодзі", 0x24a: "Диктування", 0x279: "Знімок області", 120: "Огляд вікон",
    204: "Усі програми", 0x2a0: "Макро-перемикач", 148: "Prog1", 149: "Prog2", 202: "Prog3", 203: "Prog4",
    183: "F13", 184: "F14", 185: "F15", 186: "F16", 187: "F17", 188: "F18", 189: "F19", 190: "F20",
    191: "F21", 192: "F22", 193: "F23", 194: "F24", 85: "Zenkaku/Hankaku", 89: "Ro (JIS)", 92: "Henkan",
    93: "Katakana/Hiragana", 94: "Muhenkan", 124: "Yen (JIS)", 122: "Hangeul", 123: "Hanja", 86: "< > (ISO)",
}
SPECIAL.update({0x290 + i: f"Макрос M{i + 1}" for i in range(30)})


def _bits(hexwords):
    out, words = set(), hexwords.split()
    for i, w in enumerate(reversed(words)):
        v = int(w, 16); b = 0
        while v:
            if v & 1: out.add(i * 64 + b)
            v >>= 1; b += 1
    return out


def detect_keyboards():
    """[{name, bus, keys:set, devices, mouse:bool, letters:bool}] — згруповано по фізичному пристрою."""
    try: text = Path("/proc/bus/input/devices").read_text(errors="ignore")
    except OSError: return []
    groups = {}
    for blk in text.strip().split("\n\n"):
        info = {"keys": set(), "rel": False}
        for ln in blk.splitlines():
            if ln.startswith("I:"): info["bus"] = re.search(r"Bus=(\w+)", ln).group(1)
            elif ln.startswith("N:"): info["name"] = ln.split("=", 1)[1].strip('"')
            elif ln.startswith("P:"): info["phys"] = ln.split("=", 1)[1]
            elif ln.startswith("U:"): info["uniq"] = ln.split("=", 1)[1]
            elif ln.startswith("B: KEY="): info["keys"] = _bits(ln.split("=", 1)[1])
            elif ln.startswith("B: REL="): info["rel"] = ln.split("=", 1)[1].strip() not in ("", "0")
        if not info["keys"] or "name" not in info: continue
        phys = info.get("phys") or info["name"]
        gkey = re.sub(r"/input\d+$", "", phys) or info["name"]
        base = re.sub(r"\s+(Keyboard|Consumer Control|System Control|Mouse|Wireless Radio Control)$", "", info["name"]).strip()
        g = groups.setdefault(gkey, {"name": base, "bus": info.get("bus", ""), "keys": set(), "devices": [], "mouse": False})
        if len(base) < len(g["name"]): g["name"] = base
        g["keys"] |= info["keys"]; g["devices"].append(info["name"]); g["mouse"] |= info["rel"]
    out = []
    for g in groups.values():
        g["letters"] = all(k in g["keys"] for k in (30, 31, 32, 44, 57))   # A S D Z Space
        if g["letters"]: out.append(g)
    # спершу та, яку Hyprland вважає головною (main), далі без миші, далі приймачі-комбо
    try:
        main = next((k["name"] for k in json.loads(subprocess.run(["hyprctl", "devices", "-j"], capture_output=True,
                     text=True, timeout=1).stdout)["keyboards"] if k.get("main")), "")
    except Exception:
        main = ""
    norm = lambda s: re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")
    for g in out: g["main"] = bool(main) and norm(main).startswith(norm(g["name"]))
    out.sort(key=lambda g: (not g["main"], g["mouse"], -len(g["keys"] & set(SPECIAL))))
    return out


def features(keys):
    f = {"numpad": bool(keys & NUMPAD - {55, 69, 74, 78}) and 82 in keys, "iso": 86 in keys,
         "jis": bool({89, 124, 92, 94} & keys), "korean": bool({122, 123} & keys),
         "fext": sorted(k for k in keys if 183 <= k <= 194),
         "special": sorted(k for k in keys if k in SPECIAL and not (183 <= k <= 194) and k != 86)}
    f["size"] = "Повнорозмірна (100%)" if f["numpad"] else ("Без цифрового блоку (TKL / 75%)" if 110 in keys else "Компактна (60–65%)")
    return f


def bus_name(b):
    return {"0003": "USB", "0005": "Bluetooth", "0011": "PS/2", "0019": "вбудована", "0018": "I²C"}.get(b, b)


def hypr_binds():
    """keysym-ім'я (нижній регістр) → опис дії, призначеної в Hyprland без модифікаторів."""
    try:
        out = {}
        for b in json.loads(subprocess.run(["hyprctl", "binds", "-j"], capture_output=True, text=True, timeout=2).stdout):
            if b.get("modmask", 0) == 0 and b.get("key"):
                out[b["key"].lower()] = (b.get("dispatcher", "") + " " + b.get("arg", "")).strip()
        return out
    except Exception:
        return {}


def load_store():
    try: return json.loads(STORE.read_text())
    except Exception: return {}


def save_store(d):
    STORE.parent.mkdir(parents=True, exist_ok=True)
    t = STORE.with_suffix(".tmp"); t.write_text(json.dumps(d, ensure_ascii=False, indent=1)); os.replace(t, STORE)


# ---------------- геометрія (одиниці = ширина звичайної клавіші) ----------------
def _row(y, x, items):
    out = []
    for it in items:
        if it is None: x += 0.25; continue
        name, w = (it, 1.0) if isinstance(it, str) else it
        if name.startswith("_gap"): x += w; continue
        out.append((name, x, y, w, 1.0)); x += w
    return out


def geometry(iso=False, numpad=True):
    k = []
    k += _row(0, 0, ["ESC", ("_gap", 1), "FK01", "FK02", "FK03", "FK04", ("_gap", .5), "FK05", "FK06", "FK07", "FK08",
                     ("_gap", .5), "FK09", "FK10", "FK11", "FK12"])
    k += _row(0, 15.25, ["PRSC", "SCLK", "PAUS"])
    y = 1.5
    k += _row(y, 0, ["TLDE"] + [f"AE{i:02d}" for i in range(1, 13)] + [("BKSP", 2)])
    k += _row(y + 1, 0, [("TAB", 1.5)] + [f"AD{i:02d}" for i in range(1, 13)] + ([] if iso else [("BKSL", 1.5)]))
    k += _row(y + 2, 0, [("CAPS", 1.75)] + [f"AC{i:02d}" for i in range(1, 12)] + (["BKSL"] if iso else [("RTRN", 2.25)]))
    k += _row(y + 3, 0, ([("LFSH", 1.25), "LSGT"] if iso else [("LFSH", 2.25)]) + [f"AB{i:02d}" for i in range(1, 11)] + [("RTSH", 2.75)])
    k += _row(y + 4, 0, [("LCTL", 1.25), ("LWIN", 1.25), ("LALT", 1.25), ("SPCE", 6.25), ("RALT", 1.25), ("RWIN", 1.25),
                         ("COMP", 1.25), ("RCTL", 1.25)])
    if iso:   # Enter-«чобіт»: верх 1.5 у ряду 2 + низ 1.25 у ряду 3 — малюється одним контуром
        k.append(("RTRN", 13.5, y + 1, 1.5, 2.0))
    k += _row(y, 15.25, ["INS", "HOME", "PGUP"])
    k += _row(y + 1, 15.25, ["DELE", "END", "PGDN"])
    k += _row(y + 3, 16.25, ["UP"])
    k += _row(y + 4, 15.25, ["LEFT", "DOWN", "RGHT"])
    if numpad:
        k += _row(y, 18.5, ["NMLK", "KPDV", "KPMU", "KPSU"])
        k += _row(y + 1, 18.5, ["KP7", "KP8", "KP9"]); k.append(("KPAD", 21.5, y + 1, 1, 2))
        k += _row(y + 2, 18.5, ["KP4", "KP5", "KP6"])
        k += _row(y + 3, 18.5, ["KP1", "KP2", "KP3"]); k.append(("KPEN", 21.5, y + 3, 1, 2))
        k += _row(y + 4, 18.5, [("KP0", 2), "KPDL"])
    return k


LABELS = {"ESC": "Esc", "BKSP": "⌫", "TAB": "Tab", "CAPS": "Caps", "RTRN": "Enter", "LFSH": "Shift", "RTSH": "Shift",
          "LCTL": "Ctrl", "RCTL": "Ctrl", "LWIN": "Super", "RWIN": "Super", "LALT": "Alt", "RALT": "AltGr", "COMP": "☰",
          "SPCE": "", "PRSC": "PrtSc", "SCLK": "ScrLk", "PAUS": "Pause", "INS": "Ins", "HOME": "Home", "PGUP": "PgUp",
          "DELE": "Del", "END": "End", "PGDN": "PgDn", "UP": "↑", "DOWN": "↓", "LEFT": "←", "RGHT": "→", "NMLK": "Num",
          "KPDV": "/", "KPMU": "*", "KPSU": "−", "KPAD": "+", "KPEN": "⏎", "KPDL": ".",
          **{f"FK{i:02d}": f"F{i}" for i in range(1, 13)}, **{f"KP{i}": str(i) for i in range(10)}}


class KeyboardPreview(QWidget):
    """Схема клавіатури + живий тест. changed — після кожного натискання в тесті (для оновлення підписів)."""
    changed = pyqtSignal()

    def __init__(self, theme, accent, font_ui, parent=None):
        super().__init__(parent)
        self.T, self.accent, self.font_ui = theme, accent, font_ui
        self.lays, self.active, self.main_name = system_layouts()
        self.legend = [xkb_layout(l, v) for l, v in self.lays[:2]]
        self.kbds = detect_keyboards()
        self.kb = self.kbds[0] if self.kbds else None
        self.feat = features(self.kb["keys"]) if self.kb else {"numpad": True, "iso": False, "special": [], "fext": [], "jis": False, "korean": False, "size": ""}
        st = load_store().get(self.kb_id(), {})
        self.seen_iso = bool(st.get("iso"))
        self.form = st.get("form", "auto")            # auto | ansi | iso
        self.tested = set(st.get("tested", []))       # xkb-імена клавіш, що вже натискались
        self.extra = {int(k): v for k, v in st.get("extra", {}).items()}   # спецклавіші поза схемою: код → опис
        self.down = set()
        self.testing = False
        self._idle = QTimer(self); self._idle.setSingleShot(True); self._idle.timeout.connect(self.stop_test)
        self._watch = None
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMinimumHeight(160)
        self.name_of = {v: k for k, v in keycodes().items()}

    # ---- стан ----
    def kb_id(self): return self.kb["name"] if self.kb else "keyboard"

    def iso(self):
        return self.form == "iso" or (self.form == "auto" and self.seen_iso)

    def set_form(self, f):
        self.form = f; self._save(); self.update()

    def _save(self):
        d = load_store()
        d[self.kb_id()] = {"iso": self.seen_iso, "form": self.form, "tested": sorted(self.tested),
                           "extra": {str(k): v for k, v in self.extra.items()}}
        save_store(d)

    def reset_test(self):
        self.tested.clear(); self.extra.clear(); self.seen_iso = False; self._save(); self.update(); self.changed.emit()

    # ---- тест ----
    def start_test(self):
        if self.testing: return
        self.testing = True
        # Hyprland: порожній submap — бінди mgOS (медіа, Super+…) не з'їдають натискання.
        # Сторож повертає звичайний режим, якщо «Налаштування» впадуть чи закриються посеред тесту.
        subprocess.run(["hyprctl", "dispatch", "submap", "mgos-keytest"], capture_output=True)
        self._watch = subprocess.Popen(["sh", "-c", f"while kill -0 {os.getpid()} 2>/dev/null && [ -e /proc/{os.getpid()} ]; do sleep 1; "
                                        f"[ -e \"$XDG_RUNTIME_DIR/mgos-keytest.{os.getpid()}\" ] || break; done; hyprctl dispatch submap reset >/dev/null"],
                                       start_new_session=True)
        Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), f"mgos-keytest.{os.getpid()}").touch()
        self.setFocus(); self.grabKeyboard()
        self._idle.start(45000)
        self.update(); self.changed.emit()

    def stop_test(self):
        if not self.testing: return
        self.testing = False
        self.releaseKeyboard()
        Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), f"mgos-keytest.{os.getpid()}").unlink(missing_ok=True)
        subprocess.run(["hyprctl", "dispatch", "submap", "reset"], capture_output=True)
        self.down.clear(); self._save(); self.update(); self.changed.emit()

    def hideEvent(self, e):
        self.stop_test(); super().hideEvent(e)

    def event(self, e):
        # Tab / Shift+Tab інакше забирає фокус-ланцюжок Qt — у тесті це теж просто клавіші
        if self.testing and e.type() in (QEvent.Type.KeyPress, QEvent.Type.KeyRelease):
            (self.keyPressEvent if e.type() == QEvent.Type.KeyPress else self.keyReleaseEvent)(e); return True
        return super().event(e)

    def keyPressEvent(self, e):
        if not self.testing: return super().keyPressEvent(e)
        self._idle.start(45000)
        sc = e.nativeScanCode(); name = self.name_of.get(sc)
        if name == "LSGT": self.seen_iso = True
        drawn = {k[0] for k in geometry(self.iso(), True)}
        if name and name in drawn:
            self.down.add(name); self.tested.add(name)
        else:
            code = sc - 8
            keysyms()
            sym = _KSV.get(e.nativeVirtualKey(), "") or (name or "")
            act = hypr_binds().get(sym.lower(), "")
            self.extra[code] = {"name": SPECIAL.get(code) or evnames().get(code, f"код {code}"), "ev": evnames().get(code, ""),
                                "sym": sym, "bind": act}
        self.update(); self.changed.emit()

    def keyReleaseEvent(self, e):
        if not self.testing: return super().keyReleaseEvent(e)
        self.down.discard(self.name_of.get(e.nativeScanCode(), "")); self.update()

    # ---- малювання ----
    def _geo(self):
        return geometry(self.iso(), self.feat["numpad"] or any(n.startswith("KP") or n == "NMLK" for n in self.tested))

    def resizeEvent(self, e):
        # висота — під ширину (схема має сталі пропорції), без порожнечі знизу
        geo = self._geo()
        W = max(x + w for _, x, _, w, _ in geo); H = max(y + h for _, _, y, _, h in geo)
        want = int((self.width() - 28) / W * H) + 28 + 26
        if abs(self.height() - want) > 2: self.setFixedHeight(max(160, want))
        super().resizeEvent(e)

    def paintEvent(self, e):
        T = self.T
        p = QPainter(self); p.setRenderHint(QPainter.RenderHint.Antialiasing)
        geo = self._geo()
        W = max(x + w for _, x, _, w, _ in geo); H = max(y + h for _, _, y, _, h in geo)
        pad = 14
        u = min((self.width() - 2 * pad) / W, (self.height() - 2 * pad - 26) / H)
        x0 = (self.width() - W * u) / 2; y0 = pad
        body = QRectF(x0 - 8, y0 - 8, W * u + 16, H * u + 16)
        p.setPen(QPen(T["card_border"], 1)); p.setBrush(QColor(T["card"]).lighter(112))
        p.drawRoundedRect(body, 12, 12)
        a = QColor(self.accent)
        l1, l2 = (self.legend + [{}, {}])[:2]
        gap = max(1.5, u * 0.08)
        for name, x, y, w, h in geo:
            r = QRectF(x0 + x * u + gap / 2, y0 + y * u + gap / 2, w * u - gap, h * u - gap)
            path = QPainterPath()
            if name == "RTRN" and h == 2:   # ISO Enter
                path.moveTo(r.left(), r.top()); path.lineTo(r.right(), r.top()); path.lineTo(r.right(), r.bottom())
                path.lineTo(r.left() + 0.25 * u, r.bottom()); path.lineTo(r.left() + 0.25 * u, r.top() + u - gap)
                path.lineTo(r.left(), r.top() + u - gap); path.closeSubpath()
            else:
                path.addRoundedRect(r, max(2, u * 0.12), max(2, u * 0.12))
            down, tested = name in self.down, name in self.tested
            fill = QColor(a) if down else (QColor(a.red(), a.green(), a.blue(), 55) if tested else QColor(T["text"]))
            if not down and not tested: fill.setAlpha(16)
            p.setPen(QPen(QColor(a) if tested else QColor(T["card_border"]), 1.2 if tested else 1))
            p.setBrush(fill); p.drawPath(path)
            # підписи: основна розкладка (велика літера) і друга — акцентом у правому нижньому куті
            c1 = (l1.get(name) or [""])[0]; c2 = (l2.get(name) or [""])[0]
            lab = LABELS.get(name)
            f = QFont(self.font_ui); p.setPen(QColor("white") if down else QColor(T["text"]))
            if lab is not None:
                f.setPixelSize(max(7, int(u * (0.26 if len(lab) > 2 else 0.34)))); p.setFont(f)
                p.drawText(r, Qt.AlignmentFlag.AlignCenter, lab)
            else:
                f.setPixelSize(max(8, int(u * 0.36))); f.setBold(True); p.setFont(f)
                p.drawText(r.adjusted(u * 0.12, u * 0.04, 0, 0), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop, c1.upper())
                if c2 and c2.upper() != c1.upper():
                    f.setBold(False); f.setPixelSize(max(7, int(u * 0.3))); p.setFont(f)
                    p.setPen(QColor("white") if down else QColor(a).lighter(130))
                    p.drawText(r.adjusted(0, 0, -u * 0.12, -u * 0.02), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignBottom, c2.upper())
        # нижній рядок-підказка
        f = QFont(self.font_ui); f.setPixelSize(11); p.setFont(f); p.setPen(QColor(T["text2"]))
        if self.testing:
            txt = "Натискайте клавіші — гарячі клавіші mgOS вимкнені на час перевірки (вихід — кнопка або 45 с без натискань)"
        else:
            n = len(self.tested)
            txt = (f"Перевірено клавіш: {n}" + (f" · спецклавіш поза схемою: {len(self.extra)}" if self.extra else "")) if n else \
                  "«Перевірити клавіші» — натискайте будь-які, схема покаже, що є на вашій клавіатурі"
        p.drawText(QRectF(0, body.bottom() + 6, self.width(), 18), Qt.AlignmentFlag.AlignCenter, txt)
