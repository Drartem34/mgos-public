#!/bin/bash
# Чисто перезапускає dock без залишків layer-shell вікон
DIR="$(dirname "$(readlink -f "$0")")"

# Вбиваємо ВСІ інстанси dock (і dock_new якщо є)
killall -9 dock dock_new 2>/dev/null
sleep 0.4

# Запускаємо свіжий (повністю відв'язуємо від терміналу)
setsid "$DIR/dock" >/dev/null 2>&1 &
echo "Dock restarted"
