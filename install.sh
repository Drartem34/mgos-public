#!/usr/bin/env bash
# mgOS Installer — BIOS-style TUI, centered, i18n-aware
#
# Перед перенесенням на нову машину онови знімок системи: ./sync_from_system.sh
# (конфіги, панелі SYSui, тема, курсор, кольори pywal, нові шпалери).

set -e

# Працюємо з теки інсталятора: раніше при запуску не з неї (bash ~/QSH/mgOS/install.sh)
# усі ./configs, ./fonts, ./sddm_theme тихо пропускались.
SRC_DIR="$(dirname "$(readlink -f "$0")")"
bash "$SRC_DIR/gui/register-app.sh" 2>/dev/null || true   # ярлик у списку програм
cd "$SRC_DIR"

# Оновлення самого інсталятора з GitHub (якщо він склонований через git). Пропустити: MGOS_NO_SELFUPDATE=1
if [ -z "${MGOS_NO_SELFUPDATE:-}" ] && [ -d .git ] && [ -t 0 ]; then
    _upd=$(bash gui/selfupdate.sh check 2>/dev/null)
    if [[ "$_upd" == "@@UPD "* ]] && [ "$(awk '{print $2}' <<< "$_upd")" -gt 0 ]; then
        echo "Є нова версія інсталятора mgOS: $(cut -d' ' -f3- <<< "$_upd") ($(awk '{print $2}' <<< "$_upd") змін)"
        read -rp "Оновити зараз? [Y/n] " _a
        if [[ ! "$_a" =~ ^[NnНн] ]] && bash gui/selfupdate.sh apply; then
            exec env MGOS_NO_SELFUPDATE=1 "$SRC_DIR/install.sh" "$@"
        fi
    fi
fi

# ============================================================================
#  МОВА / LANGUAGE
#  Визначається автоматично з LANG/LC_ALL. Перевизначити: MGOS_LANG=uk
# ============================================================================
_detect_lang() {
    local lc="${MGOS_LANG:-${LANG:-${LC_ALL:-${LC_MESSAGES:-en}}}}"
    [[ "$lc" == uk* ]] && echo "uk" || echo "en"
}
LANG_CODE=$(_detect_lang)

