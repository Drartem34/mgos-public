import subprocess
import json
import os
from gi.repository import Gio, Gtk, Gdk
from ignis import widgets

# --- НАЛАШТУВАННЯ ---
CACHE_DIR = os.path.expanduser("~/.config/ignis/cache/switcher")
os.makedirs(CACHE_DIR, exist_ok=True)

# Глобальні змінні
switcher_window = None
items_box = None
windows_list = []
current_index = 0

def get_windows():
    try:
        ret = subprocess.check_output(["hyprctl", "clients", "-j"], text=True)
        data = json.loads(ret)
        # Ігноруємо ignis та невидимі вікна (прибрав зайвий фільтр mapped, щоб бачило всі вікна)
        valid = [c for c in data if c.get("class") != "ignis" and c.get("title")]
        # Сортуємо за історією фокусу (активне - останнє)
        return sorted(valid, key=lambda x: x.get('focusHistoryID', 0))
    except Exception as e:
        print(f"Помилка отримання вікон: {e}")
        return []

def take_screenshot():
    """Робить скріншот ТІЛЬКИ активного вікна (на чистому Python)"""
    try:
        active_out = subprocess.check_output(["hyprctl", "activewindow", "-j"], text=True)
        active_data = json.loads(active_out)
        addr = active_data.get("address")
        if not addr: return
        
        clients_out = subprocess.check_output(["hyprctl", "clients", "-j"], text=True)
        clients = json.loads(clients_out)
        
        for c in clients:
            if c.get("address") == addr:
                x, y = c.get("at", [0, 0])
                w, h = c.get("size", [0, 0])
                path = f"{CACHE_DIR}/{addr}.png"
                
                geom = f"{x},{y} {w}x{h}"
                subprocess.run(["grim", "-g", geom, path])
                break
    except Exception as e:
        pass

def get_icon(class_name):
    icon_map = {
        "code": "visual-studio-code", 
        "telegram": "telegram-desktop", 
        "firefox": "firefox", 
        "kitty": "terminal",
        "thunar": "system-file-manager"
    }
    name = class_name.lower() if class_name else "unknown"
    for key, val in icon_map.items():
        if key in name: return val
    return name

def rebuild_list():
    global windows_list
    windows_list = get_windows()
    
    child = items_box.get_first_child()
    while child:
        items_box.remove(child)
        child = items_box.get_first_child()

    for c in windows_list:
        addr = c["address"]
        thumb = f"{CACHE_DIR}/{addr}.png"
        bg = f"background-image: url('file://{thumb}');" if os.path.exists(thumb) else "background-color: #313244;"
        
        item = widgets.Box(
            vertical=True,
            css_classes=["switcher-item"],
            style=f"min-width: 260px; min-height: 146px; border-radius: 12px; background-size: cover; background-position: center; {bg}",
            child=[
                widgets.Box(
                    vertical=True,
                    style="background: linear-gradient(180deg, rgba(0,0,0,0.5) 0%, rgba(0,0,0,0) 50%, rgba(0,0,0,0.8) 100%); padding: 12px; flex-grow: 1; border-radius: 12px;",
                    child=[
                        widgets.Icon(image=get_icon(c.get("class")), pixel_size=32, halign="start"),
                        widgets.Box(vexpand=True),
                        widgets.Label(label=c.get("title", "")[:25], halign="start", style="color: white; font-weight: bold; font-size: 11px; text-shadow: 0 1px 2px black;")
                    ]
                )
            ]
        )
        items_box.append(item)

def update_selection():
    if not items_box: return
    child = items_box.get_first_child()
    i = 0
    while child:
        if i == current_index:
            child.add_css_class("selected")
        else:
            child.remove_css_class("selected")
        child = child.get_next_sibling()
        i += 1

def step_next(action, param):
    global current_index, switcher_window
    if not switcher_window: return

    # Використовуємо надійний get_visible()
    if not switcher_window.get_visible():
        take_screenshot()
        rebuild_list()
        if not windows_list: return # Якщо вікон немає - не відкриваємо порожнє меню
        
        switcher_window.set_visible(True)
        current_index = 1 if len(windows_list) > 1 else 0
    else:
        if windows_list:
            current_index = (current_index + 1) % len(windows_list)
    
    update_selection()

def on_release(action, param):
    global switcher_window
    if switcher_window and switcher_window.get_visible():
        if windows_list and current_index < len(windows_list):
            addr = windows_list[current_index]["address"]
            subprocess.Popen(f"hyprctl dispatch focuswindow address:{addr}", shell=True)
        switcher_window.set_visible(False)

def setup(app_instance):
    global switcher_window, items_box
    items_box = widgets.Box(spacing=20)
    
    switcher_window = widgets.Window(
        name="switcher",
        namespace="ignis_switcher",
        visible=False,
        kb_mode="on_demand",
        layer="overlay",
        css_classes=["switcher-bg"],
        child=widgets.Box(css_classes=["switcher-container"], child=[items_box])
    )
    
    # ОСЬ ТУТ БУВ МІЙ КОСЯК - треба суворо передавати kwargs
    app_instance.add_window(window=switcher_window, window_name="switcher")

    for name, cb in [("next", step_next), ("release", on_release)]:
        act = Gio.SimpleAction.new(f"switcher-{name}", None)
        act.connect("activate", cb)
        app_instance.add_action(act)