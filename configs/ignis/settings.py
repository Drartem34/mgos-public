import sys
import json
import subprocess
import os
from pathlib import Path
from PyQt6.QtWidgets import (QApplication, QWidget, QHBoxLayout, QVBoxLayout, 
                             QLabel, QPushButton, QFrame, QCheckBox, QLineEdit,
                             QListWidget, QStackedWidget, QColorDialog, QSizeGrip, 
                             QGraphicsOpacityEffect, QGraphicsColorizeEffect, QFileDialog, QDialog, QRubberBand,
                             QSlider, QScrollArea)
from PyQt6.QtCore import (Qt, QPropertyAnimation, QEasingCurve, QRect, QPoint, QSize, QTimer,
                          QAbstractListModel, QModelIndex, QRunnable, QThreadPool, pyqtSignal, QObject)
from PyQt6.QtGui import QPainter, QColor, QBrush, QCursor, QIcon, QPixmap, QImage
import hashlib
import threading

# ==========================================
# 1. КАСТОМНІ ВІДЖЕТИ ТА АНІМАЦІЇ
# ==========================================

class WorkerSignals(QObject):
    thumb_ready = pyqtSignal(int, str) # row_index, thumb_path

class ThumbnailWorker(QRunnable):
    def __init__(self, index, original_path):
        super().__init__()
        self.index = index
        self.original_path = original_path
        self.signals = WorkerSignals()
        self.cache_dir = os.path.expanduser("~/.cache/ignis/wallpaper_thumbs")
        os.makedirs(self.cache_dir, exist_ok=True)

    def run(self):
        hash_name = hashlib.md5(self.original_path.encode()).hexdigest()
        thumb_path = os.path.join(self.cache_dir, f"{hash_name}.png")

        if not os.path.exists(thumb_path):
            source_path = self.original_path
            if os.path.isdir(self.original_path):
                for ext in ['jpg', 'png', 'jpeg', 'webp', 'mp4', 'gif']:
                    p = os.path.join(self.original_path, f"day.{ext}")
                    if os.path.exists(p):
                        source_path = p
                        break
            
            ext = source_path.lower().split('.')[-1]
            if ext in ['mp4', 'webm', 'gif']:
                subprocess.run(f"ffmpeg -i '{source_path}' -ss 00:00:00.000 -vframes 1 -s 300x300 '{thumb_path}' -y", shell=True, stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
            else:
                # QImage безпечний для фонових потоків, на відміну від QPixmap
                img = QImage(source_path)
                if not img.isNull():
                    img.scaled(300, 300, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation).save(thumb_path, "PNG")

        self.signals.thumb_ready.emit(self.index, thumb_path)

class WallpaperModel(QAbstractListModel):
    def __init__(self, items):
        super().__init__()
        self.items = items
        self.thumbnails = {}
        self.loading = set()
        self.pool = QThreadPool.globalInstance()
        self.fallback_icon = QIcon.fromTheme("image-loading")

    def rowCount(self, parent=QModelIndex()):
        return len(self.items)

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid(): return None
        row = index.row()
        path = self.items[row]

        if role == Qt.ItemDataRole.DisplayRole:
            return os.path.basename(path)
            
        elif role == Qt.ItemDataRole.DecorationRole:
            if row in self.thumbnails:
                return self.thumbnails[row]
            
            if row not in self.loading:
                self.loading.add(row)
                worker = ThumbnailWorker(row, path)
                worker.signals.thumb_ready.connect(self.on_thumb_ready)
                self.pool.start(worker)
                
            return self.fallback_icon
        return None

    def on_thumb_ready(self, row, thumb_path):
        if os.path.exists(thumb_path):
            self.thumbnails[row] = QIcon(thumb_path)
            idx = self.index(row, 0)
            self.dataChanged.emit(idx, idx, [Qt.ItemDataRole.DecorationRole])

    def update_items(self, new_items):
        self.beginResetModel()
        self.items = new_items
        self.thumbnails.clear()
        self.loading.clear()
        self.endResetModel()

class FadeStackedWidget(QStackedWidget):
    def setCurrentIndex(self, index):
        if index == self.currentIndex(): return
        
        super().setCurrentIndex(index)
        widget = self.currentWidget()
        
        effect = QGraphicsOpacityEffect(widget)
        widget.setGraphicsEffect(effect)
        
        self.anim = QPropertyAnimation(effect, b"opacity")
        self.anim.setDuration(300)
        self.anim.setStartValue(0.0)
        self.anim.setEndValue(1.0)
        self.anim.setEasingCurve(QEasingCurve.Type.InOutSine)
        
        # ФІКС: Знімаємо ефект після закінчення анімації
        self.anim.finished.connect(lambda: widget.setGraphicsEffect(None))
        
        self.anim.start()

class MeguminSwitch(QCheckBox):
    def __init__(self, key, current_state, save_callback, get_accent_cb, parent=None):
        super().__init__(parent)
        self.key = key
        self.save_callback = save_callback
        self.get_accent_cb = get_accent_cb
        self.setFixedSize(44, 24)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.setChecked(current_state)
        self.clicked.connect(self.on_toggle)

    def on_toggle(self):
        self.save_callback(self.key, self.isChecked())
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        
        accent = self.get_accent_cb()
        bg_color = QColor(accent) if self.isChecked() else QColor("#444444")
        p.setBrush(QBrush(bg_color))
        p.drawRoundedRect(0, 0, self.width(), self.height(), 12, 12)
        
        p.setBrush(QBrush(QColor("white")))
        x_pos = 22 if self.isChecked() else 2
        p.drawEllipse(x_pos, 2, 20, 20)

class PowerProfileSelector(QWidget):
    def __init__(self, current_profile, save_callback, get_accent_cb, parent=None):
        super().__init__(parent)
        self.save_callback = save_callback
        self.get_accent_cb = get_accent_cb
        self.current_profile = current_profile

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)

        self.buttons = {}
        profiles = [("power-saver", "Економ"), ("balanced", "Баланс"), ("performance", "Макс")]

        for prof_id, prof_name in profiles:
            btn = QPushButton(prof_name)
            btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
            btn.clicked.connect(lambda checked, p=prof_id: self.set_profile(p))
            layout.addWidget(btn)
            self.buttons[prof_id] = btn

        self.update_styles()

    def set_profile(self, profile):
        self.current_profile = profile
        self.update_styles()
        self.save_callback("power_profile", profile)

    def update_styles(self):
        accent = self.get_accent_cb()
        for prof_id, btn in self.buttons.items():
            if prof_id == self.current_profile:
                btn.setStyleSheet(f"QPushButton {{ background-color: {accent}; color: white; border-radius: 8px; padding: 6px 10px; font-weight: bold; border: none; }}")
            else:
                btn.setStyleSheet("QPushButton { background-color: rgba(100, 100, 100, 0.2); color: #aaaaaa; border-radius: 8px; padding: 6px 10px; font-weight: bold; border: none; } QPushButton:hover { background-color: rgba(150, 150, 150, 0.3); color: white; }")

