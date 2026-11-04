#!/usr/bin/env bash
# ============================================================================
#  Megumin OS 6 — графічний інсталятор (встановлення · відновлення · оновлення)
#  ./install-gui.sh            — запуск (доставить gtk4 і збере інсталятор, якщо треба)
#  ./install-gui.sh --kiosk    — одразу встановлення kioskOS
#  ./install.sh [--kiosk]      — класичний текстовий (BIOS-style) інсталятор
#  gui/selfupdate.sh check|apply — оновлення самого інсталятора з GitHub (у GUI — банер на головній)
# ============================================================================
cd "$(dirname "$(readlink -f "$0")")"
BIN=gui/mgos-installer
# ярлик «mgOS Installer» у списку програм (шлях оновлюється, якщо теку перенесли)
bash gui/register-app.sh 2>/dev/null || true
# ./install-gui.sh --kiosk — одразу сторінка kioskOS (у TTY — текстовий інсталятор kioskOS)
KIOSK_ARG=()
if [ "${1:-}" = "--kiosk" ]; then KIOSK_ARG=(--kiosk); export MGOS_DEMO_PAGE=kiosk; shift; fi

if [ -z "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    echo "Графічної сесії немає (TTY) — запускаю текстовий інсталятор."
    exec ./install.sh "${KIOSK_ARG[@]}"
fi

# Для GUI потрібні gtk4 (+ g++ для збірки під цю систему) і шрифт з іконками
need=()
for p in gtk4 ttf-jetbrains-mono-nerd; do pacman -Qq "$p" &>/dev/null || need+=("$p"); done
# бінарник не в git: збираємо, якщо його немає, бібліотек бракує або код новіший (після оновлення з git)
if [ ! -x "$BIN" ] || [ gui/mgos_installer.cpp -nt "$BIN" ] || ldd "$BIN" 2>/dev/null | grep -q 'not found'; then
    for p in base-devel pkgconf; do pacman -Qq "$p" &>/dev/null || need+=("$p"); done
    REBUILD=1
fi
if [ ${#need[@]} -gt 0 ]; then
    echo "Потрібно для графічного інсталятора: ${need[*]}"
    sudo pacman -S --needed --noconfirm "${need[@]}" || { echo "Не вдалося — запускаю текстовий інсталятор."; exec ./install.sh "${KIOSK_ARG[@]}"; }
fi
if [ "${REBUILD:-0}" = 1 ]; then bash gui/build.sh || { echo "Збірка не вдалася — текстовий інсталятор."; exec ./install.sh "${KIOSK_ARG[@]}"; }; fi

chmod +x gui/steps.sh 2>/dev/null
exec "$BIN" "$@"
