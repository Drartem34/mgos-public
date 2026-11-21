#!/bin/bash

OPTIONS="Вимкнення\nПерезавантаження\nВихід\nСкасувати"

CHOICE=$(echo -e "$OPTIONS" | wofi --show dmenu -p "Дії з системою:" -i)

case "$CHOICE" in
    Вимкнення)
        systemctl poweroff
        ;;
    Перезавантаження)
        systemctl reboot
        ;;
    Вихід)
        wlogout
        ;;
    Скасувати)
        exit 0
        ;;
esac
