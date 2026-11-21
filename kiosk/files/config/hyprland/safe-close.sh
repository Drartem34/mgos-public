#!/usr/bin/env bash

# Отримуємо клас активного вікна
ACTIVE_CLASS=$(hyprctl activewindow -j 2>/dev/null | jq -r '.class // empty' | tr '[:upper:]' '[:lower:]')

# Список класів браузерів (firefox, chrome, chromium, brave тощо)
case "$ACTIVE_CLASS" in
    *firefox*|*chromium*|*chrome*|*brave*|*zen*|*opera*|*vivaldi*|*librewolf*|*floorp*|*waterfox*|*tor-browser*|*edge*)
        notify-send -u low -t 1500 -a "kioskOS" "Браузер захищено" "Щоб закрити браузер, натисніть Super + Shift + Q"
        ;;
    *)
        hyprctl dispatch killactive
        ;;
esac