declare -A T
_load_strings() {
    if [ "$LANG_CODE" = "uk" ]; then
        T[title]="ІНСТАЛЯТОР mgOS  v1.2"
        T[subtitle]="(C) mgOS Project  --  Arch Linux Edition"
        T[will_do]="Цей інсталятор:"
        T[will_pkg]="  - встановить системні пакети (pacman/yay)"
        T[will_cfg]="  - скопіює конфіги у ~/.config (з бекапами)"
        T[will_sh]="  - за бажанням змінить оболонку"
        T[key_enter]="  [ENTER]      -- Продовжити встановлення"
        T[key_esc]="  [ESC]/[C]    -- Скасувати і вийти"
        T[autostart]="Автостарт через"
        T[interrupt]="будь-яка клавіша"
        T[cancelled]="Встановлення скасовано. Жодних змін не внесено."
        T[chk_root]="Перевірка прав користувача..."
        T[chk_lock]="Перевірка pacman lock-файлу..."
        T[chk_net]="Перевірка мережевого з'єднання..."
        T[err_root]="Запускайте скрипт БЕЗ sudo!"
        T[err_lock]="Знайдено /var/lib/pacman/db.lck. Видаліть вручну."
        T[err_net]="Немає з'єднання з інтернетом. Перевірте мережу."
        T[sec_update]="ОНОВЛЕННЯ СИСТЕМИ"
        T[sec_wall]="ШПАЛЕРИ"
        T[sec_apps]="ДОДАТКОВІ ПРОГРАМИ"
        T[sec_cfg]="КОНФІГИ"
        T[sec_pkg]="ВСТАНОВЛЕННЯ ПАКЕТІВ"
        T[sec_setup]="НАЛАШТУВАННЯ СИСТЕМИ"
        T[sec_shell]="ОБОЛОНКА"
        T[sec_gpu]="ВІДЕОКАРТА"
        T[sec_hw]="ЕКРАН, МАСШТАБ, ТАЧПАД"
        T[q_update]="Оновити систему перед встановленням (pacman -Syu)?"
        T[q_wall]="Скопіювати шпалери у нову систему?"
        T[q_apps]="Оберіть програми (пробіл — вибрати, enter — підтвердити)"
        T[q_cfg]="Оберіть конфіги (пробіл — вибрати, enter — підтвердити)"
        T[q_fish]="Зробити fish оболонкою за замовчуванням?"
        T[cfg_empty]="Папка ./configs порожня — пропуск."
        T[done_title]="ВСТАНОВЛЕННЯ mgOS ЗАВЕРШЕНО!"
        T[done_sub]="Виконано успішно."
        T[done_reboot]="  Рекомендується перезавантаження."
        T[done_gdrive]="  Google Диск автоматично змонтується у ~/GoogleDrive при запуску."
        T[done_log]="  Лог збережено у:"
        T[done_failed]="Не вдалось встановити наступні пакети:"
        T[main_hint]="  [ПРОБІЛ] — вибрати,  [ENTER] — підтвердити"
        T[spin_dirs]="Створення директорій..."
        T[spin_fonts]="Копіювання шрифтів..."
        T[spin_backup]="Бекап старого"
        T[spin_install]="Встановлення"
        T[spin_wal]="Копіювання кольорів Pywal..."
        T[spin_wall]="Копіювання шпалер..."
        T[spin_plymouth]="Налаштування екрану завантаження..."
        T[spin_initram]="Оновлення initramfs..."
        T[spin_scripts]="Копіювання скриптів..."
        T[spin_services]="Налаштування служб..."
        T[spin_fonts_gtk]="Застосування шрифтів GTK..."
        T[spin_nvidia]="Вимкнення змінних NVIDIA у hyprland.conf..."
        T[gpu_nvidia]="Знайдено NVIDIA. Залишаємо налаштування."
        T[gpu_other]="NVIDIA не знайдена (AMD/Intel). Вимикаємо специфічні змінні."
        T[font_missing]="Шрифт SF-Pro не знайдено. GTK залишається системним."
        T[no_cfg]="Жоден конфіг не обрано!"
        T[skip_wall]="Пропуск шпалер."
        T[spin_paths]="Адаптація шляхів під користувача"
        T[spin_build]="Збірка панелей SYSui з сирців..."
        T[build_fail]="Збірка не вдалася — використовуються готові бінарники з установщика."
        T[spin_theme]="Застосування теми GTK, іконок, курсора, шрифтів..."
        T[spin_cursor]="Встановлення курсора..."
        T[spin_sudoers]="Дозвіл на синхронізацію теми SDDM..."
        T[spin_state]="Відновлення шпалер та історії доку..."
        T[sec_verify]="ПЕРЕВІРКА"
        T[verify_all_ok]="Усі перевірки пройдено — система відповідає знімку."
        T[verify_bad]="Є проблеми (див. вище). Їх можна виправити і запустити інсталятор ще раз."
        T[err_core]="Не вдалося встановити критичні пакети:"
        T[sec_target]="ЩО ВСТАНОВЛЮЄМО"
        T[tgt_hint1]="mgOS    — повна система (панелі SYSui, тема, програми)"
        T[tgt_hint2]="kioskOS — енергоощадна kiosk-сесія для ноутбука"
        T[tgt_mgos]="mgOS — повна система Megumin OS"
        T[tgt_kiosk]="kioskOS — енергоощадний кіоск (Hyprland + Firefox)"
        T[k_title]="ВСТАНОВЛЕННЯ kioskOS"
        T[k_desc1]="Hyprland без анімацій/блюру/шпалер, мінімальний waybar,"
        T[k_desc2]="Spotlight Runner, Firefox з RAM-кешем і VA-API,"
        T[k_desc3]="тачпад без затримок, power-profiles, Google Drive."
        T[k_desc4]="Поруч із mgOS: економія діє ЛИШЕ в kiosk-сесії."
        T[k_sec_theme]="ТЕМА"
        T[k_q_theme]="Тема Graphite + іконки MacTahoe + шрифт SF Compact (лише в kiosk-сесії)?"
        T[k_sec_ext]="РОЗШИРЕННЯ FIREFOX"
        T[k_q_ext]="Встановити uBlock Origin, Tab Suspender, SponsorBlock, Shorts Block?"
        T[k_sec_kernel]="ПАРАМЕТРИ ЯДРА"
        T[k_q_kernel]="Додати параметри ядра в GRUB"
        T[k_fatal]="Критичний крок не вдався:"
        T[k_undo]="Повернути все як було:"
        T[k_done]="kioskOS ВСТАНОВЛЕНО!"
        T[k_login1]="  Вхід: меню сесій SDDM → «kiosk» або «mgOS»"
        T[k_login2]="  або з TTY: kiosk"
        T[k_warn]="Кроки з попередженнями (див. лог):"
        T[k_nosnap]="У інсталяторі немає знімка kioskOS (kiosk/files). Запусти kiosk/sync_kiosk.sh на kiosk-машині."
    else
        T[title]="mgOS SETUP UTILITY  v1.2"
        T[subtitle]="(C) mgOS Project  --  Arch Linux Edition"
        T[will_do]="This installer will:"
        T[will_pkg]="  - install system packages (pacman/yay)"
        T[will_cfg]="  - copy configs to ~/.config (with backups)"
        T[will_sh]="  - optionally change your default shell"
        T[key_enter]="  [ENTER]      -- Continue installation"
        T[key_esc]="  [ESC]/[C]    -- Cancel and exit"
        T[autostart]="Auto-start in"
        T[interrupt]="any key to interrupt"
        T[cancelled]="Installation cancelled. No changes were made."
        T[chk_root]="Checking user privileges (root check)..."
        T[chk_lock]="Checking pacman lock file..."
        T[chk_net]="Checking network connection..."
        T[err_root]="Run this script WITHOUT sudo!"
        T[err_lock]="Found /var/lib/pacman/db.lck. Remove it manually."
        T[err_net]="No internet connection. Check your network."
        T[sec_update]="SYSTEM UPDATE"
        T[sec_wall]="WALLPAPERS"
        T[sec_apps]="ADDITIONAL APPS"
        T[sec_cfg]="CONFIGS"
        T[sec_pkg]="INSTALLING PACKAGES"
        T[sec_setup]="SYSTEM SETUP"
        T[sec_shell]="DEFAULT SHELL"
        T[sec_gpu]="GPU DETECTION"
        T[sec_hw]="DISPLAY SCALE, TOUCHPAD"
        T[q_update]="Update the system before installation (pacman -Syu)?"
        T[q_wall]="Copy wallpapers (photos and videos) to the new system?"
        T[q_apps]="Select apps to install (space to toggle, enter to confirm)"
        T[q_cfg]="Select configs to install (space to toggle, enter to confirm)"
        T[q_fish]="Set fish as the default shell?"
        T[cfg_empty]="./configs folder is empty — skipping."
        T[done_title]="mgOS SETUP COMPLETE!"
        T[done_sub]="Installation finished successfully."
        T[done_reboot]="  Reboot recommended."
        T[done_gdrive]="  Google Drive will auto-mount at ~/GoogleDrive on next launch."
        T[done_log]="  Full install log saved to:"
        T[done_failed]="The following packages failed to install:"
        T[main_hint]="  [SPACE] to toggle,  [ENTER] to confirm"
        T[spin_dirs]="Creating directories..."
        T[spin_fonts]="Copying fonts..."
        T[spin_backup]="Backing up old"
        T[spin_install]="Installing"
        T[spin_wal]="Copying Pywal colors..."
        T[spin_wall]="Copying wallpapers..."
        T[spin_plymouth]="Setting up boot splash (Plymouth)..."
        T[spin_initram]="Rebuilding initramfs..."
        T[spin_scripts]="Copying system scripts..."
        T[spin_services]="Enabling system services..."
        T[spin_fonts_gtk]="Applying GTK fonts..."
        T[spin_nvidia]="Disabling NVIDIA vars in hyprland.conf..."
        T[gpu_nvidia]="NVIDIA GPU found. Keeping NVIDIA settings."
        T[gpu_other]="No NVIDIA GPU (AMD/Intel). Disabling NVIDIA-specific vars."
        T[font_missing]="SF-Pro font not found. GTK font remains system default."
        T[no_cfg]="No config was selected!"
        T[skip_wall]="Skipping wallpapers."
        T[spin_paths]="Adapting paths for this user"
        T[spin_build]="Building SYSui panels from source..."
        T[build_fail]="Build failed — using the prebuilt binaries shipped with the installer."
        T[spin_theme]="Applying GTK theme, icons, cursor, fonts..."
        T[spin_cursor]="Installing cursor theme..."
        T[spin_sudoers]="Allowing SDDM theme sync..."
        T[spin_state]="Restoring wallpaper and dock history..."
        T[sec_verify]="VERIFICATION"
        T[verify_all_ok]="All checks passed — the system matches the snapshot."
        T[verify_bad]="Some checks failed (see above). Fix them and re-run the installer."
        T[err_core]="Failed to install critical packages:"
        T[sec_target]="WHAT TO INSTALL"
        T[tgt_hint1]="mgOS    — full system (SYSui panels, theme, apps)"
        T[tgt_hint2]="kioskOS — power-saving kiosk session for laptops"
        T[tgt_mgos]="mgOS — full Megumin OS system"
        T[tgt_kiosk]="kioskOS — power-saving kiosk (Hyprland + Firefox)"
        T[k_title]="kioskOS SETUP"
        T[k_desc1]="Hyprland without animations/blur/wallpaper, minimal waybar,"
        T[k_desc2]="Spotlight Runner, Firefox with RAM cache and VA-API,"
        T[k_desc3]="zero-latency touchpad, power profiles, Google Drive."
        T[k_desc4]="Next to mgOS: power saving applies ONLY in the kiosk session."
        T[k_sec_theme]="THEME"
        T[k_q_theme]="Graphite theme + MacTahoe icons + SF Compact font (kiosk session only)?"
        T[k_sec_ext]="FIREFOX EXTENSIONS"
        T[k_q_ext]="Install uBlock Origin, Tab Suspender, SponsorBlock, Shorts Block?"
        T[k_sec_kernel]="KERNEL PARAMETERS"
        T[k_q_kernel]="Add kernel parameters to GRUB"
        T[k_fatal]="Critical step failed:"
        T[k_undo]="Undo everything:"
        T[k_done]="kioskOS INSTALLED!"
        T[k_login1]="  Login: SDDM session menu → \"kiosk\" or \"mgOS\""
        T[k_login2]="  or from a TTY: kiosk"
        T[k_warn]="Steps with warnings (see log):"
        T[k_nosnap]="No kioskOS snapshot in the installer (kiosk/files). Run kiosk/sync_kiosk.sh on the kiosk machine."
    fi
}
_load_strings
# Shorthand: t KEY
t() { printf '%s' "${T[$1]:-$1}"; }

