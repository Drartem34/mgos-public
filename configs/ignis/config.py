import sys
import os
import subprocess
import gi

sys.path.append(os.path.expanduser("~/.config/ignis"))

import wallpapers
import switcher
import mixer
import notifications
# import about  # замінено на SYSui about_osd (C++)
# import bar
import settings

gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, Gdk, GLib
from ignis import widgets, utils
from ignis.app import IgnisApp
from ignis.services.audio import AudioService
from ignis.services.mpris import MprisService
from ignis.services.backlight import BacklightService
from gi.repository import Gio

app = IgnisApp.get_default()
audio = AudioService.get_default()
mpris = MprisService.get_default()
backlight = BacklightService.get_default()

try:
    app.apply_css(os.path.expanduser("~/.config/ignis/style.css"))
except Exception:
    pass

def run_cmd(cmd):
    try:
        return subprocess.check_output(f"LC_ALL=C {cmd}", shell=True).decode("utf-8").strip()
    except:
        return ""

def run_async(cmd):
    subprocess.Popen(cmd, shell=True)

def get_wifi_status():
    ssid = run_cmd("nmcli -t -f TYPE,NAME connection show --active | grep '802-11-wireless' | cut -d: -f2 | head -n 1")
    state = run_cmd("nmcli -t -f STATE general")
    return ssid, "connected" in state

def get_bt_status():
    out = run_cmd("bluetoothctl show | grep 'Powered: yes'")
    return bool(out)

def ClickableBox(child, on_click, css_classes=[], spacing=0, centered=False, **kwargs):
    # ФІКС: Ignis приймає тільки текст "center", ніяких Gtk.Align.CENTER!
    if centered and child:
        child.set_halign("center")
        child.set_valign("center")
        child.set_hexpand(True)
        child.set_vexpand(True)
        
    classes = css_classes + ["unset", "clickable-box"]
    box = widgets.Box(child=[child] if child else [], css_classes=classes, spacing=spacing, **kwargs)
    gesture = Gtk.GestureClick()
    gesture.connect("released", lambda gesture, n, x, y: on_click(gesture.get_widget()))
    box.add_controller(gesture)
    def on_hover(controller, x, y):
        w = controller.get_widget()
        if w:
            try: w.set_cursor(Gdk.Cursor.new_from_name("pointer", None))
            except: pass
    def on_unhover(controller):
        w = controller.get_widget()
        if w:
            try: w.set_cursor(None)
            except: pass
    motion = Gtk.EventControllerMotion()
    motion.connect("enter", on_hover)
    motion.connect("leave", on_unhover)
    box.add_controller(motion)
    return box

