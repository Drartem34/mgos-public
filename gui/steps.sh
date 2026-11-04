#!/usr/bin/env bash
# ============================================================================
#  Кроки GUI-інсталятора Megumin OS 6 (mgos-installer).
#  Кожен крок — функція step_<id>. Усі кроки ІДЕМПОТЕНТНІ: інсталятор зберігає
#  прогрес і після збою/перезавантаження повторює крок, на якому зупинився.
#
#  Протокол виводу (рядки, які розбирає GUI):
#    @@MSG текст      — підпис поточної дії
#    @@SUB a/b        — прогрес усередині кроку
#    @@FAILPKG name   — пакет не встановився (не фатально)
#    @@CHECK ok|warn|fail назва            — результат перевірки (verify)
#    @@CHECK ok|warn|fail група|назва|деталі — результат перевірки системи (step_check)
#    @@OUT name old new repo|aur   (step_analyze) — застарілий пакет mgOS
#    @@MISS name                   (step_analyze) — відсутній пакет mgOS
#  Звичайний вивід pacman «(3/80) installing foo» GUI теж розуміє як прогрес.
#
#  Змінні від GUI: MGOS_DIR, OPT_UPDATE, OPT_WALL, OPT_APPS, OPT_DEV, OPT_DRIVERS, OPT_FISH, OPT_CONFIGS
#  Групи пакетів знімка — у gui/packages.conf.
# ============================================================================
set -o pipefail
export LC_ALL=C   # однакове сортування для join/comm у відкаті
cd "${MGOS_DIR:?MGOS_DIR не задано}" || exit 1
exec </dev/null

SRC_HOME=$(cat ./system_state/source_home.txt 2>/dev/null || echo /home/doki)
# --mflags --nocheck: без тестів збірки AUR. Деякі PKGBUILD мають у checkdepends заглушки
# (xpadneo-dkms: «LINUX-HEADERS»), через які yay падав «could not find all required packages»
YAY=(yay --noconfirm --needed --answerdiff None --answerclean None --answeredit None --answerupgrade None --removemake --mflags --nocheck)

# Критичні: без них система не запуститься (Hyprland, панелі SYSui на GTK4, налаштування на PyQt6)
PACKAGES_CORE=(
    hyprland xdg-desktop-portal-hyprland xdg-desktop-portal-gtk
    gtk4 gtk4-layer-shell base-devel pkgconf nlohmann-json
    python python-pyqt6 socat curl libnotify
    pipewire-pulse wireplumber networkmanager dunst sddm
)
PACKAGES_STABLE=(
    hyprpaper hyprpicker egl-wayland gamescope
    kitty fastfetch plymouth
    nwg-look
    grim slurp wl-clipboard cliphist awww mpv imagemagick ffmpeg
    pipewire-alsa pipewire-jack pavucontrol pamixer playerctl ddcutil
    python-pywal openrgb
    ttf-jetbrains-mono-nerd ttf-liberation ttf-opensans ttf-bitstream-vera noto-fonts noto-fonts-cjk noto-fonts-emoji woff2-font-awesome cantarell-fonts
    btop nautilus gnome-disk-utility rclone power-profiles-daemon bluez bluez-utils
    qt6-svg qt6-virtualkeyboard qt6-multimedia-ffmpeg
)
PACKAGES_AUR=(python-ignis-git ignis-gvc whitesur-icon-theme-git mpvpaper bluetuith)
SYSUI_BINS=(dock topbar cc_osd runner_osd wallpapers clipboard_osd notify_osd megumin_osd powermenu_osd volume_osd about_osd)

msg()      { echo "@@MSG $*"; }
sub()      { echo "@@SUB $1"; }
fail_pkg() { echo "@@FAILPKG $1"; }
mgos_set() { printf '%s\n' "${PACKAGES_CORE[@]}" "${PACKAGES_STABLE[@]}" "${PACKAGES_AUR[@]}"; conf_group utils; }

# ---------- групи пакетів (gui/packages.conf) ----------
conf_group() {  # conf_group <назва секції без дужок>
    awk -v s="[$1]" '/^\[/{on=($0==s); next} on && !/^[[:space:]]*#/{for(i=1;i<=NF;i++) print $i}' gui/packages.conf 2>/dev/null
}
conf_hw() { awk '/^\[hw:/{print substr($0, 5, length($0) - 5)}' gui/packages.conf 2>/dev/null; }
# PCI-пристрої читаємо прямо з /sys: lspci (pciutils) на свіжому Arch часто немає,
# і тоді жодна відеокарта «не знаходилась» — драйвери мовчки пропускались.
pci_has() {  # pci_has <vendor, напр. 0x8086> <префікс класу: 0x03 — відео, 0x0280 — Wi-Fi>
    local d
    for d in /sys/bus/pci/devices/*; do
        [ "$(cat "$d/vendor" 2>/dev/null)" = "$1" ] || continue
        [[ "$(cat "$d/class" 2>/dev/null)" == "$2"* ]] && return 0
    done
    return 1
}
gpu_nvidia() { pci_has 0x10de 0x03; }
gpu_intel()  { pci_has 0x8086 0x03; }
gpu_amd()    { pci_has 0x1002 0x03; }
hw_ok() {
    case $1 in
        nvidia)        gpu_nvidia ;;
        # ноутбук із двома відеокартами (NVIDIA + вбудована) — prime-run для запуску на дискретній
        nvidia-hybrid) gpu_nvidia && { gpu_intel || gpu_amd; } ;;
        intel-gpu)     gpu_intel ;;
        amd-gpu)       gpu_amd ;;
        amd-cpu)       grep -q AuthenticAMD /proc/cpuinfo ;;
        intel-cpu)     grep -q GenuineIntel /proc/cpuinfo ;;
        # звук через Intel SOF (майже всі ноутбуки з 2019+): без sof-firmware звуку немає зовсім.
        # Модуль ядра вантажиться навіть коли прошивки бракує — тому дивимось на модуль, а не на драйвер.
        sof)           compgen -G '/sys/module/snd_sof_pci_intel_*' >/dev/null ;;
        broadcom-wifi) pci_has 0x14e4 0x0280 ;;
        laptop)        compgen -G '/sys/class/power_supply/BAT*' >/dev/null ;;
        board:*)       grep -qF "${1#board:}" /sys/class/dmi/id/board_name 2>/dev/null ;;
        gamepad)       true ;;
        *)             false ;;
    esac
}
# пакет уже стоїть під іншою назвою (foo-git / foo-bin замість foo чи навпаки).
# Без цього yay пробує поставити «foo», pacman питає «Remove foo-git? [y/N]»,
# на --noconfirm відповідає «N» і падає (так було з ignis-gvc ↔ ignis-gvc-git).
have_variant() {
    local b=${1%-git}; b=${b%-bin}
    pacman -Qq "$b" "$b-git" "$b-bin" 2>/dev/null | grep -q .
}
classified() {
    { printf '%s\n' "${PACKAGES_CORE[@]}" "${PACKAGES_STABLE[@]}" "${PACKAGES_AUR[@]}"
      grep -v '^[[:space:]]*#\|^\[' gui/packages.conf 2>/dev/null | tr ' ' '\n'; } | grep -v '^$' | sort -u
}
# програми зі знімка: група [apps] + усе нове, що ще не розкладене по групах
snapshot_apps() { { conf_group apps; comm -23 <(sort -u pkglist.txt 2>/dev/null) <(classified); } | grep -v '^$' | sort -u; }
# драйвери, що підходять саме цьому ПК
drivers_here() { local c; while read -r c; do hw_ok "$c" && conf_group "hw:$c"; done < <(conf_hw); }

# install_list <fatal 0|1> pkgs... — ставить лише відсутні; якщо пакетна установка
# впала, ставить по одному, щоб один битий пакет не зупинив усе.
# заголовки для кожного встановленого ядра: без них *-dkms драйвери не збираються
# (AUR-пакети просять віртуальний «LINUX-HEADERS», якого yay сам не знаходить)
kernel_headers() {
    local k
    for k in $(pacman -Qq 2>/dev/null | grep -xE 'linux|linux-lts|linux-zen|linux-hardened|linux-rt|linux-rt-lts|linux-cachyos[a-z-]*'); do
        [[ "$k" == *-headers ]] || echo "$k-headers"
    done
}

# Ключі пакувальників. Старий archlinux-keyring дає «signature ... is marginal trust» і pacman
# відмовляється ставити пакет (так падав intel-media-driver). Оновлюємо один раз за запуск.
_KEYRING_DONE=0
refresh_keyring() {
    [ $_KEYRING_DONE = 1 ] && return 1
    _KEYRING_DONE=1
    msg "Оновлюю ключі пакувальників (archlinux-keyring)"
    local k=(archlinux-keyring)
    grep -q '^\[endeavouros\]' /etc/pacman.conf && k+=(endeavouros-keyring)
    grep -q '^\[chaotic-aur\]' /etc/pacman.conf && k+=(chaotic-keyring)
    # -Sy лише для keyring — офіційно рекомендований виняток із правила «без часткових оновлень»
    sudo pacman -Sy --needed --noconfirm "${k[@]}" || return 1
    sudo pacman-key --populate "${k[@]%-keyring}" >/dev/null 2>&1
    return 0
}

install_list() {
    local fatal=$1; shift
    [ $# -eq 0 ] && { msg "Нічого встановлювати"; return 0; }
    # драйвер DKMS у списку — спершу заголовки ядра
    if printf '%s\n' "$@" | grep -q -- '-dkms$'; then set -- $(kernel_headers) "$@"; fi
    # sudo мусить працювати ДО yay: інакше пакети збираються по 10 хв і падають на «incorrect password»
    if ! sudo true >/dev/null 2>&1; then
        echo "❌ sudo не дає доступу (пароль не прийнято або сесія sudo закінчилась). Перезапусти інсталятор і введи пароль."
        for p in "$@"; do pacman -T "$p" >/dev/null 2>&1 || fail_pkg "$p"; done
        return 1
    fi
    local miss=() p
    while read -r p; do
        [ -n "$p" ] || continue
        if have_variant "$p"; then echo "  $p — вже встановлено інший варіант (-git/-bin), пропускаю"; continue; fi
        miss+=("$p")
    done < <(pacman -T "$@" 2>/dev/null)
    if [ ${#miss[@]} -eq 0 ]; then msg "Усе вже встановлено ($#)"; return 0; fi
    msg "Встановлюю ${#miss[@]} з $#"
    "${YAY[@]}" -S "${miss[@]}" && return 0
    local i=0 bad=0 out
    for p in "${miss[@]}"; do
        i=$((i + 1)); sub "$i/${#miss[@]}"; msg "$p"
        pacman -T "$p" >/dev/null 2>&1 && continue
        out=$("${YAY[@]}" -S "$p" 2>&1 | tee /dev/stderr) && continue
        # «marginal trust» / «invalid or corrupted package (PGP signature)» — застарілі ключі. Оновити й повторити.
        if grep -qiE 'marginal trust|unknown trust|PGP signature|signature from .* is invalid' <<< "$out" && refresh_keyring; then
            "${YAY[@]}" -S "$p" && continue
        fi
        fail_pkg "$p"; bad=1
    done
    [ "$fatal" = 1 ] && [ $bad = 1 ] && return 1
    return 0
}


# ============================================================================
#  ТОЧКА ВІДКАТУ (cache/<запуск> у теці інсталятора, шлях — у MGOS_CACHE)
#  Перед кожною зміною крок викликає bk/bk_list/bk_service/bk_gsetting/... —
#  у manifest пишеться, що було ДО змін. step_rollback проходить manifest
#  у зворотному порядку і повертає все як було, а пакети — до версій із pkgs.before.
#  Записи manifest (через TAB):
#    E <шлях> <архів.tar>  — існував, збережено архів
#    N <шлях>              — не існував (відкат: видалити)
#    L <тека> <список>     — тека лише доповнюється (відкат: видалити нові файли)
#    S <служба> <стан>     — стан служби до змін
#    G <схема> <ключ> <значення> — gsettings до змін
#    SH <оболонка>         — оболонка користувача до змін
#    P <тема>              — тема Plymouth до змін (відкат: перезібрати initramfs)
# ============================================================================
CACHE=${MGOS_CACHE:-}
MAN=${CACHE:+$CACHE/manifest}
_in_home() { [[ "$1" == "$HOME"/* ]]; }
# існування шляху: звичайний test (системні теки читаються всіма), sudo -n — лише для закритих.
# Раніше тут був «sudo test -e»: коли sudo переставав працювати, будь-який шлях вважався
# неіснуючим і в manifest потрапляло «N /usr» — відкат зробив би rm -rf /usr.
_exists() { [ -e "$1" ] || [ -L "$1" ] || sudo -n test -e "$1" 2>/dev/null; }
# шлях, який відкат має право видалити цілком: у $HOME (не сам $HOME) або щонайменше 3 рівні вглиб
# і не одна з базових системних тек
_safe_rm_path() {
    local p; p=$(readlink -m -- "$1")
    case "$p" in
        ""|/|/usr|/usr/*/|/etc|/var|/boot|/opt|/home|/root|/srv|/run|/proc|/sys|/dev|/bin|/sbin|/lib|/lib64|"$HOME") return 1 ;;
    esac
    _in_home "$p" && return 0
    [ "$(tr -cd / <<< "$p" | wc -c)" -ge 3 ]
}
_seen() { [ -n "$MAN" ] && awk -F'\t' -v p="$1" '$2 == p {f = 1} END {exit !f}' "$MAN" 2>/dev/null; }
_rec() { [ -n "$MAN" ] && (IFS=$'\t'; echo "$*") >> "$MAN"; }

