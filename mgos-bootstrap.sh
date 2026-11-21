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
sudo pacman -Sy --needed --noconfirm \
    git python \
    hyprland xdg-desktop-portal-hyprland polkit mesa \
    gtk4 base-devel pkgconf kitty \
    ttf-jetbrains-mono-nerd noto-fonts \
    || die "pacman не зміг встановити пакети (перевір інтернет: ping archlinux.org)"

# ── інсталятор: поруч із цим файлом (уже клон) або з GitHub ──
if [ -x "$SELF/install-gui.sh" ] && [ -d "$SELF/gui" ]; then
    DIR=$SELF
elif [ -d "$DIR/.git" ]; then
    say "Оновлюю $DIR"
    git -C "$DIR" pull --ff-only --autostash || echo "  (не вдалося оновити — беру те, що є)"
else
    say "Завантажую інсталятор у $DIR"
    for t in 1 2 3; do
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 "https://github.com/$REPO.git" "$DIR" && break
        rm -rf "$DIR"; echo "  не вдалося, повтор ($t/3)…"; sleep 5
    done
    [ -d "$DIR/.git" ] || die "Не вдалося завантажити github.com/$REPO (перевір інтернет)"
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
exec-once = sh -c 'MGOS_FULLSCREEN=1 "$DIR/install-gui.sh" || kitty --hold sh -c "echo Інсталятор завершився з помилкою. Текстовий режим: $DIR/install.sh"; hyprctl dispatch exit'
EOF
} > "$C/hyprland.conf"

if command -v start-hyprland >/dev/null; then exec start-hyprland -- -c "$C/hyprland.conf"; fi
exec Hyprland -c "$C/hyprland.conf"
