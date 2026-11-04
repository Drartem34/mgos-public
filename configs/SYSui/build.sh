#!/bin/bash
# Збирає всі модулі SYSui. Використання: ./build.sh [модуль...]   (без аргументів — усі)
# Після збірки: ./build.sh --restart  — перезапускає зібрані модулі.
set -e
cd "$(dirname "$(readlink -f "$0")")"
declare -A MAP=( [dock]=dock [topbar]=topbar [runner]=runner_osd [wallpapers]=wallpapers
                 [cc]=cc_osd [clipboard_osd]=clipboard_osd [notify_osd]=notify_osd [osd]=megumin_osd
                 [powermenu_osd]=powermenu_osd [volume_osd]=volume_osd [controller_addon]=controller_addon [about_osd]=about_osd )
RESTART=0; ARGS=()
for a in "$@"; do [ "$a" = "--restart" ] && RESTART=1 || ARGS+=("$a"); done
[ ${#ARGS[@]} -eq 0 ] && ARGS=("${!MAP[@]}")
FLAGS="$(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0)"
pids=()
for src in "${ARGS[@]}"; do
    out=${MAP[$src]}; [ -z "$out" ] && { echo "невідомий модуль: $src"; exit 1; }
    ( g++ -std=c++17 -O2 -s "$src.cpp" -o "$out.new" $FLAGS && echo "✓ $out" ) & pids+=($!)
done
fail=0; for p in "${pids[@]}"; do wait $p || fail=1; done
[ $fail -ne 0 ] && { echo "✗ збірка впала, старі бінарники не чіпаю"; rm -f *.new; exit 1; }
for src in "${ARGS[@]}"; do out=${MAP[$src]}; mv -f "$out.new" "$out"; done
if [ $RESTART -eq 1 ]; then
    # попереджаємо сторож Control Center, що це плановий перезапуск, а не збій
    for src in "${ARGS[@]}"; do echo "expect|${MAP[$src]}" | socat - UNIX-CLIENT:"$PWD/cc.sock" 2>/dev/null || true; done
    for src in "${ARGS[@]}"; do
        out=${MAP[$src]}
        pkill -9 -x "$out" 2>/dev/null || true
        # wallpapers перезапускає wallpapers_watch.sh сам
        [ "$out" != wallpapers ] && { sleep 0.3; hyprctl dispatch exec "$PWD/$out" >/dev/null; }
    done
    echo "перезапущено"
fi
