"""Alt+Tab для mgOS (Ignis).

Чому раніше гальмувало: на першому натисканні синхронно запускались hyprctl activewindow,
2× hyprctl clients і grim повного вікна (PNG 1920×1080 — сотні мс), а потім GTK декодував
повнорозмірні PNG усіх вікон через CSS background-image. Тепер:
  • список вікон — напряму через сокет Hyprland у цьому ж процесі (без hyprctl);
  • вікно показується одразу, мініатюри — маленькі (25%), знімаються у фоні після зміни фокусу;
  • декодовані текстури кешуються в пам'яті, поки файл не змінився;
  • фокус перемикається через той самий сокет.
"""
import json
import os
import subprocess
from gi.repository import Gio, GLib, Gtk, Gdk
from ignis import widgets
from ignis.services.hyprland import HyprlandService

CACHE_DIR = os.path.expanduser("~/.config/ignis/cache/switcher")
os.makedirs(CACHE_DIR, exist_ok=True)

THUMB_W, THUMB_H = 240, 150

switcher_window = None
items_box = None
windows_list = []
item_widgets = []
current_index = 0
_tex_cache = {}          # path -> (mtime, Gdk.Texture)
_shot_timer = 0
hypr = None

ICON_MAP = {"code": "visual-studio-code", "telegram": "telegram-desktop", "kitty": "kitty",
            "thunar": "system-file-manager", "nautilus": "org.gnome.Nautilus", "megumin-settings": "preferences-system"}


def _ipc(cmd):
    try:
        return hypr.send_command(cmd)
    except Exception:
        return ""


def get_windows():
    try:
        data = json.loads(_ipc("j/clients") or "[]")
    except Exception:
        return []
    valid = [c for c in data if c.get("mapped", True) and not c.get("hidden") and c.get("title")
             and c.get("class") not in ("ignis", "") and c.get("workspace", {}).get("id", 1) > 0]
    return sorted(valid, key=lambda x: x.get("focusHistoryID", 0))


def thumb_path(addr):
    return f"{CACHE_DIR}/{addr}.png"