# Примусово фіксуємо UTF-8-локаль ДЛЯ ПІДРАХУНКУ СИМВОЛІВ (${#s} у bash).
# Робимо це ПІСЛЯ визначення мови (_detect_lang вже прочитав оригінальний LANG),
# бо інакше на свіжій Arch-системі (де locale-gen ще не запускали) bash рахує
# кирилицю побайтово — саме через це "з'їжджали" рамки.
# C.UTF-8 вбудована у glibc і не потребує locale-gen.
export LC_ALL=C.UTF-8
export LANG=C.UTF-8

# ============================================================================
#  TUI CORE — Python3 для правильної ширини Unicode (Кирилиця, CJK тощо)
# ============================================================================
BOX_W=66      # ширина рамки (символи відображення)
BIOS_BG=0
BIOS_FG=7
BIOS_HL=11
BIOS_OK=2
BIOS_ERR=1

# Реальна ширина рядка в символах відображення.
# Кирилиця та латиниця — завжди одинарної ширини, тож окремий CJK-розрахунок
# (і залежність від python3, якого може не бути на свіжій системі) тут не
# потрібен: досить надійного підрахунку символів bash-ом під UTF-8-локаллю
# (яку ми зафіксували вище через LC_ALL=C.UTF-8).
_dw() { printf '%s' "${#1}"; }

# Доповнити рядок пробілами справа до потрібної відображуваної ширини.
# Пише результат у змінну на ім'я якої вказує $2 (nameref) через printf -v —
# це, на відміну від command substitution $(...), НЕ обрізає кінцеві пробіли,
# тож sentinel-символ більше не потрібен.
_rpad() {
    local s="$1" w="$2" out_name="$3"
    local dw; dw=$(_dw "$s")
    local pad=$(( w - dw ))
    [ "$pad" -lt 0 ] && pad=0
    printf -v "$out_name" '%s%*s' "$s" "$pad" ''
}

hline() { printf '═%.0s' $(seq 1 "$1"); }

# Ширина рамки, яка гарантовано влазить у поточний термінал (запобігає
# переносу рядка й "artefacts" на вузьких терміналах)
_effective_box_w() {
    local cols; cols=$(tput cols 2>/dev/null || echo 80)
    local w="$BOX_W"
    if [ "$w" -gt $((cols - 2)) ]; then w=$((cols - 2)); fi
    [ "$w" -lt 20 ] && w=20
    printf '%s' "$w"
}

# Лівий відступ для центрування вікна шириною ефективної box_w
get_pad() {
    local cols; cols=$(tput cols 2>/dev/null || echo 80)
    local box_w; box_w=$(_effective_box_w)
    local n=$(( (cols - box_w) / 2 ))
    [ "$n" -lt 0 ] && n=0
    printf '%*s' "$n" ''
}

draw_bios_fullscreen() {
    local rows cols
    rows=$(tput lines 2>/dev/null || echo 24)
    cols=$(tput cols 2>/dev/null || echo 80)
    clear
    tput setab "$BIOS_BG" 2>/dev/null || true
    tput setaf "$BIOS_FG" 2>/dev/null || true
    local i; for ((i=0; i<rows; i++)); do printf '%*s\n' "$cols" ''; done
    tput cup 0 0 2>/dev/null || true
    tput setab "$BIOS_BG" 2>/dev/null || true
    tput setaf "$BIOS_FG" 2>/dev/null || true
}

