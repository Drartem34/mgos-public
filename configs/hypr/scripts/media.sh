#!/usr/bin/env bash
# Розумне керування медіа: play-pause | next | previous
# 1) якщо щось грає — керуємо ним (Spotify має пріоритет)
# 2) якщо нічого не грає — керуємо Spotify
# 3) інакше — будь-який доступний плеєр
cmd="${1:-play-pause}"

players=$(playerctl -l 2>/dev/null)
[ -z "$players" ] && exit 0

target=""
playing=$(for p in $players; do
    [ "$(playerctl -p "$p" status 2>/dev/null)" = "Playing" ] && echo "$p"
done)

if [ -n "$playing" ]; then
    target=$(echo "$playing" | grep -m1 '^spotify' || echo "$playing" | head -n1)
else
    target=$(echo "$players" | grep -m1 '^spotify' || echo "$players" | head -n1)
fi

playerctl -p "$target" "$cmd"

# показати музичний OSD SYSui
echo "show" | socat - UNIX-CLIENT:"$HOME/.config/SYSui/music.sock" 2>/dev/null
exit 0
