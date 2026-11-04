#!/usr/bin/env bash

# Затримка 0.1 сек, щоб оновилась мета-інформація
sleep 0.1

# Отримуємо статус, назву треку, виконавця та обкладинку
player_status=$(playerctl status 2>/dev/null)
title=$(playerctl metadata title 2>/dev/null)
artist=$(playerctl metadata artist 2>/dev/null)
album_art=$(playerctl metadata mpris:artUrl 2>/dev/null)

# Якщо немає треку – виходимо (не показуємо сповіщення)
if [[ -z "$title" ]]; then
    exit 0
fi

# Формуємо рядок сповіщення
if [[ -z "$artist" ]]; then
    track_info="$title"
else
    track_info="$artist - $title"
fi

# Вибір іконки для статусу
if [[ "$player_status" == "Playing" ]]; then
    music_icon=" Playing" # Пауза (Pause)
elif [[ "$player_status" == "Paused" ]]; then
    music_icon=" Paused" # Плей (Play)
else
    exit 0 # Невідомий статус
fi

# Якщо немає картинки альбому – використовуємо стандартну
if [[ -z "$album_art" ]]; then
    # Тут має бути шлях до твого стандартного svg
    album_art="/usr/share/icons/Papirus/64x64/categories/multimedia.svg" 
fi

# Оновлюємо сповіщення з картинкою
notify-send -u low -h string:x-dunst-stack-tag:music -i "$album_art" \
"🎵 $music_icon" "$track_info"