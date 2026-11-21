from gi.repository import Gtk, Gdk
from ignis import widgets, utils
from ignis.services.audio import AudioService

audio = AudioService.get_default()

# --- СЛОВНИК ІКОНОК ---
ICON_MAP = {
    "firefox": "firefox",
    "chrome": "google-chrome",
    "chromium": "chromium-browser",
    "spotify": "spotify-client",
    "discord": "discord",
    "telegram": "telegram",
    "code": "visual-studio-code",
    "vlc": "vlc",
    "obs": "obs",
    "steam": "steam",
    "kitty": "terminal"
}

# --- УНІВЕРСАЛЬНИЙ КЛІКАБЕЛЬНИЙ БЛОК ---
def InteractiveBox(child, on_click, css_classes=[], style=""):
    box = widgets.Box(
        child=[child],
        css_classes=["clickable-box"] + css_classes,
        style=f"transition: 0.2s; border-radius: 12px; {style}"
    )

    gesture = Gtk.GestureClick()
    gesture.connect("released", lambda x, n, a, b: on_click(box))
    box.add_controller(gesture)

    def on_enter(c, x, y):
        box.set_cursor(Gdk.Cursor.new_from_name("pointer", None))
        box.set_style(f"background-color: rgba(255, 255, 255, 0.05); {style}")
    
    def on_leave(c):
        box.set_cursor(None)
        box.set_style(f"{style}")

    motion = Gtk.EventControllerMotion()
    motion.connect("enter", on_enter)
    motion.connect("leave", on_leave)
    box.add_controller(motion)

    return box

# --- БЛОК ВИБОРУ ПРИСТРОЮ ---
def DeviceSelector(title, icon_name, stream_type):
    list_box = widgets.Box(vertical=True, spacing=5, style="margin-top: 5px;")
    revealer = widgets.Revealer(transition_type="slide_down", child=list_box, reveal_child=False)
    
    status_label = widgets.Label(label="...", halign="start", style="font-size: 11px; color: var(--text-sub);", ellipsize="end")
    arrow_icon = widgets.Label(label="󰅀", style="color: var(--text-sub); transition: 0.2s;")

    cached_devices = {}

    def rebuild_list(*args):
        devices = audio.speakers if stream_type == "speaker" else audio.microphones
        active_dev = audio.speaker if stream_type == "speaker" else audio.microphone

        status_label.set_label(active_dev.description if active_dev else "Немає пристроїв")

        current_names = [dev.name for dev in devices]
        
        # Видаляємо старі
        for name in list(cached_devices.keys()):
            if name not in current_names:
                list_box.remove(cached_devices[name])
                del cached_devices[name]

        for dev in devices:
            is_active = (active_dev and dev.name == active_dev.name)
            
            if dev.name not in cached_devices:
                name_lbl = widgets.Label(label=dev.description, hexpand=True, halign="start", ellipsize="end")
                check_icon = widgets.Label(label="󰄬", style="color: var(--accent);")
                
                row = InteractiveBox(
                    child=widgets.Box(spacing=10, child=[
                        widgets.Icon(image=dev.icon_name or "audio-card-symbolic", pixel_size=16),
                        name_lbl,
                        check_icon
                    ]),
                    on_click=lambda x, d=dev: d.make_default(),
                    style="padding: 8px; border-radius: 8px;"
                )
                cached_devices[dev.name] = row
                list_box.append(row)
            
            # Оновлюємо візуал активного
            target_row = cached_devices[dev.name]
            target_row.get_first_child().get_children()[2].set_visible(is_active)
            target_row.get_first_child().get_children()[1].set_style("color: var(--accent); font-weight: bold;" if is_active else "color: var(--text-main);")

    audio.connect(f"notify::{stream_type}s", rebuild_list)
    audio.connect(f"notify::{stream_type}", rebuild_list)
    rebuild_list()

    header = InteractiveBox(
        child=widgets.Box(spacing=10, child=[
            widgets.Icon(image=icon_name, pixel_size=18, style="color: var(--accent);"),
            widgets.Box(vertical=True, child=[
                widgets.Label(label=title, halign="start", style="font-weight: bold; color: var(--text-main);"),
                status_label
            ]),
            widgets.Box(hexpand=True),
            arrow_icon
        ]),
        on_click=lambda x: [
            revealer.set_reveal_child(not revealer.reveal_child),
            arrow_icon.set_label("󰅃" if not revealer.reveal_child else "󰅀")
        ],
        style="padding: 5px;"
    )

    return widgets.Box(vertical=True, child=[header, revealer])

