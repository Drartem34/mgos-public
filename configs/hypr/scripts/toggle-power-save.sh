#!/usr/bin/env bash
# Шлях: ~/.config/hypr/scripts/toggle-power-save.sh

STATE_FILE="/tmp/hypr_power_save.state"

apply_normal() {
    # Повертаємо красу з твого hyprland.hl
    hyprctl keyword decoration:rounding 15
    hyprctl keyword decoration:blur:enabled true
    hyprctl keyword decoration:shadow:enabled true
    hyprctl keyword animations:enabled true
    hyprctl keyword misc:vfr true
    
    echo "normal" > "$STATE_FILE"
    notify-send "⚡ Режим продуктивності" "Анімації та блюр увімкнено" --icon=preferences-desktop-display
}

apply_power_save() {
    # Вирубаємо все нафіг для економії
    hyprctl keyword decoration:rounding 0
    hyprctl keyword decoration:blur:enabled false
    hyprctl keyword decoration:shadow:enabled false
    hyprctl keyword animations:enabled false
    hyprctl keyword misc:vfr true # Залишаємо VFR увімкненим для економії
    
    echo "power_save" > "$STATE_FILE"
    notify-send "🔋 Режим економії" "Анімації та блюр вимкнено" --icon=battery-low
}

# Логіка перемикання
if [ -f "$STATE_FILE" ] && [ "$(cat "$STATE_FILE")" = "power_save" ]; then
    apply_normal
else
    apply_power_save
fi