# --- WI-FI ---
def WifiWidget():
    list_revealer = widgets.Revealer(transition_type="slide_down")
    list_box = widgets.Box(vertical=True, spacing=5, css_classes=["list-container"])
    icon_widget = widgets.Icon(image="network-wireless-symbolic", pixel_size=24, css_classes=["b-icon"])
    status_label = widgets.Label(label="Перевірка...", halign="start", css_classes=["card-status"], ellipsize="end")
    cached_wifi = {} 

    def get_ethernet_status():
        try:
            eth_active = run_cmd("nmcli -t -f TYPE,NAME connection show --active | grep 'ethernet' | cut -d: -f2 | head -n 1")
            return eth_active.strip() if eth_active else None
        except: return None

    def create_wifi_row(ssid, initial_active, initial_strength, initial_saved):
        entry = widgets.Entry(placeholder_text="Введіть пароль...", css_classes=["wifi-password-entry"], hexpand=True)
        entry.set_visibility(False)
        btn_connect = ClickableBox(child=widgets.Label(label="Підключити", css_classes=["btn-text-main"]), css_classes=["wifi-connect-btn"], on_click=lambda x: connect_to_network(ssid, entry.get_text()), centered=True)
        forget_label = widgets.Label(label="Забути", css_classes=["text-danger"])
        btn_forget = ClickableBox(child=forget_label, css_classes=["wifi-connect-btn"], on_click=lambda x: forget_network(ssid), centered=True) if is_saved else None
        password_area = widgets.Box(spacing=5, child=[entry, btn_connect])
        pass_revealer = widgets.Revealer(transition_type="slide_down", child=password_area, reveal_child=False)
        forget_area = widgets.Box(spacing=5, child=[btn_forget])
        forget_revealer = widgets.Revealer(transition_type="slide_down", child=forget_area, reveal_child=False)
        signal_icon_widget = widgets.Icon(pixel_size=20, css_classes=["list-icon"])
        name_label = widgets.Label(css_classes=["list-label"], ellipsize="end", halign="start", hexpand=True)
        status_icon = widgets.Icon(pixel_size=16, css_classes=["list-status"])
        row_content = widgets.Box(spacing=10, child=[signal_icon_widget, name_label, status_icon])
        state = {"is_active": initial_active, "is_saved": initial_saved}

        def connect_to_network(s, p):
            if p: run_async(f"nmcli dev wifi connect '{s}' password '{p}'"); pass_revealer.set_reveal_child(False); utils.Timeout(1500, refresh_wifi_list)
        def forget_network(s):
            run_async(f"nmcli connection delete id '{s}'"); forget_revealer.set_reveal_child(False); utils.Timeout(1000, refresh_wifi_list)
        def on_click_network(x):
            if state["is_active"]: run_async(f"nmcli connection down id '{ssid}'"); utils.Timeout(1000, refresh_wifi_list)
            elif state["is_saved"]: run_async(f"nmcli connection up id '{ssid}'"); utils.Timeout(1500, refresh_wifi_list)
            else: pass_revealer.set_reveal_child(not pass_revealer.get_reveal_child())
        def on_right_click(gesture, n_press, x, y):
            if gesture.get_current_button() == 3 and state["is_saved"]: forget_revealer.set_reveal_child(not forget_revealer.get_reveal_child())

        clickable_row = ClickableBox(child=row_content, on_click=on_click_network, hexpand=True)
        right_click = Gtk.GestureClick(); right_click.set_button(3); right_click.connect("released", on_right_click); clickable_row.add_controller(right_click)
        item_box = widgets.Box(vertical=True, css_classes=["list-item"], child=[clickable_row, pass_revealer, forget_revealer])

        def update_ui(is_active, strength, is_saved):
            state["is_active"] = is_active; state["is_saved"] = is_saved
            if strength >= 75: icn = "network-wireless-signal-excellent-symbolic"
            elif strength >= 50: icn = "network-wireless-signal-good-symbolic"
            elif strength >= 25: icn = "network-wireless-signal-ok-symbolic"
            else: icn = "network-wireless-signal-weak-symbolic"
            signal_icon_widget.set_image(icn)
            name_label.set_label(f"{ssid} ({strength}%)" + (" 🔒" if is_saved else ""))
            status_icon.set_image("object-select-symbolic" if is_active else "")
            if is_active: item_box.add_css_class("list-item-active")
            else: item_box.remove_css_class("list-item-active")
            if not is_saved: forget_revealer.set_reveal_child(False)

        update_ui(initial_active, initial_strength, initial_saved)
        return {"box": item_box, "update": update_ui}

    def refresh_wifi_list():
        def scan_process():
            run_cmd("nmcli dev wifi rescan 2>/dev/null || true"); utils.Timeout(500, populate_list)
        def populate_list():
            try:
                raw = run_cmd("nmcli -t -f SSID,IN-USE,SIGNAL,SECURITY dev wifi list")
                saved_networks = set()
                try: saved = run_cmd("nmcli -t -f NAME connection show"); saved_networks = set(saved.split('\n')) if saved else set()
                except: pass
                if not raw: return
                lines = raw.split('\n')
                unique_ssids = set()
                for line in lines:
                    if not line: continue
                    parts = line.split(':')
                    if len(parts) < 3: continue
                    ssid = parts[0].strip(); is_active = (parts[1] == "*")
                    try: strength = int(parts[2])
                    except: strength = 0
                    if not ssid or ssid in unique_ssids: continue
                    unique_ssids.add(ssid)
                    is_saved = ssid in saved_networks
                    if ssid not in cached_wifi:
                        row_data = create_wifi_row(ssid, is_active, strength, is_saved); cached_wifi[ssid] = row_data; list_box.append(row_data["box"])
                    else: cached_wifi[ssid]["update"](is_active, strength, is_saved)
                for s in list(cached_wifi.keys()):
                    if s not in unique_ssids: list_box.remove(cached_wifi[s]["box"]); del cached_wifi[s]
            except: pass
        utils.Timeout(100, scan_process)

    def toggle_list(widget):
        is_open = list_revealer.get_reveal_child()
        if not is_open: refresh_wifi_list()
        list_revealer.set_reveal_child(not is_open)

    def poll_status(widget):
        ssid = run_cmd("nmcli -t -f TYPE,NAME connection show --active | grep '802-11-wireless' | cut -d: -f2 | head -n 1")
        eth_name = get_ethernet_status()
        if ssid:
            status_label.set_label(ssid.strip()); icon_widget.set_image("network-wireless-connected-symbolic"); icon_widget.remove_css_class("dim-icon"); icon_widget.add_css_class("accent-icon")
        elif eth_name:
            status_label.set_label(f"Ethernet: {eth_name.strip()}"); icon_widget.set_image("network-wired-symbolic"); icon_widget.remove_css_class("dim-icon"); icon_widget.add_css_class("accent-icon")
        else:
            radio = run_cmd("nmcli radio wifi")
            if radio == "disabled": status_label.set_label("Вимкнено"); icon_widget.set_image("network-wireless-disabled-symbolic")
            else: status_label.set_label("Не підключено"); icon_widget.set_image("network-wireless-offline-symbolic")
            icon_widget.remove_css_class("accent-icon"); icon_widget.add_css_class("dim-icon")
    
    from ignis.services.network import NetworkService
    net = NetworkService.get_default()
    
    def on_net_change(*args):
        poll_status(None)
        
    net.wifi.connect("notify::is-connected", on_net_change)
    net.wifi.connect("notify::enabled", on_net_change)
    net.ethernet.connect("notify::is-connected", on_net_change)
    poll_status(None)

    power_btn = ClickableBox(child=widgets.Icon(image="system-shutdown-symbolic", pixel_size=20), css_classes=["power-btn"], on_click=lambda x: toggle_wifi_power(), centered=True)
    def toggle_wifi_power():
        current = run_cmd("nmcli radio wifi")
        if current == "enabled": run_async("nmcli radio wifi off")
        else: run_async("nmcli radio wifi on")
        utils.Timeout(500, lambda: poll_status(None))
    
    header_left = ClickableBox(child=widgets.Box(spacing=12, child=[icon_widget, widgets.Box(vertical=True, valign="center", child=[widgets.Label(label="Wi-Fi / Мережа", halign="start", css_classes=["card-title"]), status_label])]), on_click=toggle_list, css_classes=["header-left"], hexpand=True)
    header = widgets.Box(spacing=0, child=[header_left, widgets.Box(hexpand=True), power_btn])
    scroll = widgets.Scroll(height_request=300, child=list_box); list_revealer.set_child(scroll)
    return widgets.Box(vertical=True, css_classes=["block", "dark", "wide"], child=[widgets.Box(css_classes=["header-padding"], child=[header]), list_revealer])