# --- РЯДОК ПРОГРАМИ ---
def AppStreamRow(stream):
    def get_icon(*args):
        name = (stream.name or "").lower()
        for k, v in ICON_MAP.items():
            if k in name: return v
        return stream.icon_name or "application-x-executable"

    vol_label = widgets.Label(label=stream.bind("volume", lambda v: f"{int(v)}%"), style="font-size: 10px; color: var(--text-sub);")

    scale = widgets.Scale(
        min=0, max=100, step=1, hexpand=True,
        value=stream.bind("volume"),
        on_change=lambda x: stream.set_volume(x.value),
        style="margin: 0 5px;"
    )

    mute_btn = InteractiveBox(
        child=widgets.Label(
            label=stream.bind("is_muted", lambda m: "󰝟" if m else "󰕾"),
            style=stream.bind("is_muted", lambda m: "color: var(--danger);" if m else "color: var(--text-main);")
        ),
        on_click=lambda x: stream.set_is_muted(not stream.is_muted),
        style="padding: 5px; min-width: 30px;"
    )

    return widgets.Box(
        spacing=10, style="margin-bottom: 10px; padding: 5px; border-radius: 10px; background: rgba(255,255,255,0.02);",
        child=[
            widgets.Icon(image=get_icon(), pixel_size=24, style=stream.bind("is_muted", lambda m: "opacity: 0.4;" if m else "opacity: 1;")),
            widgets.Box(vertical=True, hexpand=True, child=[
                widgets.Box(child=[
                    widgets.Label(label=stream.name, halign="start", ellipsize="end", style="font-weight: bold; color: var(--text-main);", max_width_chars=20),
                    widgets.Box(hexpand=True),
                    vol_label
                ]),
                widgets.Box(spacing=5, child=[scale, mute_btn])
            ])
        ]
    )

# --- ГОЛОВНИЙ ВІДЖЕТ ---
def MixerWidget():
    list_box = widgets.Box(vertical=True, spacing=5)
    cached_streams = {}

    def update_streams(*args):
        streams = audio.apps
        current_ids = [str(id(s)) for s in streams] # Для додатків id(s) ок, поки вони живі в сесії

        for s_id in list(cached_streams.keys()):
            if s_id not in current_ids:
                list_box.remove(cached_streams[s_id])
                del cached_streams[s_id]

        for s in streams:
            s_id = str(id(s))
            if s_id not in cached_streams:
                row = AppStreamRow(s)
                cached_streams[s_id] = row
                list_box.append(row)

    audio.connect("notify::apps", update_streams)
    update_streams()

    return widgets.Box(
        vertical=True, spacing=10, style="padding: 15px; min-width: 350px; background-color: var(--bg-color); border-radius: 16px;",
        child=[
            widgets.Label(label="󰓃 Audio Mixer", style="font-size: 18px; font-weight: 900; color: var(--accent); margin-bottom: 5px;", halign="start"),
            DeviceSelector("Вихід", "audio-speakers-symbolic", "speaker"),
            DeviceSelector("Вхід", "audio-input-microphone-symbolic", "microphone"),
            widgets.Box(style="background: var(--border-color); min-height: 1px; margin: 10px 0;"),
            widgets.Label(label="󰀻 Applications", style="font-weight: bold; color: var(--text-sub);", halign="start"),
            list_box
        ]
    )