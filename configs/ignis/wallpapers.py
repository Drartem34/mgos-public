import os
import subprocess
import urllib.parse
import hashlib
import threading
import json
import datetime
from concurrent.futures import ThreadPoolExecutor
import gi
from gi.repository import Gio, Gtk, Gdk, GdkPixbuf, GLib
from ignis import widgets, utils

# --- НАЛАШТУВАННЯ ---
WALLPAPER_DIR = os.path.expanduser("~/Pictures/wallpapers")
CACHE_DIR = os.path.expanduser("~/.cache/ignis/wallpaper_thumbs")
LAST_WALLPAPER_FILE = os.path.expanduser("~/.cache/ignis/last_wallpaper.txt")
THUMB_SIZE = 300  
MAX_WORKERS = 4   

os.makedirs(CACHE_DIR, exist_ok=True)
os.makedirs(os.path.dirname(LAST_WALLPAPER_FILE), exist_ok=True)

wp_window = None
wp_flowbox = None
images_cache = []  
thumbnail_cache = {}  
current_dynamic_folder = None 
current_category = "all" 

def ClickableBox(child, on_click, css_classes=[], spacing=0):
    box = widgets.Box(child=[child] if child else [], css_classes=css_classes, spacing=spacing)
    gesture = Gtk.GestureClick()
    gesture.connect("released", lambda gesture, n, x, y: on_click(gesture.get_widget()))
    box.add_controller(gesture)
    motion = Gtk.EventControllerMotion()
    motion.connect("enter", lambda c, x, y: c.get_widget().set_cursor(Gdk.Cursor.new_from_name("pointer", None)) if c.get_widget() else None)
    motion.connect("leave", lambda c: c.get_widget().set_cursor(None) if c.get_widget() else None)
    box.add_controller(motion)
    return box

def get_images():
    global images_cache
    if images_cache: return images_cache
    if not os.path.exists(WALLPAPER_DIR): return []
    valid_extensions = ('.png', '.jpg', '.jpeg', '.webp', '.mp4', '.webm', '.gif')
    dynamic_items, static_items = [], []
    for f in os.listdir(WALLPAPER_DIR):
        full_path = os.path.join(WALLPAPER_DIR, f)
        if os.path.isdir(full_path): dynamic_items.append(full_path)
        elif os.path.isfile(full_path) and f.lower().endswith(valid_extensions): static_items.append(full_path)
    images_cache = sorted(dynamic_items) + sorted(static_items)
    return images_cache

def force_refresh_cache(x=None):
    global images_cache
    images_cache = [] # Скидаємо кеш, щоб перечитати папку
    populate_grid()

def get_thumb_source(original_path):
    if os.path.isdir(original_path):
        for ext in ['jpg', 'png', 'jpeg', 'webp', 'mp4', 'gif']:
            p = os.path.join(original_path, f"day.{ext}")
            if os.path.exists(p): return p
        files = [f for f in os.listdir(original_path) if f.split('.')[-1] in ['jpg', 'png', 'jpeg', 'webp', 'mp4', 'gif']]
        return os.path.join(original_path, files[0]) if files else None
    return original_path

def get_thumbnail_path(original_path):
    filename_hash = hashlib.md5(original_path.encode()).hexdigest()
    return os.path.join(CACHE_DIR, f"{filename_hash}.png")