bk() {  # bk <шлях> — зберегти шлях перед зміною (один раз за запуск)
    [ -n "$CACHE" ] || return 0
    local p; p=$(readlink -m -- "$1")
    _seen "$p" && return 0
    local S=(); _in_home "$p" || S=(sudo)
    if _exists "$p"; then
        local id; id=$(printf '%s' "$p" | md5sum | cut -c1-16).tar
        mkdir -p "$CACHE/files"
        "${S[@]}" tar -cpf "$CACHE/files/$id" -C / "${p#/}" 2>/dev/null || { echo "⚠ не вдалося зберегти $p"; return 0; }
        _rec E "$p" "$id"
    else
        # не існував: записуємо найвищу теку, якої теж не було, — відкат прибере все створене дерево
        local top=$p up
        while up=$(dirname "$top"); [ "$up" != / ] && ! _exists "$up"; do top=$up; done
        if ! _safe_rm_path "$top"; then echo "⚠ точку відкату для $p не записано (небезпечний шлях $top)"; return 0; fi
        _seen "$top" || _rec N "$top"
    fi
}
bk_list() {  # bk_list <тека> — тека лише доповнюється: запам'ятати наявні файли
    [ -n "$CACHE" ] || return 0
    local d; d=$(readlink -m -- "$1")
    _seen "$d" && return 0
    if [ -d "$d" ]; then
        local id; id=$(printf '%s' "$d" | md5sum | cut -c1-16).list
        mkdir -p "$CACHE/files"; find "$d" -mindepth 1 > "$CACHE/files/$id"
        _rec L "$d" "$id"
    else
        _rec N "$d"
    fi
}
bk_service()  { _seen "$1" || _rec S "$1" "$(systemctl is-enabled "$1" 2>/dev/null || echo disabled)"; }
bk_gsetting() {
    [ -n "$MAN" ] || return 0
    awk -F'\t' -v s="$1" -v k="$2" '$1 == "G" && $2 == s && $3 == k {f = 1} END {exit !f}' "$MAN" 2>/dev/null && return 0
    local v; v=$(gsettings get "$1" "$2" 2>/dev/null) && _rec G "$1" "$2" "$v"
}
bk_shell()    { grep -q "^SH"$'\t' "$MAN" 2>/dev/null || _rec SH "$(getent passwd "$USER" | cut -d: -f7)"; }
bk_plymouth() { grep -q "^P"$'\t' "$MAN" 2>/dev/null || _rec P "$(plymouth-set-default-theme 2>/dev/null || echo none)"; }

