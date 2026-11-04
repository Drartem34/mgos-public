#!/bin/bash
# Перезапускає всі модулі SYSui (без перезбірки).
# Спершу попереджає сторож Control Center, що це плановий перезапуск, а не збій.
DIR="$(dirname "$(readlink -f "$0")")"
echo "expect|all" | socat - UNIX-CLIENT:"$DIR/cc.sock" 2>/dev/null

for m in dock topbar runner_osd clipboard_osd notify_osd megumin_osd powermenu_osd volume_osd about_osd cc_osd; do
    pkill -x "$m" 2>/dev/null
    sleep 0.2
    hyprctl dispatch exec "$DIR/$m" >/dev/null
done
# шпалери піднімає wallpapers_watch.sh
pkill -x wallpapers 2>/dev/null
echo "SYSui перезапущено"
