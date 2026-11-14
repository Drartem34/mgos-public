#!/bin/bash
# Збирає всі модулі SYSui. Використання: ./build.sh [модуль...]   (без аргументів — усі)
# Після збірки: ./build.sh --restart  — перезапускає зібрані модулі.
set -e
cd "$(dirname "$(readlink -f "$0")")"
# osd_hub — один процес для гучності, яскравості, сповіщень і музики (volume_osd/brightness_osd/notify_osd/osd.cpp)
declare -A MAP=( [dock]=dock [topbar]=topbar [runner]=runner_osd [wallpapers]=wallpapers
                 [cc]=cc_osd [clipboard_osd]=clipboard_osd [osd_hub]=osd_hub
                 [powermenu_osd]=powermenu_osd [controller_addon]=controller_addon [about_osd]=about_osd
                 [pixel_wallpaper]=pixel_wallpaper [welcome]=welcome )
RESTART=0; ARGS=()
for a in "$@"; do [ "$a" = "--restart" ] && RESTART=1 || ARGS+=("$a"); done
# mgos-pageout (C, без GTK) — «стискання» схованих програм у zram; ставить mgos-zram setup з cap_sys_nice
PO=0; NEW=()
for a in "${ARGS[@]}"; do [ "$a" = pageout ] && PO=1 || NEW+=("$a"); done
if [ ${#ARGS[@]} -eq 0 ] || [ $PO -eq 1 ]; then gcc -O2 -s -Wall mgos_pageout.c -o mgos-pageout && echo "✓ mgos-pageout"; fi
[ $PO -eq 1 ] && [ ${#NEW[@]} -eq 0 ] && exit 0
ARGS=("${NEW[@]}")
[ ${#ARGS[@]} -eq 0 ] && ARGS=("${!MAP[@]}")
FLAGS="$(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0 gio-unix-2.0)"
pids=()
for src in "${ARGS[@]}"; do
    out=${MAP[$src]}; [ -z "$out" ] && { echo "невідомий модуль: $src"; exit 1; }
    OPT=-O2; [ "$src" = pixel_wallpaper ] && OPT="-O3 -ffast-math"   # шпалери рахують десятки тисяч пікселів щокадру
    ( g++ -std=c++17 $OPT -s "$src.cpp" -o "$out.new" $FLAGS && echo "✓ $out" ) & pids+=($!)
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
        # wallpapers перезапускає wallpapers_watch.sh сам; pixel_wallpaper — лише якщо це поточні шпалери
        [ "$out" = welcome ] && continue   # привітання запускається лише на першому вході
        [ "$out" = pixel_wallpaper ] && ! grep -q '^pixel:' ~/.cache/ignis/last_wallpaper.txt 2>/dev/null && continue
        [ "$out" != wallpapers ] && { sleep 0.3; hyprctl dispatch exec "$PWD/$out" >/dev/null; }
    done
    echo "перезапущено"
fi