step_snapshot() {
    [ -n "$CACHE" ] || { echo "MGOS_CACHE не задано"; return 1; }
    mkdir -p "$CACHE" || { echo "Не можу створити $CACHE"; return 1; }
    touch "$MAN"
    if [ -s "$CACHE/pkgs.before" ]; then msg "Точка відкату вже є (продовження)"; return 0; fi
    msg "Запам'ятовую пакети"
    pacman -Q > "$CACHE/pkgs.before"
    pacman -Qqe > "$CACHE/explicit.before"
    date '+%Y-%m-%d %H:%M:%S' > "$CACHE/started"
    # тримаємо лише одну точку відкату — останню
    local d
    for d in "$(dirname "$CACHE")"/*/; do
        [ "$(readlink -m "$d")" = "$(readlink -m "$CACHE")" ] && continue
        rm -rf "$d" 2>/dev/null || sudo rm -rf "$d"
    done
    echo "Точка відкату створена: $(wc -l < "$CACHE/pkgs.before") пакетів. Кнопка «Скасувати» поверне систему до цього стану."
}

# пошук файлу пакета потрібної версії: кеш pacman → кеш yay → наш кеш → Arch Linux Archive
_find_pkg() {
    local n=$1 v=$2 f d a e
    for d in /var/cache/pacman/pkg "$HOME/.cache/yay/$n" "$CACHE/pkgs"; do
        for f in "$d/$n-$v-"*.pkg.tar.*; do [ -f "$f" ] && [[ "$f" != *.sig ]] && { echo "$f"; return 0; }; done
    done
    mkdir -p "$CACHE/pkgs"
    for a in x86_64 any; do for e in zst xz; do
        f="$CACHE/pkgs/$n-$v-$a.pkg.tar.$e"
        curl -fsSL --max-time 300 -o "$f" "https://archive.archlinux.org/packages/${n:0:1}/$n/$n-$v-$a.pkg.tar.$e" 2>/dev/null && { echo "$f"; return 0; }
        rm -f "$f"
    done; done
    return 1
}

step_rollback() {
    [ -s "$CACHE/pkgs.before" ] || { echo "Немає точки відкату ($CACHE)"; return 1; }
    local now; now=$(mktemp)
    pacman -Q | sort > "$now"
    local before; before=$(mktemp); sort "$CACHE/pkgs.before" > "$before"

    # 1) змінені та зниклі пакети → старі версії
    msg "Пакети: шукаю старі версії"
    local files=() lost=() n v cur i=0 total
    mapfile -t changed < <(join -a1 "$before" "$now" | awk 'NF==2 || $2!=$3 {print $1, $2}')
    total=${#changed[@]}
    for line in "${changed[@]}"; do
        read -r n v <<< "$line"; i=$((i + 1)); sub "$i/$((total + 2))"; msg "$n → $v"
        if f=$(_find_pkg "$n" "$v"); then files+=("$f"); else lost+=("$n"); fail_pkg "$n (версію $v не знайдено — лишається нова)"; fi
    done
    if [ ${#files[@]} -gt 0 ]; then
        msg "Повертаю ${#files[@]} пакетів до старих версій"
        sudo pacman -U --noconfirm "${files[@]}" || {
            for f in "${files[@]}"; do sudo pacman -U --noconfirm "$f" || fail_pkg "$(basename "$f")"; done; }
    fi
    # 2) додані пакети → видалити
    mapfile -t added < <(join -v2 "$before" <(pacman -Q | sort) | awk '{print $1}')
    if [ ${#added[@]} -gt 0 ]; then
        msg "Видаляю ${#added[@]} доданих пакетів"
        sudo pacman -R --noconfirm "${added[@]}" || {
            for n in "${added[@]}"; do pacman -Q "$n" &>/dev/null && { sudo pacman -R --noconfirm "$n" || fail_pkg "$n (не вдалося видалити)"; }; done; }
    fi
    rm -f "$now" "$before"

    # 3) файли та налаштування — у зворотному порядку
    msg "Файли та налаштування"; sub "$((total + 1))/$((total + 2))"
    local kind a b c rebuild=0
    while IFS=$'\t' read -r kind a b c; do
        case $kind in
            E)  local S=(); _in_home "$a" || S=(sudo)
                "${S[@]}" rm -rf -- "$a"; "${S[@]}" tar -xpf "$CACHE/files/$b" -C / && msg "повернуто $a" ;;
            N)  if ! _safe_rm_path "$a"; then fail_pkg "$a (небезпечний шлях — не видаляю)"; continue; fi
                if _in_home "$a"; then rm -rf -- "$a"; else sudo rm -rf -- "$a"; fi; msg "прибрано $a" ;;
            L)  [ -d "$a" ] && comm -13 <(sort "$CACHE/files/$b") <(find "$a" -mindepth 1 | sort) | sort -r | while read -r x; do rm -rf -- "$x"; done ;;
            S)  [ "$b" != enabled ] && sudo systemctl disable --now "$a" 2>/dev/null && msg "вимкнено $a" ;;
            G)  gsettings set "$a" "$b" "$c" 2>/dev/null ;;
            SH) sudo chsh -s "$a" "$USER" ;;
            P)  [ "$a" != none ] && sudo plymouth-set-default-theme "$a" 2>/dev/null; rebuild=1 ;;
        esac
    done < <(tac "$MAN")
    if [ $rebuild = 1 ]; then
        msg "initramfs"
        if command -v dracut &>/dev/null; then sudo dracut -f --regenerate-all 2>&1 | tail -2; elif command -v mkinitcpio &>/dev/null; then sudo mkinitcpio -P 2>&1 | tail -2; fi
    fi
    grep -q $'\t/etc/udev/' "$MAN" && sudo udevadm control --reload 2>/dev/null
    sub "$((total + 2))/$((total + 2))"
    echo "Відкат завершено. Не повернуто: ${#lost[@]}"
    # точка відкату використана — прибираємо, щоб не лишати сміття
    rm -rf "$CACHE" 2>/dev/null || sudo rm -rf "$CACHE"
    return 0
}

step_retry_pkgs() {
    local a p; read -ra a <<< "${OPT_RETRY:-}"
    local i=0
    printf '%s\n' "${a[@]}" | grep -q -- '-dkms$' && a=($(kernel_headers) "${a[@]}")
    for p in "${a[@]}"; do
        i=$((i + 1)); sub "$i/${#a[@]}"; msg "$p"
        pacman -Q "$p" &>/dev/null && continue
        have_variant "$p" && { echo "  $p — вже встановлено інший варіант (-git/-bin)"; continue; }
        "${YAY[@]}" -S "$p" && continue
        refresh_keyring && "${YAY[@]}" -S "$p" && continue
        fail_pkg "$p"
    done
    return 0
}

# ---------------------------------------------------------------------------
step_preflight() {
    msg "Користувач"; [ "$EUID" -ne 0 ] || { echo "Не запускай інсталятор від root"; return 1; }
    sub 1/4; msg "Блокування pacman"
    [ ! -f /var/lib/pacman/db.lck ] || { echo "pacman зайнятий (/var/lib/pacman/db.lck). Закрий інші оновлення."; return 1; }
    sub 2/4; msg "Права sudo"
    sudo true || { echo "sudo не дав доступу"; return 1; }
    sub 3/4; msg "Мережа"
    ping -c 1 -W 3 archlinux.org &>/dev/null || curl -fsS --max-time 8 -o /dev/null https://archlinux.org \
        || { echo "Немає інтернету"; return 1; }
    sub 4/4; echo "Система готова до встановлення"
}

step_yay() {
    if command -v yay &>/dev/null; then msg "yay вже є"; return 0; fi
    msg "base-devel, git"
    sudo pacman -S --needed --noconfirm base-devel git || return 1
    local d; d=$(mktemp -d)
    msg "Збираю yay з AUR"
    git clone --depth 1 https://aur.archlinux.org/yay-bin.git "$d/yay" && (cd "$d/yay" && makepkg -si --noconfirm)
    local r=$?; rm -rf "$d"; return $r
}

step_sysupdate() { msg "pacman -Syu"; sudo pacman -Syu --noconfirm; }
step_core()      { install_list 1 "${PACKAGES_CORE[@]}"; }
step_stable()    { install_list 0 "${PACKAGES_STABLE[@]}"; }
step_aur()       { install_list 0 "${PACKAGES_AUR[@]}"; }
step_apps()      { read -ra a <<< "${OPT_APPS:-}"; install_list 0 "${a[@]}"; }
step_utils()     { install_list 0 $(conf_group utils); }
step_dev()       { install_list 0 $(conf_group dev); }
step_drivers() {
    local list; mapfile -t list < <(drivers_here)
    # lib32-* (Steam/Wine) — лише якщо multilib увімкнено, інакше «target not found»
    grep -q '^\[multilib\]' /etc/pacman.conf || mapfile -t list < <(printf '%s\n' "${list[@]}" | grep -v '^lib32-')
    local found=()
    gpu_intel && found+=("Intel GPU"); gpu_amd && found+=("AMD GPU"); gpu_nvidia && found+=("NVIDIA GPU")
    hw_ok sof && found+=("звук Intel SOF"); hw_ok laptop && found+=("ноутбук")
    echo "Знайдено: ${found[*]:-нічого особливого}"
    [ ${#list[@]} -eq 0 ] && { msg "Під це залізо додаткових драйверів не треба"; return 0; }
    echo "Під це залізо: ${list[*]}"
    install_list 0 "${list[@]}"
}
# multilib (Steam, Wine, lib32-*) — вмикаємо, якщо він був у знімку
step_repos() {
    if [ "$(cat system_state/multilib.txt 2>/dev/null)" = 1 ] && ! grep -q '^\[multilib\]' /etc/pacman.conf; then
        msg "Вмикаю multilib у /etc/pacman.conf"
        bk /etc/pacman.conf
        sudo sed -i '/^#\[multilib\]/,/^#Include/ s/^#//' /etc/pacman.conf
    fi
    msg "Синхронізую бази пакетів"
    sudo pacman -Sy --noconfirm || { msg "Дзеркало не відповіло — повтор"; sleep 3; sudo pacman -Sy --noconfirm; }
    # свіжі ключі ДО встановлення: інакше частина пакетів падає на «marginal trust»
    refresh_keyring || true
}

# Немає шрифтів Apple SF (публічна версія інсталятора) — ставимо Inter і підміняємо SF на нього
_sf_fallback() {
    fc-list : family 2>/dev/null | grep -qiE 'SF Pro|SF Compact' && return 0
    msg "Шрифтів SF немає — ставлю схожий вільний Inter"
    install_list 0 inter-font
    bk ~/.config/fontconfig/conf.d/60-mgos-sf-fallback.conf
    mkdir -p ~/.config/fontconfig/conf.d
    cp gui/fonts-fallback.conf ~/.config/fontconfig/conf.d/60-mgos-sf-fallback.conf
    fc-cache -f >/dev/null 2>&1 || true
}

step_files() {
    msg "Теки"
    local d
    for d in ~/.config ~/.cache/wal ~/Pictures/wallpapers ~/GoogleDrive ~/.local/share/fonts; do [ -d "$d" ] || bk "$d"; done
    mkdir -p ~/.config ~/.cache/wal ~/Pictures/wallpapers ~/GoogleDrive ~/.local/share/fonts
    bk_list ~/.local/share/fonts
    if [ -d ./fonts ]; then
        msg "Шрифти"; cp -r ./fonts/* ~/.local/share/fonts/ 2>/dev/null || true
        msg "fc-cache"; bk ~/.cache/fontconfig; fc-cache -f >/dev/null 2>&1 || true
    fi
    _sf_fallback
}

step_configs() {
    [ -d ./configs ] || { echo "У інсталяторі немає ./configs"; return 1; }
    local list
    if [ -n "${OPT_CONFIGS:-}" ]; then read -ra list <<< "$OPT_CONFIGS"
    else mapfile -t list < <(find ./configs -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort); fi
    local i=0 c
    for c in "${list[@]}"; do
        i=$((i + 1)); sub "$i/${#list[@]}"; msg "$c"
        [ -d "./configs/$c" ] || continue
        bk ~/.config/"$c"                       # стара версія — у точці відкату
        rm -rf ~/.config/"$c"
        cp -r "./configs/$c" ~/.config/
        # знімок зроблено з домашньої теки автора — підміняємо шляхи у текстових файлах
        if [ "$SRC_HOME" != "$HOME" ]; then
            grep -rlI --null -F "$SRC_HOME" ~/.config/"$c" 2>/dev/null | xargs -0 -r sed -i "s|$SRC_HOME|$HOME|g"
        fi
    done
    echo "Старі конфіги збережено в точці відкату: $CACHE"
    if [ -d ~/.config/SYSui ]; then
        find ~/.config/SYSui/ -type s -delete 2>/dev/null || true
        chmod +x ~/.config/SYSui/*_osd ~/.config/SYSui/dock ~/.config/SYSui/topbar ~/.config/SYSui/wallpapers \
                 ~/.config/SYSui/controller_addon ~/.config/SYSui/*.sh 2>/dev/null || true
    fi
    [ -f ~/.config/megumin-os/sync_sddm.sh ] && chmod +x ~/.config/megumin-os/sync_sddm.sh
    # конфіг fish з mgOS замінив теку повністю — повертаємо функцію kiosk, якщо kioskOS встановлено
    _kiosk_installed && { bk ~/.config/fish/functions/kiosk.fish; _kiosk_shell_hooks; }
    return 0
}

step_home() {
    [ -d ./home ] || { msg "У знімку немає домашніх файлів"; return 0; }
    local f t
    while IFS= read -r -d '' f; do
        t="$HOME/${f#./home/}"
        bk "$t"                                   # стан до змін — у точку відкату
        mkdir -p "$(dirname "$t")"
        cp -a "$f" "$t"
        if grep -qI . "$t" 2>/dev/null; then
            [ "$SRC_HOME" != "$HOME" ] && sed -i "s|$SRC_HOME|$HOME|g" "$t"
            # «. $HOME/.local/bin/env» тощо — лише якщо файл існує (інакше помилка на кожен запуск shell)
            sed -i -E 's|^([[:space:]]*)\. "([^"]+)"[[:space:]]*$|\1[ -f "\2" ] \&\& . "\2"|' "$t"
        fi
        msg "${f#./home/}"
    done < <(find ./home -type f -print0)
    chmod +x ~/.local/bin/wal 2>/dev/null
    _kiosk_shell_hooks   # mgOS перезаписав ~/.bashrc — повертаємо команду kiosk, якщо kioskOS встановлено
    bk ~/.local/share/applications/mimeinfo.cache
    update-desktop-database ~/.local/share/applications 2>/dev/null || true
    return 0
}

# Автозапуски hyprland.conf, що ведуть у нікуди (програми немає) — коментуємо, щоб не було сміття.
step_cleanup() {
    local f=~/.config/hypr/hyprland.conf n=0 line cmd
    [ -f "$f" ] || return 0
    bk "$f"
    while IFS= read -r line; do
        cmd=$(sed -E 's/^exec-once[[:space:]]*=[[:space:]]*//; s/^\[[^]]*\][[:space:]]*//' <<< "$line" | awk '{print $1}')
        cmd=${cmd/#\~/$HOME}
        [ -z "$cmd" ] && continue
        local dead="" pth
        if [[ "$cmd" == /* ]]; then [ -e "$cmd" ] || dead=$cmd; else command -v "$cmd" &>/dev/null || dead=$cmd; fi
        # шляхи всередині команди (cd /mnt/... тощо), яких на цьому ПК немає
        if [ -z "$dead" ]; then
            for pth in $(grep -oE '(^|[[:space:]"])(/|~/)[^[:space:]"'"'"';&|]+' <<< "${line#*=}" | tr -d ' "'); do
                pth=${pth/#\~/$HOME}
                [[ "$pth" == /dev/* || "$pth" == /tmp/* ]] && continue
                [ -e "$pth" ] || { dead=$pth; break; }
            done
        fi
        [ -z "$dead" ] && continue
        cmd=$dead
        grep -nF -x -- "$line" "$f" >/dev/null || continue
        sed -i "\|^$(sed 's/[][\\.*^$|]/\\&/g' <<< "$line")\$|s|^|# [mgOS: немає $(basename "$cmd")] |" "$f"
        echo "вимкнено автозапуск: $cmd"; n=$((n + 1))
    done < <(grep -E '^exec-once' "$f")
    echo "Мертвих автозапусків прибрано: $n"
    return 0
}

step_build() {
    [ -x ~/.config/SYSui/build.sh ] || { msg "SYSui не встановлено — пропуск"; return 0; }
    bk ~/.config/SYSui
    msg "g++ — панелі під бібліотеки цієї системи"
    # build.sh не чіпає готові бінарники, якщо збірка впала
    bash ~/.config/SYSui/build.sh || { echo "Збірка впала — лишаю бінарники з інсталятора"; return 0; }
}

step_theme() {
    if [ -d ./cache_wal ]; then
        msg "Кольори pywal"
        bk ~/.cache/wal
        mkdir -p ~/.cache/wal; cp -r ./cache_wal/* ~/.cache/wal/ 2>/dev/null || true
        [ "$SRC_HOME" != "$HOME" ] && grep -rlI --null -F "$SRC_HOME" ~/.cache/wal 2>/dev/null | xargs -0 -r sed -i "s|$SRC_HOME|$HOME|g"
    fi
    sub 1/3
    if [ -d ./system_state ]; then
        msg "Стан системи"
        bk ~/.cache/ignis/last_wallpaper.txt; bk ~/.cache/megumin-os/dock_history.txt
        mkdir -p ~/.cache/ignis ~/.cache/megumin-os
        [ -f ./system_state/last_wallpaper.txt ] && sed "s|$SRC_HOME|$HOME|g" ./system_state/last_wallpaper.txt > ~/.cache/ignis/last_wallpaper.txt
        [ -f ./system_state/dock_history.txt ] && [ ! -f ~/.cache/megumin-os/dock_history.txt ] && cp ./system_state/dock_history.txt ~/.cache/megumin-os/
    fi
    sub 2/3
    if [ "${OPT_WALL:-0}" = 1 ] && [ -d ./wallpapers ]; then
        msg "Шпалери (це може зайняти час)"
        [ -d ~/Pictures/wallpapers ] && bk_list ~/Pictures/wallpapers || bk ~/Pictures/wallpapers
        mkdir -p ~/Pictures/wallpapers
        rsync -a ./wallpapers/ ~/Pictures/wallpapers/ 2>/dev/null || cp -rn ./wallpapers/* ~/Pictures/wallpapers/ 2>/dev/null || true
    fi
    sub 3/3; return 0
}

step_boot() {
    [ -d ./plymouth_theme ] || { msg "Немає теми Plymouth — пропуск"; return 0; }
    msg "Тема Plymouth"
    bk_plymouth; bk /usr/share/plymouth/themes/spinner
    sudo mkdir -p /usr/share/plymouth/themes/spinner/
    sudo cp -r ./plymouth_theme/* /usr/share/plymouth/themes/spinner/ 2>/dev/null || true
    sudo plymouth-set-default-theme spinner 2>/dev/null || true
    sub 1/2; msg "initramfs (dracut / mkinitcpio)"
    if command -v dracut &>/dev/null; then sudo dracut -f --regenerate-all 2>&1 | tail -3 || true
    elif command -v mkinitcpio &>/dev/null; then sudo mkinitcpio -P 2>&1 | tail -3 || true; fi
    return 0
}

step_scripts() {
    [ -d ./scripts ] || return 0
    local f
    for f in ./scripts/*; do [ -f "$f" ] && { msg "$(basename "$f")"; bk "/usr/local/bin/$(basename "$f")"; sudo install -m 755 "$f" /usr/local/bin/; }; done
    return 0
}

# ============================================================================
#  Меню сесій SDDM: лише «mgOS» і «kiosk»
#  Свої .desktop лежать у /usr/local/share/mgos-sessions, SDDM дивиться тільки туди
#  ([Wayland] SessionDir). Пакетні hyprland.desktop / hyprland-uwsm.desktop не чіпаємо —
#  оновлення Hyprland меню не зламає. Викликається і з mgOS (step_services), і з kioskOS.
# ============================================================================
MGOS_SESS_DIR=/usr/local/share/mgos-sessions
MGOS_SESS_CONF=/etc/sddm.conf.d/20-mgos-sessions.conf
_mgos_session_entry() {   # _mgos_session_entry <файл> <Name> <Comment> <Exec>
    bk "$MGOS_SESS_DIR/$1"
    printf '[Desktop Entry]\nName=%s\nComment=%s\nExec=%s\nType=Application\nDesktopNames=Hyprland\n' "$2" "$3" "$4" \
        | sudo install -Dm644 /dev/stdin "$MGOS_SESS_DIR/$1"
}
_mgos_sessions_menu() {
    msg "Меню сесій: mgOS і kiosk"
    # mgOS — звичайний Hyprland з ~/.config/hypr (як пакетний hyprland.desktop)
    if [ -f ~/.config/hypr/hyprland.conf ]; then
        local ex=/usr/bin/start-hyprland; [ -x "$ex" ] || ex=Hyprland
        _mgos_session_entry mgos.desktop "mgOS" "Megumin OS — повна оболонка, повна потужність" "$ex"
    fi
    if _kiosk_installed; then
        _mgos_session_entry kiosk.desktop "kiosk" "Енергоощадна kiosk-сесія (економія лише в ній)" "$KIOSK_SESSION_BIN"
        # стара назва пункту (попередня версія інсталятора)
        if [ -f /usr/share/wayland-sessions/kioskos.desktop ]; then bk /usr/share/wayland-sessions/kioskos.desktop; sudo rm -f /usr/share/wayland-sessions/kioskos.desktop; fi
    fi
    ls "$MGOS_SESS_DIR"/*.desktop &>/dev/null || { echo "Немає жодної сесії mgOS/kiosk — меню SDDM не змінюю"; return 0; }
    bk "$MGOS_SESS_CONF"
    printf '# mgOS: у меню вибору сесії — лише mgOS і kiosk\n[Wayland]\nSessionDir=%s\n' "$MGOS_SESS_DIR" \
        | sudo install -Dm644 /dev/stdin "$MGOS_SESS_CONF"
    # SDDM пам'ятає останню сесію за шляхом: стара (hyprland.desktop / kioskos.desktop) зникла з меню —
    # переводимо на відповідний новий пункт, щоб за замовчуванням не вибралось щось випадкове
    local st=/var/lib/sddm/state.conf new=""
    if sudo test -f "$st"; then
        if sudo grep -qE '^Session=.*/(hyprland|hyprland-uwsm)\.desktop$' "$st" && [ -f "$MGOS_SESS_DIR/mgos.desktop" ]; then new=$MGOS_SESS_DIR/mgos.desktop
        elif sudo grep -qE '^Session=.*/kioskos\.desktop$' "$st" && [ -f "$MGOS_SESS_DIR/kiosk.desktop" ]; then new=$MGOS_SESS_DIR/kiosk.desktop; fi
        if [ -n "$new" ]; then bk "$st"; sudo sed -i "s|^Session=.*|Session=$new|" "$st"; fi
    fi
    command -v sddm &>/dev/null || echo "SDDM не встановлено — меню з'явиться, коли буде SDDM; зараз вхід з TTY (kiosk / Hyprland)"
    echo "У меню сесій: $(cd "$MGOS_SESS_DIR" && grep -h '^Name=' *.desktop | cut -d= -f2 | paste -sd, | sed 's/,/, /g')"
}

step_services() {
    local s list=(power-profiles-daemon bluetooth NetworkManager)
    [ -f ./system_state/services.txt ] && mapfile -t -O ${#list[@]} list < ./system_state/services.txt
    for s in $(printf '%s\n' "${list[@]}" | sort -u); do
        [ "$s" = sddm.service ] && continue
        [ "$s" = fancontrol.service ] && [ ! -f /etc/fancontrol ] && continue   # конфіг під конкретну плату
        systemctl cat "$s" &>/dev/null || continue                                # пакет не встановлено
        msg "$s"; bk_service "$s"; sudo systemctl enable --now "$s" 2>/dev/null || echo "не вдалося увімкнути $s"
    done
    msg "sddm"; bk_service sddm.service; sudo systemctl enable -f sddm 2>/dev/null || true
    if [ -d ./sddm_theme ]; then
        msg "Тема SDDM"
        bk /usr/share/sddm/themes/sddm-astronaut-theme; bk /etc/sddm.conf.d
        sudo mkdir -p /usr/share/sddm/themes/sddm-astronaut-theme /etc/sddm.conf.d
        sudo rsync -a --delete ./sddm_theme/ /usr/share/sddm/themes/sddm-astronaut-theme/ 2>/dev/null \
            || sudo cp -r ./sddm_theme/* /usr/share/sddm/themes/sddm-astronaut-theme/
        printf "[Theme]\nCurrent=sddm-astronaut-theme\n" | sudo tee /etc/sddm.conf.d/theme.conf >/dev/null
    fi
    bk /etc/sddm.conf.d
    for s in ./system_state/etc/sddm.conf.d/*.conf; do [ -f "$s" ] && sudo install -m 644 "$s" /etc/sddm.conf.d/; done
    _mgos_sessions_menu
    if ls ./system_state/etc/udev/*.rules &>/dev/null; then
        msg "udev-правила"
        for s in ./system_state/etc/udev/*.rules; do bk "/etc/udev/rules.d/$(basename "$s")"; sudo install -m 644 "$s" /etc/udev/rules.d/; done
        sudo udevadm control --reload 2>/dev/null || true
    fi
    return 0
}

step_look() {
    if [ -d ./icons ]; then
        msg "Курсор"
        local d n
        for d in ./icons/*/; do
            n=$(basename "$d")
            if [ "$n" = default ]; then bk ~/.local/share/icons/default; mkdir -p ~/.local/share/icons/default; cp -r "$d". ~/.local/share/icons/default/
            elif [ ! -d "/usr/share/icons/$n" ]; then bk "/usr/share/icons/$n"; sudo cp -r "$d" "/usr/share/icons/$n"; fi
        done
    fi
    sub 1/3
    if [ -f ./system_state/gsettings.txt ]; then
        msg "GTK-тема, іконки, шрифти"
        local GS=(gsettings) schema key value
        [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && GS=(dbus-run-session -- gsettings)
        while read -r schema key value; do
            [ -n "$key" ] && bk_gsetting "$schema" "$key"
            [ -n "$key" ] && "${GS[@]}" set "$schema" "$key" "$value" 2>/dev/null || true
        done < ./system_state/gsettings.txt
    fi
    sub 2/3
    # Wallpapers/Settings викликають "sudo -n sync_sddm.sh"
    if [ -f ~/.config/megumin-os/sync_sddm.sh ]; then
        msg "sudoers для sync_sddm"
        local t; t=$(mktemp)
        printf '%s ALL=(ALL) NOPASSWD: %s\n' "$USER" "$HOME/.config/megumin-os/sync_sddm.sh" > "$t"
        bk /etc/sudoers.d/mgos-sddm-sync
        sudo visudo -cf "$t" >/dev/null 2>&1 && sudo install -m 440 -o root -g root "$t" /etc/sudoers.d/mgos-sddm-sync
        rm -f "$t"
    fi
    sub 3/3; return 0
}

step_shell() {
    [ "$SHELL" = /usr/bin/fish ] && { msg "fish вже за замовчуванням"; return 0; }
    install_list 0 fish
    msg "chsh → fish"; bk_shell; sudo chsh -s /usr/bin/fish "$USER"
}

step_gpu() {
    if gpu_nvidia; then echo "NVIDIA — лишаю env-змінні nvidia"; return 0; fi
    msg "Не NVIDIA — вимикаю nvidia-змінні в hyprland.conf"
    local f=~/.config/hypr/hyprland.conf
    [ -f "$f" ] && bk "$f"
    [ -f "$f" ] && sed -i -E 's/^(env = (LIBVA_DRIVER_NAME, nvidia|GBM_BACKEND, nvidia-drm|NVD_BACKEND,direct|WLR_NO_HARDWARE_CURSORS, 1))/# \1/' "$f"
    return 0
}

# ============================================================================
#  ЕКРАН, МАСШТАБ, ТАЧПАД — автоналаштування під залізо (і mgOS, і kioskOS)
#  Знімок містить екрани машини автора (eDP-1 2880x1800 ×2, Samsung M80D ×1.5) —
#  на іншому ПК це дрібний або гігантський інтерфейс. Тут:
#   • mgos-display (scripts/) рахує масштаб за щільністю пікселів з EDID і пише
#     monitor = … у <конфіг>/mgos-auto.conf; у сесії підхоплює нові монітори (watch)
#   • жести тачпада з kiosk (3 пальці: столи / Runner), плавний скрол, швидкість лише тачпада
#   • розкладки системи до us,ua; NumLock на ПК без батареї; великий шрифт TTY на HiDPI
#   • udev: тачпад і I2C-контролер без авто-сну (будь-який I2C-HID, не лише ELAN автора)
#  hyprland.conf / kiosk.conf лише доповнюється рядком source у кінці — знімок не ламається.
# ============================================================================
HW_DISPLAY_BIN=/usr/local/bin/mgos-display
HW_TOUCHPAD_RULE=/etc/udev/rules.d/99-touchpad-power.rules
HW_SRC_LINE='# mgOS: автоналаштування під це залізо (крок «Екран, масштаб, тачпад»)'

_hw_display_bin() {
    [ -f ./scripts/mgos-display ] || { echo "У інсталяторі немає scripts/mgos-display"; return 1; }
    cmp -s ./scripts/mgos-display "$HW_DISPLAY_BIN" && return 0
    bk "$HW_DISPLAY_BIN"; sudo install -Dm755 ./scripts/mgos-display "$HW_DISPLAY_BIN"
}
_hw_touchpads() { python3 ./scripts/mgos-display touchpads 2>/dev/null; }
_hw_laptop() { compgen -G '/sys/class/power_supply/BAT*' >/dev/null; }

# _hw_layouts "us,ua" → ті самі + розкладки системи (localectl) і мов локалей (uk_UA → ua), до 4 штук
_hw_layouts() {
    local out=() l x
    local -A seen=()
    _hw_add() { [ -n "$1" ] && [ -z "${seen[$1]:-}" ] && [ ${#out[@]} -lt 4 ] && [ -f "/usr/share/X11/xkb/symbols/$1" ] && { out+=("$1"); seen[$1]=1; }; }
    IFS=, read -ra x <<< "$1"; for l in "${x[@]}"; do l=${l// /}; [ -n "$l" ] && { out+=("$l"); seen[$l]=1; }; done
    IFS=, read -ra x <<< "$(localectl status 2>/dev/null | awk -F: '/X11 Layout/{gsub(/ /,"",$2); print $2}')"
    for l in "${x[@]}"; do _hw_add "$l"; done
    for l in $( { localectl status 2>/dev/null; echo "LANG=${LANG:-}"; } | grep -oE '(LANG|LC_[A-Z]+)=[a-z]{2,3}_' | sed -E 's/.*=//; s/_$//' | sort -u); do
        case $l in
            uk) l=ua ;; en) l=us ;; cs) l=cz ;; da) l=dk ;; el) l=gr ;; et) l=ee ;; sv) l=se ;; ja) l=jp ;;
            ko) l=kr ;; be) l=by ;; kk) l=kz ;; sr) l=rs ;; sl) l=si ;; he) l=il ;; ar) l=ara ;; fa) l=ir ;;
            zh) l=cn ;; vi) l=vn ;; nb|nn) l=no ;; ka) l=ge ;; hy) l=am ;; hi) l=in ;;
        esac
        _hw_add "$l"
    done
    (IFS=,; echo "${out[*]}")
}