# draw_box "line1" "line2" ...
# Рядок "@TEXT" — заголовок (жовтий жирний)
# Рядок "---"   — роздільник ╠══╣
draw_box() {
    local cols rows
    cols=$(tput cols 2>/dev/null || echo 80)
    rows=$(tput lines 2>/dev/null || echo 24)
    local box_w; box_w=$(_effective_box_w)
    local INNER=$((box_w - 4))
    local box_h=$(( $# + 4 ))
    local col_off=$(( (cols - box_w) / 2 ))
    local row_off=$(( (rows - box_h) / 2 ))
    [ "$col_off" -lt 0 ] && col_off=0
    [ "$row_off" -lt 1 ] && row_off=1
    local PAD; PAD=$(printf '%*s' "$col_off" '')

    tput cup "$row_off" 0 2>/dev/null || true
    tput bold 2>/dev/null || true; tput setaf "$BIOS_HL" 2>/dev/null || true
    printf '%s╔' "$PAD"; hline $((box_w-2)); printf '╗\n'
    printf '%s║%*s║\n' "$PAD" $((box_w-2)) ''
    tput sgr0 2>/dev/null || true; tput setab "$BIOS_BG" 2>/dev/null || true

    local _cell
    for line in "$@"; do
        if [[ "$line" == @* ]]; then
            _rpad "${line#@}" "$INNER" _cell
            tput bold 2>/dev/null || true; tput setaf "$BIOS_HL" 2>/dev/null || true
            printf '%s║  %s  ║\n' "$PAD" "$_cell"
            tput sgr0 2>/dev/null || true; tput setab "$BIOS_BG" 2>/dev/null || true; tput setaf "$BIOS_FG" 2>/dev/null || true
        elif [[ "$line" == "---" ]]; then
            tput setaf "$BIOS_HL" 2>/dev/null || true
            printf '%s╠' "$PAD"; hline $((box_w-2)); printf '╣\n'
            tput setaf "$BIOS_FG" 2>/dev/null || true
        else
            _rpad "$line" "$INNER" _cell
            printf '%s║  %s  ║\n' "$PAD" "$_cell"
        fi
    done

    tput bold 2>/dev/null || true; tput setaf "$BIOS_HL" 2>/dev/null || true
    printf '%s║%*s║\n' "$PAD" $((box_w-2)) ''
    printf '%s╚' "$PAD"; hline $((box_w-2)); printf '╝\n'
    tput sgr0 2>/dev/null || true
}

# Заголовок секції — рамка ╔══╗ по центру
bios_section() {
    local PAD; PAD=$(get_pad)
    local box_w; box_w=$(_effective_box_w)
    local INNER=$((box_w-4))
    local _cell
    _rpad "$1" "$INNER" _cell
    echo
    tput bold 2>/dev/null || true; tput setaf "$BIOS_HL" 2>/dev/null || true
    printf '%s╔' "$PAD"; hline $((box_w-2)); printf '╗\n'
    printf '%s║  %s  ║\n' "$PAD" "$_cell"
    printf '%s╚' "$PAD"; hline $((box_w-2)); printf '╝\n'
    tput sgr0 2>/dev/null || true; echo
}

# POST рядок
post_line() {
    local PAD; PAD=$(get_pad)
    local cell; _rpad "$1" 50 cell   # printf %-50s рахує байти → кирилиця з'їжджала
    printf '%s  %s' "$PAD" "$cell"
}
post_ok()   { tput setaf "$BIOS_OK" 2>/dev/null||true; echo "[ OK ]"; tput sgr0 2>/dev/null||true; tput setab "$BIOS_BG" 2>/dev/null||true; }
post_fail() { tput setaf "$BIOS_ERR" 2>/dev/null||true; echo "[ FAIL ]"; tput sgr0 2>/dev/null||true; }

# ============================================================================
#  СТАРТОВИЙ ЕКРАН
# ============================================================================
show_cancel_screen() {
    draw_bios_fullscreen
    draw_box \
        "@$(t title)" \
        "@$(t subtitle)" \
        "---" \
        "$(t will_do)" \
        "$(t will_pkg)" \
        "$(t will_cfg)" \
        "$(t will_sh)" \
        "---" \
        "$(t key_enter)" \
        "$(t key_esc)"

    local rows cols PAD
    rows=$(tput lines 2>/dev/null || echo 24)
    cols=$(tput cols 2>/dev/null || echo 80)
    PAD=$(get_pad)
    tput cup $(( rows - 2 )) 0 2>/dev/null || true
    tput setaf "$BIOS_HL" 2>/dev/null || true

    local countdown=57 key=""
    while [ "$countdown" -gt 0 ]; do
        printf '\r%s  %s %2d s.  (%s)   ' "$PAD" "$(t autostart)" "$countdown" "$(t interrupt)"
        if read -t 1 -n 1 -s key; then
            echo
            case "$key" in
                $'\e'|c|C)
                    tput sgr0 2>/dev/null || true; clear
                    echo "$(t cancelled)"; exit 0 ;;
                *) break ;;
            esac
        fi
        countdown=$((countdown-1))
    done
    tput sgr0 2>/dev/null || true; clear
}

show_cancel_screen

# ============================================================================
#  POST ПЕРЕВІРКИ
# ============================================================================
draw_bios_fullscreen

rows=$(tput lines 2>/dev/null || echo 24)
tput cup $(( rows / 2 - 6 )) 0 2>/dev/null || true
local_pad=$(get_pad)
tput setaf "$BIOS_HL" 2>/dev/null || true; tput bold 2>/dev/null || true
printf '%s  mgOS BIOS-Style Installer v1.2\n' "$local_pad"
printf '%s  Copyright (C) 2026 mgOS Project, All Rights Reserved\n' "$local_pad"
tput sgr0 2>/dev/null || true; tput setab "$BIOS_BG" 2>/dev/null || true; tput setaf "$BIOS_FG" 2>/dev/null || true
printf '%s\n' ""; sleep 0.3

post_line "$(t chk_root)"
if [ "$EUID" -eq 0 ]; then
    post_fail; echo; echo "$(t err_root)"; exit 1
fi
post_ok; sleep 0.2

post_line "$(t chk_lock)"
if [ -f /var/lib/pacman/db.lck ]; then
    post_fail; echo; echo "$(t err_lock)"; exit 1
fi
post_ok; sleep 0.2

post_line "$(t chk_net)"
if ! ping -c 1 -W 3 archlinux.org &>/dev/null && ! curl -fsS --max-time 6 -o /dev/null https://archlinux.org; then
    post_fail; echo; echo "$(t err_net)"; exit 1
fi
post_ok; sleep 0.2; echo

# ============================================================================
#  ЛОГ + SUDO
# ============================================================================
LOG_FILE="$HOME/mgOS_install_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee -a "$LOG_FILE") 2>&1

sudo -v
while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null &

# ============================================================================
#  YAY + GUM
# ============================================================================
if ! command -v yay &>/dev/null; then
    sudo pacman -S --needed --noconfirm base-devel git
    rm -rf /tmp/yay
    git clone https://aur.archlinux.org/yay.git /tmp/yay
    cd /tmp/yay; makepkg -si --noconfirm; cd -; rm -rf /tmp/yay
fi

if ! command -v gum &>/dev/null; then
    yay -S --needed --noconfirm gum
fi

