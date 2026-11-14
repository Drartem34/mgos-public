#!/usr/bin/env bash
# Шлях: ~/.config/hypr/scripts/toggle-power-save.sh

STATE_FILE="/tmp/hypr_power_save.state"

# «Економ ефекти» — спільний прапорець із Control Center (settings.json → eco_effects):
# topbar керує блюром (повноекранний режим) і не вмикатиме його назад, поки прапорець стоїть.
set_eco_flag() {
    python3 -I - "$1" <<'PYEOF'
import json, os, sys
p = os.path.expanduser("~/.config/megumin-os/settings.json")
try: d = json.load(open(p))
except Exception: d = {}
d["eco_effects"] = sys.argv[1] == "1"
open(p + ".tmp", "w").write(json.dumps(d, indent=4, ensure_ascii=False)); os.replace(p + ".tmp", p)
PYEOF
}

apply_normal() {
    # Повертаємо красу з твого hyprland.hl
    set_eco_flag 0
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
    set_eco_flag 1
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