# --- BLUETOOTH ---
bt_scan_process = None
def BluetoothWidget():
    list_revealer = widgets.Revealer(transition_type="slide_down")
    list_box = widgets.Box(vertical=True, spacing=5, css_classes=["list-container"])
    icon_widget = widgets.Icon(image="bluetooth-active-symbolic", pixel_size=24, css_classes=["b-icon"])
    status_label = widgets.Label(label="Checking...", halign="start", css_classes=["card-status"])
    scan_btn_label = widgets.Label(label="Scan")
    cached_bt = {} 

    def toggle_scan(btn_box):
        global bt_scan_process
        if bt_scan_process: run_async("bluetoothctl scan off"); bt_scan_process = None; scan_btn_label.set_label("Scan"); btn_box.remove_css_class("active-scan-btn")
        else: run_async("bluetoothctl scan on"); bt_scan_process = True; scan_btn_label.set_label("Stop"); btn_box.add_css_class("active-scan-btn"); refresh_bt_list()

    def create_bt_row(mac, initial_name, initial_connected, initial_paired):
        pair_btn = ClickableBox(child=widgets.Label(label="Pair & Connect", css_classes=["btn-text-main"]), css_classes=["wifi-connect-btn"], on_click=lambda x: run_async(f"bluetoothctl trust {mac} && bluetoothctl pair {mac} && bluetoothctl connect {mac}"), centered=True)
        action_revealer = widgets.Revealer(transition_type="slide_down", child=widgets.Box(child=[pair_btn]), reveal_child=False)
        name_label = widgets.Label(css_classes=["list-label"], ellipsize="end", halign="start", hexpand=True)
        status_icon = widgets.Icon(pixel_size=16, css_classes=["list-status"])
        row_content = widgets.Box(spacing=10, child=[widgets.Icon(image="bluetooth-symbolic", pixel_size=20, css_classes=["list-icon"]), name_label, status_icon])
        state = {"is_connected": initial_connected, "is_paired": initial_paired}

        def on_click_device(x):
            if state["is_connected"]: run_async(f"bluetoothctl disconnect {mac}")
            elif state["is_paired"]: run_async(f"bluetoothctl connect {mac}")
            else: action_revealer.set_reveal_child(not action_revealer.get_reveal_child())

        item_box = widgets.Box(vertical=True, css_classes=["list-item"], child=[ClickableBox(child=row_content, on_click=on_click_device, hexpand=True), action_revealer])
        def update_ui(name, is_connected, is_paired):
            state["is_connected"] = is_connected; state["is_paired"] = is_paired; name_label.set_label(name if name else mac)
            status_icon.set_image("object-select-symbolic" if is_connected else ("network-wired-symbolic" if is_paired else ""))
            if is_connected: item_box.add_css_class("list-item-active")
            else: item_box.remove_css_class("list-item-active")
        update_ui(initial_name, initial_connected, initial_paired)
        return {"box": item_box, "update": update_ui}

    def refresh_bt_list():
        if not list_revealer.get_reveal_child(): return
        try:
            raw = run_cmd("bluetoothctl devices"); lines = raw.split('\n'); current_macs = []
            for line in lines:
                if not line: continue
                parts = line.split(' ', 2)
                if len(parts) < 3: continue
                mac, name = parts[1], parts[2]; current_macs.append(mac)
                info = run_cmd(f"bluetoothctl info {mac}"); is_connected = "Connected: yes" in info; is_paired = "Paired: yes" in info
                if mac not in cached_bt: row_data = create_bt_row(mac, name, is_connected, is_paired); cached_bt[mac] = row_data; list_box.append(row_data["box"])
                else: cached_bt[mac]["update"](name, is_connected, is_paired)
            for m in list(cached_bt.keys()):
                if m not in current_macs: list_box.remove(cached_bt[m]["box"]); del cached_bt[m]
        except: pass
        if bt_scan_process: utils.Timeout(2000, refresh_bt_list)

    def toggle_list(widget):
        is_open = list_revealer.get_reveal_child()
        if not is_open: refresh_bt_list()
        list_revealer.set_reveal_child(not is_open)

    def poll_status(widget):
        is_on = get_bt_status(); status_label.set_label("On" if is_on else "Off"); icon_widget.set_image("bluetooth-active-symbolic" if is_on else "bluetooth-disabled-symbolic")
        if is_on: icon_widget.add_css_class("accent-icon"); icon_widget.remove_css_class("dim-icon")
        else: icon_widget.add_css_class("dim-icon"); icon_widget.remove_css_class("accent-icon")

    def on_bt_changed(conn, sender, path, interface, signal, params, *args):
        GLib.idle_add(lambda: poll_status(None))

    Gio.bus_get(Gio.BusType.SYSTEM, None, lambda obj, res: Gio.bus_get_finish(res).signal_subscribe(
        "org.bluez",
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        None,
        None,
        Gio.DBusSignalFlags.NONE,
        on_bt_changed,
        None
    ))
    poll_status(None)
    scan_btn = ClickableBox(child=scan_btn_label, css_classes=["wifi-connect-btn"], on_click=toggle_scan, centered=True)
    power_btn = ClickableBox(child=widgets.Icon(image="system-shutdown-symbolic", pixel_size=20), css_classes=["power-btn"], on_click=lambda x: run_async("bluetoothctl power off" if get_bt_status() else "bluetoothctl power on"), centered=True)
    header_left = ClickableBox(child=widgets.Box(spacing=12, child=[icon_widget, widgets.Box(vertical=True, valign="center", child=[widgets.Label(label="Bluetooth", halign="start", css_classes=["card-title"]), status_label])]), on_click=toggle_list, css_classes=["header-left"], hexpand=True)
    header = widgets.Box(spacing=5, child=[header_left, widgets.Box(hexpand=True), scan_btn, power_btn])
    scroll = widgets.Scroll(height_request=250, child=list_box); list_revealer.set_child(scroll)
    return widgets.Box(vertical=True, css_classes=["block", "dark", "wide"], child=[widgets.Box(css_classes=["header-padding"], child=[header]), list_revealer])