def snapshot_active():
    """Мініатюра активного вікна у фоні (grim у 25% розміру — швидко й легко декодувати)."""
    global _shot_timer
    _shot_timer = 0
    try:
        a = json.loads(_ipc("j/activewindow") or "{}")
    except Exception:
        return False
    addr = a.get("address")
    if not addr or a.get("class") == "ignis" or (switcher_window and switcher_window.get_visible()):
        return False
    (x, y), (w, h) = a.get("at", [0, 0]), a.get("size", [0, 0])
    if w < 40 or h < 40:
        return False
    out = thumb_path(addr)
    try:
        subprocess.Popen(["nice", "-n", "19", "sh", "-c",
                          f'grim -s 0.25 -l 1 -g "{x},{y} {w}x{h}" "{out}.tmp" && mv -f "{out}.tmp" "{out}"'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    except Exception:
        pass
    return False


def on_active_changed(*_):
    # через 700 мс після зміни фокусу (вікно встигло домалюватись) — один знімок, з дебаунсом
    global _shot_timer
    if _shot_timer:
        GLib.source_remove(_shot_timer)
    _shot_timer = GLib.timeout_add(700, snapshot_active)


def load_texture(path):
    try:
        mt = os.path.getmtime(path)
    except OSError:
        return None
    hit = _tex_cache.get(path)
    if hit and hit[0] == mt:
        return hit[1]
    try:
        tex = Gdk.Texture.new_from_filename(path)
    except Exception:
        return None
    _tex_cache[path] = (mt, tex)
    return tex


def get_icon(class_name):
    name = (class_name or "unknown").lower()
    for key, val in ICON_MAP.items():
        if key in name:
            return val
    return name


def make_item(c):
    overlay = Gtk.Overlay()
    overlay.set_size_request(THUMB_W, THUMB_H)
    overlay.add_css_class("sw-thumb")
    tex = load_texture(thumb_path(c["address"]))
    if tex:
        pic = Gtk.Picture.new_for_paintable(tex)
        pic.set_content_fit(Gtk.ContentFit.COVER)
        pic.set_can_shrink(True)
        overlay.set_child(pic)
    else:
        ph = Gtk.Image.new_from_icon_name(get_icon(c.get("class")))
        ph.set_pixel_size(64)
        ph.add_css_class("sw-placeholder")
        overlay.set_child(ph)
    badge = Gtk.Image.new_from_icon_name(get_icon(c.get("class")))
    badge.set_pixel_size(36)
    badge.set_halign(Gtk.Align.CENTER)
    badge.set_valign(Gtk.Align.END)
    badge.set_margin_bottom(-14)
    badge.add_css_class("sw-badge")
    overlay.add_overlay(badge)

    title = Gtk.Label(label=c.get("title", ""), max_width_chars=26, ellipsize=3)  # PANGO_ELLIPSIZE_END
    title.add_css_class("sw-title")
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=18)
    box.add_css_class("sw-item")
    box.append(overlay)
    box.append(title)
    return box


def rebuild_list():
    global windows_list, item_widgets
    windows_list = get_windows()
    child = items_box.get_first_child()
    while child:
        items_box.remove(child)
        child = items_box.get_first_child()
    item_widgets = []
    for c in windows_list:
        w = make_item(c)
        items_box.append(w)
        item_widgets.append(w)


def update_selection():
    for i, w in enumerate(item_widgets):
        if i == current_index:
            w.add_css_class("selected")
        else:
            w.remove_css_class("selected")


def _step(delta):
    global current_index
    if not switcher_window:
        return
    if not switcher_window.get_visible():
        rebuild_list()
        if not windows_list:
            return
        current_index = (1 if delta > 0 else len(windows_list) - 1) if len(windows_list) > 1 else 0
        update_selection()
        switcher_window.set_visible(True)
        return
    if windows_list:
        current_index = (current_index + delta) % len(windows_list)
    update_selection()


def step_next(action, param): _step(+1)
def step_prev(action, param): _step(-1)


def on_release(action, param):
    if switcher_window and switcher_window.get_visible():
        switcher_window.set_visible(False)
        if windows_list and current_index < len(windows_list):
            _ipc(f"dispatch focuswindow address:{windows_list[current_index]['address']}")


CSS = """
.sw-bg { background-color: transparent; }
.sw-panel {
    background-color: alpha(#1e1e20, 0.90);
    border: 1px solid alpha(white, 0.12);
    border-radius: 28px;
    padding: 26px 28px 22px 28px;
    box-shadow: 0 18px 50px alpha(black, 0.45);
}
.sw-item { padding: 10px; border-radius: 20px; border: 2px solid transparent;
           transition: background-color 120ms ease, border-color 120ms ease; }
.sw-item.selected { background-color: alpha(white, 0.10); border-color: alpha(white, 0.28); }
.sw-thumb { border-radius: 14px; background-color: alpha(white, 0.06); }
.sw-thumb picture { border-radius: 14px; }
.sw-placeholder { opacity: 0.55; }
.sw-badge { -gtk-icon-shadow: 0 2px 6px alpha(black, 0.6); }
.sw-title { color: #f5f5f7; font-size: 12px; font-weight: 600; }
.sw-item:not(.selected) .sw-title { color: alpha(#f5f5f7, 0.65); }
"""


def setup(app_instance):
    global switcher_window, items_box, hypr
    hypr = HyprlandService.get_default()

    prov = Gtk.CssProvider()
    prov.load_from_string(CSS)
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), prov,
                                              Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION + 2)

    items_box = Gtk.Box(spacing=14)
    items_box.add_css_class("sw-panel")
    switcher_window = widgets.Window(
        name="switcher",
        namespace="ignis_switcher",
        visible=False,
        kb_mode="none",          # клавіші ловлять бінди Hyprland (Alt+Tab / відпускання Alt)
        layer="overlay",
        css_classes=["sw-bg"],
        child=items_box,
    )
    app_instance.add_window(window=switcher_window, window_name="switcher")

    for name, cb in [("next", step_next), ("prev", step_prev), ("release", on_release)]:
        act = Gio.SimpleAction.new(f"switcher-{name}", None)
        act.connect("activate", cb)
        app_instance.add_action(act)

    hypr.connect("notify::active-window", on_active_changed)
    on_active_changed()
