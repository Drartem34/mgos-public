#!/usr/bin/env bash
# ============================================================================
#  mgOS bootstrap — з чистого Arch Linux (TTY) одразу в графічний інсталятор
#
#  1. ставить мінімум: git, gh, Hyprland, GTK4, компілятор, шрифти
#  2. git clone публічного Drartem34/mgos-public → ~/mgOS (без входу в GitHub)
#  3. запускає мінімальну сесію Hyprland з mgOS Installer на весь екран
#  Після встановлення → «Перезавантажити» → вхід у mgOS → вікно «Все готово до роботи».
#
#  Запуск від звичайного користувача з sudo:   bash mgos-bootstrap.sh
#  Змінні: MGOS_HOME (куди клонувати, типово ~/mgOS), MGOS_REPO (типово Drartem34/mgos-public;
#          повна приватна версія — MGOS_REPO=Drartem34/mgos, потрібен «gh auth login»)
# ============================================================================
set -uo pipefail
REPO=${MGOS_REPO:-Drartem34/mgos-public}
DIR=${MGOS_HOME:-$HOME/mgOS}
SELF=$(dirname "$(readlink -f "$0")")

say()  { printf '\n\033[1;35m▸ %s\033[0m\n' "$*"; }
die()  { printf '\n\033[1;31m✗ %s\033[0m\n' "$*"; exit 1; }

[ "$EUID" -eq 0 ] && die "Запускай від звичайного користувача (з правами sudo), не від root."
command -v pacman >/dev/null || die "Це не Arch Linux (немає pacman)."

say "Пакети для графічного інсталятора"
sudo -v || die "Потрібні права sudo: запусти в терміналі й введи пароль (bash mgos-bootstrap.sh)."
sudo pacman -Syu --needed --noconfirm \
    git python jq rsync \
    hyprland xdg-desktop-portal-hyprland polkit mesa \
    gtk4 base-devel pkgconf kitty \
    ttf-jetbrains-mono-nerd noto-fonts \
    || die "pacman не зміг встановити пакети (перевір інтернет: ping archlinux.org)"

# ── інсталятор: поруч із цим файлом (уже клон) або з GitHub ──
if [ -x "$SELF/install-gui.sh" ] && [ -d "$SELF/gui" ]; then
    DIR=$SELF
elif [ -d "$DIR/.git" ]; then
    say "Оновлюю $DIR (канал stable)"
    CH=${MGOS_CHANNEL_BRANCH:-stable}
    if [ "$(git -C "$DIR" rev-parse --abbrev-ref HEAD 2>/dev/null)" != "$CH" ]; then
        echo "  оновлення вимкнено: гілка не «$CH» — беру те, що є"
    elif [ -n "$(git -C "$DIR" status --porcelain --untracked-files=no)" ]; then
        echo "  є локальні зміни — оновлення скасовано, беру те, що є:"; git -C "$DIR" status --short --untracked-files=no | head -10
    else
        { git -C "$DIR" fetch --quiet origin "$CH" && git -C "$DIR" merge --ff-only --quiet "origin/$CH"; } \
            || echo "  (не вдалося оновити — беру те, що є)"
    fi
else
    say "Завантажую інсталятор у $DIR"
    for t in 1 2 3; do
        { GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch "${MGOS_CHANNEL_BRANCH:-stable}" "https://github.com/$REPO.git" "$DIR" \
          || GIT_TERMINAL_PROMPT=0 git clone --depth 1 "https://github.com/$REPO.git" "$DIR"; } && break
        rm -rf "$DIR"; echo "  не вдалося, повтор ($t/3)…"; sleep 5
    done
    [ -d "$DIR/.git" ] || die "Не вдалося завантажити github.com/$REPO (перевір інтернет; для приватного репо спершу виконай «gh auth login && gh auth setup-git», а потім повтори)"
fi
chmod +x "$DIR"/install-gui.sh "$DIR"/install.sh "$DIR"/gui/*.sh "$DIR"/scripts/* 2>/dev/null

say "Збираю графічний інсталятор"
bash "$DIR/gui/build.sh" || { echo "Збірка не вдалася — запускаю текстовий інсталятор"; exec "$DIR/install.sh"; }

# уже в графічній сесії — просто на весь екран
if [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    MGOS_FULLSCREEN=1 exec "$DIR/install-gui.sh"
fi

# ── мінімальна сесія Hyprland лише для інсталятора ──
say "Запускаю графічний інсталятор"
C=${XDG_CACHE_HOME:-$HOME/.cache}/mgos-bootstrap
mkdir -p "$C"
{
    echo "# mgOS bootstrap: сесія лише для інсталятора (створено $(date '+%F %T'))"
    # масштаб під екран, як у встановленій системі
    python3 "$DIR/scripts/mgos-display" conf --profile mgos 2>/dev/null || echo "monitor = , preferred, auto, auto"
    cat <<EOF
input {
    kb_layout = us,ua
    kb_options = grp:alt_shift_toggle
    touchpad {
        tap-to-click = true
        disable_while_typing = false
    }
}
general {
    gaps_in = 0
    gaps_out = 0
    border_size = 0
}
decoration {
    rounding = 0
}
misc {
    disable_hyprland_logo = true
    disable_splash_rendering = true
    background_color = 0x0a0a21
}
ecosystem {
    no_update_news = true
    no_donation_nag = true
}
env = GDK_BACKEND, wayland
windowrule = fullscreen on, match:class ^(com\.megumin\.installer)$
# аварійний термінал і вихід
bind = SUPER, Return, exec, kitty
bind = CTRL ALT, Delete, exit
# інсталятор закрили без перезавантаження — повертаємось у TTY; впав — термінал з поясненням
exec-once = bash $(printf '%q' "$C/run.sh")
EOF
} > "$C/hyprland.conf"
# запуск через окремий скрипт — шлях з пробілами/лапками не ламає конфіг Hyprland
{
    echo '#!/usr/bin/env bash'
    printf 'D=%q\n' "$DIR"
    cat <<'EOF'
MGOS_FULLSCREEN=1 "$D/install-gui.sh" || kitty --hold sh -c 'echo "Інсталятор завершився з помилкою. Текстовий режим: $1/install.sh"' _ "$D"
hyprctl dispatch exit
EOF
} > "$C/run.sh"
chmod +x "$C/run.sh"

if command -v start-hyprland >/dev/null; then exec start-hyprland -- -c "$C/hyprland.conf"; fi
exec Hyprland -c "$C/hyprland.conf"
