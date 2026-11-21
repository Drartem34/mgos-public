#!/usr/bin/env bash

volume=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ | awk '{print int($2 * 100)}')
muted=$(wpctl get-mute @DEFAULT_AUDIO_SINK@)

if [[ "$muted" == "Muted" ]]; then
    notify-send -u low -h string:x-dunst-stack-tag:volume -i audio-volume-muted \
    " Muted" "<span color='#888888'>░░░░░\n░░░░░</span>" # Dark Grey
    exit 0
fi

# Визначаємо градієнтний колір
if [[ $volume -le 35 ]]; then
    bar_color="#8affb6"  # М'який зелений
elif [[ $volume -le 70 ]]; then
    bar_color="#aadfff"  # Блакитний
else
    bar_color="#fa1955"  # Червоний (Критичний)
fi

filled=$((volume / 10))
empty=$((10 - filled))
bar=""

for ((i = 0; i < filled; i++)); do
    bar+="█"
done
for ((i = 0; i < empty; i++)); do
    bar+="░"
done

bar_top=${bar:0:5}
bar_bot=${bar:5:5}

notify-send -u low -h string:x-dunst-stack-tag:volume -i audio-volume-high \
"󰕾 Volume: $volume%" "<span color='$bar_color'>$bar_top\n$bar_bot</span>"