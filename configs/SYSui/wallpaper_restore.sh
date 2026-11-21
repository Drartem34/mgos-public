#!/bin/bash
# ============================================================================
#  Відновлення шпалер при вході в mgOS — ОДИН бекенд, без «спершу картинка, потім відео».
#  Останні шпалери — відео → mpvpaper; картинка → awww.
#
#  Щоб після SDDM не було секунди порожнього (синього) фону, поки mpvpaper/awww
#  піднімаються: спершу МИТТЄВО показуємо постер (нерухомий кадр тих самих шпалер)
#  через swaybg, а коли живі шпалери намалювались поверх — прибираємо swaybg.
#  Вибір нових шпалер робить SYSui/wallpapers (Super+W) або «Налаштування» → Шпалери.
# ============================================================================
SETTINGS="$HOME/.config/megumin-os/settings.json"
POSTER_TOOL="$HOME/.local/bin/mgos-wp-poster"

# 1) постер — одразу (готовий з кешу; якщо застарів — темний фон, а постер дороблюється на потім)
poster=$("$POSTER_TOOL" --cached 2>/dev/null)
if [ -n "$poster" ] && [ -f "$poster" ]; then
    swaybg -m fill -i "$poster" >/dev/null 2>&1 &
else
    swaybg -c "#000000" >/dev/null 2>&1 &
    ( nice -n 19 "$POSTER_TOOL" >/dev/null 2>&1 & )
fi
SWAYBG=$!

p=$("$POSTER_TOOL" --source 2>/dev/null)

# прибрати swaybg, коли живі шпалери вже перекрили його (і закінчилось їх проявлення)
drop_poster() { sleep 1.2; kill "$SWAYBG" 2>/dev/null; }

# піксельні живі шпалери (Налаштування → Шпалери): last_wallpaper.txt = pixel:<стиль>
if [[ $(head -1 "$HOME/.cache/ignis/last_wallpaper.txt" 2>/dev/null) == pixel:* ]]; then
    pkill -x awww-daemon 2>/dev/null; pkill -x mpvpaper 2>/dev/null
    "$HOME/.config/SYSui/pixel_wallpaper" >/dev/null 2>&1 &
    drop_poster
    exit 0
fi

ext=${p##*.}; ext=${ext,,}
case "$ext" in
    mp4|webm|gif|mkv|mov)
        [ -f "$p" ] || exit 0
        pkill -x awww-daemon 2>/dev/null
        rm -f /tmp/mpv-socket
        if grep -Eq '"wp_anime4k"[[:space:]]*:[[:space:]]*true' "$SETTINGS" 2>/dev/null; then
            opts="profile=wallpaper"
        else
            opts="no-config glsl-shaders="
        fi
        opts="$opts input-ipc-server=/tmp/mpv-socket loop-file=inf hwdec=auto-safe no-audio panscan=1.0 cache=no demuxer-max-bytes=512KiB demuxer-max-back-bytes=128KiB"
        # mgos-power у режимі батареї тримає відео на паузі — стартуємо одразу на паузі
        [ -e "${XDG_RUNTIME_DIR:-/run/user/$UID}/mgos-power/battery" ] && opts="$opts pause"
        # легка копія (≤ роздільності монітора, 30 к/с, HEVC на NVENC) — mgos-wp-optimize; поки її немає — оригінал.
        # Без nice 19: на найнижчому пріоритеті під навантаженням (Steam, браузер) mpv пропускав кадри — відео смикалось
        play=$("$HOME/.local/bin/mgos-wp-optimize" get "$p" 2>/dev/null); [ -f "$play" ] || play=$p
        nice -n 5 mpvpaper -o "$opts" '*' "$play" >/dev/null 2>&1 &
        MPV=$!
        # чекаємо, поки mpv створить вихід і покаже перший кадр (до 15 с)
        for _ in $(seq 150); do
            kill -0 "$MPV" 2>/dev/null || exit 0          # mpvpaper впав — постер лишається
            r=$(printf '{"command":["get_property","vo-configured"]}\n' | socat -t 0.2 - /tmp/mpv-socket 2>/dev/null)
            [[ $r == *'"data":true'* ]] && break
            sleep 0.1
        done
        drop_poster
        "$POSTER_TOOL" >/dev/null 2>&1   # освіжити постер на наступний вхід (якщо треба)
        ;;
    *)
        pgrep -x awww-daemon >/dev/null || { awww-daemon >/dev/null 2>&1 & }
        [ -f "$p" ] || exit 0
        for _ in $(seq 50); do awww query >/dev/null 2>&1 && break; sleep 0.1; done
        awww img "$p" --resize crop --transition-type none
        drop_poster
        ;;
esac