# _hw_write <головний конфіг Hyprland> <профіль mgos|kiosk> — mgos-auto.conf поруч + source у кінці
_hw_write() {
    local main=$1 profile=$2 dir auto cur lay t tps=()
    dir=$(dirname "$main"); auto=$dir/mgos-auto.conf
    bk "$main"; bk "$auto"
    bash ./gui/register-app.sh 2>/dev/null || true   # ярлик + ~/.local/share/mgos/installer для exec-once
    # екрани машини автора зі знімка — вимикаємо (коментар лишається для довідки)
    sed -i -E 's/^([[:space:]]*monitor[[:space:]]*=)/# [mgOS: екрани — у mgos-auto.conf] \1/' "$main"
    # source — останнім рядком, щоб авто-налаштування мали останнє слово; повторний запуск не дублює
    sed -i -e "\|^$HW_SRC_LINE\$|d" -e '\|^source = ~/.config/[a-z]*/mgos-auto.conf$|d' "$main"
    sed -i -e :a -e '/^\n*$/{$d;N;ba' -e '}' "$main"
    printf '\n%s\nsource = %s\n' "$HW_SRC_LINE" "${auto/#$HOME/\~}" >> "$main"

    mapfile -t tps < <(_hw_touchpads)
    cur=$(grep -m1 -E '^[[:space:]]*kb_layout[[:space:]]*=' "$main" | sed -E 's/^[^=]*=[[:space:]]*//; s/[[:space:]]*(#.*)?$//')
    lay=$(_hw_layouts "${cur:-us}")
    {
        echo "# ━━ mgOS: автоналаштування під це залізо ━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "# Створено інсталятором $(date '+%Y-%m-%d %H:%M') (профіль $profile). При перевстановленні"
        echo "# файл пишеться заново. Свій масштаб екрана — у ~/.config/mgos/display.conf, напр.:"
        echo "#     eDP-1  1.5        або   Samsung  1   2560x1440@144"
        echo "# Перевірити, що вибрано: mgos-display detect"
        echo
        echo "# ── Екрани: масштаб за щільністю пікселів, рідна роздільність ──"
        python3 ./scripts/mgos-display conf --profile "$profile" 2>/dev/null || echo "monitor = , preferred, auto, auto"
        echo "# нові монітори (HDMI, док-станція) — масштаб підбирається сам при підключенні"
        echo "exec-once = $HW_DISPLAY_BIN watch --profile $profile"
        echo
        echo "# ── Інсталятор (~/.local/share/mgos/installer → тека репозиторію mgos) ──"
        echo "# перший вхід після встановлення — вікно «Все готово до роботи» (один раз)"
        echo "exec-once = ~/.local/share/mgos/installer/gui/welcome.sh"
        echo "# сповіщення «Є нова версія mgOS» (GitHub; вимкнути: touch ~/.config/mgos/no-update-notify)"
        echo "exec-once = ~/.local/share/mgos/installer/gui/update-notify.sh --loop"
        t=""
        [ -n "$lay" ] && [ "$lay" != "${cur:-us}" ] && t+="    kb_layout = $lay    # до розкладок знімка додано розкладки системи"$'\n'
        _hw_laptop || t+="    numlock_by_default = true    # ПК без батареї — NumLock увімкнено"$'\n'
        if [ -n "$t" ]; then echo; echo "# ── Клавіатура ──"; printf 'input {\n%s}\n' "$t"; fi
        if [ ${#tps[@]} -gt 0 ]; then
            echo
            echo "# ── Тачпад: швидкість і прискорення як у kiosk (миша лишається як була) ──"
            for t in "${tps[@]}"; do printf 'device {\n    name = %s\n    sensitivity = 0.25\n    accel_profile = adaptive\n}\n' "$t"; done
            [ "$profile" = mgos ] && echo "source = ~/.config/hypr/touchpad.conf    # жести 3 пальцями, плавний скрол"
        fi
    } > "$auto"
}

_hw_touchpad_power() {   # тачпад без авто-сну — нічого не обмежує, діє в обох сесіях
    [ -n "$(_hw_touchpads)" ] || { msg "Тачпада немає — правило udev не потрібне"; return 0; }
    msg "udev: тачпад без авто-сну (нуль затримки першого дотику)"
    local t dev drv; t=$(mktemp)
    cat > "$t" <<'EOF'
# mgOS / kioskOS: тачпад і його I2C-контролер без runtime-сну.
# Інакше перший дотик після паузи «прокидає» контролер (затримка 100–1000 мс) і рух рветься.
ACTION=="add|bind", SUBSYSTEM=="i2c", DRIVER=="i2c_hid_acpi", ATTR{power/control}="on"
ACTION=="add|bind", SUBSYSTEM=="platform", DRIVER=="i2c_designware", ATTR{power/control}="on"
ACTION=="add", SUBSYSTEM=="pci", ATTR{vendor}=="0x8086", ATTR{class}=="0x0c8000", ATTR{power/control}="on"
ACTION=="add", SUBSYSTEM=="i2c", ATTRS{name}=="ELAN0B00:00*", ATTR{power/control}="on"
EOF
    if ! cmp -s "$t" "$HW_TOUCHPAD_RULE"; then
        bk "$HW_TOUCHPAD_RULE"; sudo install -m 644 "$t" "$HW_TOUCHPAD_RULE"
        sudo udevadm control --reload 2>/dev/null || true
    fi
    rm -f "$t"
    # правило діє з наступного завантаження — зараз застосовуємо його ефект напряму (без udevadm trigger)
    for dev in /sys/bus/i2c/devices/* /sys/bus/platform/devices/*; do
        drv=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null)
        case $drv in i2c_hid_acpi|i2c_designware) ;; *) continue ;; esac
        [ -f "$dev/power/control" ] && echo on | sudo tee "$dev/power/control" >/dev/null
    done
    for dev in /sys/bus/pci/devices/*; do
        [ "$(cat "$dev/vendor" 2>/dev/null)" = 0x8086 ] && [ "$(cat "$dev/class" 2>/dev/null)" = 0x0c8000 ] \
            && [ -f "$dev/power/control" ] && echo on | sudo tee "$dev/power/control" >/dev/null
    done
    return 0
}

_hw_console_font() {   # HiDPI-екран ноутбука (масштаб ≥ 2): шрифт TTY ter-132b, інакше текст у консолі мікроскопічний
    local max
    max=$(python3 ./scripts/mgos-display detect --json 2>/dev/null \
          | python3 -c 'import json,sys; d=json.load(sys.stdin); print(max([p["scale"] for p in d if p["internal"]] or [0]))' 2>/dev/null)
    awk -v m="${max:-0}" 'BEGIN { exit !(m >= 2) }' || return 0
    if grep -q '^FONT=' /etc/vconsole.conf 2>/dev/null; then msg "Шрифт TTY уже задано у /etc/vconsole.conf"; return 0; fi
    msg "HiDPI: великий шрифт TTY"
    install_list 0 terminus-font
    [ -f /usr/share/kbd/consolefonts/ter-132b.psf.gz ] || return 0
    bk /etc/vconsole.conf
    echo "FONT=ter-132b" | sudo tee -a /etc/vconsole.conf >/dev/null
    echo "Шрифт TTY ter-132b — діє після перезавантаження"
}

step_hw() {
    _hw_display_bin || return 1
    msg "Екрани"
    python3 ./scripts/mgos-display detect --profile mgos 2>&1 | sed 's/^/  /'
    sub 1/4
    if [ -f ~/.config/hypr/hyprland.conf ]; then
        msg "Масштаб і режим екранів, розкладки (mgos-auto.conf)"
        _hw_write ~/.config/hypr/hyprland.conf mgos
        if [ -n "$(_hw_touchpads)" ]; then
            msg "Жести тачпада з kiosk (touchpad.conf)"
            bk ~/.config/hypr/touchpad.conf
            sed "s|@HOME@|$HOME|g" gui/hypr-touchpad.conf > ~/.config/hypr/touchpad.conf
            # libinput-gestures (xdotool) теж ловить 3 пальці — двічі спрацьовувало б; нативні жести плавніші
            sed -i -E 's/^(exec-once[[:space:]]*=[[:space:]]*libinput-gestures)/# [mgOS: жести — нативні Hyprland, touchpad.conf] \1/' ~/.config/hypr/hyprland.conf
        fi
    else
        msg "Конфіг Hyprland mgOS не встановлено — лише системні налаштування"
    fi
    sub 2/4; _hw_touchpad_power
    sub 3/4; _hw_console_font
    sub 4/4; return 0
}

step_kiosk_hw() {
    _hw_display_bin || return 1
    [ -f ~/.config/hyprland/kiosk.conf ] || { echo "Немає kiosk.conf — спершу крок «Конфіги kioskOS»"; return 1; }
    msg "Екрани"
    python3 ./scripts/mgos-display detect --profile kiosk 2>&1 | sed 's/^/  /'
    sub 1/3
    msg "Масштаб і режим екранів (~60 Гц — економія), розкладки"
    _hw_write ~/.config/hyprland/kiosk.conf kiosk
    sub 2/3; _hw_touchpad_power
    sub 3/3; _hw_console_font; return 0
}

# ---------- Оновлення ----------
# Аналіз без root: які пакети mgOS застарілі та яких бракує.
step_analyze() {
    local set; set=$( { mgos_set; cat ./pkglist.txt 2>/dev/null; } | sort -u)
    msg "Репозиторії (checkupdates)"
    checkupdates 2>/dev/null | awk '{print $1, $2, $4}' | while read -r n o v; do
        grep -qxF "$n" <<< "$set" && echo "@@OUT $n $o $v repo"
    done
    sub 1/3; msg "AUR (yay -Qua)"
    if command -v yay &>/dev/null; then
        yay -Qua 2>/dev/null | awk '{print $1, $2, $4}' | while read -r n o v; do
            grep -qxF "$n" <<< "$set" && echo "@@OUT $n $o $v aur"
        done
    fi
    sub 2/3; msg "Відсутні пакети"
    pacman -T $(mgos_set) 2>/dev/null | while read -r n; do echo "@@MISS $n"; done
    sub 3/3; return 0
}

step_upd_repo() { msg "pacman -Syu (безпечно лише повне оновлення)"; sudo pacman -Syu --noconfirm; }

step_upd_aur() {
    command -v yay &>/dev/null || return 0
    local set; set=$( { printf '%s\n' "${PACKAGES_AUR[@]}"; cat ./pkglist.txt 2>/dev/null; } | sort -u)
    local list; mapfile -t list < <(yay -Qua 2>/dev/null | awk '{print $1}' | grep -xF -f <(echo "$set"))
    [ ${#list[@]} -eq 0 ] && { msg "AUR-пакети mgOS актуальні"; return 0; }
    local i=0 p
    for p in "${list[@]}"; do
        i=$((i + 1)); sub "$i/${#list[@]}"; msg "$p"
        yay --noconfirm --answerdiff None --answerclean None --answeredit None --removemake --mflags --nocheck -S "$p" || fail_pkg "$p"
    done
    return 0
}

step_upd_missing() { install_list 0 $(mgos_set); }

# ---------- Перевірка системи (без root, нічого не змінює) ----------
# @@CHECK ok|warn|fail група|назва|деталі
step_check() {
    c() { echo "@@CHECK $1 $2|$3|$4"; }
    local m n x list

    msg "Пакети"; sub 1/6
    mapfile -t m < <(pacman -T "${PACKAGES_CORE[@]}" 2>/dev/null)
    [ ${#m[@]} -eq 0 ] && c ok "Пакети" "Ключові пакети" "усі ${#PACKAGES_CORE[@]} на місці" || c fail "Пакети" "Ключові пакети" "немає: ${m[*]}"
    mapfile -t m < <(pacman -T "${PACKAGES_STABLE[@]}" "${PACKAGES_AUR[@]}" 2>/dev/null)
    [ ${#m[@]} -eq 0 ] && c ok "Пакети" "Пакети mgOS" "усі на місці" || c warn "Пакети" "Пакети mgOS" "немає ${#m[@]}: ${m[*]}"
    mapfile -t m < <(pacman -T $(conf_group utils) 2>/dev/null)
    [ ${#m[@]} -eq 0 ] && c ok "Пакети" "Утиліти" "усі на місці" || c warn "Пакети" "Утиліти" "немає ${#m[@]}: ${m[*]}"
    mapfile -t m < <(pacman -T $(drivers_here) 2>/dev/null)
    [ ${#m[@]} -eq 0 ] && c ok "Пакети" "Драйвери під це залізо" "встановлені" || c warn "Пакети" "Драйвери під це залізо" "немає: ${m[*]}"
    n=$(checkupdates 2>/dev/null | wc -l)
    [ "$n" -eq 0 ] && c ok "Пакети" "Оновлення" "система актуальна" || { [ "$n" -gt 100 ] && x=fail || x=warn; c $x "Пакети" "Оновлення" "$n пакетів чекають оновлення"; }
    n=$(pacman -Qtdq 2>/dev/null | wc -l)
    [ "$n" -eq 0 ] && c ok "Пакети" "Сироти" "немає" || c warn "Пакети" "Сироти" "$n непотрібних залежностей (pacman -Qtdq)"
    n=$(du -sm /var/cache/pacman/pkg 2>/dev/null | awk '{print $1}')
    [ "${n:-0}" -lt 5000 ] && c ok "Пакети" "Кеш pacman" "${n:-0} МБ" || c warn "Пакети" "Кеш pacman" "$((n / 1024)) ГБ — можна почистити (paccache -r)"

    msg "Панелі SYSui"; sub 2/6
    if [ -d ~/.config/SYSui ]; then
        local bad=() down=()
        for x in "${SYSUI_BINS[@]}"; do
            { [ -x ~/.config/SYSui/$x ] && ! ldd ~/.config/SYSui/$x 2>/dev/null | grep -q 'not found'; } || bad+=("$x")
            [ "$x" = wallpapers ] && continue
            pgrep -x "$x" >/dev/null || down+=("$x")
        done
        [ ${#bad[@]} -eq 0 ] && c ok "Панелі SYSui" "Бінарники" "${#SYSUI_BINS[@]} модулів, бібліотеки знайдено" || c fail "Панелі SYSui" "Бінарники" "зламані: ${bad[*]}"
        if [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]; then
            [ ${#down[@]} -eq 0 ] && c ok "Панелі SYSui" "Запущені" "усі модулі працюють" || c warn "Панелі SYSui" "Запущені" "не працюють: ${down[*]}"
        fi
    else
        c fail "Панелі SYSui" "~/.config/SYSui" "немає — панелі не встановлено"
    fi

    msg "Конфіги"; sub 3/6
    local missing=() drift=0 d
    for d in ./configs/*/; do
        d=$(basename "$d")
        [ -d ~/.config/"$d" ] || { missing+=("$d"); continue; }
        # текстові файли, що відрізняються від знімка (бінарники зібрані локально — не рахуємо)
        n=$(diff -rq --exclude='*.sock' ./configs/"$d" ~/.config/"$d" 2>/dev/null | grep -c '^Files') || true
        drift=$((drift + n))
    done
    [ ${#missing[@]} -eq 0 ] && c ok "Конфіги" "Набір конфігів" "усі $(ls -d ./configs/*/ | wc -l) на місці" || c fail "Конфіги" "Набір конфігів" "немає: ${missing[*]}"
    [ "$drift" -eq 0 ] && c ok "Конфіги" "Відповідність знімку" "збігаються" || c warn "Конфіги" "Відповідність знімку" "$drift файлів відрізняються від знімка ($(cat system_state/snapshot_date.txt 2>/dev/null))"
    if [ -f ~/.config/megumin-os/settings.json ]; then
        python3 -c "import json,sys; json.load(open(sys.argv[1]))" ~/.config/megumin-os/settings.json 2>/dev/null \
            && c ok "Конфіги" "settings.json" "коректний" || c fail "Конфіги" "settings.json" "зіпсований JSON — налаштування не читаються"
    else c fail "Конфіги" "settings.json" "немає ~/.config/megumin-os/settings.json"; fi
    [ -x ~/.local/bin/wal ] && c ok "Конфіги" "Обгортка wal" "є (кольори з відео-шпалер)" || c warn "Конфіги" "Обгортка wal" "немає ~/.local/bin/wal — кольори з відео-шпалер можуть не оновлюватись"

    msg "Тема"; sub 4/6
    if [ -f ~/.cache/wal/colors.css ] && grep -q -- '--color5:' ~/.cache/wal/colors.css; then
        c ok "Тема" "Кольори pywal" "акцент $(grep -- '--color5:' ~/.cache/wal/colors.css | grep -oE '#[0-9a-fA-F]{6}')"
    else c fail "Тема" "Кольори pywal" "немає ~/.cache/wal/colors.css — панелі без акценту"; fi
    x=$(gsettings get org.gnome.desktop.interface cursor-theme 2>/dev/null | tr -d "'")
    { [ -d /usr/share/icons/$x ] || [ -d ~/.local/share/icons/$x ]; } && c ok "Тема" "Курсор" "$x" || c warn "Тема" "Курсор" "тема «$x» не знайдена"
    x=$(gsettings get org.gnome.desktop.interface icon-theme 2>/dev/null | tr -d "'")
    { [ -d /usr/share/icons/$x ] || [ -d ~/.local/share/icons/$x ]; } && c ok "Тема" "Іконки" "$x" || c warn "Тема" "Іконки" "тема «$x» не знайдена"
    list=""; x=$(fc-list : family 2>/dev/null)
    grep -q "JetBrainsMono Nerd" <<< "$x" || list+="JetBrainsMono Nerd "
    grep -qi "SF Pro" <<< "$x" || list+="SF Pro "
    [ -z "$list" ] && c ok "Тема" "Шрифти" "JetBrainsMono Nerd, SF Pro" || c warn "Тема" "Шрифти" "немає: $list"

    msg "Hyprland"; sub 5/6
    if command -v hyprctl &>/dev/null && [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]; then
        x=$(hyprctl configerrors 2>/dev/null | grep -v '^\s*$' | head -3 | tr '\n' ' ')
        [ -z "$x" ] && c ok "Hyprland" "Конфіг" "без помилок" || c fail "Hyprland" "Конфіг" "$x"
    fi
    local dead=() cmd line
    while IFS= read -r line; do
        cmd=$(sed -E 's/^exec-once[[:space:]]*=[[:space:]]*//; s/^\[[^]]*\][[:space:]]*//' <<< "$line" | awk '{print $1}')
        cmd=${cmd/#\~/$HOME}; [ -z "$cmd" ] && continue
        if [[ "$cmd" == /* ]]; then [ -e "$cmd" ] || dead+=("$(basename "$cmd")"); else command -v "$cmd" &>/dev/null || dead+=("$cmd"); fi
    done < <(grep -E '^exec-once' ~/.config/hypr/hyprland.conf 2>/dev/null)
    [ ${#dead[@]} -eq 0 ] && c ok "Hyprland" "Автозапуск" "усі програми існують" || c warn "Hyprland" "Автозапуск" "${#dead[@]} ведуть у нікуди: ${dead[*]}"
    local bdead=()
    while IFS= read -r cmd; do
        [ -z "$cmd" ] && continue
        x=${cmd/#\~/$HOME}
        if [[ "$x" == /* ]]; then [ -e "$x" ] || bdead+=("$(basename "$x")"); else command -v "$x" &>/dev/null || bdead+=("$x"); fi
    done < <(grep -E '^\$term|^bind[a-z]* *=' ~/.config/hypr/hyprland.conf 2>/dev/null | grep -E '^\$term|, exec,' \
             | sed -E 's/^\$term *= *//; s/.*, exec, *//' | awk '{print $1}' | grep -vE '^\$|^(echo|sh|bash|gdbus)$' | sort -u)
    [ ${#bdead[@]} -eq 0 ] && c ok "Hyprland" "Гарячі клавіші" "усі команди існують" || c warn "Hyprland" "Гарячі клавіші" "команд немає: ${bdead[*]}"
    # масштаб екранів: чи відповідає щільності пікселів
    if [ -x "$HW_DISPLAY_BIN" ] && [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]; then
        local prof=mgos; [ "${KIOSKOS:-}" = 1 ] && prof=kiosk
        while IFS='|' read -r n rec curs why; do
            [ -n "$n" ] || continue
            if awk -v a="$rec" -v b="$curs" 'BEGIN { d = a - b; exit !(d < 0.01 && d > -0.01) }'; then
                c ok "Hyprland" "Екран $n" "масштаб $rec ($why)"
            else c warn "Hyprland" "Екран $n" "масштаб $curs, під цей екран краще $rec ($why) — mgos-display apply"; fi
        done < <("$HW_DISPLAY_BIN" detect --json --profile $prof 2>/dev/null | python3 -c '
import json, sys
for p in json.load(sys.stdin):
    print("|".join([p["name"], p["scale_str"], str(p["current_scale"] if p["current_scale"] is not None else p["scale"]), p["why"]]))' 2>/dev/null)
    elif [ ! -x "$HW_DISPLAY_BIN" ]; then
        c warn "Hyprland" "Масштаб екранів" "mgos-display не встановлено — «Відновити» → Конфіги"
    fi
    if [ -n "$(_hw_touchpads)" ] && [ -f ~/.config/hypr/hyprland.conf ]; then
        grep -q '^gesture = 3, horizontal, workspace' ~/.config/hypr/touchpad.conf 2>/dev/null \
            && c ok "Hyprland" "Жести тачпада" "3 пальці: столи, Runner (як у kiosk)" \
            || c warn "Hyprland" "Жести тачпада" "не налаштовані — «Відновити» → Конфіги"
    fi

    msg "Система"; sub 6/6
    x=$(systemctl --failed --no-legend --plain 2>/dev/null | awk '{print $1}' | tr '\n' ' ')
    [ -z "$x" ] && c ok "Система" "Служби" "збоїв немає" || c fail "Система" "Служби" "впали: $x"
    x=$(systemctl --user --failed --no-legend --plain 2>/dev/null | awk '{print $1}' | tr '\n' ' ')
    [ -z "$x" ] && c ok "Система" "Служби користувача" "збоїв немає" || c warn "Система" "Служби користувача" "впали: $x"
    if [ -f ./system_state/services.txt ]; then
        list=""
        while read -r x; do systemctl cat "$x" &>/dev/null && ! systemctl is-enabled -q "$x" 2>/dev/null && list+="$x "; done < ./system_state/services.txt
        [ -z "$list" ] && c ok "Система" "Служби зі знімка" "увімкнені" || c warn "Система" "Служби зі знімка" "вимкнені: $list"
    fi
    systemctl is-enabled -q sddm 2>/dev/null && c ok "Система" "SDDM" "увімкнено" || c fail "Система" "SDDM" "вимкнено — не буде екрана входу"
    if [ -f "$MGOS_SESS_CONF" ]; then
        x=$(cd "$MGOS_SESS_DIR" 2>/dev/null && grep -h '^Name=' *.desktop 2>/dev/null | cut -d= -f2 | paste -sd, | sed 's/,/, /g')
        [ -f "$MGOS_SESS_DIR/mgos.desktop" ] && c ok "Система" "Меню сесій" "$x" || c fail "Система" "Меню сесій" "немає пункту mgOS (є: ${x:-нічого}) — «Відновити» → Системне"
    else c warn "Система" "Меню сесій" "стандартне (Hyprland) — «Відновити» → Системне додасть пункти mgOS і kiosk"; fi
    n=$(df --output=pcent / | tail -1 | tr -dc 0-9)
    [ "$n" -lt 85 ] && c ok "Система" "Диск /" "зайнято $n%" || { [ "$n" -ge 95 ] && x=fail || x=warn; c $x "Система" "Диск /" "зайнято $n% — мало місця"; }
    [ ! -f /var/lib/pacman/db.lck ] && c ok "Система" "База pacman" "не заблокована" || c warn "Система" "База pacman" "є db.lck — якщо pacman не працює, це завислий замок"
    if [ "$(cat system_state/multilib.txt 2>/dev/null)" = 1 ]; then
        grep -q '^\[multilib\]' /etc/pacman.conf && c ok "Система" "multilib" "увімкнено" || c warn "Система" "multilib" "вимкнено — Steam/Wine не встановляться"
    fi
    n=$(journalctl -b -p err --no-pager -q 2>/dev/null | wc -l)
    [ "$n" -lt 50 ] && c ok "Система" "Журнал" "$n помилок з моменту завантаження" || c warn "Система" "Журнал" "$n помилок з моменту завантаження (journalctl -b -p err)"

    # kioskOS поруч із mgOS: економія kiosk не повинна діяти поза kiosk-сесією
    if _kiosk_installed; then
        x=$(kioskos-mode status 2>/dev/null)
        if grep -q аварійно <<< "$x"; then c fail "kioskOS" "Стан kiosk" "kiosk завершився аварійно, його тема/налаштування ще діють — kioskos-mode guard"
        elif grep -q 'kiosk активний' <<< "$x"; then c ok "kioskOS" "Стан kiosk" "зараз іде kiosk-сесія"
        else c ok "kioskOS" "Стан kiosk" "неактивний — економія й тема kiosk вимкнені"; fi
        if ! grep -q 'kiosk активний' <<< "$x" && ls /var/lib/kioskos/*.ppd &>/dev/null; then
            c warn "kioskOS" "Профіль живлення" "лишився профіль з kiosk ($(powerprofilesctl get 2>/dev/null)); до kiosk був $(cat /var/lib/kioskos/*.ppd | head -1) — повернеться при перезавантаженні"
        else c ok "kioskOS" "Профіль живлення" "$(powerprofilesctl get 2>/dev/null) — kiosk його не тримає"; fi
        systemctl is-enabled -q kioskos-ppd-guard 2>/dev/null && [ -L ~/.config/systemd/user/default.target.wants/kioskos-guard.service ] \
            && c ok "kioskOS" "Захист після збою" "увімкнено" || c warn "kioskOS" "Захист після збою" "kioskos-ppd-guard / kioskos-guard не увімкнені — встанови kioskOS ще раз"
        [ -e ~/.config/systemd/user/default.target.wants/rclone-gdrive.service ] && grep -q 'rclone mount' ~/.config/hypr/hyprland.conf 2>/dev/null \
            && c warn "kioskOS" "Google Drive" "монтується двічі (rclone-gdrive.service і hyprland.conf) — systemctl --user disable rclone-gdrive" \
            || c ok "kioskOS" "Google Drive" "одне монтування на сесію"
        [ -f /usr/lib/firefox/kioskos.cfg ] && c ok "kioskOS" "Firefox" "kiosk-налаштування лише при KIOSKOS=1" \
            || c warn "kioskOS" "Firefox" "немає kioskos.cfg"
    fi
    return 0
}

# ---------- Перевірка ----------
step_verify() {
    local fail=0 total=0
    chk() { local n=$1; shift; total=$((total + 1))
        if "$@" >/dev/null 2>&1; then echo "@@CHECK ok $n"; else echo "@@CHECK fail $n"; fail=$((fail + 1)); fi; }
    local c
    for c in hyprctl ignis dunst awww mpvpaper socat nmcli wpctl grim slurp cliphist; do chk "$c" command -v "$c"; done
    chk "python3 + PyQt6" python3 -c "import PyQt6.QtWidgets"
    if [ -d ~/.config/SYSui ]; then
        local b
        for b in "${SYSUI_BINS[@]}"; do
            chk "SYSui/$b" bash -c "[ -x ~/.config/SYSui/$b ] && ! ldd ~/.config/SYSui/$b | grep -q 'not found'"
        done
    fi
    [ -f ~/.config/hypr/hyprland.conf ] && [ "$SRC_HOME" != "$HOME" ] && chk "hyprland.conf (шляхи)" bash -c "! grep -q '$SRC_HOME/' ~/.config/hypr/hyprland.conf"
    # SDDM вмикає лише крок «Служби»; якщо його не обирали — це попередження, а не помилка
    total=$((total + 1))
    if systemctl is-enabled sddm >/dev/null 2>&1; then echo "@@CHECK ok SDDM увімкнено"
    else echo "@@CHECK warn SDDM вимкнено (вхід не через SDDM)"; fi
    chk "пункт «mgOS» у меню сесій" test -f "$MGOS_SESS_DIR/mgos.desktop" -a -f "$MGOS_SESS_CONF"
    chk "автомасштаб екранів (mgos-display)" test -x "$HW_DISPLAY_BIN"
    [ -f ~/.config/hypr/hyprland.conf ] && chk "mgos-auto.conf підключено" bash -c "test -f ~/.config/hypr/mgos-auto.conf && grep -q '^source = ~/.config/hypr/mgos-auto.conf' ~/.config/hypr/hyprland.conf"
    [ -n "$(_hw_touchpads)" ] && [ -f ~/.config/hypr/hyprland.conf ] && chk "жести тачпада (touchpad.conf)" grep -q '^gesture = 3, horizontal, workspace' ~/.config/hypr/touchpad.conf
    echo "Перевірок: $total, з помилками: $fail"
    return 0
}

# ============================================================================
#  kioskOS — окремий режим: енергоощадна kiosk-сесія Hyprland
#  (знімок з kiosk-машини: kiosk/sync_kiosk.sh → kiosk/files; логіка сесії — kiosk/session)
#
#  СПІВІСНУВАННЯ З mgOS: kioskOS нічого не вмикає глобально. Профіль power-saver,
#  тема Graphite, gsettings, маска localsearch, Google Drive і Firefox-налаштування
#  діють ЛИШЕ всередині kiosk-сесії (kiosk-session + kioskos-mode) і повертаються
#  при виході — навіть якщо kiosk впав або ПК вимкнули (kioskos-guard, kioskos-ppd-guard).
#  Змінні від GUI/TUI: OPT_UPDATE, OPT_KIOSK_THEME, OPT_KIOSK_EXT, OPT_KIOSK_KERNEL
# ============================================================================
KIOSK_DIR=./kiosk/files
KIOSK_SESS_SRC=./kiosk/session
KIOSK_SRC_HOME=$(cat "$KIOSK_DIR/source_home.txt" 2>/dev/null || echo /home/doki)
# Без цього сесія не запуститься або зламаються бінди/панель
KIOSK_CORE=(
    hyprland xdg-desktop-portal-hyprland waybar wofi kitty firefox
    pipewire pipewire-pulse wireplumber python jq libnotify dunst dconf
    brightnessctl playerctl grim slurp wl-clipboard hyprpicker
    power-profiles-daemon networkmanager
    gtk4 gtk4-layer-shell yyjson base-devel pkgconf plocate xdg-utils
    ttf-cascadia-code-nerd ttf-nerd-fonts-symbols
)
# Бажані: файли, Google Drive, монітор, Bluetooth, VA-API-діагностика
KIOSK_EXTRA=(nautilus gvfs btop bluez bluez-utils bluetuith rclone fuse3 libva-utils firefox-i18n-uk)
KIOSK_SESSION_BIN=/usr/local/bin/kiosk-session
KIOSK_MODE_BIN=/usr/local/bin/kioskos-mode
KIOSK_SESSION_DESKTOP=/usr/local/share/mgos-sessions/kiosk.desktop
KIOSK_TOUCHPAD_RULE=/etc/udev/rules.d/99-touchpad-power.rules
KIOSK_PPD_GUARD=/etc/systemd/system/kioskos-ppd-guard.service
KIOSK_FF_CFG=/usr/lib/firefox/kioskos.cfg
KIOSK_FF_AUTOCONFIG=/usr/lib/firefox/defaults/pref/kioskos-autoconfig.js
KIOSK_HSESS=~/.config/hyprland/kioskOS/session   # що kioskos-mode вмикає на час kiosk-сесії

_kiosk_has() { [ -e "$KIOSK_DIR/$1" ]; }
_kiosk_installed() { [ -f ~/.config/hyprland/kiosk.conf ]; }
_intel_gpu()  { gpu_intel; }
_nvidia_gpu() { gpu_nvidia; }
# шляхи автора знімка → $HOME цього користувача (лише текстові файли)
_kiosk_paths() {
    [ "$KIOSK_SRC_HOME" = "$HOME" ] && return 0
    grep -rlI --null -F "$KIOSK_SRC_HOME" "$@" 2>/dev/null | xargs -0 -r sed -i "s|$KIOSK_SRC_HOME|$HOME|g"
}
# курсор kiosk: 'default' у знімку → реальна тема, яку успадковував default/index.theme
_kiosk_cursor() {
    local c; c=$(awk -F= '/^Inherits=/{print $2}' "$KIOSK_DIR/icons/default/index.theme" 2>/dev/null)
    echo "${c:-Bibata-Modern-Ice}"
}

# Команда kiosk у bash і fish. Викликається і з кроків mgOS (step_home/step_configs),
# бо mgOS перезаписує ~/.bashrc і ~/.config/fish — без цього kiosk «губився».
_kiosk_shell_hooks() {
    _kiosk_installed || return 0
    # kiosk-session — якщо kioskOS встановлено цим інсталятором; інакше (kiosk лише вручну) — як було
    local cmd="Hyprland -c ~/.config/hyprland/kiosk.conf"
    [ -x "$KIOSK_SESSION_BIN" ] && cmd=$KIOSK_SESSION_BIN
    sed -i -E '/^alias kiosk=/d; /^# kioskOS — енергоощадна сесія Hyprland/d' ~/.bashrc 2>/dev/null
    printf '\n# kioskOS — енергоощадна сесія Hyprland (з TTY: kiosk)\nalias kiosk="%s"\n' "$cmd" >> ~/.bashrc
    if command -v fish &>/dev/null || [ -d ~/.config/fish ]; then
        mkdir -p ~/.config/fish/functions
        printf 'function kiosk --description "kioskOS — енергоощадна сесія Hyprland"\n    %s $argv\nend\n' "$cmd" \
            > ~/.config/fish/functions/kiosk.fish
    fi
}

step_kiosk_pkgs() {
    _kiosk_has config/hyprland/kiosk.conf || { echo "У інсталяторі немає kiosk/files — спершу запусти kiosk/sync_kiosk.sh на kiosk-машині"; return 1; }
    # без оновлення системи бази можуть бути застарілі (так само робить крок repos у mgOS)
    [ "${OPT_UPDATE:-0}" = 1 ] || { msg "Синхронізую бази пакетів"; sudo pacman -Sy --noconfirm || return 1; }
    install_list 1 "${KIOSK_CORE[@]}"
}

step_kiosk_extra() {
    local list=("${KIOSK_EXTRA[@]}")
    # VA-API для Intel Iris Xe: без iHD-драйвера Firefox декодує відео процесором
    # (на kiosk-машині автора драйвера не було — vainfo падав, відео йшло на CPU)
    _intel_gpu && list+=(intel-media-driver)
    # Super+C запускає code — ставимо VS Code, лише якщо його ще немає
    command -v code &>/dev/null || list+=(visual-studio-code-bin)
    install_list 0 "${list[@]}"
}

step_kiosk_files() {
    _kiosk_has config/hyprland/kiosk.conf || { echo "Немає kiosk/files/config/hyprland"; return 1; }
    msg "Hyprland kiosk (~/.config/hyprland — окремо від ~/.config/hypr mgOS)"
    bk ~/.config/hyprland
    mkdir -p ~/.config
    rm -rf ~/.config/hyprland
    cp -a "$KIOSK_DIR/config/hyprland" ~/.config/
    ln -sfn ../kiosk ~/.config/hyprland/kioskOS/kiosk
    _kiosk_paths ~/.config/hyprland
    chmod +x ~/.config/hyprland/*.sh ~/.config/hyprland/kiosk/*.sh ~/.config/hyprland/kiosk-dock/start-dock.sh \
             ~/.config/hyprland/kiosk-dock/dock ~/.config/hyprland/kioskOS/runner/runner \
             ~/.config/hyprland/kioskOS/runner/start-runner.sh 2>/dev/null || true

    # Змінні лише цієї сесії: KIOSKOS=1 (Firefox), свій курсор (icons/default mgOS не чіпаємо), NVIDIA як у mgOS
    msg "Змінні kiosk-сесії (session.conf)"
    local cur; cur=$(_kiosk_cursor)
    {
        echo "# kioskOS (інсталятор): змінні ЛИШЕ kiosk-сесії — інші сесії їх не бачать"
        echo "env = KIOSKOS, 1"
        echo "env = XCURSOR_THEME, $cur"
        echo "env = XCURSOR_SIZE, 24"
        if _nvidia_gpu; then
            echo "# NVIDIA — ті самі змінні, що й у mgOS hyprland.conf"
            echo "env = LIBVA_DRIVER_NAME, nvidia"
            echo "env = GBM_BACKEND, nvidia-drm"
            echo "env = NVD_BACKEND, direct"
        fi
    } > ~/.config/hyprland/kioskOS/session.conf
    grep -q 'kioskOS/session.conf' ~/.config/hyprland/kiosk.conf \
        || printf '\n# kioskOS (інсталятор): змінні сесії\nsource = ~/.config/hyprland/kioskOS/session.conf\n' >> ~/.config/hyprland/kiosk.conf
    # Nautilus без фризів — лише в kiosk (у mgOS лишаються підрахунок файлів і пошук)
    mkdir -p "$KIOSK_HSESS"
    grep '^org.gnome.nautilus.preferences ' "$KIOSK_DIR/gsettings.txt" > "$KIOSK_HSESS/gsettings.txt" 2>/dev/null || true
    sub 1/5

    msg "Waybar kiosk + анімований модуль гучності"
    [ -d ~/.config/waybar ] || bk ~/.config/waybar
    mkdir -p ~/.config/waybar
    local f
    for f in kiosk.jsonc kiosk.css volume.py; do
        bk ~/.config/waybar/"$f"
        cp -a "$KIOSK_DIR/config/waybar/$f" ~/.config/waybar/
    done
    _kiosk_paths ~/.config/waybar/kiosk.jsonc ~/.config/waybar/kiosk.css ~/.config/waybar/volume.py
    chmod +x ~/.config/waybar/volume.py
    sub 2/5

    # dunst: якщо вже налаштований (наприклад, mgOS) — не чіпаємо, лише доставляємо music_notify.sh
    if _kiosk_has config/dunst; then
        if [ ! -d ~/.config/dunst ]; then
            msg "Dunst"; bk ~/.config/dunst
            cp -a "$KIOSK_DIR/config/dunst" ~/.config/
            _kiosk_paths ~/.config/dunst
        elif [ ! -f ~/.config/dunst/music_notify.sh ]; then
            bk ~/.config/dunst/music_notify.sh
            cp -a "$KIOSK_DIR/config/dunst/music_notify.sh" ~/.config/dunst/
        fi
        chmod +x ~/.config/dunst/*.sh 2>/dev/null || true
    fi
    sub 3/5

    if _kiosk_has bin/carpeted; then
        msg "carpeted (килимок Super+R)"
        bk ~/.local/bin/carpeted
        install -Dm755 "$KIOSK_DIR/bin/carpeted" ~/.local/bin/carpeted
    fi
    sub 4/5

    msg "Команда kiosk (запуск із TTY)"
    bk ~/.bashrc; bk ~/.config/fish/functions/kiosk.fish
    _kiosk_shell_hooks
    sub 5/5
    echo "Конфіги kioskOS на місці. Старі — у точці відкату: $CACHE"
}

step_kiosk_build() {
    local R=~/.config/hyprland/kioskOS/runner D=~/.config/hyprland/kiosk-dock flags
    flags=$(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0 2>/dev/null) \
        || { echo "pkg-config не знайшов gtk4 / gtk4-layer-shell — лишаю готові бінарники"; return 0; }
    if [ -f "$R/runner.cpp" ]; then
        msg "Spotlight Runner (g++ під бібліотеки цієї системи)"
        # shellcheck disable=SC2086
        if g++ -O3 -std=c++17 -s "$R/runner.cpp" -o "$R/runner.new" $flags; then mv -f "$R/runner.new" "$R/runner"
        else rm -f "$R/runner.new"; echo "⚠ runner не зібрався — лишаю бінарник з інсталятора"; fi
    fi
    sub 1/2
    if [ -f "$D/dock.cpp" ]; then
        msg "Kiosk Dock (g++)"
        # shellcheck disable=SC2086
        if g++ -O3 -std=c++17 -s "$D/dock.cpp" -o "$D/dock.new" $flags -lyyjson; then mv -f "$D/dock.new" "$D/dock"
        else rm -f "$D/dock.new"; echo "⚠ dock не зібрався — лишаю бінарник з інсталятора"; fi
    fi
    chmod +x "$R/runner" "$D/dock" 2>/dev/null || true
    sub 2/2; return 0
}

# kioskos.cfg із user.js: у kiosk-сесії (KIOSKOS=1) — налаштування kiosk; в інших (mgOS) —
# значення, які виставив kiosk, скидаються до стандартних. Один профіль, вкладки й паролі спільні.
_kiosk_ff_cfg() {
    python3 - "$1" <<'PY'
import json, re, sys
items = []
for line in open(sys.argv[1], encoding="utf-8"):
    m = re.match(r'\s*user_pref\(\s*"([^"]+)"\s*,\s*(.+?)\s*\)\s*;', line)
    if m:
        items.append((m.group(1), json.loads(m.group(2))))
print("// kioskOS autoconfig — перший рядок Firefox пропускає")
print("// Згенеровано інсталятором mgOS із kiosk/files/firefox/user.js. Не редагуй вручну.")
print("// KIOSKOS=1 (kiosk-сесія): RAM-кеш, VA-API, авто-сон вкладок, кінетичний скрол.")
print("// Інші сесії: якщо значення лишилось від kiosk — повертаємо стандартне, щоб не обмежувати ПК.")
print('var kioskos = (getenv("KIOSKOS") == "1");')
print("var kioskosPrefs = " + json.dumps(items, ensure_ascii=False) + ";")
print("for (var i = 0; i < kioskosPrefs.length; i++) {")
print("  var n = kioskosPrefs[i][0], v = kioskosPrefs[i][1];")
print("  try {")
print("    if (kioskos) pref(n, v);")
print("    else if (getPref(n) === v) clearPref(n);")
print("  } catch (e) {}")
print("}")
PY
}

step_kiosk_firefox() {
    local uj="$KIOSK_DIR/firefox/user.js"
    [ -f "$uj" ] || { echo "Немає kiosk/files/firefox/user.js"; return 1; }
    [ -d /usr/lib/firefox ] || { echo "Firefox не встановлено в /usr/lib/firefox"; return 1; }

    # 1) autoconfig: налаштування kiosk лише при KIOSKOS=1
    msg "Firefox: kiosk-налаштування лише в kiosk-сесії (autoconfig)"
    local other
    other=$(grep -l 'general.config.filename' /usr/lib/firefox/defaults/pref/*.js 2>/dev/null | grep -v kioskos-autoconfig.js)
    if [ -n "$other" ]; then
        echo "⚠ Firefox уже має свій autoconfig ($other) — kiosk-налаштування Firefox не встановлено"
    else
        local t; t=$(mktemp)
        _kiosk_ff_cfg "$uj" > "$t" || { rm -f "$t"; echo "⚠ не вдалося згенерувати kioskos.cfg"; return 1; }
        bk "$KIOSK_FF_CFG"; bk "$KIOSK_FF_AUTOCONFIG"
        sudo install -Dm644 "$t" "$KIOSK_FF_CFG"
        printf '// kioskOS: підключення kioskos.cfg\npref("general.config.filename", "kioskos.cfg");\npref("general.config.obscure_value", 0);\n' \
            | sudo install -Dm644 /dev/stdin "$KIOSK_FF_AUTOCONFIG"
        rm -f "$t"
    fi
    # стара схема (діяла в усіх сесіях) — прибираємо
    local old=/usr/lib/firefox/browser/defaults/preferences/kioskos.js
    if [ -f "$old" ]; then bk "$old"; sudo rm -f "$old"; fi
    sub 1/3

    # 2) user.js у профілях діє ЗАВЖДИ (і в mgOS) — kiosk-копію знімаємо, її замінив autoconfig
    msg "Профілі Firefox: прибираю глобальний kiosk user.js"
    local n=0 base p
    for base in ~/.mozilla/firefox ~/.config/mozilla/firefox; do
        [ -f "$base/profiles.ini" ] || continue
        while IFS= read -r p; do
            [[ "$p" = /* ]] || p="$base/$p"
            [ -f "$p/user.js" ] || continue
            if cmp -s "$p/user.js" "$uj"; then bk "$p/user.js"; rm -f "$p/user.js"; n=$((n + 1))
            elif grep -q 'browser.cache.memory.capacity", 524288' "$p/user.js"; then
                echo "⚠ $p/user.js містить kiosk-налаштування, але змінений вручну — лишаю як є (він діє і в mgOS)"
            fi
        done < <(awk -F= '/^Path=/{print substr($0, 6)}' "$base/profiles.ini")
    done
    [ $n -gt 0 ] && echo "Знято kiosk user.js з профілів: $n (налаштування тепер вмикаються лише в kiosk)"
    sub 2/3

    # 3) розширення через політики (додаються, нічого не обмежують — діють в обох сесіях)
    if [ "${OPT_KIOSK_EXT:-1}" = 1 ] && [ -f ./kiosk/firefox-extensions.txt ]; then
        msg "Розширення Firefox (policies.json)"
        local pol=/etc/firefox/policies/policies.json t; t=$(mktemp -d)
        bk "$pol"
        sudo cat "$pol" > "$t/in" 2>/dev/null || : > "$t/in"
        python3 - "$t/in" "$t/out" ./kiosk/firefox-extensions.txt <<'PY' || { rm -rf "$t"; echo "⚠ policies.json не оновлено"; return 0; }
import json, sys
src, dst, lst = sys.argv[1:4]
try:
    d = json.load(open(src))
except Exception:
    d = {}
ext = d.setdefault("policies", {}).setdefault("ExtensionSettings", {})
for line in open(lst, encoding="utf-8"):
    line = line.split("#", 1)[0].split()
    if len(line) < 2:
        continue
    gid, slug = line[0], line[1]
    ext.setdefault(gid, {
        "installation_mode": "normal_installed",
        "install_url": f"https://addons.mozilla.org/firefox/downloads/latest/{slug}/latest.xpi",
    })
json.dump(d, open(dst, "w"), indent=2, ensure_ascii=False)
PY
        sudo install -Dm644 "$t/out" "$pol"
        rm -rf "$t"
    fi
    sub 3/3; return 0
}

step_kiosk_theme() {
    msg "Іконки MacTahoe та курсор"
    mkdir -p ~/.local/share/icons
    local d n
    for d in "$KIOSK_DIR"/icons/*/; do
        [ -d "$d" ] || continue
        n=$(basename "$d")
        case $n in
            default) ;;   # курсор kiosk задається змінними сесії — default/index.theme лишається за mgOS
            MacTahoe*) bk ~/.local/share/icons/"$n"; rm -rf ~/.local/share/icons/"$n"; cp -a "$d" ~/.local/share/icons/"$n" ;;
            *) [ -d "/usr/share/icons/$n" ] || [ -d ~/.local/share/icons/"$n" ] || { bk "/usr/share/icons/$n"; sudo cp -a "$d" "/usr/share/icons/$n"; } ;;
        esac
    done
    gtk-update-icon-cache -f -t ~/.local/share/icons/MacTahoe >/dev/null 2>&1 || true
    sub 1/3

    if [ -d "$KIOSK_DIR/fonts" ]; then
        msg "Шрифт SF Compact Display"
        mkdir -p ~/.local/share/fonts; bk_list ~/.local/share/fonts
        cp --update=none "$KIOSK_DIR"/fonts/* ~/.local/share/fonts/ 2>/dev/null || true
        fc-cache -f >/dev/null 2>&1 || true
    fi
    _sf_fallback
    sub 2/3

    # Тема НЕ застосовується глобально: kioskos-mode вмикає її при вході в kiosk і повертає тему mgOS при виході
    msg "Тема Graphite для kiosk-сесії"
    local T=~/.config/hyprland/kiosk
    [ -f "$T/graphite.css" ] || { echo "Немає $T/graphite.css — спершу крок «Конфіги kioskOS»"; return 1; }
    mkdir -p "$KIOSK_HSESS/gtk-3.0" "$KIOSK_HSESS/gtk-4.0"
    ln -sfn "$T/graphite.css" "$KIOSK_HSESS/gtk-4.0/gtk.css"
    ln -sfn "$T/graphite.css" "$KIOSK_HSESS/gtk-4.0/gtk-dark.css"
    ln -sfn "$T/gtk-3.0.css"  "$KIOSK_HSESS/gtk-3.0/gtk.css"
    local v
    for v in gtk-3.0 gtk-4.0; do
        if [ -f "$KIOSK_DIR/gtk/$v-settings.ini" ]; then cp "$KIOSK_DIR/gtk/$v-settings.ini" "$KIOSK_HSESS/$v/settings.ini"
        else cp "$T/settings.ini" "$KIOSK_HSESS/$v/settings.ini"; fi
    done
    # gsettings інтерфейсу (темний режим, іконки, шрифт, курсор) — теж лише на час сесії
    local cur; cur=$(_kiosk_cursor)
    { grep -v '^org.gnome.desktop.interface ' "$KIOSK_HSESS/gsettings.txt" 2>/dev/null
      grep '^org.gnome.desktop.interface ' "$KIOSK_DIR/gsettings.txt" | sed "s/ cursor-theme 'default'$/ cursor-theme '$cur'/"
    } > "$KIOSK_HSESS/gsettings.new" && mv -f "$KIOSK_HSESS/gsettings.new" "$KIOSK_HSESS/gsettings.txt"
    # закладка Google Drive у бічній панелі — лише додається
    bk ~/.config/gtk-3.0/bookmarks
    mkdir -p ~/.config/gtk-3.0
    grep -q GoogleDrive ~/.config/gtk-3.0/bookmarks 2>/dev/null || echo "file://$HOME/GoogleDrive Google Drive" >> ~/.config/gtk-3.0/bookmarks
    sub 3/3; return 0
}

step_kiosk_system() {
    # тачпад без авто-сну — правило спільне з mgOS (будь-який I2C-HID тачпад, не лише ELAN автора)
    _hw_touchpad_power
    sub 1/5

    # Профіль живлення НЕ змінюємо глобально: power-saver тримає kiosk-session лише поки працює kiosk
    msg "power-profiles-daemon (профіль mgOS не чіпаємо)"
    if systemctl is-enabled -q tlp 2>/dev/null || systemctl is-enabled -q auto-cpufreq 2>/dev/null; then
        echo "⚠ увімкнено tlp/auto-cpufreq — вони конфліктують з power-profiles-daemon; перемикання режимів може не працювати"
    fi
    bk_service power-profiles-daemon.service
    sudo systemctl enable --now power-profiles-daemon 2>/dev/null || echo "⚠ не вдалося увімкнути power-profiles-daemon"
    sub 2/5

    msg "Перемикач сесії kioskos-mode + kiosk-session"
    bk "$KIOSK_MODE_BIN"; bk "$KIOSK_SESSION_BIN"; bk /var/lib/kioskos
    sudo install -Dm755 "$KIOSK_SESS_SRC/kioskos-mode" "$KIOSK_MODE_BIN"
    sudo install -Dm755 "$KIOSK_SESS_SRC/kiosk-session" "$KIOSK_SESSION_BIN"
    sudo install -d -m 755 -o "$USER" /var/lib/kioskos
    # пункти «mgOS» і «kiosk» у меню SDDM (mgOS — якщо він уже встановлений)
    _mgos_sessions_menu
    # якщо ПК вимкнули прямо з kiosk — при завантаженні повернути профіль живлення, що був до kiosk
    bk "$KIOSK_PPD_GUARD"; bk /etc/systemd/system/multi-user.target.wants/kioskos-ppd-guard.service
    sudo install -Dm644 "$KIOSK_SESS_SRC/kioskos-ppd-guard.service" "$KIOSK_PPD_GUARD"
    sudo systemctl daemon-reload
    sudo systemctl enable kioskos-ppd-guard.service 2>/dev/null || echo "⚠ не вдалося увімкнути kioskos-ppd-guard"
    # при вході в будь-яку сесію: kiosk упав — повернути тему/gsettings/Drive
    bk ~/.config/systemd/user/kioskos-guard.service; bk ~/.config/systemd/user/default.target.wants/kioskos-guard.service
    install -Dm644 "$KIOSK_SESS_SRC/kioskos-guard.service" ~/.config/systemd/user/kioskos-guard.service
    mkdir -p ~/.config/systemd/user/default.target.wants
    ln -sfn ~/.config/systemd/user/kioskos-guard.service ~/.config/systemd/user/default.target.wants/kioskos-guard.service
    sub 3/5

    # постійні маски localsearch (ставила стара версія / kiosk-машина) ламали пошук Nautilus у mgOS —
    # тепер маска --runtime лише на час kiosk-сесії
    local u
    for u in localsearch-3 localsearch-control-3 localsearch-writeback-3; do
        if [ "$(readlink ~/.config/systemd/user/$u.service 2>/dev/null)" = /dev/null ]; then
            bk ~/.config/systemd/user/$u.service; rm -f ~/.config/systemd/user/$u.service
        fi
    done
    sub 4/5

    if [ -f "$KIOSK_DIR/system/rclone-gdrive.service" ]; then
        # сервіс НЕ вмикається при вході: mgOS монтує Drive сам (exec-once rclone mount) — два монтування
        # однієї теки конфліктують. kioskos-mode запускає сервіс лише в kiosk і лише якщо Drive ще не змонтовано.
        msg "Google Drive для kiosk (rclone, VFS-кеш)"
        bk ~/.config/systemd/user/rclone-gdrive.service
        mkdir -p ~/.config/systemd/user
        cp "$KIOSK_DIR/system/rclone-gdrive.service" ~/.config/systemd/user/
        _kiosk_paths ~/.config/systemd/user/rclone-gdrive.service
        if [ -L ~/.config/systemd/user/default.target.wants/rclone-gdrive.service ]; then
            bk ~/.config/systemd/user/default.target.wants/rclone-gdrive.service
            rm -f ~/.config/systemd/user/default.target.wants/rclone-gdrive.service
        fi
        [ -d ~/GoogleDrive ] || { bk ~/GoogleDrive; mkdir -p ~/GoogleDrive; }
        rclone listremotes 2>/dev/null | grep -qx 'drive:' \
            || echo "rclone ще не налаштовано: виконай «rclone config» (remote з назвою drive) — kiosk змонтує Drive сам"
    fi
    systemctl --user daemon-reload 2>/dev/null || true
    sub 5/5; return 0
}

step_kiosk_kernel() {
    # nowatchdog / loglevel=3 лише прибирають зайві переривання й спам — потужність ПК не обмежують
    local want; want=$(cat "$KIOSK_DIR/kernel_params.txt" 2>/dev/null)
    [ -n "$want" ] || { msg "Параметрів ядра у знімку немає"; return 0; }
    if [ ! -f /etc/default/grub ] || ! command -v grub-mkconfig &>/dev/null; then
        echo "GRUB не знайдено — додай параметри ядра вручну у свій завантажувач: $want"; return 0
    fi
    local line val q added="" t key
    line=$(grep -E '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub | tail -1)
    val=${line#*=}; q=${val:0:1}
    if [ "$q" = "'" ] || [ "$q" = '"' ]; then val=${val:1:${#val}-2}; else q='"'; fi
    for t in $want; do
        [[ " $val " == *" $t "* ]] && continue
        key=${t%%=*}
        if [[ "$t" == *=* ]] && grep -qE "(^| )$key=" <<< "$val"; then val=$(sed -E "s/(^| )$key=[^ ]*/\1$t/" <<< "$val")
        else val="$val $t"; fi
        added+=" $t"
    done
    [ -z "$added" ] && { echo "Параметри ядра вже на місці: $want"; return 0; }
    msg "Параметри ядра:$added"
    bk /etc/default/grub; bk /boot/grub/grub.cfg
    val=$(sed -E 's/^ +//; s/ +/ /g' <<< "$val")
    if grep -qE '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub; then
        sudo sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=$q$val$q|" /etc/default/grub
    else
        echo "GRUB_CMDLINE_LINUX_DEFAULT=$q$val$q" | sudo tee -a /etc/default/grub >/dev/null
    fi
    msg "grub-mkconfig"
    sudo grub-mkconfig -o /boot/grub/grub.cfg 2>&1 | tail -2
    echo "Додано:$added (діє після перезавантаження)"
}

step_kiosk_verify() {
    local fail=0 total=0
    chk() { local n=$1; shift; total=$((total + 1))
        if "$@" >/dev/null 2>&1; then echo "@@CHECK ok $n"; else echo "@@CHECK fail $n"; fail=$((fail + 1)); fi; }
    local c
    for c in Hyprland waybar wofi kitty firefox jq notify-send wpctl brightnessctl playerctl grim slurp wl-copy hyprpicker powerprofilesctl dunst locate; do
        chk "$c" command -v "$c"
    done
    chk "kiosk.conf" test -f ~/.config/hyprland/kiosk.conf
    chk "kiosk.conf (конфіг без помилок)" bash -c "timeout 15 Hyprland --verify-config -c ~/.config/hyprland/kiosk.conf 2>&1 | grep -q 'config ok'"
    [ "$KIOSK_SRC_HOME" != "$HOME" ] && chk "kiosk.conf (шляхи)" bash -c "! grep -q '$KIOSK_SRC_HOME/' ~/.config/hyprland/kiosk.conf ~/.config/hyprland/touchpad.conf ~/.config/waybar/kiosk.jsonc"
    chk "Spotlight Runner" bash -c "[ -x ~/.config/hyprland/kioskOS/runner/runner ] && ! ldd ~/.config/hyprland/kioskOS/runner/runner | grep -q 'not found'"
    chk "carpeted" bash -c "[ -x ~/.local/bin/carpeted ] && ! ldd ~/.local/bin/carpeted | grep -q 'not found'"
    chk "waybar volume.py" test -x ~/.config/waybar/volume.py
    chk "Firefox: kiosk-налаштування лише в kiosk" test -f "$KIOSK_FF_CFG" -a -f "$KIOSK_FF_AUTOCONFIG"
    chk "Firefox: немає глобального kiosk user.js" bash -c "! cmp -s '$KIOSK_DIR/firefox/user.js' ~/.mozilla/firefox/*/user.js 2>/dev/null"
    chk "power-profiles-daemon" systemctl is-active power-profiles-daemon
    chk "пункт «kiosk» у меню сесій" test -x "$KIOSK_SESSION_BIN" -a -f "$KIOSK_SESSION_DESKTOP" -a -f "$MGOS_SESS_CONF"
    [ -f ~/.config/hypr/hyprland.conf ] && chk "пункт «mgOS» у меню сесій" test -f "$MGOS_SESS_DIR/mgos.desktop"
    chk "перемикач kioskos-mode" test -x "$KIOSK_MODE_BIN"
    chk "захист профілю живлення (при завантаженні)" systemctl is-enabled kioskos-ppd-guard.service
    chk "захист теми (при вході)" test -L ~/.config/systemd/user/default.target.wants/kioskos-guard.service
    chk "kiosk не лишив економ/тему в системі" bash -c "! '$KIOSK_MODE_BIN' status | grep -q аварійно"
    chk "Drive не монтується двічі (rclone-gdrive не автозапуск)" test ! -e ~/.config/systemd/user/default.target.wants/rclone-gdrive.service
    _intel_gpu && chk "VA-API (intel-media-driver)" test -f /usr/lib/dri/iHD_drv_video.so
    chk "автомасштаб екранів (mgos-display)" test -x "$HW_DISPLAY_BIN"
    chk "mgos-auto.conf підключено" bash -c "test -f ~/.config/hyprland/mgos-auto.conf && grep -q '^source = ~/.config/hyprland/mgos-auto.conf' ~/.config/hyprland/kiosk.conf"
    echo "Перевірок: $total, з помилками: $fail"
    return 0
}

# запуск окремого кроку вручну: bash steps.sh <id>
if [ "${BASH_SOURCE[0]}" = "$0" ] && [ -n "${1:-}" ]; then "step_$1"; fi
