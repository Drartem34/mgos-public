#!/bin/bash

# Отримуємо рівень яскравості
brightness=$(brightnessctl get)
max_brightness=$(brightnessctl max)
brightness_percent=$((brightness * 100 / max_brightness))

# Визначаємо колір візуалізації
if [[ $brightness_percent -ge 80 ]]; then
    bar_color="#fa1955"  # Червоний при >= 80%
elif [[ $brightness_percent -le 25 ]]; then
    bar_color="#8affb6"  # Зелений при <= 25%
else
    bar_color="#ffffff"  # Білий в інших випадках
fi

# Створюємо візуалізацію у вигляді лінії
bar=""
filled=$((brightness_percent / 10))  # Заповнені сегменти (0-10)
empty=$((10 - filled))               # Порожні сегменти

for ((i = 0; i < filled; i++)); do
    bar+="█"
done
for ((i = 0; i < empty; i++)); do
    bar+="─"
done

# Відправляємо повідомлення у dunst
notify-send -u low -h string:x-dunst-stack-tag:brightness -i display-brightness-high \
"󰃠 Brightness: $brightness_percent%" "<span color='$bar_color'>$bar</span>"
