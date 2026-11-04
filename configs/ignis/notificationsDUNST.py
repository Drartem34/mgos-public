from ignis import widgets, utils
from gi.repository import Gtk, Gdk
from ignis.services.notifications import NotificationService

notifications = NotificationService.get_default()

def NotificationWidget(n):
    # Іконка сповіщення
    icon = widgets.Icon(
        image=n.icon if n.icon else "dialog-information-symbolic",
        pixel_size=40,
        style="color: var(--accent); margin-right: 15px;"
    )

    # Кнопка закриття (хрестик)
    close_btn = widgets.Button(
        child=widgets.Icon(image="window-close-symbolic", pixel_size=16),
        on_click=lambda x: n.close(),
        style="background: transparent; border: none; box-shadow: none; color: var(--text-sub); padding: 0;"
    )

    content = widgets.Box(vertical=True, hexpand=True, child=[
        widgets.Box(spacing=10, child=[
            widgets.Label(label=n.app_name, style="font-size: 11px; font-weight: bold; color: var(--text-sub);", halign="start", hexpand=True),
            close_btn
        ]),
        widgets.Label(label=n.summary, style="font-weight: bold; font-size: 14px; color: var(--text-main); margin-top: 5px;", halign="start", ellipsize="end"),
        widgets.Label(label=n.body, style="font-size: 12px; color: var(--text-main); opacity: 0.8; margin-top: 2px;", halign="start", wrap=True)
    ])

    # ОСЬ ТУТ ПРОЗОРІСТЬ: var(--bg-color) підтягне твій rgba(24, 24, 24, 0.8)
    box = widgets.Box(
        css_classes=["osd-box"],
        child=[icon, content]
    )

    # Закриття по кліку на саму картку
    gesture = Gtk.GestureClick()
    gesture.connect("released", lambda x, clicks, a, b: n.close())
    box.add_controller(gesture)

    return box

def setup(app):
    notif_list = widgets.Box(vertical=True, spacing=10)
    notif_widgets = {}

    def on_notif_added(service, n):
        w = NotificationWidget(n)
        notif_widgets[n.id] = w
        notif_list.append(w)
        
        # Автозакриття (5 секунд, якщо система не передала свій час)
        timeout = n.timeout if n.timeout > 0 else 5000
        utils.Timeout(timeout, lambda: n.close() if n.id in notif_widgets else None)

    def on_notif_removed(service, n):
        w = notif_widgets.pop(n.id, None)
        if w:
            notif_list.remove(w)

    notifications.connect("notification-added", on_notif_added)
    notifications.connect("notification-closed", on_notif_removed)
    notifications.connect("notification-dismissed", on_notif_removed)

    # Головне прозоре вікно-контейнер
    win = widgets.Window(
        name="notifications_popup",
        namespace="ignis_notifications", # ЦЕЙ НЕЙМСПЕЙС ТРЕБА ДЛЯ БЛЮРУ
        anchor=["top", "right"],
        margin_top=15,
        margin_right=15,
        layer="overlay",
        exclusivity="ignore",
        style="background-color: transparent;", 
        child=notif_list
    )
    win.remove_css_class("background") # Вбиваємо дефолтний фон GTK
    app.add_window(win, window_name="notifications_popup")