# --- УСІ СПОВІЩЕННЯ ---
def NotificationsWidget():
    list_revealer = widgets.Revealer(transition_type="slide_down")
    list_box = widgets.Box(vertical=True, spacing=5, css_classes=["list-container"])

    BLACKLIST_TAGS = ["volume", "brightness", "music", "backlight"]
    
    status_label = widgets.Label(label="Немає нових", halign="start", css_classes=["card-status"])
    icon_widget = widgets.Icon(image="preferences-system-notifications-symbolic", pixel_size=32, css_classes=["dim-icon"])

    def get_all_notifs(out):
        try:
            if not out: return []
            import json
            data = json.loads(out)
            notifs = []
            
            history_list = data.get("data", [[]])[0]
            for item in history_list:
                stack_tag = item.get("stack_tag", {}).get("data", "")
                appname = item.get("appname", {}).get("data", "System")
                
                if stack_tag in BLACKLIST_TAGS or appname.lower() in BLACKLIST_TAGS:
                    continue

                id_val = item.get("id", {}).get("data", 0)
                summary = item.get("summary", {}).get("data", "")
                body = item.get("body", {}).get("data", "")
                body = body.replace("<b>", "").replace("</b>", "").replace("<i>", "").replace("</i>", "")
                notifs.append({"id": id_val, "appname": appname, "summary": summary, "body": body})
            return notifs
        except Exception:
            return []

    def clear_all_notifs(x=None):
        nonlocal last_history_raw
        run_async("dunstctl history-clear")
        last_history_raw = None
        list_revealer.set_reveal_child(False)
        utils.Timeout(500, update_notif_list)

    def create_message_row(notif):
        icon_path = notif.get("icon_path", "dialog-information")
        msg_icon = widgets.Icon(image=icon_path, pixel_size=32, css_classes=["notif-list-icon"])

        header_text = f"[{notif['appname']}] {notif['summary']}"
        
        row_content = widgets.Box(spacing=10, child=[
            msg_icon,
            widgets.Box(vertical=True, hexpand=True, child=[
                widgets.Label(label=header_text, halign="start", css_classes=["list-label"], ellipsize="end"),
                widgets.Label(label=str(notif['body']), halign="start", css_classes=["b-subtext"], ellipsize="end", max_width_chars=35)
            ])
        ])
        
        def on_left_click(x):
            run_async(f"dunstctl history-rm {notif['id']}")
            utils.Timeout(300, update_notif_list)
            
        clickable_row = ClickableBox(child=row_content, on_click=on_left_click, css_classes=["list-item"], hexpand=True)
        
        mid_click = Gtk.GestureClick()
        mid_click.set_button(2)
        mid_click.connect("released", lambda gesture, n, x, y: (run_async(f"dunstctl history-rm {notif['id']}"), utils.Timeout(300, update_notif_list)))
        clickable_row.add_controller(mid_click)
        
        return clickable_row

    last_history_raw = None

    def update_notif_list(*args):
        nonlocal last_history_raw
        
        raw_out = run_cmd("dunstctl history")
        if raw_out == last_history_raw:
            return
        last_history_raw = raw_out
        
        while list_box.get_first_child():
            list_box.remove(list_box.get_first_child())
            
        all_notifs = get_all_notifs(raw_out)
        
        if not all_notifs:
            status_label.set_label("Немає нових")
            icon_widget.remove_css_class("accent-icon")
            icon_widget.add_css_class("dim-icon")
            if list_revealer.get_reveal_child():
                list_revealer.set_reveal_child(False)
        else:
            status_label.set_label(f"{len(all_notifs)} повідомлень")
            icon_widget.remove_css_class("dim-icon")
            icon_widget.add_css_class("accent-icon")
            
            clear_all_btn = ClickableBox(
                child=widgets.Label(label="🗑 Очистити все", css_classes=["text-danger"], style="font-size: 11px; font-weight: bold;"),
                on_click=clear_all_notifs, css_classes=["list-item"], style="margin-bottom: 5px; border-bottom: 1px solid var(--border-color);"
            )
            list_box.append(clear_all_btn)

            for n in all_notifs:
                list_box.append(create_message_row(n))

    utils.Poll(2000, lambda x: update_notif_list())
    GLib.idle_add(update_notif_list)

    def on_header_click(x):
        if get_all_notifs(run_cmd("dunstctl history")):
            list_revealer.set_reveal_child(not list_revealer.get_reveal_child())

    header = ClickableBox(
        on_click=on_header_click,
        css_classes=["header-left"], hexpand=True,
        child=widgets.Box(spacing=15, child=[
            icon_widget,
            widgets.Box(vertical=True, valign="center", hexpand=True, child=[
                widgets.Label(label="Сповіщення", halign="start", css_classes=["card-title"]),
                status_label
            ]),
            widgets.Icon(image="go-down-symbolic", pixel_size=16, css_classes=["dim-icon"])
        ])
    )
    
    list_revealer.set_child(widgets.Scroll(height_request=250, child=list_box))
    return widgets.Box(vertical=True, css_classes=["main-bg"], child=[header, list_revealer])

