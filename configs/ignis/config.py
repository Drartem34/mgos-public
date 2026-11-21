# ignis у MGos 6.5 — тільки Alt+Tab (switcher.py).
# Решту замінив SYSui: Control Center (cc_osd), шпалери (wallpapers), сповіщення (dunst/osd_hub), about (about_osd).
# Налаштування (settings.py) — окремий PyQt-застосунок, ignis для нього не потрібен.
# Стара повна версія: ~/.config/ignis.bak-2026-11-21
import os
import sys

sys.path.append(os.path.expanduser("~/.config/ignis"))

from ignis.app import IgnisApp
import switcher

switcher.setup(IgnisApp.get_initialized())
