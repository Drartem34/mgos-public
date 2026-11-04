#!/bin/bash

APPNAME="$1"
SUMMARY="$2" # Ім'я чувака
BODY="$3"    # Текст
ICON="$4"    # Шлях до картинки (якого нема)

# Якщо Телега чи інша прога не дала аватарку (ICON пустий)
if [ -z "$ICON" ] || [ "$ICON" == " " ]; then
    # Дивимось по назві проги і підставляємо системний логотип
    if [[ "${APPNAME,,}" == *"telegram"* ]]; then
        ICON="telegram"
    elif [[ "${APPNAME,,}" == *"discord"* ]]; then
        ICON="discord"
    else
        ICON="dialog-information" # Дефолтна іконка для інших
    fi
fi

# Пишемо у файл для нашої плашки
echo "SENDER=$SUMMARY" > /tmp/notify_osd_data
echo "ICON=$ICON" >> /tmp/notify_osd_data

if [ -z "$BODY" ]; then
    echo "BODY=" >> /tmp/notify_osd_data
else
    CLEAN_BODY=$(echo "$BODY" | sed -e 's/<[^>]*>//g' | tr '\n' ' ')
    echo "BODY=$CLEAN_BODY" >> /tmp/notify_osd_data
fi

# НОВА МАГІЯ СОКЕТІВ ЗАМІСТЬ KILLALL
echo "show" | socat - UNIX-CLIENT:/home/doki/.config/SYSui/notify.sock
dunstctl close

echo "notify|$2|$3" | socat - UNIX-CONNECT:$HOME/.config/SYSui/cc.sock &