# --- МЕДІА ---
def MediaWidget():
    players_box = widgets.Box(vertical=True, spacing=10)
    
    def get_art_css(url):
        if not url: return ""
        if not url.startswith("http") and not url.startswith("file"): url = "file://" + url
        return f"background-image: url('{url}'); background-size: cover; background-position: center;"
        
    def on_player_added(service, player):
        player_widget = widgets.Box(
            vertical=True, css_classes=["music-box"], 
            child=[
                widgets.Box(spacing=15, child=[
                    widgets.Box(css_classes=["music-art-icon"], style=player.bind("art_url", get_art_css),
                        child=[widgets.Icon(image="folder-music-symbolic", visible=player.bind("art_url", lambda u: not u), pixel_size=32)]),
                    widgets.Box(vertical=True, valign="center", hexpand=True, child=[
                        widgets.Label(label=player.bind("title"), halign="start", ellipsize="end", css_classes=["music-title"]),
                        widgets.Label(label=player.bind("artist"), halign="start", ellipsize="end", css_classes=["music-artist"])
                    ])
                ]),
                widgets.Scale(hexpand=True, css_classes=["b-slider", "media-scale"], min=0, max=player.bind("length"),
                    value=player.bind("position"), on_change=lambda x: player.set_position(x.value),
                    visible=player.bind("length", lambda l: l > 0)),
                widgets.Box(halign="center", spacing=20, child=[
                    ClickableBox(child=widgets.Icon(image="media-skip-backward-symbolic", pixel_size=24), on_click=lambda x: player.previous(), css_classes=["control-btn"], centered=True),
                    ClickableBox(child=widgets.Icon(image=player.bind("playback_status", lambda s: "media-playback-pause-symbolic" if s == "Playing" else "media-playback-start-symbolic"), pixel_size=32), on_click=lambda x: player.play_pause(), css_classes=["play-btn"], centered=True),
                    ClickableBox(child=widgets.Icon(image="media-skip-forward-symbolic", pixel_size=24), on_click=lambda x: player.next(), css_classes=["control-btn"], centered=True)
                ])
            ]
        )
        
        mid_click = Gtk.GestureClick()
        mid_click.set_button(2) 
        
        def close_player_ui(gesture, n_press, x, y):
            try: players_box.remove(player_widget)
            except Exception: pass
            try: player.quit()
            except Exception: pass
                
        mid_click.connect("released", close_player_ui)
        player_widget.add_controller(mid_click)

        def on_player_closed(x):
            try: players_box.remove(player_widget)
            except Exception: pass
                
        player.connect("closed", on_player_closed)
        players_box.append(player_widget)

    mpris.connect("player-added", on_player_added)
    for p in mpris.players: on_player_added(mpris, p)

    return widgets.Box(
        css_classes=["main-bg"],
        vertical=True, 
        child=[
            players_box,
            widgets.Box(visible=mpris.bind("players", lambda p: len(p) == 0), css_classes=["music-box"],
                child=[widgets.Label(label="Немає активних плеєрів", css_classes=["music-title"], halign="center")])
        ]
    )

def VolumeSlider(stream_type):
    stream = getattr(audio, stream_type)
    icon_box = ClickableBox(child=widgets.Icon(image=stream.bind("icon_name"), pixel_size=20), css_classes=["b-icon-btn"], on_click=lambda x: stream.set_is_muted(not stream.is_muted), centered=True)
    scale = widgets.Scale(min=0, max=100, step=1, hexpand=True, css_classes=["b-slider"], value=stream.bind("volume"), on_change=lambda x: stream.set_volume(x.value))
    return widgets.Box(css_classes=["block", "dark", "wide"], spacing=10, child=[icon_box, scale])

