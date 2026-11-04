#!/usr/bin/env bash
# Ярлик «mgOS Installer» у списку програм (Runner, wofi, меню). Викликається з install-gui.sh
# і install.sh при кожному запуску: якщо теку інсталятора перенесли — шлях оновиться сам.
#   gui/register-app.sh           — створити/оновити
#   gui/register-app.sh --remove  — прибрати з меню
DIR=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
APP=com.megumin.installer          # = app_id вікна GTK — док і панелі знаходять іконку
DESK=~/.local/share/applications/$APP.desktop
ICON=~/.local/share/icons/hicolor/scalable/apps/$APP.svg

if [ "${1:-}" = --remove ]; then rm -f "$DESK" "$ICON"; update-desktop-database ~/.local/share/applications 2>/dev/null; exit 0; fi

# стабільний шлях до інсталятора для exec-once у mgos-auto.conf (вікно привітання, сповіщення про оновлення)
mkdir -p ~/.local/share/mgos && ln -sfn "$DIR" ~/.local/share/mgos/installer
mkdir -p "$(dirname "$DESK")" "$(dirname "$ICON")"
cmp -s "$DIR/gui/icon.svg" "$ICON" 2>/dev/null || cp "$DIR/gui/icon.svg" "$ICON"
q=$(printf '%s' "$DIR/install-gui.sh" | sed 's/["`$\\]/\\&/g')
new=$(cat <<DESKTOP
[Desktop Entry]
Type=Application
Name=mgOS Installer
GenericName=Інсталятор Megumin OS
Comment=Встановити, відновити, оновити або перевірити mgOS і kioskOS
Exec="$q"
Path=$DIR
Icon=$APP
Terminal=false
Categories=System;
Keywords=mgos;megumin;installer;kiosk;update;інсталятор;оновлення;
StartupWMClass=$APP
StartupNotify=true
Actions=kiosk;tui;

[Desktop Action kiosk]
Name=Встановити kioskOS
Exec="$q" --kiosk

[Desktop Action tui]
Name=Текстовий інсталятор (термінал)
Exec=kitty -e "$(printf '%s' "$DIR/install.sh" | sed 's/["`$\\]/\\&/g')"
DESKTOP
)
if [ "$(cat "$DESK" 2>/dev/null)" != "$new" ]; then
    printf '%s\n' "$new" > "$DESK"
    update-desktop-database ~/.local/share/applications 2>/dev/null || true
fi
exit 0