# ============================================================================
#  GUM ТЕМА
# ============================================================================
export GUM_CONFIRM_PROMPT_FOREGROUND="15"
export GUM_CONFIRM_SELECTED_FOREGROUND="0"
export GUM_CONFIRM_SELECTED_BACKGROUND="11"
export GUM_CONFIRM_UNSELECTED_FOREGROUND="15"
export GUM_CONFIRM_UNSELECTED_BACKGROUND="0"
export GUM_CHOOSE_CURSOR_FOREGROUND="11"
export GUM_CHOOSE_SELECTED_FOREGROUND="11"
export GUM_CHOOSE_ITEM_FOREGROUND="15"
export GUM_CHOOSE_HEADER_FOREGROUND="11"
export GUM_CHOOSE_CURSOR_PREFIX="▶ "
export GUM_CHOOSE_SELECTED_PREFIX="[x] "
export GUM_CHOOSE_UNSELECTED_PREFIX="[ ] "
export GUM_SPIN_SPINNER="line"
export GUM_SPIN_SPINNER_FOREGROUND="11"
export GUM_SPIN_TITLE_FOREGROUND="15"
export GUM_STYLE_FOREGROUND="15"

_COLS=$(tput cols 2>/dev/null || echo 80)
_EFF_BOX_W=$(_effective_box_w)
_GUM_INDENT=$(( (_COLS - _EFF_BOX_W) / 2 ))
[ "$_GUM_INDENT" -lt 0 ] && _GUM_INDENT=0
export GUM_CONFIRM_PROMPT_MARGIN="0 0 0 $_GUM_INDENT"
export GUM_CHOOSE_HEADER_MARGIN="0 0 0 $_GUM_INDENT"