def BrightnessSlider():
    return widgets.Box(visible=backlight.bind("available"), css_classes=["block", "dark", "wide"], spacing=10, child=[
        widgets.Icon(image="display-brightness-symbolic", pixel_size=20, css_classes=["b-icon"]),
        widgets.Scale(min=0, max=backlight.max_brightness, hexpand=True, css_classes=["b-slider"], value=backlight.bind("brightness"), on_change=lambda x: backlight.set_brightness(x.value))
    ])

# --- ОКРЕМИЙ БЛОК НАКОПИЧУВАЧІВ (УСІ ДИСКИ) ---
def DisksWidget():
    disk_box = widgets.Box(vertical=True, css_classes=["block", "dark", "wide"], spacing=10)
    disk_box.append(widgets.Label(label="Накопичувачі", css_classes=["card-title"], halign="start"))
    
    rows_container = widgets.Box(vertical=True, spacing=10)
    disk_box.append(rows_container)
    cached_disks = {}
    
    def update_disks(widget=None):
        current_mounts = []
        seen_fs = set() 
        
        try:
            with open("/proc/mounts", "r") as f:
                mounts = f.readlines()
        except:
            return
            
        for line in mounts:
            parts = line.split()
            if len(parts) < 2: continue
            fs, mount = parts[0], parts[1]
            
            if not fs.startswith("/dev/"): continue
            if "loop" in fs or "/snap" in mount or "/boot" in mount or fs in seen_fs: continue
            
            try:
                st = os.statvfs(mount)
                total = st.f_blocks * st.f_frsize
                free = st.f_bavail * st.f_frsize
                used = total - free
                if total == 0: continue
                percent = (used / total) * 100
            except:
                continue
                
            seen_fs.add(fs)
            current_mounts.append(mount)
            
            if mount not in cached_disks:
                icon = "drive-harddisk-system-symbolic" if mount == "/" else "drive-harddisk-symbolic"
                mnt_label = widgets.Label(label=mount, css_classes=["sys-label"], halign="start", ellipsize="end", max_width_chars=15)
                val_label = widgets.Label(label="0/0 GB", css_classes=["sys-value"], halign="end", hexpand=True)
                bar = widgets.Scale(min=0, max=100, css_classes=["sys-bar"], hexpand=True); bar.set_sensitive(False)
                
                row = widgets.Box(vertical=True, spacing=2, child=[
                    widgets.Box(spacing=10, child=[widgets.Icon(image=icon, pixel_size=16, css_classes=["sys-icon"]), mnt_label, val_label]), bar
                ])
                cached_disks[mount] = {"row": row, "val": val_label, "bar": bar}
                rows_container.append(row)
            
            item = cached_disks[mount]
            item["val"].set_label(f"{round(used/(1024**3), 1)}/{round(total/(1024**3), 1)} GB")
            item["bar"].set_value(percent)
            
            item["bar"].remove_css_class("disk-ok")
            item["bar"].remove_css_class("disk-warn")
            item["bar"].remove_css_class("disk-crit")
            item["bar"].add_css_class("disk-crit" if percent >= 85 else "disk-warn" if percent >= 50 else "disk-ok")
                
        for m in list(cached_disks.keys()):
            if m not in current_mounts:
                rows_container.remove(cached_disks[m]["row"]); del cached_disks[m]
                
    utils.Poll(5000, lambda x: update_disks())
    update_disks()
    return disk_box

