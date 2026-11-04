import os
import json
from gi.repository import Gtk, GLib, Gdk
from ignis.widgets import Widget
from ignis import utils
from ignis import widgets

HIDDEN_NOTIF_IDS = set()
PINNED_NOTIF_IDS = set()

# Допоміжна функція для створення кліків (з config.py)
def ClickableBox(child, on_click, css_classes=[], spacing=0, centered=False, **kwargs):
    if centered and child:
        child.set_halign("center")
        child.set_valign("center")
        child.set_hexpand(True)
        child.set_vexpand(True)
    
    classes = css_classes + ["unset", "clickable-box"]
    box = widgets.Box(child=[child] if child else [], css_classes=classes, spacing=spacing, **kwargs)
    
    gesture = Gtk.GestureClick()
    gesture.connect("released", lambda x, n, a, b: on_click(box))
    box.add_controller(gesture)
    
    def on_hover(controller, x, y):
        try: box.set_cursor(Gdk.Cursor.new_from_name("pointer", None))
        except: pass
    
    def on_unhover(controller):
        try: box.set_cursor(None)
        except: pass
    
    motion = Gtk.EventControllerMotion()
    motion.connect("enter", on_hover)
    motion.connect("leave", on_unhover)
    box.add_controller(motion)
    return box

# Допоміжна функція run_cmd (з config.py)
def run_cmd(cmd):
    try:
        import subprocess
        return subprocess.check_output(f"LC_ALL=C {cmd}", shell=True).decode("utf-8").strip()
    except:
        return ""

def NotificationItem(appname, summary, body, icon_data, item_id, on_dismiss, on_pin, on_refresh, is_pinned=False):
    """Створює елемент сповіщення з розширеним функціоналом Android-стиль"""
    
    def get_val(key):
        val = icon_data.get(key)
        if isinstance(val, dict): 
            return val.get("data", "")
        return val if val else ""
    
    # Отримання іконки
    raw_path = get_val("icon_path")
    raw_name = get_val("app_icon")
    image_source = "dialog-information"
    
    if raw_path and "/" in raw_path:
        clean_path = raw_path.replace("file://", "")
        if os.path.exists(clean_path): 
            image_source = clean_path
    elif raw_name: 
        image_source = raw_name
    elif appname: 
        image_source = appname.lower().split()[0]
    
    # Покращена ідентифікація іконок
    image_source_lower = image_source.lower()
    icon_mapping = {
        "telegram": "telegram",
        "code": "code",
        "vscode": "code",
        "firefox": "firefox",
        "chrome": "google-chrome",
        "chromium": "google-chrome",
        "discord": "discord",
        "spotify": "spotify",
        "mail": "mail",
        "thunderbird": "mail",
        "slack": "slack",
        "signal": "signal",
        "teams": "teams",
        "zoom": "zoom"
    }
    
    for key, icon in icon_mapping.items():
        if key in image_source_lower:
            image_source = icon
            break
    
    if image_source_lower in ["notify-send", "dunst", "notification-daemon"]:
        image_source = "dialog-information"
    
    # Отримання часу сповіщення
    timestamp = get_val("timestamp")
    time_label = ""
    if timestamp:
        try:
            import time
            time_diff = int(time.time()) - int(timestamp) // 1000000
            if time_diff < 60:
                time_label = "щойно"
            elif time_diff < 3600:
                time_label = f"{time_diff // 60}хв"
            elif time_diff < 86400:
                time_label = f"{time_diff // 3600}год"
            else:
                time_label = f"{time_diff // 86400}д"
        except:
            time_label = ""
    
    # Кнопки дій
    actions_box = widgets.Box(spacing=6, css_classes=["notif-actions"])
    
    # Кнопка закріплення
    pin_icon = "pin-symbolic" if not is_pinned else "starred-symbolic"
    pin_btn = ClickableBox(
        child=widgets.Icon(image=pin_icon, pixel_size=16),
        css_classes=["notif-action-btn", "pin-btn"] + (["active"] if is_pinned else []),
        on_click=lambda x: [on_pin(item_id), GLib.timeout_add(100, on_refresh)],
        centered=True
    )
    
    # Кнопка видалення
    dismiss_btn = ClickableBox(
        child=widgets.Icon(image="user-trash-symbolic", pixel_size=16),
        css_classes=["notif-action-btn", "dismiss-btn"],
        on_click=lambda x: on_dismiss(box, item_id),
        centered=True
    )
    
    # Кнопка копіювання
    def copy_to_clipboard():
        text = f"{summary}\n{body}"
        try:
            clipboard = Gtk.Clipboard.get_default(Gdk.Display.get_default())
            clipboard.set_text(text, -1)
        except:
            pass
    
    copy_btn = ClickableBox(
        child=widgets.Icon(image="edit-copy-symbolic", pixel_size=16),
        css_classes=["notif-action-btn", "copy-btn"],
        on_click=lambda x: copy_to_clipboard(),
        centered=True
    )
    
    actions_box.child = [pin_btn, copy_btn, dismiss_btn]
    
    # Індикатор закріплення
    pin_indicator = widgets.Box(
        css_classes=["pin-indicator"],
        visible=is_pinned,
        width_request=4
    )
    
    # Контент сповіщення
    content_box = widgets.Box(vertical=True, hexpand=True, child=[
        # Заголовок з іконкою та часом
        widgets.Box(spacing=8, child=[
            widgets.Icon(
                image=image_source, 
                pixel_size=32, 
                css_classes=["notif-icon-img"]
            ),
            widgets.Box(vertical=True, valign="center", hexpand=True, child=[
                widgets.Label(
                    label=appname or "Система", 
                    css_classes=["notif-appname"], 
                    halign="start", 
                    ellipsize="end"
                ),
                widgets.Label(
                    label=summary or "Сповіщення", 
                    css_classes=["notif-summary"], 
                    halign="start", 
                    ellipsize="end", 
                    max_width_chars=35
                )
            ]),
            widgets.Label(
                label=time_label,
                css_classes=["notif-time"],
                halign="end",
                valign="start"
            ) if time_label else widgets.Box()
        ]),
        widgets.Box(height_request=6),
        widgets.Label(
            label=body or "", 
            css_classes=["notif-body"], 
            halign="start", 
            wrap=True, 
            max_width_chars=50,
            visible=bool(body)
        ),
        widgets.Box(height_request=6),
        actions_box
    ])
    
    box = widgets.Box(
        css_classes=["notif-item"] + (["pinned"] if is_pinned else []), 
        child=[pin_indicator, content_box]
    )
    
    # Обробка кліків
    expanded = [False]  # Використовуємо список для мутабельності
    
    def on_click_handler(gesture, n_press, x, y):
        button = gesture.get_current_button()
        if button == 1:  # Лівий клік - показати/сховати дії
            expanded[0] = not expanded[0]
            actions_box.set_visible(expanded[0])
        elif button == 2:  # Середній клік - закрити
            on_dismiss(box, item_id)
        elif button == 3:  # Правий клік - закріпити
            on_pin(item_id)
            GLib.timeout_add(100, on_refresh)
    
    gesture = Gtk.GestureClick()
    gesture.set_button(0)
    gesture.connect("released", on_click_handler)
    box.add_controller(gesture)
    
    # Свайп для видалення (як на Android)
    swipe = Gtk.GestureSwipe()
    def on_swipe(gesture, velocity_x, velocity_y):
        if abs(velocity_x) > abs(velocity_y) and abs(velocity_x) > 500:
            if velocity_x < 0:  # Свайп вліво - видалити
                on_dismiss(box, item_id)
            elif velocity_x > 0:  # Свайп вправо - закріпити
                on_pin(item_id)
                GLib.timeout_add(100, on_refresh)
    swipe.connect("swipe", on_swipe)
    box.add_controller(swipe)
    
    return box