class DockWhitelistEditor(QWidget):
    def __init__(self, current_whitelist, save_callback, get_accent_cb, parent=None):
        super().__init__(parent)
        self.save_callback = save_callback
        self.get_accent_cb = get_accent_cb
        
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        
        self.input_field = QLineEdit(current_whitelist)
        self.input_field.setPlaceholderText("nautilus, kitty, steam...")
        self.input_field.setFixedWidth(200)
        self.input_field.editingFinished.connect(self.save_whitelist)
        
        layout.addWidget(self.input_field)
        self.update_styles()

    def save_whitelist(self):
        self.save_callback("dock_whitelist", self.input_field.text())

    def update_styles(self):
        accent = self.get_accent_cb()
        self.input_field.setStyleSheet(f"QLineEdit {{ background-color: rgba(100, 100, 100, 0.2); border: 1px solid rgba(150, 150, 150, 0.3); border-radius: 6px; padding: 4px 8px; color: white; }} QLineEdit:focus {{ border-color: {accent}; background-color: rgba(100, 100, 100, 0.4); }}")

# Значення за замовчуванням для налаштувань панелей (ті самі, що в SYSui/sysui_common.h)
PANEL_DEFAULTS = {
    "accent_mode": "wal", "accent_color": "#9929CF", "animations": True,
    "topbar_style": "bubbles", "topbar_opacity": 80, "topbar_border": True, "topbar_lang": "flag",
    "dock_opacity": 80, "dock_border": True, "dock_border_style": "subtle", "dock_border_opacity": 100,
    "dock_icon_size": 38, "dock_magnify": True, "dock_magnify_pct": 160,
    "cc_opacity": 78, "watchdog": True, "sys_popups": True,
    "dock_files": True, "dock_files_source": "both", "dock_files_count": 4, "dock_files_days": 3,
}


class MeguminSlider(QWidget):
    """Повзунок зі значенням. Зберігає з невеликою затримкою — модулі SYSui підхоплюють зміни наживо."""
    def __init__(self, key, value, minimum, maximum, suffix, save_callback, get_accent_cb, parent=None):
        super().__init__(parent)
        self.key, self.suffix = key, suffix
        self.save_callback, self.get_accent_cb = save_callback, get_accent_cb
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setRange(minimum, maximum)
        self.slider.setValue(int(value))
        self.slider.setFixedWidth(170)
        self.slider.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.label = QLabel(f"{int(value)}{suffix}")
        self.label.setFixedWidth(46)
        self.label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        layout.addWidget(self.slider)
        layout.addWidget(self.label)
        self.timer = QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.setInterval(250)
        self.timer.timeout.connect(lambda: self.save_callback(self.key, self.slider.value()))
        self.slider.valueChanged.connect(self.on_change)
        self.update_styles()

    def on_change(self, v):
        self.label.setText(f"{v}{self.suffix}")
        self.timer.start()

    def update_styles(self):
        a = self.get_accent_cb()
        self.slider.setStyleSheet(
            "QSlider::groove:horizontal { height: 6px; border-radius: 3px; background: rgba(150,150,150,0.3); }"
            f"QSlider::sub-page:horizontal {{ height: 6px; border-radius: 3px; background: {a}; }}"
            "QSlider::handle:horizontal { width: 16px; height: 16px; margin: -5px 0; border-radius: 8px; background: white; }")
        self.label.setStyleSheet("color: #aaaaaa; font-weight: bold; background: transparent;")


class SegmentedSelector(QWidget):
    """Перемикач з кількох варіантів (як режим живлення)."""
    def __init__(self, key, options, current, save_callback, get_accent_cb, parent=None):
        super().__init__(parent)
        self.key, self.current = key, current
        self.save_callback, self.get_accent_cb = save_callback, get_accent_cb
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)
        self.buttons = {}
        for opt_id, name in options:
            btn = QPushButton(name)
            btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
            btn.clicked.connect(lambda checked, o=opt_id: self.select(o))
            layout.addWidget(btn)
            self.buttons[opt_id] = btn
        self.update_styles()

    def select(self, opt):
        self.current = opt
        self.update_styles()
        self.save_callback(self.key, opt)

    def update_styles(self):
        a = self.get_accent_cb()
        for opt, btn in self.buttons.items():
            if opt == self.current:
                btn.setStyleSheet(f"QPushButton {{ background-color: {a}; color: white; border-radius: 8px; padding: 6px 12px; font-weight: bold; border: none; }}")
            else:
                btn.setStyleSheet("QPushButton { background-color: rgba(100,100,100,0.2); color: #aaaaaa; border-radius: 8px; padding: 6px 12px; font-weight: bold; border: none; } QPushButton:hover { background-color: rgba(150,150,150,0.3); color: white; }")