# --- SYSTEM STATS (БЕЗ ДИСКІВ) ---
def SystemStatsWidget():
    cpu_label = widgets.Label(label="CPU", css_classes=["sys-label"], halign="start")
    cpu_name = widgets.Label(label="Завантаження...", css_classes=["b-subtext"], halign="start", ellipsize="end", hexpand=True)
    cpu_value = widgets.Label(label="0% | 0°C", css_classes=["sys-value"], halign="end")
    cpu_bar = widgets.Scale(min=0, max=100, css_classes=["sys-bar"], hexpand=True); cpu_bar.set_sensitive(False)
    
    gpu_label = widgets.Label(label="GPU", css_classes=["sys-label"], halign="start")
    gpu_name = widgets.Label(label="Завантаження...", css_classes=["b-subtext"], halign="start", ellipsize="end", hexpand=True)
    gpu_value = widgets.Label(label="0% | 0°C", css_classes=["sys-value"], halign="end")
    gpu_bar = widgets.Scale(min=0, max=100, css_classes=["sys-bar"], hexpand=True); gpu_bar.set_sensitive(False)
    
    ram_label = widgets.Label(label="RAM", css_classes=["sys-label"], halign="start")
    ram_name = widgets.Label(label="System Memory", css_classes=["b-subtext"], halign="start", ellipsize="end", hexpand=True)
    ram_value = widgets.Label(label="0/0 GB", css_classes=["sys-value"], halign="end")
    ram_bar = widgets.Scale(min=0, max=100, css_classes=["sys-bar"], hexpand=True); ram_bar.set_sensitive(False)
    
    def fetch_hardware_names():
        try:
            c_name = run_cmd("grep -m 1 'model name' /proc/cpuinfo | cut -d: -f2").strip()
            c_name = c_name.replace("AMD ", "").replace("Processor", "").replace("-Core", "C").strip()
            cpu_name.set_label(c_name if c_name else "Ryzen CPU")
        except: pass
        
        try:
            g_name = run_cmd("nvidia-smi --query-gpu=name --format=csv,noheader").strip()
            g_name = g_name.replace("NVIDIA GeForce ", "")
            gpu_name.set_label(g_name if g_name else "NVIDIA GPU")
        except: pass
    
    fetch_hardware_names()

    last_idle = 0; last_total = 0
    def update_stats(widget):
        nonlocal last_idle, last_total
        try:
            with open('/proc/stat', 'r') as f: line = f.readline().split()
            times = [int(x) for x in line[1:8]]
            idle = times[3] + times[4]; total = sum(times)
            idle_delta = idle - last_idle; total_delta = total - last_total
            last_idle, last_total = idle, total
            cpu_usage = 0
            if total_delta > 0: cpu_usage = 100.0 * (1.0 - idle_delta / total_delta)
            
            from ignis.services.fetch import FetchService
            try:
                cpu_temp = int(FetchService.get_default().cpu_temp)
            except: cpu_temp = 0
                
            cpu_value.set_label(f"{int(cpu_usage)}% | {cpu_temp}°C")
            cpu_bar.set_value(cpu_usage)
            
            try:
                gpu_info = run_cmd("nvidia-smi --query-gpu=utilization.gpu,temperature.gpu --format=csv,noheader,nounits")
                if gpu_info:
                    g_util, g_temp = gpu_info.split(',')
                    gpu_value.set_label(f"{int(g_util)}% | {int(g_temp)}°C")
                    gpu_bar.set_value(int(g_util))
            except: gpu_value.set_label("N/A")
            
            with open('/proc/meminfo', 'r') as f: lines = f.read().splitlines()
            total_mem = int(lines[0].split()[1]); avail_mem = int(lines[2].split()[1])
            used_mem = total_mem - avail_mem; percent = (used_mem / total_mem) * 100
            ram_value.set_label(f"{round(used_mem/1048576, 1)}/{round(total_mem/1048576, 1)} GB")
            ram_bar.set_value(percent)
            
        except Exception as e: pass

    utils.Poll(2000, lambda x: update_stats(None))
    
    def create_row(icon, label_main, label_name, label_value, bar):
        text_row = widgets.Box(spacing=10, child=[
            widgets.Icon(image=icon, pixel_size=16, css_classes=["sys-icon"]), 
            label_main, label_name, label_value
        ])
        return widgets.Box(vertical=True, spacing=2, child=[text_row, bar])
    
    return widgets.Box(vertical=True, css_classes=["block", "dark", "wide"], spacing=10, child=[
        create_row("cpu-symbolic", cpu_label, cpu_name, cpu_value, cpu_bar), 
        create_row("video-display-symbolic", gpu_label, gpu_name, gpu_value, gpu_bar), 
        create_row("memory-symbolic", ram_label, ram_name, ram_value, ram_bar)
    ])

# --- ХЕДЕР ---
def HeaderWidget():
    def toggle_global_theme(x):
        cc_win = app.get_window("control_center")
        is_light = cc_win.has_css_class("light-theme")
        
        for win in app.windows:
            if is_light: win.remove_css_class("light-theme")
            else: win.add_css_class("light-theme")
                
        gnome_settings = Gio.Settings.new("org.gnome.desktop.interface")
        if is_light:
            run_async("cp ~/.config/waybar/style-dark.css ~/.config/waybar/style.css && killall -SIGUSR2 waybar")
            gnome_settings.set_string("color-scheme", "prefer-dark")
        else:
            run_async("cp ~/.config/waybar/style-light.css ~/.config/waybar/style.css && killall -SIGUSR2 waybar")
            gnome_settings.set_string("color-scheme", "default")

    power_save_btn = ClickableBox(
        child=widgets.Icon(image="battery-symbolic", pixel_size=18, css_classes=["icon-inverse"]),
        on_click=lambda x: run_async("/home/doki/.config/hypr/scripts/toggle-power-save.sh"),
        css_classes=["accent-bg-btn", "rounded-icon-btn"],
        centered=True
    )

    theme_btn = ClickableBox(
        child=widgets.Icon(image="weather-clear-symbolic", pixel_size=18, css_classes=["icon-inverse"]),
        on_click=toggle_global_theme,
        css_classes=["accent-bg-btn", "rounded-icon-btn"], centered=True
    )

    power_btn = ClickableBox(
        child=widgets.Icon(image="system-shutdown-symbolic", pixel_size=18, css_classes=["icon-inverse"]), 
        on_click=lambda x: run_async("echo 'show' | socat - UNIX-CLIENT:/home/doki/.config/SYSui/powermenu.sock"), 
        css_classes=["danger-bg-btn", "rounded-icon-btn"], centered=True
    )
    
    def go_to_page2(x):
        window = app.get_window("control_center")
        if hasattr(window, '_switch_page'): window._switch_page(1)
    
    settings_btn = ClickableBox(
        child=widgets.Icon(image="system-run-symbolic", pixel_size=18, css_classes=["icon-inverse"]),
        on_click=go_to_page2,
        css_classes=["accent-bg-btn", "rounded-icon-btn"], centered=True
    )
    
    return widgets.Box(
        style="margin-bottom: 5px;",
        child=[
            widgets.Label(label="Control Center", css_classes=["card-title"], style="font-size: 18px;", halign="start", hexpand=True),
            power_save_btn, theme_btn, settings_btn, power_btn
        ]
    )

