#!/usr/bin/env bash
# ============================================================================
#  Тест step_configs (gui/steps.sh) у fake HOME — без sudo, без реального ~/.config.
#    • репозиторій підставний: $T/repo (configs, gui — симлінки; system_state — копія)
#    • sudo — заглушка, що ЗАВЖДИ відмовляє; hyprctl — лише пише в лог
#    • сценарії: 1) перший запуск  2) повтор (ідемпотентність)  3) битий settings.json
#                4) залишок перерваного запуску (.mgos-new.*)  5) version-gate
#                6) битий hyprland.conf у знімку → повернення старого
#  Код виходу 0 — усі перевірки пройшли.   Запуск: bash tests/step_configs_fakehome.sh
# ============================================================================
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
H=$T/home; R=$T/repo; FAIL=0
ok()  { printf '  \033[32m✓\033[0m %s\n' "$*"; }
bad() { printf '  \033[31m✗\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }

mkdir -p "$H/.config" "$T/bin" "$T/cache" "$R" "$T/run"; chmod 700 "$T/run"
# configs — ЛИШЕ КОПІЯ (не симлінк): cp -a скопіював би симлінк, і merge/sed/rm писали б у справжній репозиторій
mk_repo() {  # mk_repo <тека>
    mkdir -p "$1/configs"; ln -s "$ROOT/gui" "$1/gui"; cp -r "$ROOT/system_state" "$1/system_state"
    cp -r "$ROOT/configs/hypr" "$ROOT/configs/megumin-os" "$1/configs/"; cp "$ROOT/configs/.mgos-manifest" "$1/configs/"
}
mk_repo "$R"
BEFORE=$(git -C "$ROOT" status --porcelain -- configs 2>/dev/null)
printf '#!/bin/sh\necho "STUB-SUDO: $*" >&2; exit 1\n' > "$T/bin/sudo"
printf '#!/bin/sh\necho "hyprctl $*" >> %q\n' "$T/hyprctl.log" > "$T/bin/hyprctl"
chmod +x "$T/bin/"*

reset_home() {
    rm -rf "$H/.config"; mkdir -p "$H/.config/megumin-os" "$H/.config/hypr"
    echo '{"accent_color":"#ff0000","my_custom":1,"dark_theme":false}' > "$H/.config/megumin-os/settings.json"
    echo '# user-tp' > "$H/.config/hypr/touchpad-user.conf"
    echo '# auto' > "$H/.config/hypr/mgos-auto.conf"
    echo '# junk' > "$H/.config/hypr/old-junk.conf"
}
run() {  # run <repo> <cache> → вивід у $T/out, код у $RC
    env -i HOME="$H" USER="${USER:-u}" LOGNAME="${USER:-u}" PATH="$T/bin:/usr/local/bin:/usr/bin:/bin" \
        XDG_RUNTIME_DIR="$T/run" HYPRLAND_INSTANCE_SIGNATURE=fake-review \
        MGOS_DIR="$1" MGOS_CACHE="$2" OPT_CONFIGS="hypr megumin-os" \
        dbus-run-session -- bash "$1/gui/steps.sh" configs > "$T/out" 2>&1; RC=$?
    grep -q 'STUB-SUDO' "$T/out" && bad "step_configs викликав sudo: $(grep -m1 STUB-SUDO "$T/out")"
}
check_state() {  # після вдалого запуску
    local s=$H/.config/megumin-os/settings.json
    [ "$(jq -r .accent_color "$s")" = "#ff0000" ] && [ "$(jq -r .my_custom "$s")" = 1 ] && [ "$(jq -r .dark_theme "$s")" = false ] \
        && ok "settings.json: ключі користувача збережено" || bad "settings.json: ключі користувача втрачено"
    [ "$(jq -r '.topbar_style // empty' "$s")" != "" ] && ok "settings.json: нові ключі з defaults додано" || bad "settings.json: нові ключі не додано"
    [ "$(cat "$H/.config/hypr/touchpad-user.conf")" = '# user-tp' ] && ok "touchpad-user.conf (SEED) — користувача" || bad "touchpad-user.conf перезаписано"
    [ "$(cat "$H/.config/hypr/mgos-auto.conf" 2>/dev/null)" = '# auto' ] && ok "mgos-auto.conf (STATE) збережено" || bad "mgos-auto.conf втрачено"
    [ ! -e "$H/.config/hypr/old-junk.conf" ] && ok "CODE-тека замінена (зайве прибрано)" || bad "old-junk.conf лишився"
    [ -f "$H/.config/hypr/hyprland.conf" ] && ok "hyprland.conf на місці" || bad "немає hyprland.conf"
    [ -z "$(ls -d "$H"/.config/.mgos-new.* 2>/dev/null)" ] && ok "без залишків .mgos-new.*" || bad "залишились .mgos-new.*"
}

echo "== 1. перший запуск"; reset_home; : > "$T/hyprctl.log"
run "$R" "$T/cache/r1"; [ "$RC" = 0 ] && ok "код 0" || { bad "код $RC"; tail -5 "$T/out"; }
check_state
grep -q 'keyword misc:disable_autoreload 1' "$T/hyprctl.log" && grep -q '^hyprctl reload' "$T/hyprctl.log" && ok "autoreload вимкнено → reload" || bad "hyprctl: $(cat "$T/hyprctl.log")"
[ -s "$T/cache/r1/manifest" ] && ok "точка відкату записана" || bad "manifest порожній"
jq -S . "$H/.config/megumin-os/settings.json" > "$T/s1.json"

echo "== 2. повторний запуск (ідемпотентність)"
run "$R" "$T/cache/r1"; [ "$RC" = 0 ] && ok "код 0" || bad "код $RC"
check_state
jq -S . "$H/.config/megumin-os/settings.json" > "$T/s2.json"
cmp -s "$T/s1.json" "$T/s2.json" && ok "settings.json однаковий" || bad "settings.json змінився на повторі"

echo "== 3. битий settings.json"
echo '{broken' > "$H/.config/megumin-os/settings.json"
run "$R" "$T/cache/r1"; [ "$RC" = 0 ] && ok "код 0" || bad "код $RC"
jq -e 'type=="object"' "$H/.config/megumin-os/settings.json" >/dev/null && ok "settings.json валідний" || bad "settings.json невалідний"
cmp -s <(jq -S . "$H/.config/megumin-os/settings.json") <(jq -S . "$ROOT/configs/megumin-os/defaults.json") \
    && ok "= defaults.json" || bad "не дорівнює defaults.json"
grep -q 'пошкоджений' "$T/out" && ok "попередження показано" || bad "немає попередження"

echo "== 4. залишок перерваного запуску"
reset_home; mkdir -p "$H/.config/.mgos-new.zzz/.old"; echo hypr > "$H/.config/.mgos-new.zzz/.name"
mv "$H/.config/hypr"/* "$H/.config/.mgos-new.zzz/.old/"; rmdir "$H/.config/hypr"
run "$R" "$T/cache/r1"; [ "$RC" = 0 ] && ok "код 0" || bad "код $RC"
check_state

echo "== 5. version-gate (знімок під hyprland 99)"
reset_home; mk_repo "$T/repo5"
echo 'hyprland 99.0.0' > "$T/repo5/system_state/versions.txt"
run "$T/repo5" "$T/cache/r5"; [ "$RC" != 0 ] && ok "крок зупинено (код $RC)" || bad "gate пропустив"
[ -e "$H/.config/hypr/old-junk.conf" ] && ok "конфіги не змінено" || bad "конфіги змінено попри gate"

echo "== 6. битий hyprland.conf у знімку"
reset_home; mk_repo "$T/repo6"
printf '\nthis is = not = valid\nbind = SUPER, Q, nonexistentdispatcher_xyz\n' >> "$T/repo6/configs/hypr/hyprland.conf"
run "$T/repo6" "$T/cache/r6"; [ "$RC" != 0 ] && ok "крок повернув помилку (код $RC)" || bad "битий конфіг прийнято (код 0)"
[ -e "$H/.config/hypr/old-junk.conf" ] && [ ! -f "$H/.config/hypr/hyprland.conf" ] && ok "стару теку hypr повернуто" || bad "стару теку hypr не повернуто"
[ -z "$(ls -d "$H"/.config/.mgos-new.* 2>/dev/null)" ] && ok "без залишків" || bad "залишки .mgos-new.*"

echo "== 7. репозиторій не зачеплено"
[ "$(git -C "$ROOT" status --porcelain -- configs 2>/dev/null)" = "$BEFORE" ] && ok "git status configs/ без змін" || bad "тест змінив configs/ у репозиторії!"

echo
if [ "$FAIL" -gt 0 ]; then printf '\033[31mstep_configs: ПОМИЛОК %d\033[0m\n' "$FAIL"; exit 1; fi
printf '\033[32mstep_configs: OK\033[0m\n'
