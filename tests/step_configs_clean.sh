#!/usr/bin/env bash
# ============================================================================
#  Тест step_configs на ЧИСТІЙ системі (fake HOME без ~/.config) і без jq.
#    A) чиста система: STATE (dock_stash.json, rclone.conf) прибрано зі знімка,
#       MERGE (settings.json) = defaults.json, SEED/CODE на місці
#    B) оновлення БЕЗ jq: settings.json користувача лишається як є
#  Код виходу 0 — усе гаразд.   Запуск: bash tests/step_configs_clean.sh
# ============================================================================
set -u
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
H=$T/home; R=$T/repo; FAIL=0
ok()  { printf '  \033[32m✓\033[0m %s\n' "$*"; }
bad() { printf '  \033[31m✗\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }

mkdir -p "$T/bin" "$T/nojq" "$T/run" "$R/configs" "$H"; chmod 700 "$T/run"
ln -s "$ROOT/gui" "$R/gui"; cp -r "$ROOT/system_state" "$R/system_state"
cp -r "$ROOT/configs/hypr" "$ROOT/configs/megumin-os" "$ROOT/configs/rclone" "$R/configs/"
cp "$ROOT/configs/.mgos-manifest" "$R/configs/"
# у знімку «особисте» відрізняється від defaults — щоб помітити, що MERGE його замінив
echo '{"accent_color":"#123456","owner_only":1}' > "$R/configs/megumin-os/settings.json"
printf '#!/bin/sh\necho "STUB-SUDO: $*" >&2; exit 1\n' > "$T/bin/sudo"; chmod +x "$T/bin/sudo"
# PATH без jq: симлінки на все з /usr/bin, крім jq
for f in /usr/bin/*; do b=${f##*/}; [ "$b" = jq ] || [ "$b" = sudo ] || ln -s "$f" "$T/nojq/$b" 2>/dev/null; done
cp "$T/bin/sudo" "$T/nojq/sudo"

run() {  # run <PATH> <cache>
    env -i HOME="$H" USER="${USER:-u}" LOGNAME="${USER:-u}" PATH="$1" XDG_RUNTIME_DIR="$T/run" \
        MGOS_DIR="$R" MGOS_CACHE="$2" OPT_CONFIGS="hypr megumin-os rclone" \
        dbus-run-session -- bash "$R/gui/steps.sh" configs > "$T/out" 2>&1; RC=$?
    grep -q 'STUB-SUDO' "$T/out" && bad "step_configs викликав sudo"
}

echo "== A. чиста система (немає ~/.config)"
rm -rf "$H"; mkdir -p "$H"
run "$T/bin:/usr/bin:/bin" "$T/cache/a"; [ "$RC" = 0 ] && ok "код 0" || { bad "код $RC"; tail -5 "$T/out"; }
M=$H/.config/megumin-os
cmp -s <(jq -S . "$M/settings.json") <(jq -S . "$ROOT/configs/megumin-os/defaults.json") && ok "settings.json = defaults" || bad "settings.json не з defaults"
[ ! -e "$M/dock_stash.json" ] && ok "dock_stash.json (STATE) не поставлено" || bad "dock_stash.json поставлено зі знімка"
[ ! -e "$H/.config/rclone/rclone.conf" ] && ok "rclone.conf (токени) не поставлено" || bad "rclone.conf поставлено"
[ -f "$M/defaults.json" ] && [ -f "$H/.config/hypr/hyprland.conf" ] && ok "CODE-файли на місці" || bad "CODE-файли відсутні"
[ ! -e "$H/.config/hypr/mgos-auto.conf" ] || ok "(mgos-auto.conf зі знімка — лише якщо є в CODE)"

echo "== B. оновлення без jq"
rm -rf "$H"; mkdir -p "$M"
echo '{"accent_color":"#ff0000","my_custom":1}' > "$M/settings.json"
run "$T/nojq" "$T/cache/b"; [ "$RC" = 0 ] && ok "код 0" || { bad "код $RC"; tail -5 "$T/out"; }
[ "$(cat "$M/settings.json")" = '{"accent_color":"#ff0000","my_custom":1}' ] && ok "settings.json користувача не змінено" || bad "settings.json змінено без jq"
grep -q 'jq не встановлено' "$T/out" && ok "попередження про jq" || bad "немає попередження"

echo
if [ "$FAIL" -gt 0 ]; then printf '\033[31mstep_configs_clean: ПОМИЛОК %d\033[0m\n' "$FAIL"; exit 1; fi
printf '\033[32mstep_configs_clean: OK\033[0m\n'