# ============================================================================
#  ВИБІР СИСТЕМИ: mgOS чи kioskOS
#  kioskOS ставиться тими самими кроками, що й у GUI (gui/steps.sh: step_kiosk_*),
#  з точкою відкату. Пропустити меню: ./install.sh --kiosk
# ============================================================================
run_kiosk() {
    clear; draw_bios_fullscreen
    draw_box "@$(t k_title)" "@$(t subtitle)" "---" "$(t k_desc1)" "$(t k_desc2)" "$(t k_desc3)" "$(t k_desc4)"
    echo
    local K_UPDATE=0 K_THEME=0 K_EXT=0 K_KERNEL=0
    bios_section "$(t sec_update)";       gum confirm "$(t q_update)"   && K_UPDATE=1
    bios_section "$(t k_sec_theme)";      gum confirm "$(t k_q_theme)"  && K_THEME=1
    bios_section "$(t k_sec_ext)";        gum confirm "$(t k_q_ext)"    && K_EXT=1
    bios_section "$(t k_sec_kernel)";     gum confirm "$(t k_q_kernel) ($(cat ./kiosk/files/kernel_params.txt 2>/dev/null))" && K_KERNEL=1

    local steps=(preflight snapshot yay)
    [ $K_UPDATE = 1 ] && steps+=(sysupdate)
    steps+=(kiosk_pkgs kiosk_extra kiosk_files kiosk_build kiosk_firefox)
    [ $K_THEME = 1 ] && steps+=(kiosk_theme)
    steps+=(kiosk_system kiosk_hw)
    [ $K_KERNEL = 1 ] && steps+=(kiosk_kernel)
    steps+=(kiosk_verify)

    local cache; cache="$SRC_DIR/cache/kiosk_$(date +%Y%m%d_%H%M%S)"
    [ -w "$SRC_DIR" ] || cache="$HOME/.local/state/mgos-installer/cache/kiosk_$(date +%Y%m%d_%H%M%S)"
    local fatal=" preflight snapshot yay sysupdate kiosk_pkgs kiosk_files " s rc failed=()
    for s in "${steps[@]}"; do
        bios_section "kioskOS: $s"
        # @@-протокол GUI → зрозумілий текст у терміналі
        MGOS_DIR="$SRC_DIR" MGOS_CACHE="$cache" OPT_UPDATE=$K_UPDATE OPT_KIOSK_THEME=$K_THEME \
        OPT_KIOSK_EXT=$K_EXT OPT_KIOSK_KERNEL=$K_KERNEL \
            bash -c 'source "$MGOS_DIR/gui/steps.sh" && step_'"$s" 2>&1 \
            | sed -u -e '/^@@SUB /d' -e 's/^@@MSG /  › /' -e 's/^@@FAILPKG /  ⚠ пакет: /' \
                     -e 's/^@@CHECK ok /  [ OK ]  /' -e 's/^@@CHECK fail /  [FAIL]  /'
        rc=${PIPESTATUS[0]}
        if [ "$rc" -ne 0 ]; then
            if [[ "$fatal" == *" $s "* ]]; then
                echo; echo "❌ $(t k_fatal) $s"; echo "   $(t k_undo)"
                echo "   MGOS_DIR=\"$SRC_DIR\" MGOS_CACHE=\"$cache\" bash gui/steps.sh rollback"
                echo "   Log: $LOG_FILE"; exit 1
            fi
            failed+=("$s")
        fi
    done

    mkdir -p "${XDG_STATE_HOME:-$HOME/.local/state}/mgos-installer" && echo kiosk > "${XDG_STATE_HOME:-$HOME/.local/state}/mgos-installer/welcome-pending"
    clear; draw_bios_fullscreen
    draw_box "@$(t k_done)" "@$(t done_sub)" "---" "$(t k_login1)" "$(t k_login2)" "$(t done_reboot)" "$(t done_log) $LOG_FILE"
    echo
    [ ${#failed[@]} -gt 0 ] && echo "⚠️  $(t k_warn) ${failed[*]}"
    echo "  $(t k_undo)"
    echo "  MGOS_DIR=\"$SRC_DIR\" MGOS_CACHE=\"$cache\" bash gui/steps.sh rollback"
    exit 0
}

INSTALL_TARGET=mgos
for _a in "$@"; do [ "$_a" = "--kiosk" ] && INSTALL_TARGET=kiosk; done
if [ "$INSTALL_TARGET" = mgos ] && [ -f ./kiosk/files/config/hyprland/kiosk.conf ]; then
    clear; draw_bios_fullscreen
    draw_box "@$(t title)" "@$(t subtitle)" "---" "$(t tgt_hint1)" "$(t tgt_hint2)"
    echo
    bios_section "$(t sec_target)"
    _choice=$(gum choose "$(t tgt_mgos)" "$(t tgt_kiosk)" || true)
    [ "$_choice" = "$(t tgt_kiosk)" ] && INSTALL_TARGET=kiosk
fi
if [ "$INSTALL_TARGET" = kiosk ]; then
    [ -f ./kiosk/files/config/hyprland/kiosk.conf ] || { echo "$(t k_nosnap)"; exit 1; }
    run_kiosk
fi

# ============================================================================
#  ГОЛОВНЕ МЕНЮ
# ============================================================================
clear; draw_bios_fullscreen
draw_box \
    "@$(t title)" \
    "@$(t subtitle)" \
    "---" \
    "$(t main_hint)"
echo

# 2. Опитування
bios_section "$(t sec_update)"
DO_UPDATE=false
if gum confirm "$(t q_update)"; then DO_UPDATE=true; fi

bios_section "$(t sec_wall)"
COPY_WALLPAPERS=false
if gum confirm "$(t q_wall)"; then COPY_WALLPAPERS=true; fi

bios_section "$(t sec_apps)"
EXTRA_APPS_ARRAY=("steam" "spotify" "telegram-desktop" "prismlauncher" "minecraft-launcher")
echo "$(t q_apps):"
SELECTED_APPS=$(gum choose --no-limit "${EXTRA_APPS_ARRAY[@]}" || true)

SELECTED_CONFIGS=""
if [ -d "./configs" ]; then
    bios_section "$(t sec_cfg)"
    mapfile -t CONFIG_ARRAY < <(find ./configs -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)
    if [ "${#CONFIG_ARRAY[@]}" -gt 0 ]; then
        echo "$(t q_cfg):"
        SELECTED_LIST=$(IFS=','; echo "${CONFIG_ARRAY[*]}")
        SELECTED_CONFIGS=$(gum choose --no-limit --selected="$SELECTED_LIST" "${CONFIG_ARRAY[@]}" || true)
    else
        echo "$(t cfg_empty)"
    fi
fi

# 3. Встановлення
if [ "$DO_UPDATE" = true ]; then
    bios_section "$(t sec_update)"
    sudo pacman -Syu --noconfirm
fi

bios_section "$(t sec_pkg)"
# Критичні: без них система не запуститься (Hyprland, панелі SYSui на GTK4, налаштування на PyQt6)
PACKAGES_CORE=(
    hyprland xdg-desktop-portal-hyprland xdg-desktop-portal-gtk
    gtk4 gtk4-layer-shell base-devel pkgconf nlohmann-json
    python python-pyqt6 socat curl libnotify
    pipewire-pulse wireplumber networkmanager dunst sddm
)
# Решта офіційних пакетів. (swww більше не існує — перейменовано на awww, через нього
# інсталятор раніше падав на цьому кроці.)
PACKAGES_STABLE=(
    hyprpaper hyprpicker egl-wayland gamescope
    kitty fastfetch fish plymouth
    nwg-look
    grim slurp wl-clipboard cliphist swaybg awww mpv imagemagick ffmpeg
    pipewire-alsa pipewire-jack pavucontrol pamixer playerctl ddcutil
    python-pywal openrgb
    ttf-jetbrains-mono-nerd ttf-liberation ttf-opensans ttf-bitstream-vera noto-fonts noto-fonts-cjk noto-fonts-emoji woff2-font-awesome cantarell-fonts
    btop nautilus gnome-disk-utility rclone polkit-kde-agent power-profiles-daemon bluez bluez-utils
    qt6-svg qt6-virtualkeyboard qt6-multimedia-ffmpeg
)
# AUR (тема SDDM копіюється з ./sddm_theme, тому неіснуючий sddm-astronaut-theme-git прибрано)
PACKAGES_AUR=(python-ignis-git ignis-gvc whitesur-icon-theme-git bibata-cursor-theme-bin mpvpaper bluetuith)
FAILED_PACKAGES=()

if ! yay -S --needed --noconfirm "${PACKAGES_CORE[@]}"; then
    CORE_FAILED=()
    for pkg in "${PACKAGES_CORE[@]}"; do
        yay -S --needed --noconfirm "$pkg" || CORE_FAILED+=("$pkg")
    done
    if [ "${#CORE_FAILED[@]}" -gt 0 ]; then
        echo "❌ $(t err_core) ${CORE_FAILED[*]}"; echo "Log: $LOG_FILE"; exit 1
    fi
fi

# Один неіснуючий/зламаний пакет більше не зупиняє все встановлення
if ! yay -S --needed --noconfirm "${PACKAGES_STABLE[@]}"; then
    for pkg in "${PACKAGES_STABLE[@]}"; do
        yay -S --needed --noconfirm "$pkg" || FAILED_PACKAGES+=("$pkg")
    done
fi

for pkg in "${PACKAGES_AUR[@]}"; do
    if ! yay -S --needed --noconfirm "$pkg"; then
        echo "⚠️ Failed AUR package: $pkg"
        FAILED_PACKAGES+=("$pkg")
    fi
done

if [ -n "$SELECTED_APPS" ]; then
    mapfile -t APPS_ARRAY <<< "$SELECTED_APPS"
    for app in "${APPS_ARRAY[@]}"; do
        if ! yay -S --needed --noconfirm "$app"; then
            FAILED_PACKAGES+=("$app")
        fi
    done
fi

bios_section "$(t sec_setup)"
gum spin --spinner line --title "$(t spin_dirs)" -- mkdir -p ~/.config ~/.cache/wal ~/Pictures/wallpapers ~/GoogleDrive ~/.local/share/fonts

if [ -d "./fonts" ]; then
    gum spin --spinner line --title "$(t spin_fonts)" -- bash -c 'cp -r ./fonts/* ~/.local/share/fonts/ 2>/dev/null||true; fc-cache -fv>/dev/null 2>&1'
fi

if [ -n "$SELECTED_CONFIGS" ]; then
    BACKUP_DIR=~/.config/mgOS_backup_$(date +%Y%m%d_%H%M%S)
    mapfile -t SELECTED_ARRAY <<< "$SELECTED_CONFIGS"
    for conf in "${SELECTED_ARRAY[@]}"; do
        if [ -n "$conf" ] && [ -d "./configs/$conf" ]; then
            if [ -d ~/.config/"$conf" ]; then
                mkdir -p "$BACKUP_DIR"
                gum spin --spinner line --title "$(t spin_backup) $conf..." -- mv ~/.config/"$conf" "$BACKUP_DIR/"
            fi
            gum spin --spinner line --title "$(t spin_install) $conf..." -- cp -r "./configs/$conf" ~/.config/
        fi
    done
    # Шляхи: знімок зроблено з домашньої теки автора (/home/doki) — замінюємо на $HOME
    # нового користувача в усіх ТЕКСТОВИХ файлах (бінарники не чіпаємо: вони вже без шляхів).
    SRC_HOME=$(cat ./system_state/source_home.txt 2>/dev/null || echo /home/doki)
    if [ "$SRC_HOME" != "$HOME" ]; then
        gum spin --spinner line --title "$(t spin_paths) ($SRC_HOME → $HOME)..." -- bash -c '
            for conf in "$@"; do
                [ -d "$HOME/.config/$conf" ] || continue
                grep -rlI --null -F "'"$SRC_HOME"'" "$HOME/.config/$conf" 2>/dev/null | xargs -0 -r sed -i "s|'"$SRC_HOME"'|$HOME|g"
            done' _ "${SELECTED_ARRAY[@]}"
    fi
    if [ -d ~/.config/SYSui ]; then
        find ~/.config/SYSui/ -type s -delete 2>/dev/null||true
        chmod +x ~/.config/SYSui/*_osd ~/.config/SYSui/dock ~/.config/SYSui/topbar ~/.config/SYSui/wallpapers \
                 ~/.config/SYSui/controller_addon ~/.config/SYSui/*.sh 2>/dev/null||true
        # Збираємо з сирців під бібліотеки ЦІЄЇ системи. build.sh не чіпає готові
        # бінарники, якщо збірка впала, — тоді лишаються ті, що в установщику.
        if [ -x ~/.config/SYSui/build.sh ]; then
            if ! gum spin --spinner line --title "$(t spin_build)" -- bash ~/.config/SYSui/build.sh; then
                echo "⚠️ $(t build_fail)"
            fi
        fi
    fi
    [ -f ~/.config/megumin-os/sync_sddm.sh ] && chmod +x ~/.config/megumin-os/sync_sddm.sh
else
    echo "$(t no_cfg)"
fi

SRC_HOME=$(cat ./system_state/source_home.txt 2>/dev/null || echo /home/doki)
if [ -d "./cache_wal" ]; then
    gum spin --spinner line --title "$(t spin_wal)" -- bash -c 'cp -r ./cache_wal/* ~/.cache/wal/ 2>/dev/null||true'
    # colors-hyprland.conf, colors.css тощо містять шляхи до шпалер
    if [ "$SRC_HOME" != "$HOME" ]; then
        grep -rlI --null -F "$SRC_HOME" ~/.cache/wal 2>/dev/null | xargs -0 -r sed -i "s|$SRC_HOME|$HOME|g"
    fi
fi

if [ -d "./system_state" ]; then
    gum spin --spinner line --title "$(t spin_state)" -- bash -c '
        mkdir -p ~/.cache/ignis ~/.cache/megumin-os
        [ -f ./system_state/last_wallpaper.txt ] && sed "s|'"$SRC_HOME"'|$HOME|g" ./system_state/last_wallpaper.txt > ~/.cache/ignis/last_wallpaper.txt
        [ -f ./system_state/dock_history.txt ] && [ ! -f ~/.cache/megumin-os/dock_history.txt ] && cp ./system_state/dock_history.txt ~/.cache/megumin-os/
        true'
fi

if [ "$COPY_WALLPAPERS" = true ] && [ -d "./wallpapers" ]; then
    gum spin --spinner line --title "$(t spin_wall)" -- bash -c 'cp -r ./wallpapers/* ~/Pictures/wallpapers/ 2>/dev/null||true'
else
    echo "$(t skip_wall)"
fi

if [ -d "./plymouth_theme" ]; then
    gum spin --spinner line --title "$(t spin_plymouth)" -- bash -c 'sudo mkdir -p /usr/share/plymouth/themes/spinner/; sudo cp -r ./plymouth_theme/* /usr/share/plymouth/themes/spinner/ 2>/dev/null||true; sudo plymouth-set-default-theme -R spinner 2>/dev/null||true'
    gum spin --spinner line --title "$(t spin_initram)" -- bash -c 'if command -v dracut &>/dev/null; then sudo dracut -f --no-hostonly-cmdline 2>/dev/null||true; elif command -v mkinitcpio &>/dev/null; then sudo mkinitcpio -P 2>/dev/null||true; fi'
fi

if [ -d "./scripts" ]; then
    gum spin --spinner line --title "$(t spin_scripts)" -- bash -c 'for f in ./scripts/*; do [ -f "$f" ] || continue; sudo install -m 755 "$f" /usr/local/bin/; done'
fi

gum spin --spinner line --title "$(t spin_services)" -- bash -c '
    sudo systemctl enable --now power-profiles-daemon 2>/dev/null||true
    sudo systemctl enable --now bluetooth 2>/dev/null||true
    sudo systemctl enable --now NetworkManager 2>/dev/null||true
    sudo systemctl enable -f sddm 2>/dev/null||true
'

# Копіюємо тему SDDM (sddm-astronaut-theme / megumin)
if [ -d "./sddm_theme" ]; then
    gum spin --spinner line --title "🌟 Тема SDDM..." -- bash -c '
        sudo mkdir -p /usr/share/sddm/themes/sddm-astronaut-theme
        sudo cp -r ./sddm_theme/* /usr/share/sddm/themes/sddm-astronaut-theme/
        # Прописуємо тему в налаштуваннях SDDM
        sudo mkdir -p /etc/sddm.conf.d
        printf "[Theme]\nCurrent=sddm-astronaut-theme\n" | sudo tee /etc/sddm.conf.d/theme.conf >/dev/null
    ' 2>/dev/null || true
fi

# Курсор: пакет bibata-cursor-theme-bin АБО копія з установщика (на системі автора він стоїть вручну)
if [ -d ./icons ]; then
    gum spin --spinner line --title "$(t spin_cursor)" -- bash -c '
        for d in ./icons/*/; do
            n=$(basename "$d")
            if [ "$n" = default ]; then mkdir -p ~/.local/share/icons/default; cp -r "$d". ~/.local/share/icons/default/
            elif [ ! -d "/usr/share/icons/$n" ]; then sudo cp -r "$d" "/usr/share/icons/$n"; fi
        done; true'
fi

# Тема GTK / іконки / курсор / шрифти — рівно як на системі в момент знімка.
# Без сесійного D-Bus (запуск з TTY) gsettings нічого не зберігає, тому піднімаємо свою шину.
if [ -f ./system_state/gsettings.txt ]; then
    GS=(gsettings)
    [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && GS=(dbus-run-session -- gsettings)
    while read -r schema key value; do
        [ -n "$key" ] && "${GS[@]}" set "$schema" "$key" "$value" 2>/dev/null || true
    done < ./system_state/gsettings.txt
    echo "  $(t spin_theme) OK"
elif ls ~/.local/share/fonts/*SF-Pro* 1>/dev/null 2>&1; then
    gsettings set org.gnome.desktop.interface font-name 'SF-Pro-Text-Bold 11' 2>/dev/null||true
else
    echo "$(t font_missing)"
fi

# Wallpapers/Settings викликають "sudo -n sync_sddm.sh" — без цього правила синхронізація теми SDDM не працює
if [ -f ~/.config/megumin-os/sync_sddm.sh ]; then
    SUDOERS_TMP=$(mktemp)
    printf '%s ALL=(ALL) NOPASSWD: %s\n' "$USER" "$HOME/.config/megumin-os/sync_sddm.sh" > "$SUDOERS_TMP"
    if sudo visudo -cf "$SUDOERS_TMP" >/dev/null 2>&1; then
        sudo install -m 440 -o root -g root "$SUDOERS_TMP" /etc/sudoers.d/mgos-sddm-sync
        echo "  $(t spin_sudoers) OK"
    fi
    rm -f "$SUDOERS_TMP"
fi

bios_section "$(t sec_shell)"
if [ "$SHELL" != "/usr/bin/fish" ]; then
    if gum confirm "$(t q_fish)"; then chsh -s /usr/bin/fish; fi
fi

bios_section "$(t sec_gpu)"
GPU_INFO=$(lspci | grep -iE "VGA|3D" || true)
if echo "$GPU_INFO" | grep -iq "nvidia"; then
    echo "$(t gpu_nvidia)"
else
    echo "$(t gpu_other)"
    gum spin --spinner line --title "$(t spin_nvidia)" -- bash -c "if [ -f ~/.config/hypr/hyprland.conf ]; then sed -i 's/^env = LIBVA_DRIVER_NAME, nvidia/# env = LIBVA_DRIVER_NAME, nvidia/g' ~/.config/hypr/hyprland.conf; sed -i 's/^env = GBM_BACKEND, nvidia-drm/# env = GBM_BACKEND, nvidia-drm/g' ~/.config/hypr/hyprland.conf; sed -i 's/^env = NVD_BACKEND,direct/# env = NVD_BACKEND,direct/g' ~/.config/hypr/hyprland.conf; sed -i 's/^env = WLR_NO_HARDWARE_CURSORS, 1/# env = WLR_NO_HARDWARE_CURSORS, 1/g' ~/.config/hypr/hyprland.conf; fi"
fi

# Масштаб за щільністю пікселів екрана, жести тачпада з kiosk, розкладки — той самий крок, що в GUI
bios_section "$(t sec_hw)"
MGOS_DIR="$SRC_DIR" bash -c 'source "$MGOS_DIR/gui/steps.sh" && step_hw' 2>&1 \
    | sed -u -e '/^@@SUB /d' -e 's/^@@MSG /  › /' -e 's/^@@FAILPKG /  ⚠ пакет: /'

# ============================================================================
#  ПЕРЕВІРКА: чи система справді відповідає знімку
# ============================================================================
bios_section "$(t sec_verify)"
VERIFY_FAIL=0
vcheck() {  # vcheck "назва" команда...
    local name="$1"; shift
    post_line "$name"
    if "$@" >/dev/null 2>&1; then post_ok; else post_fail; VERIFY_FAIL=$((VERIFY_FAIL+1)); fi
}
for cmd in hyprctl ignis dunst awww mpvpaper socat nmcli wpctl grim slurp cliphist; do
    vcheck "$cmd" command -v "$cmd"
done
vcheck "python3 + PyQt6" python3 -c "import PyQt6.QtWidgets"
if [ -d ~/.config/SYSui ]; then
    for b in dock topbar cc_osd runner_osd wallpapers clipboard_osd notify_osd megumin_osd powermenu_osd volume_osd about_osd; do
        # бінарник є, виконуваний і всі бібліотеки знаходяться
        vcheck "SYSui/$b" bash -c "[ -x ~/.config/SYSui/$b ] && ! ldd ~/.config/SYSui/$b | grep -q 'not found'"
    done
fi
[ -f ~/.config/hypr/hyprland.conf ] && vcheck "hyprland.conf (шляхи)" bash -c "! grep -q '${SRC_HOME:-/home/doki}/' ~/.config/hypr/hyprland.conf || [ '${SRC_HOME:-/home/doki}' = '$HOME' ]"
CUR_THEME=$(awk '$2=="cursor-theme"{gsub("\x27","",$3); print $3}' ./system_state/gsettings.txt 2>/dev/null)
[ -n "$CUR_THEME" ] && vcheck "курсор $CUR_THEME" bash -c "[ -d /usr/share/icons/$CUR_THEME ] || [ -d ~/.local/share/icons/$CUR_THEME ]"
# питаємо sudo, чи дозволено без пароля (правило може лежати будь-де в sudoers)
[ -f ~/.config/megumin-os/sync_sddm.sh ] && vcheck "sudoers sync_sddm" sudo -n -l "$HOME/.config/megumin-os/sync_sddm.sh"
vcheck "SDDM увімкнено" systemctl is-enabled sddm
echo
if [ "$VERIFY_FAIL" -eq 0 ]; then echo "  ✅ $(t verify_all_ok)"; else echo "  ⚠️  $(t verify_bad)"; fi
sleep 2

# після перезавантаження в mgOS — вікно «Все готово до роботи» (gui/welcome.sh)
mkdir -p "${XDG_STATE_HOME:-$HOME/.local/state}/mgos-installer" && echo mgos > "${XDG_STATE_HOME:-$HOME/.local/state}/mgos-installer/welcome-pending"

# ============================================================================
#  ФІНАЛЬНИЙ ЕКРАН
# ============================================================================
clear; draw_bios_fullscreen
draw_box \
    "@$(t done_title)" \
    "@$(t done_sub)" \
    "---" \
    "$(t done_reboot)" \
    "$(t done_gdrive)" \
    "$(t done_log) $LOG_FILE"
echo

if [ "${#FAILED_PACKAGES[@]}" -gt 0 ]; then
    echo "⚠️  $(t done_failed)"
    printf '   - %s\n' "${FAILED_PACKAGES[@]}"
fi
