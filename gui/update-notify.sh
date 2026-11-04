#!/usr/bin/env bash
# Сповіщення «Є нова версія mgOS» (нові коміти в репозиторії mgos на GitHub).
# Запускається з mgos-auto.conf: exec-once = …/gui/update-notify.sh --loop
#   перша перевірка через 3 хв після входу, далі кожні 6 год; про ту саму версію — лише раз.
#   Кнопка в сповіщенні відкриває інсталятор (там банер «Оновити інсталятор»).
#   Вимкнути: touch ~/.config/mgos/no-update-notify
DIR=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
ST=${XDG_STATE_HOME:-$HOME/.local/state}/mgos-installer
mkdir -p "$ST"

check_once() {
    [ -f ~/.config/mgos/no-update-notify ] && return 0
    local out n subj sha act
    out=$(bash "$DIR/gui/selfupdate.sh" check 2>/dev/null) || return 0
    [[ "$out" == "@@UPD "* ]] || return 0
    read -r _ n subj <<< "$out"
    [ "${n:-0}" -gt 0 ] || return 0
    sha=$(git -C "$DIR" rev-parse '@{u}' 2>/dev/null)
    [ -n "$sha" ] && [ "$(cat "$ST/notified" 2>/dev/null)" = "$sha" ] && return 0
    echo "$sha" > "$ST/notified"
    act=$(notify-send -a "mgOS" -i com.megumin.installer -u normal \
          --action=open="Відкрити інсталятор" \
          "Є нова версія mgOS" "$subj ($n змін на GitHub)" 2>/dev/null)
    [ "$act" = open ] && setsid -f "$DIR/install-gui.sh" >/dev/null 2>&1
    return 0
}

if [ "${1:-}" = --loop ]; then
    sleep 180
    while :; do check_once; sleep 21600; done
else
    check_once
fi