def NotificationWidget():
    """Головний віджет сповіщень (ОПТИМІЗОВАНИЙ)"""
    notif_list = widgets.Box(vertical=True, spacing=8)
    pinned_list = widgets.Box(vertical=True, spacing=8)
    
    BLACKLIST = ["notify-send", "volume", "brightness", "backlight", "microphone", "battery"]
    stats_label = widgets.Label(label="0 сповіщень", css_classes=["notif-stats"], halign="start")
    
    # СЛОВНИК ДЛЯ КЕШУВАННЯ (фікс витоку пам'яті і лагів)
    cached_widgets = {}

    def dismiss_item(widget, notif_id):
        try: notif_list.remove(widget)
        except: pass
        try: pinned_list.remove(widget)
        except: pass
        
        if notif_id:
            HIDDEN_NOTIF_IDS.add(notif_id)
            if notif_id in PINNED_NOTIF_IDS:
                PINNED_NOTIF_IDS.remove(notif_id)
            # Видаляємо з кешу
            if notif_id in cached_widgets:
                del cached_widgets[notif_id]
                
        GLib.timeout_add(50, lambda: [refresh_notifications(), False][1])
    
    def toggle_pin(notif_id):
        if notif_id in PINNED_NOTIF_IDS:
            PINNED_NOTIF_IDS.remove(notif_id)
        else:
            PINNED_NOTIF_IDS.add(notif_id)
    
    def refresh_notifications():
        nonlocal cached_widgets
        
        # Забираємо віджети з екрану, але НЕ знищуємо їх
        child = notif_list.get_first_child()
        while child:
            notif_list.remove(child)
            child = notif_list.get_first_child()
        
        child = pinned_list.get_first_child()
        while child:
            pinned_list.remove(child)
            child = pinned_list.get_first_child()
        
        try:
            raw_json = run_cmd("dunstctl history")
            if not raw_json:
                notif_list.append(widgets.Label(label="📭 Немає історії", css_classes=["b-subtext", "empty-state"]))
                stats_label.set_label("0 сповіщень")
                return False
            
            data = json.loads(raw_json)
            history = data.get("data", [[]])[0]
            count = 0
            pinned_count = 0
            
            new_cache = {}
            
            for item in history:
                if count >= 50: break
                
                item_id = item.get("id", {}).get("data")
                if not item_id or item_id in HIDDEN_NOTIF_IDS: continue
                
                appname = item.get("appname", {}).get("data", "Система")
                summary = item.get("summary", {}).get("data", "")
                body = item.get("body", {}).get("data", "")
                
                check_str = (appname + " " + summary + " " + body).lower()
                if any(bad_word in check_str for bad_word in BLACKLIST): continue
                
                is_pinned = item_id in PINNED_NOTIF_IDS
                
                # РОЗУМНЕ ОНОВЛЕННЯ: беремо з кешу або робимо новий
                if item_id in cached_widgets:
                    notif_item = cached_widgets[item_id]
                else:
                    notif_item = NotificationItem(
                        appname, summary, body, item, item_id,
                        dismiss_item, toggle_pin, refresh_notifications, is_pinned
                    )
                
                new_cache[item_id] = notif_item
                
                if is_pinned:
                    pinned_list.append(notif_item)
                    pinned_count += 1
                else:
                    notif_list.append(notif_item)
                count += 1
            
            # Оновлюємо кеш. Старі віджети, яких нема в новій історії, знищить збирач сміття
            cached_widgets = new_cache
            
            if count == 0:
                notif_list.append(widgets.Label(label="🎉 Пусто", css_classes=["b-subtext", "empty-state"]))
            
            stats_label.set_label(f"{count} шт." + (f" • {pinned_count} 📌" if pinned_count else ""))
            pinned_section.set_visible(pinned_count > 0)
            
        except Exception as e:
            pass
        return False
    
        utils.Poll(3000, lambda x: refresh_notifications())
    
    def clear_all():
        os.system("dunstctl close-all")
        os.system("dunstctl history-clear") # Вбиваємо історію під корінь
        HIDDEN_NOTIF_IDS.clear()
        PINNED_NOTIF_IDS.clear()
        cached_widgets.clear()
        GLib.timeout_add(200, lambda: [refresh_notifications(), False][1])
    
    def clear_read():
        try:
            raw_json = run_cmd("dunstctl history")
            if raw_json:
                data = json.loads(raw_json)
                for item in data.get("data", [[]])[0]:
                    item_id = item.get("id", {}).get("data")
                    if item_id and item_id not in PINNED_NOTIF_IDS:
                        HIDDEN_NOTIF_IDS.add(item_id)
        except: pass
        GLib.timeout_add(100, lambda: [refresh_notifications(), False][1])
    
    controls_box = widgets.Box(spacing=8, css_classes=["notif-controls"], child=[
        ClickableBox(child=widgets.Box(spacing=6, child=[widgets.Icon(image="edit-clear-symbolic", pixel_size=16), widgets.Label(label="Прочитані")]), css_classes=["notif-control-btn"], on_click=lambda x: clear_read()),
        ClickableBox(child=widgets.Box(spacing=6, child=[widgets.Icon(image="notifications-disabled-symbolic", pixel_size=16), widgets.Label(label="Тиша")]), css_classes=["notif-control-btn"], on_click=lambda x: os.system("dunstctl set-paused toggle")),
        ClickableBox(child=widgets.Box(spacing=6, child=[widgets.Icon(image="user-trash-full-symbolic", pixel_size=16), widgets.Label(label="Все")]), css_classes=["notif-control-btn", "clear-all"], on_click=lambda x: clear_all())
    ])
    
    pinned_section = widgets.Box(vertical=True, css_classes=["pinned-section"], visible=False, child=[
        widgets.Label(label="📌 Закріплені", css_classes=["section-header"], halign="start"),
        widgets.Box(height_request=6), pinned_list, widgets.Box(height_request=12)
    ])
    
    refresh_notifications()
    
    return widgets.Box(vertical=True, css_classes=["dunst-block"], vexpand=True, child=[
        widgets.Box(child=[widgets.Label(label="Сповіщення", css_classes=["dunst-header"]), widgets.Box(hexpand=True), stats_label]),
        widgets.Box(height_request=10), controls_box, widgets.Box(height_request=10),
        widgets.Scroll(vexpand=True, child=widgets.Box(vertical=True, child=[pinned_section, notif_list]))
    ])