# --- МАГІЯ ВІКНА ---
def create_control_center():
    current_page = [0] 

    page1_content = widgets.Box(vertical=True, vexpand=True, child=[HeaderWidget(), widgets.Box(height_request=10), widgets.Box(spacing=10, child=[WifiWidget(), BluetoothWidget()]), widgets.Box(height_request=15), VolumeSlider("speaker"), widgets.Box(height_request=10), VolumeSlider("microphone"), widgets.Box(height_request=10), BrightnessSlider()])
    page2_header = widgets.Box(style="margin-bottom: 5px;", child=[widgets.Label(label="Система та Аудіо", css_classes=["card-title"], style="font-size: 18px;", halign="start", hexpand=True)])
    
    page2_content_items = [page2_header, widgets.Box(height_request=10), SystemStatsWidget(), widgets.Box(height_request=15), DisksWidget(), widgets.Box(height_request=15)]
    
    try: page2_content_items.append(mixer.MixerWidget())
    except: pass
    
    page2_content_items.extend([widgets.Box(height_request=15), widgets.Box(vertical=True, css_classes=["block", "dark", "wide"], child=[widgets.Label(label="Аудіо пристрої", css_classes=["card-title"], halign="start"), widgets.Box(height_request=10), widgets.Label(label="Системний вивід активний", css_classes=["b-subtext"], halign="start")])])
    page2_content = widgets.Box(vertical=True, child=page2_content_items)
    
    pages_stack = widgets.Box(vertical=True, css_classes=["main-bg"], vexpand=True)

    tg_rev = widgets.Revealer(transition_type="slide_up", transition_duration=350, child=NotificationsWidget())
    media_rev = widgets.Revealer(transition_type="slide_up", transition_duration=350, child=MediaWidget())
    main_rev = widgets.Revealer(transition_type="slide_up", transition_duration=350, child=pages_stack)

    def switch_page(page_num):
        current_page[0] = page_num
        while pages_stack.get_first_child():
            pages_stack.remove(pages_stack.get_first_child())
        
        if page_num == 0:
            pages_stack.append(page1_content)
            media_rev.visible = True
            tg_rev.visible = True
            utils.Timeout(50, lambda: media_rev.set_reveal_child(True) or False)
            utils.Timeout(50, lambda: tg_rev.set_reveal_child(True) or False)
        else:
            media_rev.set_reveal_child(False)
            tg_rev.set_reveal_child(False)
            
            def force_hide():
                if current_page[0] == 1:
                    media_rev.visible = False
                    tg_rev.visible = False
                return False
            
            utils.Timeout(350, force_hide)
            
            if not any(isinstance(c, widgets.Box) and "header-action-btn" in c.get_css_classes() for c in page2_header):
                back_btn = ClickableBox(child=widgets.Icon(image="go-previous-symbolic", pixel_size=20), on_click=lambda x: switch_page(0), css_classes=["header-action-btn"], centered=True)
                page2_header.append(back_btn)
            
            pages_stack.append(page2_content)

    switch_page(0)
    
    floating_container = widgets.Box(vertical=True, spacing=15, child=[tg_rev, media_rev, main_rev])

    window = widgets.Window(name="control_center", namespace="ignis", anchor=["bottom", "right"], margin_bottom=20, margin_right=20, css_classes=["unset-window"], visible=False, kb_mode="exclusive", child=floating_container)
    window._switch_page = switch_page

    def on_window_visible(win, param=None):
        if win.visible:
            main_rev.set_reveal_child(True)
            if current_page[0] == 0:
                tg_rev.visible = True
                media_rev.visible = True
                utils.Timeout(150, lambda: media_rev.set_reveal_child(True) or False)
                utils.Timeout(300, lambda: tg_rev.set_reveal_child(True) or False)
        else:
            tg_rev.set_reveal_child(False); media_rev.set_reveal_child(False); main_rev.set_reveal_child(False)

    window.connect("notify::visible", on_window_visible)

    key_controller = Gtk.EventControllerKey()
    def on_key_pressed(controller, keyval, keycode, state):
        if keyval == Gdk.KEY_Escape: window.visible = False; return True
        elif keyval in (Gdk.KEY_Page_Down, Gdk.KEY_Right):
            if current_page[0] == 0: switch_page(1)
            return True
        elif keyval in (Gdk.KEY_Page_Up, Gdk.KEY_Left):
            if current_page[0] == 1: switch_page(0)
            return True
        return False
    key_controller.connect("key-pressed", on_key_pressed); window.add_controller(key_controller)
    
    return window

app.add_window(window=create_control_center(), window_name="control_center")

for mod in [wallpapers, switcher]:
    try: mod.setup(app)
    except Exception as e: print(f"Error in {mod.__name__}: {e}")