def generate_thumbnail(original_path):
    thumb_path = get_thumbnail_path(original_path)
    if thumb_path in thumbnail_cache: return thumbnail_cache[thumb_path]
    source_path = get_thumb_source(original_path)
    if not source_path: return original_path 
    try:
        if not os.path.exists(thumb_path):
            ext = source_path.lower().split('.')[-1]
            if ext in ['mp4', 'webm', 'gif']:
                subprocess.run(f"ffmpeg -i '{source_path}' -ss 00:00:00.000 -vframes 1 -s {THUMB_SIZE}x{THUMB_SIZE} '{thumb_path}' -y", shell=True, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
            else:
                pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(source_path, THUMB_SIZE, -1, True)
                pixbuf.savev(thumb_path, "png", [], [])
        thumbnail_cache[thumb_path] = thumb_path
        return thumb_path
    except: return source_path

def create_card_widget(item_path, thumb_path):
    filename = os.path.basename(item_path)
    label_text = f"🕒 {filename}" if os.path.isdir(item_path) else (f"🎬 {filename}" if item_path.lower().split('.')[-1] in ['mp4', 'webm', 'gif'] else filename)
    safe_thumb_path = urllib.parse.quote(thumb_path)
    card_content = widgets.Box(vertical=True, child=[widgets.Box(hexpand=True, vexpand=True), widgets.Box(css_classes=["wp-label-box"], child=[widgets.Label(label=label_text, css_classes=["wp-label"], ellipsize="end", max_width_chars=18)])])
    btn = ClickableBox(child=card_content, on_click=lambda x, p=item_path: handle_wallpaper_selection(p), css_classes=["wp-card"])
    btn.set_style(f"background-image: url('file://{safe_thumb_path}'); background-size: cover; background-position: center;")
    return btn

def populate_grid():
    if not wp_flowbox: return
    while wp_flowbox.get_child_at_index(0): wp_flowbox.remove(wp_flowbox.get_child_at_index(0))
    images = get_images()
    filtered = []
    for path in images:
        is_dir = os.path.isdir(path)
        ext = path.lower().split('.')[-1] if not is_dir else ""
        if current_category == "all": filtered.append(path)
        elif current_category == "dynamic" and is_dir: filtered.append(path)
        elif current_category == "videos" and ext in ['mp4', 'webm', 'gif']: filtered.append(path)
        elif current_category == "images" and ext in ['png', 'jpg', 'jpeg', 'webp']: filtered.append(path)
    
    for img_path in filtered:
        thumb_path = get_thumbnail_path(img_path)
        if os.path.exists(thumb_path): wp_flowbox.append(create_card_widget(img_path, thumb_path))
        else: threading.Thread(target=lambda p=img_path: GLib.idle_add(lambda: wp_flowbox.append(create_card_widget(p, generate_thumbnail(p)))), daemon=True).start()

def set_category(cat):
    global current_category
    current_category = cat
    populate_grid()

def apply_actual_wallpaper(path):
    ext = path.lower().split('.')[-1]
    is_video = ext in ['mp4', 'webm', 'gif']
    
    if is_video:
        # Для відео шпалер
        subprocess.run("pkill -9 mpvpaper", shell=True, stderr=subprocess.DEVNULL)
        subprocess.run("killall awww-daemon", shell=True, stderr=subprocess.DEVNULL)
        subprocess.run("rm -f /tmp/mpv-socket", shell=True, stderr=subprocess.DEVNULL)
        
        anime4k_enabled = False
        config_path = os.path.expanduser("~/.config/megumin-os/settings.json")
        if os.path.exists(config_path):
            try:
                import json
                with open(config_path, "r") as f:
                    anime4k_enabled = json.load(f).get("wp_anime4k", False)
            except: pass
            
        shaders_opt = "glsl-shaders=/home/doki/.config/mpv/shaders/Anime4K_Restore_CNN_VL.glsl,/home/doki/.config/mpv/shaders/Anime4K_Upscale_CNN_x2_VL.glsl,/home/doki/.config/mpv/shaders/Anime4K_Restore_CNN_M.glsl" if anime4k_enabled else "glsl-shaders="
        os.system(f"nohup mpvpaper -o 'config=no {shaders_opt} input-ipc-server=/tmp/mpv-socket loop-file=inf hwdec=auto no-audio panscan=1.0' '*' '{path}' >/dev/null 2>&1 &")
    else:
        # Для звичайних картинок
        subprocess.run("pkill -9 mpvpaper", shell=True, stderr=subprocess.DEVNULL)
        subprocess.run("pgrep awww-daemon || (awww-daemon & sleep 0.5)", shell=True)
        subprocess.Popen(f"awww img '{path}' --resize crop --transition-type fade --transition-angle 30 --transition-fps 60 --transition-duration 2", shell=True)

    def update_theme_colors():
        if is_video:
            # ФІКС 1: Робимо унікальне ім'я файлу, щоб wal не використовував старий кеш
            safe_name = os.path.basename(path).replace(" ", "_")
            target = os.path.expanduser(f"~/.cache/ignis/sddm_frame_{safe_name}.png")
            subprocess.run(f"ffmpeg -i '{path}' -ss 00:00:00.000 -vframes 1 '{target}' -y", shell=True, stderr=subprocess.DEVNULL)
        else:
            target = path

        # Генеруємо нові кольори
        subprocess.run(f"wal -q -t -i '{target}'", shell=True)
        
        # Перезапускаємо Waybar для підтягування стилів
        subprocess.run("killall -SIGUSR2 waybar", shell=True)
        
        # ФІКС 2: Popen замість run, щоб sudo не блокував скрипт, якщо попросить пароль
        subprocess.Popen("sudo /home/doki/.config/megumin-os/sync_sddm.sh", shell=True)
        
        def reload_css():
            from gi.repository import Gtk, Gdk
            try:
                with open(os.path.expanduser("~/.cache/wal/colors.css"), 'r') as f: wal_css = f.read()
                with open(os.path.expanduser("~/.config/ignis/style.css"), 'r') as f: main_css = f.read()
                
                # Додаємо глобальну плавність для всіх елементів (0.8 секунди)
                smooth_transition = """
                * {
                    transition: background-color 0.5s ease-out, color 0.5s ease-out, border-color 0.5s ease-out;
                }
                """
                
                # Зліплюємо все до купи
                css = (wal_css + "\n" + main_css + "\n" + smooth_transition).replace('@import url("file:///home/doki/.cache/wal/colors.css");', '')
                
                provider = Gtk.CssProvider()
                provider.load_from_data(css.encode('utf-8'))
                Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
            except: pass
        
        GLib.idle_add(reload_css)

    # ФІКС 3: Запускаємо важкі процеси у фоні, щоб не фрізити вікно
    threading.Thread(target=update_theme_colors, daemon=True).start()

def handle_wallpaper_selection(path):
    global current_dynamic_folder
    with open(LAST_WALLPAPER_FILE, "w") as f: f.write(path)
    if os.path.isdir(path):
        current_dynamic_folder = path
        check_and_apply_dynamic_wallpaper() 
    else:
        current_dynamic_folder = None 
        apply_actual_wallpaper(path)
    if wp_window: wp_window.set_visible(False)

def check_and_apply_dynamic_wallpaper():
    global current_dynamic_folder
    if not current_dynamic_folder: return True 
    hour = datetime.datetime.now().hour
    period = "morning" if 6 <= hour < 12 else "day" if 12 <= hour < 18 else "evening" if 18 <= hour < 22 else "night"
    for ext in ['jpg', 'png', 'jpeg', 'webp', 'mp4', 'webm', 'gif']:
        target = os.path.join(current_dynamic_folder, f"{period}.{ext}")
        if os.path.exists(target): apply_actual_wallpaper(target); break
    return True 

def on_toggle(action, param):
    if wp_window:
        wp_window.visible = not wp_window.visible
        if wp_window.visible:
            global images_cache
            images_cache = [] # Автоматично скидаємо кеш при кожному відкритті вікна
            populate_grid()

from ignis.services.hyprland import HyprlandService

def check_video_optimization(hypr_svc=None, *args):
    try:
        config_path = os.path.expanduser("~/.config/megumin-os/settings.json")
        opt_enabled = False
        if os.path.exists(config_path):
            with open(config_path, "r") as f:
                opt_enabled = json.load(f).get("video_optimization", False)
        
        if not opt_enabled:
            os.system("echo '{ \"command\": [\"set_property\", \"pause\", false] }' | socat - /tmp/mpv-socket 2>/dev/null")
            return

        hypr = HyprlandService.get_default()
        window = hypr.active_window
        if window and window.address:
            os.system("echo '{ \"command\": [\"set_property\", \"pause\", true] }' | socat - /tmp/mpv-socket 2>/dev/null")
        else:
            os.system("echo '{ \"command\": [\"set_property\", \"pause\", false] }' | socat - /tmp/mpv-socket 2>/dev/null")
    except:
        pass

def setup(app_instance):
    global wp_window, wp_flowbox
    
    threading.Thread(target=lambda: [generate_thumbnail(img) for img in get_images()], daemon=True).start()
    GLib.timeout_add_seconds(600, check_and_apply_dynamic_wallpaper)
    
    hypr = HyprlandService.get_default()
    hypr.connect("notify::active-window", check_video_optimization)
    check_video_optimization()
    
    wp_flowbox = Gtk.FlowBox(valign=Gtk.Align.START, max_children_per_line=30, min_children_per_line=1, selection_mode=Gtk.SelectionMode.NONE, column_spacing=20, row_spacing=20)
    scroll = widgets.Scroll(vexpand=True, hexpand=True, child=wp_flowbox, css_classes=["wp-scroll"])
    
    tabs_box = widgets.Box(spacing=15, halign="center", css_classes=["wp-tabs-box"], child=[
        ClickableBox(child=widgets.Label(label="Всі", css_classes=["wp-tab-label"]), on_click=lambda x: set_category("all"), css_classes=["wp-tab-btn"]),
        ClickableBox(child=widgets.Label(label="🖼 Картинки", css_classes=["wp-tab-label"]), on_click=lambda x: set_category("images"), css_classes=["wp-tab-btn"]),
        ClickableBox(child=widgets.Label(label="🎬 Відео", css_classes=["wp-tab-label"]), on_click=lambda x: set_category("videos"), css_classes=["wp-tab-btn"]),
        ClickableBox(child=widgets.Label(label="🕒 Динамічні", css_classes=["wp-tab-label"]), on_click=lambda x: set_category("dynamic"), css_classes=["wp-tab-btn"]),
    ])

    # Оновлений хедер з кнопкою "Оновити"
    header_box = widgets.Box(spacing=10, child=[
        widgets.Label(label="Галерея шпалер", css_classes=["wp-title"], halign="start", hexpand=True), 
        ClickableBox(child=widgets.Label(label="🔄 Оновити", css_classes=["wp-close-label"]), on_click=force_refresh_cache, css_classes=["wp-close-btn"]),
        ClickableBox(child=widgets.Label(label="Закрити", css_classes=["wp-close-label"]), on_click=lambda x: on_toggle(None, None), css_classes=["wp-close-btn"])
    ])

    wp_window = widgets.Window(
        name="wallpaper_selector", namespace="ignis_wallpapers", width_request=950, height_request=700, visible=False, kb_mode="on_demand", css_classes=["wp-window"],
        child=widgets.Box(vertical=True, spacing=15, css_classes=["wp-container"], child=[
            header_box,
            tabs_box, scroll
        ])
    )

    key_controller = Gtk.EventControllerKey()
    key_controller.connect("key-pressed", lambda c, k, kc, s: wp_window.set_visible(False) if k == Gdk.KEY_Escape else False)
    wp_window.add_controller(key_controller)
    
    app_instance.add_window(window=wp_window, window_name="wallpaper_selector")
    action = Gio.SimpleAction.new("toggle-wallpapers", None)
    action.connect("activate", on_toggle)
    app_instance.add_action(action)

    # --- ВІДНОВЛЕННЯ ПРИ ЗАПУСКУ ---
    def restore():
        if os.path.exists(LAST_WALLPAPER_FILE):
            with open(LAST_WALLPAPER_FILE, "r") as f: path = f.read().strip()
            if os.path.exists(path): apply_actual_wallpaper(path)
    
    utils.Timeout(1000, restore)