class SettingRow(QFrame):
    def __init__(self, icon_name, fallback_emoji, title, description="", control=None, is_last=False, on_click=None):
        super().__init__()
        self.icon_name = icon_name # Зберігаємо назву іконки
        self.on_click_callback = on_click
        self.control = control
        self.setFixedHeight(64)
        
        if self.on_click_callback or self.control:
            self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
            
        self.border_css = "border-bottom: none;" if is_last else "border-bottom: 1px solid {border_color};"
        self.is_last = is_last
        
        layout = QHBoxLayout(self)
        layout.setContentsMargins(20, 0, 20, 0)

        self.icon_lbl = QLabel()
        self.icon_lbl.setStyleSheet("background: transparent;")
        
        text_layout = QVBoxLayout()
        text_layout.setSpacing(2)
        text_layout.setAlignment(Qt.AlignmentFlag.AlignVCenter)
        
        self.title_lbl = QLabel(title)
        text_layout.addWidget(self.title_lbl)
        
        if description:
            self.desc_lbl = QLabel(description)
            self.desc_lbl.setStyleSheet("font-size: 11px; background: transparent; color: #888888;")
            text_layout.addWidget(self.desc_lbl)
            
        layout.addWidget(self.icon_lbl)
        layout.addSpacing(10)
        layout.addLayout(text_layout)
        layout.addStretch()
        
        if control:
            layout.addWidget(control)
        else:
            arrow = QLabel("〉")
            arrow.setStyleSheet("color: #666666; font-size: 14px; background: transparent; font-weight: bold;")
            layout.addWidget(arrow)

        self.update_theme(True) # Викликаємо після ініціалізації всього

    def update_theme(self, is_dark):
        border = "rgba(255,255,255,0.1)" if is_dark else "rgba(0,0,0,0.1)"
        bg = "rgba(255,255,255,0.03)" if is_dark else "rgba(0,0,0,0.03)"
        hover = "rgba(255,255,255,0.08)" if is_dark else "rgba(0,0,0,0.08)"
        
        b_css = self.border_css.format(border_color=border)
        # Селектор за objectName: раніше "QFrame {...}" чіплявся і до QLabel (вони теж QFrame) → смуги під текстом
        self.setObjectName("settingRow")
        self.setStyleSheet(f"QFrame#settingRow {{ background-color: {bg}; {b_css} }} QFrame#settingRow:hover {{ background-color: {hover}; }}")
        
        if hasattr(self, 'title_lbl'):
            self.title_lbl.setStyleSheet(f"font-size: 14px; font-weight: 600; background: transparent; color: {'white' if is_dark else 'black'};")
        
        # ДІСТАЄМО ПРАВИЛЬНУ ІКОНКУ БЕЗ ФІЗИЧНОГО ПЕРЕФАРБУВАННЯ
        sys_icon = QIcon.fromTheme(self.icon_name)
        if sys_icon.isNull() or sys_icon.pixmap(24, 24).isNull():
            sys_icon = QIcon.fromTheme("preferences-system")
        self.icon_lbl.setPixmap(sys_icon.pixmap(24, 24))
        
        if hasattr(self, 'control') and self.control and hasattr(self.control, 'update_styles'):
            self.control.update_styles()

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            if self.on_click_callback:
                self.on_click_callback()
            elif self.control and isinstance(self.control, MeguminSwitch):
                self.control.setChecked(not self.control.isChecked())
                self.control.on_toggle()
        super().mouseReleaseEvent(event)


# ==========================================
# 2. ВІКНО ОБРІЗАННЯ ФОТО (CROPPER)
# ==========================================

