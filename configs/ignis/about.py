import subprocess
import json
import os
import threading
from ignis import widgets, utils, app
from gi.repository import Gtk, Gdk, GLib

# --- ДОПОМІЖНІ ФУНКЦІЇ ---
def run_cmd(cmd):
    try:
        return subprocess.check_output(f"LC_ALL=C {cmd}", shell=True, stderr=subprocess.DEVNULL).decode("utf-8").strip()
    except:
        return "N/A"

def run_async(cmd):
    subprocess.Popen(cmd, shell=True)

def get_updates_count():
    try:
        output = subprocess.check_output("checkupdates | wc -l", shell=True, stderr=subprocess.DEVNULL).decode().strip()
        return output if output else "0"
    except:
        return "N/A"

def get_nvidia_info():
    try:
        raw = run_cmd("nvidia-smi --query-gpu=driver_version,name --format=csv,noheader")
        if raw and "N/A" not in raw:
            parts = raw.split(',')
            return parts[0].strip(), parts[1].strip()
    except: pass
    return "Інтегрована / Невідомо", "Intel/AMD Graphics"

def get_ascii_logo():
    return r"""
       /\
      /  \
     /    \
    /      \
   /   ,,   \
  /   |  |   \
 /_-''    ''-_\
"""

def CustomButton(label, on_click, bg_var="var(--accent)"):
    lbl = widgets.Label(label=label, style="font-weight: bold; color: var(--bg-color);")
    
    box = widgets.Box(
        child=[lbl],
        style=f"background-color: {bg_var}; border-radius: 12px; padding: 8px 16px; transition: 0.2s;",
        css_classes=["clickable-box"]
    )
    
    gesture = Gtk.GestureClick()
    gesture.connect("released", lambda x, n, a, b: on_click())
    box.add_controller(gesture)

    def on_hover(c, x, y):
        box.set_cursor(Gdk.Cursor.new_from_name("pointer", None))
        box.set_style(f"background-color: {bg_var}; border-radius: 12px; padding: 8px 16px; transform: scale(1.05); filter: brightness(1.2); transition: 0.2s;")
    
    def on_leave(c):
        box.set_cursor(None)
        box.set_style(f"background-color: {bg_var}; border-radius: 12px; padding: 8px 16px; transform: scale(1.0); transition: 0.2s;")

    motion = Gtk.EventControllerMotion()
    motion.connect("enter", on_hover)
    motion.connect("leave", on_leave)
    box.add_controller(motion)

    return box

# --- НОВА АНІМОВАНА КНОПКА ОНОВЛЕННЯ ---
def AnimatedUpdateButton(on_finish_callback):
    # Створюємо системний спінер (крутилку)
    spinner = Gtk.Spinner()
    spinner.set_visible(False) # Ховаємо її за замовчуванням
    
    lbl = widgets.Label(label="󰚰 Оновити", style="font-weight: bold; color: var(--bg-color);")
    
    # Пакуємо крутилку і текст разом
    content = widgets.Box(spacing=8, child=[spinner, lbl])
    bg_var = "var(--success)"
    
    box = widgets.Box(
        child=[content],
        style=f"background-color: {bg_var}; border-radius: 12px; padding: 8px 16px; transition: 0.2s;",
        css_classes=["clickable-box"]
    )
    
    def run_update_process():
        # Запускаємо yay. Додано 'read' в кінці, щоб термінал не зникав відразу і ти бачив результат
        subprocess.run("kitty -e sh -c 'yay -Syu; echo \"\n✅ Оновлення завершено! Натисніть Enter для виходу...\"; read'", shell=True)
        # Коли термінал закриється — повертаємо UI в норму (робимо це в головному потоці через GLib)
        GLib.idle_add(finish_update)

    def finish_update():
        spinner.stop()
        spinner.set_visible(False)
        lbl.set_label("󰚰 Оновити")
        box.set_sensitive(True) # Розблокувати кнопку
        if on_finish_callback:
            on_finish_callback() # Оновлюємо лічильник пакетів в меню "About"

    def on_click():
        box.set_sensitive(False) # Блокуємо від повторного кліку
        lbl.set_label("Оновлюю...")
        spinner.set_visible(True)
        spinner.start()
        # Запускаємо термінал в окремому потоці, щоб інтерфейс Ignis не завис
        threading.Thread(target=run_update_process, daemon=True).start()

    # Вішаємо клік
    gesture = Gtk.GestureClick()
    gesture.connect("released", lambda x, n, a, b: on_click())
    box.add_controller(gesture)

    # Анімації наведення (працюють тільки коли кнопка не заблокована)
    def on_hover(c, x, y):
        if box.is_sensitive():
            box.set_cursor(Gdk.Cursor.new_from_name("pointer", None))
            box.set_style(f"background-color: {bg_var}; border-radius: 12px; padding: 8px 16px; transform: scale(1.05); filter: brightness(1.2); transition: 0.2s;")
    
    def on_leave(c):
        if box.is_sensitive():
            box.set_cursor(None)
            box.set_style(f"background-color: {bg_var}; border-radius: 12px; padding: 8px 16px; transform: scale(1.0); transition: 0.2s;")

    motion = Gtk.EventControllerMotion()
    motion.connect("enter", on_hover)
    motion.connect("leave", on_leave)
    box.add_controller(motion)

    return box


