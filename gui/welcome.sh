#!/usr/bin/env bash
# Перший вхід після встановлення: вікно «Все готово до роботи».
# Інсталятор після успішного встановлення лишає ~/.local/state/mgos-installer/welcome-pending;
# Hyprland запускає цей скрипт з mgos-auto.conf (exec-once) — вікно з'являється один раз.
M=${XDG_STATE_HOME:-$HOME/.local/state}/mgos-installer/welcome-pending
[ -f "$M" ] || exit 0
kind=$(head -1 "$M" 2>/dev/null); rm -f "$M"
cd "$(dirname "$(readlink -f "$0")")/.." || exit 0
sleep 3   # дати панелям і шпалерам з'явитися
BIN=gui/mgos-installer
[ -x "$BIN" ] || exit 0
MGOS_WELCOME=${kind:-mgos} exec "$BIN"