class ImageCropperDialog(QDialog):
    def __init__(self, image_path, accent_color, is_dark, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Обрізати аватарку")
        self.setFixedSize(540, 600)
        
        bg_col = "#242424" if is_dark else "#F5F5F5"
        text_col = "white" if is_dark else "black"
        self.setStyleSheet(f"QDialog {{ background-color: {bg_col}; color: {text_col}; }}")
        
        layout = QVBoxLayout(self)
        
        lbl = QLabel("Натисни і тягни мишкою, щоб виділити квадрат:")
        lbl.setStyleSheet("font-weight: bold; font-size: 14px;")
        layout.addWidget(lbl)
        
        self.original_pixmap = QPixmap(image_path)
        self.display_pixmap = self.original_pixmap.scaled(500, 500, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
        
        self.image_label = QLabel()
        self.image_label.setPixmap(self.display_pixmap)
        self.image_label.setFixedSize(self.display_pixmap.size())
        
        img_layout = QHBoxLayout()
        img_layout.addStretch()
        img_layout.addWidget(self.image_label)
        img_layout.addStretch()
        layout.addLayout(img_layout)
        
        btn_layout = QHBoxLayout()
        btn_layout.addStretch()
        
        cancel_btn = QPushButton("Скасувати")
        save_btn = QPushButton("Зберегти")
        
        cancel_btn.setStyleSheet(f"QPushButton {{ background-color: rgba(100,100,100,0.3); color: {text_col}; border-radius: 8px; padding: 8px 16px; border: none; font-weight: bold; }}")
        save_btn.setStyleSheet(f"QPushButton {{ background-color: {accent_color}; color: white; border-radius: 8px; padding: 8px 16px; border: none; font-weight: bold; }}")
        cancel_btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        save_btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        
        btn_layout.addWidget(cancel_btn)
        btn_layout.addWidget(save_btn)
        layout.addLayout(btn_layout)
        
        cancel_btn.clicked.connect(self.reject)
        save_btn.clicked.connect(self.accept)
        
        # Логіка виділення
        self.rubberBand = QRubberBand(QRubberBand.Shape.Rectangle, self.image_label)
        s = min(self.display_pixmap.width(), self.display_pixmap.height())
        self.rubberBand.setGeometry((self.display_pixmap.width()-s)//2, (self.display_pixmap.height()-s)//2, s, s)
        self.rubberBand.show()
        
        self.origin = QPoint()
        self.dragging = False
        
        self.image_label.mousePressEvent = self.img_press
        self.image_label.mouseMoveEvent = self.img_move
        self.image_label.mouseReleaseEvent = self.img_release

    def img_press(self, event):
        self.origin = event.pos()
        self.rubberBand.setGeometry(QRect(self.origin, QSize()))
        self.dragging = True

    def img_move(self, event):
        if not self.dragging: return
        diff = event.pos() - self.origin
        side = min(abs(diff.x()), abs(diff.y()))
        x = self.origin.x() if diff.x() > 0 else self.origin.x() - side
        y = self.origin.y() if diff.y() > 0 else self.origin.y() - side
        rect = QRect(x, y, side, side).intersected(self.image_label.rect())
        self.rubberBand.setGeometry(rect)

    def img_release(self, event):
        self.dragging = False

    def get_cropped_image(self):
        rect = self.rubberBand.geometry()
        ratio_x = self.original_pixmap.width() / self.display_pixmap.width()
        ratio_y = self.original_pixmap.height() / self.display_pixmap.height()
        
        orig_rect = QRect(
            int(rect.x() * ratio_x), int(rect.y() * ratio_y),
            int(rect.width() * ratio_x), int(rect.height() * ratio_y)
        )
        return self.original_pixmap.copy(orig_rect).scaled(256, 256, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)


# ==========================================
# 3. ГОЛОВНЕ ВІКНО & ЛОГІКА СИСТЕМИ
# ==========================================

class MeguminSettings(QWidget):
    def __init__(self):
        super().__init__()
        self.oldPos = None
        self.config_dir = Path.home() / ".config" / "megumin-os"
        self.config_file = self.config_dir / "settings.json"

        self.setup_icon_theme()
        
        self.accent_color = self.get_wal_accent_color()
        self.settings = self.get_real_system_states()
        self.initUI()
        self.apply_ui_theme()

    def setup_icon_theme(self):
        try:
            theme = subprocess.run("gsettings get org.gnome.desktop.interface icon-theme", shell=True, capture_output=True, text=True).stdout.strip().replace("'", "")
            QIcon.setThemeName(theme if theme else "WhiteSur")
        except:
            QIcon.setThemeName("WhiteSur")
        
        paths = QIcon.themeSearchPaths()
        custom_paths = [
            "/usr/share/icons",
            os.path.expanduser("~/.local/share/icons"),
            os.path.expanduser("~/.icons")
        ]
        for p in custom_paths:
            if p not in paths:
                paths.append(p)
        QIcon.setThemeSearchPaths(paths)

    def get_wal_accent_color(self):
        # Той самий акцент, що й у всіх модулях SYSui: власний колір або color5 з pywal
        try:
            with open(self.config_file, "r") as f:
                saved = json.load(f)
            if saved.get("accent_mode") == "custom" and str(saved.get("accent_color", "")).startswith("#"):
                return saved["accent_color"]
        except Exception: pass
        wal_path = os.path.expanduser("~/.cache/wal/colors.json")
        try:
            if os.path.exists(wal_path):
                with open(wal_path, "r") as f:
                    return json.load(f)["colors"]["color5"]
        except: pass
        return "#C86FA3"

    def get_real_system_states(self):
        state = dict(PANEL_DEFAULTS)
        try:
            if self.config_file.exists():
                with open(self.config_file, "r") as f:
                    saved = json.load(f)
                    state.update(saved)  # усі збережені ключі (раніше нові ключі губились)
                    state["video_optimization"] = saved.get("video_optimization", False)
                    state["wp_anime4k"] = saved.get("wp_anime4k", False)
                    state["dock_whitelist"] = saved.get("dock_whitelist", "nautilus, kitty, wezterm, ignis, code, modrinth-app, steam, spotify")
            else:
                state["video_optimization"] = False
                state["wp_anime4k"] = False
                state["dock_whitelist"] = "nautilus, kitty, wezterm, ignis, code, modrinth-app, steam, spotify"
        except:
            state["video_optimization"] = False
            state["wp_anime4k"] = False
            state["dock_whitelist"] = "nautilus, kitty, wezterm, ignis, code, modrinth-app, steam, spotify"

        DEVNULL = subprocess.DEVNULL
        
        try:
            wifi = subprocess.run("nmcli -t -f WIFI general", shell=True, capture_output=True, text=True, timeout=1).stdout.strip()
            state["wifi_enabled"] = (wifi == "enabled")
        except: state["wifi_enabled"] = False

        try:
            bt = subprocess.run("bluetoothctl show | grep 'Powered: yes'", shell=True, capture_output=True, text=True, timeout=1).stdout.strip()
            state["bluetooth_enabled"] = bool(bt)
        except: state["bluetooth_enabled"] = False

        try:
            blur = subprocess.run("hyprctl getoption decoration:blur:enabled -j", shell=True, capture_output=True, text=True, timeout=1).stdout.strip()
            state["hyprland_blur"] = bool(json.loads(blur).get("int", 1))
        except: state["hyprland_blur"] = True

        try:
            theme = subprocess.run("gsettings get org.gnome.desktop.interface color-scheme", shell=True, capture_output=True, text=True, timeout=1).stdout.strip()
            state["dark_theme"] = "prefer-dark" in theme
        except: state["dark_theme"] = True

        try:
            nl = subprocess.run("pgrep hyprsunset", shell=True, capture_output=True, text=True, timeout=1).stdout.strip()
            state["night_light"] = bool(nl)
        except: state["night_light"] = False
        
        try:
            prof = subprocess.run("powerprofilesctl get", shell=True, capture_output=True, text=True, timeout=1).stdout.strip()
            state["power_profile"] = prof if prof else "balanced"
        except: state["power_profile"] = "balanced"

        return state

    def get_current_accent(self):
        return self.accent_color

    def save_setting(self, key, value):
        self.settings[key] = value
        self.config_dir.mkdir(parents=True, exist_ok=True)
        # Перечитуємо файл і змінюємо ЛИШЕ один ключ: модулі SYSui (Control Center)
        # теж пишуть у settings.json, і повний перезапис затирав би їхні зміни.
        data = {}
        try:
            with open(self.config_file, "r") as f:
                data = json.load(f)
        except Exception:
            data = dict(self.settings)
        data[key] = value
        tmp = str(self.config_file) + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
        os.replace(tmp, self.config_file)
        self.apply_system_setting(key, value)

    def apply_system_setting(self, key, value):
        DEVNULL = subprocess.DEVNULL
        try:
            if key == "wifi_enabled":
                cmd = "nmcli radio wifi on" if value else "nmcli radio wifi off"
                subprocess.Popen(cmd, shell=True, stdout=DEVNULL, stderr=DEVNULL)
            elif key == "bluetooth_enabled":
                cmd = "bluetoothctl power on" if value else "bluetoothctl power off"
                subprocess.Popen(cmd, shell=True, stdout=DEVNULL, stderr=DEVNULL)
            elif key == "hyprland_blur":
                subprocess.Popen(f"hyprctl keyword decoration:blur:enabled {str(value).lower()}", shell=True, stdout=DEVNULL, stderr=DEVNULL)
            elif key == "dark_theme":
                self.apply_ui_theme() 
                mode = "prefer-dark" if value else "default"
                wb_css = "style-dark.css" if value else "style-light.css"
                
                cmd = f"gsettings set org.gnome.desktop.interface color-scheme '{mode}' && " \
                      f"cp ~/.config/waybar/{wb_css} ~/.config/waybar/style.css && " \
                      f"killall -SIGUSR2 waybar"
                subprocess.Popen(cmd, shell=True, stdout=DEVNULL, stderr=DEVNULL)
            elif key == "night_light":
                if value: subprocess.Popen(["hyprsunset", "-t", "4000"], start_new_session=True, stdout=DEVNULL, stderr=DEVNULL)
                else: subprocess.Popen(["killall", "hyprsunset"], stdout=DEVNULL, stderr=DEVNULL)
            elif key == "power_profile":
                subprocess.Popen(f"powerprofilesctl set {value}", shell=True, stdout=DEVNULL, stderr=DEVNULL)
        except: pass

    def launch_app(self, cmd, close_ui=False):
        subprocess.Popen(cmd, shell=True, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if close_ui: self.close()

    def pick_accent_color(self):
        color = QColorDialog.getColor(QColor(self.accent_color))
        if color.isValid():
            hex_color = color.name()
            self.accent_color = hex_color
            subprocess.Popen(f"hyprctl keyword general:col.active_border rgba({hex_color.replace('#','')}ff)", shell=True)
            # Застосовується до ВСІХ модулів SYSui наживо
            self.save_setting("accent_color", hex_color)
            self.save_setting("accent_mode", "custom")
            if hasattr(self, "accent_wal_switch"):
                self.accent_wal_switch.setChecked(False)
            self.apply_ui_theme()

    def set_accent_from_wal(self, key, value):
        self.save_setting("accent_mode", "wal" if value else "custom")
        self.accent_color = self.get_wal_accent_color()
        self.apply_ui_theme()

    def set_sddm_avatar(self):
        file_path, _ = QFileDialog.getOpenFileName(
            self, "Вибрати аватарку для SDDM", str(Path.home()), "Images (*.png *.jpg *.jpeg)"
        )
        
        if file_path:
            is_dark = self.settings.get("dark_theme", True)
            dialog = ImageCropperDialog(file_path, self.accent_color, is_dark, self)
            
            if dialog.exec() == QDialog.DialogCode.Accepted:
                try:
                    cropped = dialog.get_cropped_image()
                    dest_icon = Path.home() / ".face.icon"
                    dest_face = Path.home() / ".face"
                    
                    # Перевіряємо чи реально зберегло
                    if not cropped.save(str(dest_icon), "PNG"):
                        raise Exception(f"Немає прав для запису у файл .face.icon")
                    cropped.save(str(dest_face), "PNG")
                    
                    subprocess.Popen(f"notify-send 'Аватарку встановлено' 'Наступного разу в SDDM буде нове фото'", shell=True)
                except Exception as e:
                    # Тепер виведе точну причину помилки
                    subprocess.Popen(f"notify-send -u critical 'Помилка' 'Деталі: {str(e)}'", shell=True)

    def apply_ui_theme(self):
        is_dark = self.settings.get("dark_theme", True)
        
        # Прибрали тупе дописування "-dark", тепер беремо тему як є
        try:
            theme = subprocess.run("gsettings get org.gnome.desktop.interface icon-theme", shell=True, capture_output=True, text=True).stdout.strip().replace("'", "")
            QIcon.setThemeName(theme if theme else "WhiteSur")
        except:
            QIcon.setThemeName("WhiteSur")

        bg_main = "#242424" if is_dark else "#F5F5F5"
        bg_sidebar = "rgba(30, 30, 40, 0.6)" if is_dark else "rgba(220, 220, 230, 0.6)"
        
        self.right_panel.setStyleSheet(f"QFrame {{ background-color: {bg_main}; border-top-right-radius: 15px; border-bottom-right-radius: 15px; }}")
        self.left_panel.setStyleSheet(f"QFrame {{ background-color: {bg_sidebar}; border-top-left-radius: 15px; border-bottom-left-radius: 15px; }}")
        self.title_lbl.setStyleSheet(f"color: {self.accent_color}; font-size: 20px; font-weight: bold; background: transparent; padding-left: 10px; margin-bottom: 20px;")
        
        self.nav_list.setStyleSheet(f"QListWidget {{ background: transparent; border: none; outline: none; }} QListWidget::item {{ color: {'#cccccc' if is_dark else '#444444'}; padding: 12px; border-radius: 8px; margin-bottom: 5px; }} QListWidget::item:selected {{ background: {self.accent_color}; color: white; font-weight: bold; }}")
        
        for label in self.page_titles:
            label.setStyleSheet(f"color: {self.accent_color}; font-size: 26px; font-weight: 800; margin-bottom: 20px;")
        for card in self.cards:
            card.setStyleSheet(f"background-color: {'#333333' if is_dark else '#FFFFFF'}; border-radius: 12px;")
        for row in self.all_rows:
            row.update_theme(is_dark)

        if hasattr(self, 'wp_filter_btns'):
            for f_type, btn in self.wp_filter_btns.items():
                if f_type == self.current_wp_filter:
                    btn.setStyleSheet(f"QPushButton {{ background-color: {self.accent_color}; color: white; border-radius: 8px; padding: 6px 12px; font-weight: bold; border: none; }}")    

    def initUI(self):
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.resize(900, 600)
        
        self.page_titles, self.cards, self.all_rows = [], [], []
        main_layout = QHBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        self.left_panel = QFrame()
        left_layout = QVBoxLayout(self.left_panel)
        left_layout.setContentsMargins(15, 30, 15, 30)

        self.title_lbl = QLabel("Megumin OS")
        self.nav_list = QListWidget()
        items = [("preferences-desktop", "✨", " Зовнішній вигляд"), ("image-x-generic", "", "Шпалери"), ("network-wireless", "󰤨", " Мережа"), ("video-display", "󰍹", " Екран"), ("audio-volume-high", "󰕾", " Звук"), ("utilities-system-monitor", "󰒋", " Система"), ("user-bookmarks", "🗂", " Док-панель"), ("preferences-desktop-theme", "🎛", " Панелі та віджети")]
        
        for icon_name, fallback, text in items:
            sys_icon = QIcon.fromTheme(icon_name)
            item = QListWidgetItem(sys_icon if not sys_icon.isNull() else QIcon(), text if not sys_icon.isNull() else f"{fallback} {text}")
            self.nav_list.addItem(item)
        self.nav_list.setCurrentRow(0)

        left_layout.addWidget(self.title_lbl)
        left_layout.addWidget(self.nav_list)

        self.right_panel = QFrame()
        right_layout = QVBoxLayout(self.right_panel)
        right_layout.setContentsMargins(0, 10, 10, 0)

        top_bar = QHBoxLayout()
        top_bar.addStretch()
        close_btn = QPushButton("✕")
        close_btn.setFixedSize(30, 30)
        close_btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        close_btn.setStyleSheet("QPushButton { background-color: #d75f5f; color: white; border-radius: 15px; font-weight: bold; }")
        close_btn.clicked.connect(self.close)
        top_bar.addWidget(close_btn)

        self.pages = FadeStackedWidget()
        self.pages.addWidget(self.create_appearance_page())
        self.pages.addWidget(self.create_wallpapers_page())
        self.pages.addWidget(self.create_network_page())
        self.pages.addWidget(self.create_display_page())
        self.pages.addWidget(self.create_audio_page())
        self.pages.addWidget(self.create_system_page())
        self.pages.addWidget(self.create_dock_page())
        self.pages.addWidget(self.create_panels_page())

        self.nav_list.currentRowChanged.connect(self.pages.setCurrentIndex)
        right_layout.addLayout(top_bar)
        right_layout.addWidget(self.pages)
        
        size_grip = QSizeGrip(self)
        right_layout.addWidget(size_grip, 0, Qt.AlignmentFlag.AlignRight)

        main_layout.addWidget(self.left_panel, 1)
        main_layout.addWidget(self.right_panel, 3)

    def create_card(self, rows):
        card = QFrame()
        self.cards.append(card)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        for row in rows:
            self.all_rows.append(row)
            layout.addWidget(row)
        return card

    def create_page_base(self, title_text):
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(40, 20, 40, 40)
        layout.setAlignment(Qt.AlignmentFlag.AlignTop)
        title = QLabel(title_text)
        self.page_titles.append(title)
        layout.addWidget(title)
        return page, layout

    def create_appearance_page(self):
        page, layout = self.create_page_base("Зовнішній вигляд")
        rows = [
            SettingRow("weather-clear", "🌓", "Темна тема", "Глобальна тема системи", MeguminSwitch("dark_theme", self.settings["dark_theme"], self.save_setting, self.get_current_accent)),
            SettingRow("preferences-system", "✨", "Ефекти Hyprland", "Блюр та тіні вікон", MeguminSwitch("hyprland_blur", self.settings["hyprland_blur"], self.save_setting, self.get_current_accent)),
            SettingRow("preferences-desktop-color", "🎨", "Акцентний колір", "Вибрати колір рамок", on_click=self.pick_accent_color),
            SettingRow("preferences-desktop-wallpaper", "🖼", "Шпалери", "Відкрити галерею", on_click=lambda: self.launch_app("ignis toggle-window wallpaper_selector")),
            
            # ОСЬ НАША НОВА КНОПКА
            SettingRow("media-playback-pause", "⏸", "Оптимізація відео-шпалер", "Пауза при активному вікні", MeguminSwitch("video_optimization", self.settings.get("video_optimization", False), self.save_setting, self.get_current_accent)),
            SettingRow("video-display", "✨", "Шейдери Anime4K", "Деблюр + апскейл мильних шпалер", MeguminSwitch("wp_anime4k", self.settings.get("wp_anime4k", False), self.save_setting, self.get_current_accent)),
            
            SettingRow("system-users", "👤", "Аватарка екрану входу", "Змінити фото в SDDM", is_last=True, on_click=self.set_sddm_avatar)
        ]
        layout.addWidget(self.create_card(rows))
        return page

    def create_wallpapers_page(self):
        page, layout = self.create_page_base("Галерея шпалер")
        
        # Панель фільтрів
        filter_layout = QHBoxLayout()
        filters = [("Всі", "all", "folder-pictures"), ("Картинки", "images", "image-x-generic"), 
                   ("Відео", "videos", "video-x-generic"), ("Динамічні", "dynamic", "preferences-desktop-wallpaper")]
        
        self.wp_filter_btns = {}
        for text, f_type, icon in filters:
            btn = QPushButton(text)
            btn.setIcon(QIcon.fromTheme(icon))
            btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
            btn.clicked.connect(lambda checked, t=f_type: self.load_wallpapers(t))
            filter_layout.addWidget(btn)
            self.wp_filter_btns[f_type] = btn
            
        refresh_btn = QPushButton("Оновити")
        refresh_btn.setIcon(QIcon.fromTheme("view-refresh"))
        refresh_btn.clicked.connect(lambda: self.load_wallpapers(self.current_wp_filter))
        filter_layout.addWidget(refresh_btn)
        filter_layout.addStretch()
        layout.addLayout(filter_layout)

        # Список мініатюр (Lazy Load)
        from PyQt6.QtWidgets import QListView
        self.wp_list = QListView()
        self.wp_list.setViewMode(QListView.ViewMode.IconMode)
        self.wp_list.setIconSize(QSize(200, 150))
        self.wp_list.setGridSize(QSize(220, 190))
        self.wp_list.setResizeMode(QListView.ResizeMode.Adjust)
        self.wp_list.setUniformItemSizes(True)
        self.wp_list.setWordWrap(True)
        self.wp_list.setStyleSheet("""
            QListView { background: transparent; border: none; outline: none; }
            QListView::item { padding: 8px; border-radius: 12px; }
        """)
        
        self.wp_model = WallpaperModel([])
        self.wp_list.setModel(self.wp_model)
        self.wp_list.clicked.connect(self.on_wallpaper_selected)
        
        layout.addWidget(self.wp_list)
        
        self.current_wp_filter = "all"
        self.load_wallpapers("all") # Завантажуємо список файлів (самі картинки не вантажаться)
        
        return page

    def load_wallpapers(self, category):
        self.current_wp_filter = category
        wp_dir = os.path.expanduser("~/Pictures/wallpapers")
        if not os.path.exists(wp_dir): return
        
        valid_img = ('png', 'jpg', 'jpeg', 'webp')
        valid_vid = ('mp4', 'webm', 'gif')
        
        filtered = []
        for f in sorted(os.listdir(wp_dir)):
            path = os.path.join(wp_dir, f)
            is_dir = os.path.isdir(path)
            ext = path.lower().split('.')[-1] if not is_dir else ""
            
            if category == "all": filtered.append(path)
            elif category == "dynamic" and is_dir: filtered.append(path)
            elif category == "videos" and ext in valid_vid: filtered.append(path)
            elif category == "images" and ext in valid_img: filtered.append(path)
            
        self.wp_model.update_items(filtered)
        
        # Оновлення стилів кнопок фільтра
        for f_type, btn in self.wp_filter_btns.items():
            if f_type == category:
                btn.setStyleSheet(f"QPushButton {{ background-color: {self.accent_color}; color: white; border-radius: 8px; padding: 6px 12px; font-weight: bold; border: none; }}")
            else:
                btn.setStyleSheet("QPushButton { background-color: rgba(100, 100, 100, 0.2); color: #aaaaaa; border-radius: 8px; padding: 6px 12px; font-weight: bold; border: none; } QPushButton:hover { background-color: rgba(150, 150, 150, 0.3); color: white; }")

    def on_wallpaper_selected(self, index):
        path = self.wp_model.items[index.row()]
        # Запускаємо зміну шпалер у фоні, щоб не фрізити UI на час роботи ffmpeg/wal
        threading.Thread(target=self.apply_actual_wallpaper, args=(path,), daemon=True).start()

    def apply_actual_wallpaper(self, path):
        import datetime
        LAST_WALLPAPER_FILE = os.path.expanduser("~/.cache/ignis/last_wallpaper.txt")
        
        # Логіка для динамічних
        target_path = path
        if os.path.isdir(path):
            hour = datetime.datetime.now().hour
            period = "morning" if 6 <= hour < 12 else "day" if 12 <= hour < 18 else "evening" if 18 <= hour < 22 else "night"
            for ext in ['jpg', 'png', 'jpeg', 'webp', 'mp4', 'webm', 'gif']:
                test = os.path.join(path, f"{period}.{ext}")
                if os.path.exists(test): 
                    target_path = test
                    break

        with open(LAST_WALLPAPER_FILE, "w") as f: f.write(path)
        
        ext = target_path.lower().split('.')[-1]
        is_video = ext in ['mp4', 'webm', 'gif']
        
        if is_video:
            subprocess.run("pkill -9 mpvpaper; killall awww-daemon; rm -f /tmp/mpv-socket", shell=True, stderr=subprocess.DEVNULL)
            anime4k_enabled = self.settings.get("wp_anime4k", False)
            # Оптимізована команда для мінімального споживання RAM:
            # - profile=wallpaper: легші M-шейдери, 30fps, cache=no, idle priority
            # - config=yes: дозволяємо mpv.conf, але задаємо профіль явно
            # - nvdec: вся декодування на GPU, CPU не навантажений
            # - demuxer-max-bytes=512KiB: буфер в RAM мінімальний
            if anime4k_enabled:
                mpv_opts = (
                    "profile=wallpaper "
                    "input-ipc-server=/tmp/mpv-socket "
                    "loop-file=inf "
                    "hwdec=nvdec "
                    "no-audio "
                    "panscan=1.0 "
                    "cache=no "
                    "demuxer-max-bytes=512KiB "
                    "demuxer-max-back-bytes=128KiB"
                )
            else:
                mpv_opts = (
                    "config=no "
                    "glsl-shaders= "
                    "input-ipc-server=/tmp/mpv-socket "
                    "loop-file=inf "
                    "hwdec=nvdec "
                    "no-audio "
                    "panscan=1.0 "
                    "cache=no "
                    "demuxer-max-bytes=512KiB "
                    "demuxer-max-back-bytes=128KiB"
                )
            # nice -n 19: знижуємо пріоритет CPU до мінімального (ігри/робота мають перевагу)
            os.system(f"nohup nice -n 19 mpvpaper -o '{mpv_opts}' '*' '{target_path}' >/dev/null 2>&1 &")
            safe_name = os.path.basename(target_path).replace(" ", "_")
            frame_target = os.path.expanduser(f"~/.cache/ignis/sddm_frame_{safe_name}.png")
            subprocess.run(f"ffmpeg -i '{target_path}' -ss 00:00:00.000 -vframes 1 '{frame_target}' -y", shell=True, stderr=subprocess.DEVNULL)
            wal_target = frame_target
        else:
            subprocess.run("pkill -9 mpvpaper; pgrep awww-daemon || (awww-daemon & sleep 0.5)", shell=True, stderr=subprocess.DEVNULL)
            subprocess.Popen(f"awww img '{target_path}' --resize crop --transition-type fade --transition-angle 30 --transition-fps 60 --transition-duration 2", shell=True)
            wal_target = target_path

        # Генеруємо кольори та рестартимо Waybar
        subprocess.run(f"wal -q -t -i '{wal_target}'", shell=True)
        subprocess.run("killall -SIGUSR2 waybar", shell=True)
        subprocess.Popen("sudo /home/doki/.config/megumin-os/sync_sddm.sh", shell=True)
        subprocess.Popen("ignis reload", shell=True) # Перезапуск Ignis для підтягування кольорів    

    def create_network_page(self):
        page, layout = self.create_page_base("Мережа")
        rows = [
            SettingRow("network-wireless", "󰤨", "Wi-Fi", "Керування модулем", MeguminSwitch("wifi_enabled", self.settings["wifi_enabled"], self.save_setting, self.get_current_accent)),
            SettingRow("bluetooth-active", "󰂯", "Bluetooth", "Живлення", MeguminSwitch("bluetooth_enabled", self.settings["bluetooth_enabled"], self.save_setting, self.get_current_accent), is_last=True)
        ]
        layout.addWidget(self.create_card(rows))
        return page

    def create_display_page(self):
        page, layout = self.create_page_base("Екран")
        rows = [
            SettingRow("video-display", "󰍹", "Дисплеї", "Відкрити wdisplays", on_click=lambda: self.launch_app("wdisplays")),
            SettingRow("weather-clear-night", "󰽠", "Нічне світло", "Фільтр синього", MeguminSwitch("night_light", self.settings["night_light"], self.save_setting, self.get_current_accent), is_last=True)
        ]
        layout.addWidget(self.create_card(rows))
        return page

    def create_audio_page(self):
        page, layout = self.create_page_base("Звук")
        rows = [SettingRow("audio-volume-high", "󰕾", "Мікшер", "Pavucontrol", is_last=True, on_click=lambda: self.launch_app("pavucontrol"))]
        layout.addWidget(self.create_card(rows))
        return page

    def create_system_page(self):
        page, layout = self.create_page_base("Система")
        rows = [
            SettingRow("utilities-system-monitor", "󰒋", "Монітор", "btop", on_click=lambda: self.launch_app("kitty -e btop")),
            SettingRow("system-software-update", "󰚰", "Оновлення", "yay", on_click=lambda: self.launch_app("kitty -e yay")),
            SettingRow("utilities-terminal", "🚀", "Рестарт Ignis", "Підняти оболонку після killall", on_click=lambda: self.launch_app("ignis init")),
            # Прибрали is_last=True звідси
            SettingRow("battery", "󰾆", "Режим живлення", "Профіль процесора", PowerProfileSelector(self.settings.get("power_profile", "balanced"), self.save_setting, self.get_current_accent)),
            
            # НОВА КНОПКА ДЛЯ ПЕРЕЗАПУСКУ C++ МОДУЛІВ
            # Використовуємо твою функцію launch_app для виконання shell-команд
            SettingRow("system-reboot", "🔄", "Рестарт C++ Модулів", "Перезапустити всі панелі SYSui", is_last=True, on_click=lambda: self.launch_app("bash ~/.config/SYSui/restart_all.sh"))
        ]
        layout.addWidget(self.create_card(rows))
        return page

    def create_dock_page(self):
        page, layout = self.create_page_base("Налаштування Док-панелі")
        rows = [
            SettingRow("utilities-terminal", "🛠", "Список винятків", "Док не ховається, якщо ці застосунки активні", DockWhitelistEditor(self.settings.get("dock_whitelist", ""), self.save_setting, self.get_current_accent)),
            SettingRow("system-reboot", "🔄", "Перезапустити Док", "Застосувати нові налаштування", is_last=True, on_click=lambda: self.launch_app("bash ~/.config/SYSui/restart_dock.sh"))
        ]
        layout.addWidget(self.create_card(rows))
        return page

    def create_panels_page(self):
        page, layout = self.create_page_base("Панелі та віджети")
        g = lambda k: self.settings.get(k, PANEL_DEFAULTS[k])
        sw = lambda k: MeguminSwitch(k, bool(g(k)), self.save_setting, self.get_current_accent)
        sl = lambda k, lo, hi, suf="%": MeguminSlider(k, g(k), lo, hi, suf, self.save_setting, self.get_current_accent)

        content = QWidget()
        cl = QVBoxLayout(content)
        cl.setContentsMargins(0, 0, 0, 0)
        cl.setSpacing(18)

        def section(title):
            lbl = QLabel(title)
            lbl.setStyleSheet("color: #888888; font-size: 12px; font-weight: bold; background: transparent; margin-left: 4px;")
            cl.addWidget(lbl)

        self.accent_wal_switch = MeguminSwitch("accent_mode", g("accent_mode") != "custom", self.set_accent_from_wal, self.get_current_accent)
        section("ЗАГАЛЬНЕ")
        cl.addWidget(self.create_card([
            SettingRow("preferences-desktop-effects", "✨", "Живі анімації", "Пружинні наведення, поява панелей, збільшення", sw("animations")),
            SettingRow("preferences-desktop-color", "🎨", "Акцент із шпалер", "Колір усіх панелей береться з pywal", self.accent_wal_switch),
            SettingRow("color-select", "🖌", "Свій акцентний колір", "Застосовується до всіх модулів одразу", is_last=True, on_click=self.pick_accent_color),
        ]))

        section("ТОПБАР")
        cl.addWidget(self.create_card([
            SettingRow("view-dual", "▭", "Стиль", "Бульбашки, бульбашки з крапками замість цифр, або суцільна плашка як у macOS",
                       SegmentedSelector("topbar_style", [("bubbles", "Бульбашки"), ("dots", "Крапки"), ("macos", "macOS")], g("topbar_style"), self.save_setting, self.get_current_accent)),
            SettingRow("input-keyboard", "⌨", "Мова", "Як показувати розкладку клавіатури",
                       SegmentedSelector("topbar_lang", [("flag", "Прапорець"), ("text", "Букви"), ("icon", "Іконка")], g("topbar_lang"), self.save_setting, self.get_current_accent)),
            SettingRow("weather-fog", "◐", "Прозорість фону", "0% — повністю прозорий", sl("topbar_opacity", 0, 100)),
            SettingRow("draw-rectangle", "▢", "Рамки", "Тонкий контур бульбашок / нижній край плашки", sw("topbar_border"), is_last=True),
        ]))

        section("ДОК")
        cl.addWidget(self.create_card([
            SettingRow("weather-fog", "◐", "Прозорість фону", "0% — повністю прозорий", sl("dock_opacity", 0, 100)),
            SettingRow("draw-rectangle", "▢", "Рамка", "Контур доку", sw("dock_border")),
            SettingRow("format-stroke-color", "◇", "Стиль рамки", "Тонка — як у топбару та Control Center; акцентна — товста кольорова",
                       SegmentedSelector("dock_border_style", [("subtle", "Тонка"), ("accent", "Акцентна")], g("dock_border_style"), self.save_setting, self.get_current_accent)),
            SettingRow("format-stroke-color", "▢", "Прозорість рамки", "", sl("dock_border_opacity", 0, 100)),
            SettingRow("zoom-in", "🔍", "Збільшення іконок", "Як у macOS — іконки ростуть під курсором", sw("dock_magnify")),
            SettingRow("zoom-fit-best", "↕", "Сила збільшення", "Максимальний розмір іконки під курсором", sl("dock_magnify_pct", 110, 220)),
            SettingRow("preferences-desktop-icons", "▦", "Розмір іконок", "", sl("dock_icon_size", 28, 56, " px"), is_last=True),
        ]))

        section("ДОК · НЕДАВНІ ФАЙЛИ")
        cl.addWidget(self.create_card([
            SettingRow("folder-download", "⬇", "Показувати недавні файли", "Біля індикатора екрана: ЛКМ — відкрити, СКМ — у теці, можна перетягнути", sw("dock_files")),
            SettingRow("edit-paste", "📋", "Джерело", "Завантажені (~/Downloads), скопійовані у файловому менеджері, або обидва",
                       SegmentedSelector("dock_files_source", [("downloads", "Завантаження"), ("clipboard", "Скопійовані"), ("both", "Обидва")],
                                         g("dock_files_source"), self.save_setting, self.get_current_accent)),
            SettingRow("view-list-icons", "#", "Кількість", "", sl("dock_files_count", 1, 8, "")),
            SettingRow("document-open-recent", "🕑", "Завантаження не старші за", "Старші файли з ~/Downloads не показуються", sl("dock_files_days", 1, 30, " дн"), is_last=True),
        ]))

        section("CONTROL CENTER")
        cl.addWidget(self.create_card([
            SettingRow("weather-fog", "◐", "Прозорість панелі", "", sl("cc_opacity", 30, 100)),
            SettingRow("security-high", "🛡", "Сторож модулів", "Перезапускати модулі SYSui, якщо вони впали", sw("watchdog")),
            SettingRow("dialog-warning", "⚠", "Системні сповіщення", "Спливаючі попередження про збої та помилки", sw("sys_popups"), is_last=True),
        ]))
        cl.addStretch()

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setStyleSheet("QScrollArea { background: transparent; border: none; } QScrollArea > QWidget > QWidget { background: transparent; }"
                             "QScrollBar:vertical { width: 6px; background: transparent; } QScrollBar::handle:vertical { background: rgba(150,150,150,0.4); border-radius: 3px; }"
                             "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }")
        scroll.setWidget(content)
        layout.addWidget(scroll)
        return page

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton: self.oldPos = event.globalPosition().toPoint()
    def mouseMoveEvent(self, event):
        if self.oldPos: self.move(self.pos() + event.globalPosition().toPoint() - self.oldPos); self.oldPos = event.globalPosition().toPoint()
    def mouseReleaseEvent(self, event): self.oldPos = None

if __name__ == '__main__':
    from PyQt6.QtWidgets import QListWidgetItem
    app = QApplication(sys.argv)
    app.setDesktopFileName("megumin-settings.desktop")
    app.setApplicationName("megumin-settings")
    
    ex = MeguminSettings()
    # settings.py --page 7  — відкрити одразу на потрібній сторінці (7 = «Панелі та віджети»)
    if "--page" in sys.argv:
        try: ex.nav_list.setCurrentRow(int(sys.argv[sys.argv.index("--page") + 1]))
        except (ValueError, IndexError): pass
    ex.show()
    sys.exit(app.exec())