def AboutWidget():
    driver_ver, gpu_name = get_nvidia_info()
    cpu_name = run_cmd("awk -F: '/model name/ {print $2; exit}' /proc/cpuinfo | sed 's/^ //'")
    ram_info = run_cmd("free -h | awk 'NR==2{print $3 \" / \" $2}'")
    disk_info = run_cmd("df -h / | awk 'NR==2{print $3 \" / \" $2}'")
    kernel_ver = run_cmd("uname -r")
    uptime = run_cmd("uptime -p | sed 's/up //'")
    
    # --- ДИНАМІЧНИЙ ЛІЧИЛЬНИК ПАКЕТІВ ---
    pkgs_label = widgets.Label(label="Рахую...", halign="end", hexpand=True, style="color: var(--text-sub);")

    def refresh_packages_info():
        pkgs = run_cmd("pacman -Qq | wc -l")
        updates_count = get_updates_count()
        pkgs_label.set_label(f"{pkgs} (Оновлень: {updates_count})")

    # Запускаємо підрахунок при відкритті вікна
    refresh_packages_info()
    
    logo_label = widgets.Label(
        label=get_ascii_logo(),
        style="font-family: monospace; font-size: 16px; font-weight: 900; color: var(--accent); margin-right: 25px;"
    )

    def row(icon, title, widget_or_text):
        if isinstance(widget_or_text, str):
            val_widget = widgets.Label(label=widget_or_text, halign="end", hexpand=True, style="color: var(--text-sub);")
        else:
            val_widget = widget_or_text

        return widgets.Box(spacing=10, child=[
            widgets.Label(label=icon, style="font-size: 16px; color: var(--accent);"),
            widgets.Label(label=title, style="font-weight: bold; color: var(--text-main);", halign="start"),
            val_widget
        ])

    info_box = widgets.Box(vertical=True, spacing=8, child=[
        widgets.Label(label="Arch Linux (MeguminOS)", style="font-size: 22px; font-weight: 900; color: var(--accent); margin-bottom: 15px;", halign="start"),
        row("", "CPU:", cpu_name),
        row("󰢮", "GPU:", gpu_name),
        row("󰘚", "RAM:", ram_info),
        row("󰋊", "Disk (/):", disk_info),
        row("", "Kernel:", kernel_ver),
        row("󰔚", "Uptime:", uptime),
        widgets.Box(height_request=5),
        row("󰏗", "Packages:", pkgs_label) # Тепер тут стоїть Label, який вміє сам оновлюватися
    ])

    buttons_row = widgets.Box(spacing=10, halign="end", child=[
        CustomButton("󰒋 btop", lambda: run_async("kitty -e btop"), bg_var="var(--warning)"),
        AnimatedUpdateButton(on_finish_callback=refresh_packages_info), # ДОДАЛИ НОВУ КНОПКУ
        CustomButton("󰅙 Закрити", lambda: app.get_default().get_window("about").set_visible(False), bg_var="var(--danger)")
    ])

    content = widgets.Box(spacing=0, child=[
        logo_label,
        widgets.Box(vertical=True, spacing=10, hexpand=True, child=[
            info_box,
            widgets.Box(height_request=15),
            buttons_row
        ])
    ])

    main_card = widgets.Box(
        child=[content],
        css_classes=["block", "dark"],
        style="background-color: var(--bg-color); border: 1px solid var(--border-color); border-radius: 16px; padding: 30px; min-width: 550px;"
    )

    return main_card

def setup(app):
    window = widgets.Window(
        name="about",
        namespace="ignis_about",
        anchor=[], 
        visible=False,
        kb_mode="exclusive",
        style="background-color: transparent;",
        child=AboutWidget()
    )
    
    window.remove_css_class("background")
    
    key_ctl = Gtk.EventControllerKey()
    def on_key(controller, keyval, keycode, state):
        if keyval == Gdk.KEY_Escape:
            window.visible = False
            return True
        return False
    key_ctl.connect("key-pressed", on_key)
    window.add_controller(key_ctl)

    app.add_window(window=window, window_name="about")