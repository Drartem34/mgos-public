#!/usr/bin/env bash
# ============================================================================
#  mgos-arch-install — встановлення Arch Linux / Arch + Hyprland / Megumin OS
#  з живої флешки на диск. Запускає графічний mgos-setup (через sudo), але
#  працює і сам:   sudo mgos-arch-install.sh /шлях/до/install.conf
#
#  install.conf — KEY=VALUE (bash), файл 0600; видаляється одразу після читання:
#    EDITION=arch|hyprland|mgos
#    DISK=/dev/nvme0n1   MODE=erase|free|part   ROOT_PART=/dev/… (лише MODE=part)
#    FS=btrfs|ext4       ENCRYPT=0|1   LUKS_PASS=…
#    SWAP_ZRAM=0|1
#    LOCALE=uk_UA.UTF-8  EXTRA_LOCALES="en_US.UTF-8"  TIMEZONE=Europe/Kyiv  NTP=0|1
#    KB_LAYOUTS=us,ua    KB_TOGGLE=grp:alt_shift_toggle
#    FULLNAME="Doki"     USERNAME=doki   USER_PASS=…   HOSTNAME=doki-laptop   AUTOLOGIN=0|1
#    ROOT_PASS=…         (порожньо → root заблоковано, адмін — через sudo)
#    EXTRA_PKGS="steam obs-studio"
#    MGOS_SRC=/opt/mgos/recovery   (тека інсталятора mgOS на флешці)
#    DRY_RUN=1           (нічого не змінює — лише показує кроки; для перевірки GUI)
#
#  Протокол виводу (його розбирає GUI, кожен рядок окремо):
#    @@STEP id           — почався крок
#    @@DONE id           — крок завершено
#    @@PCT n             — прогрес усередині кроку, 0..100
#    @@MSG текст         — що зараз відбувається
#    @@WARN текст        — щось не вийшло, але встановлення продовжується
#    @@FAIL текст        — фатальна помилка, встановлення зупинено
#    @@END ok            — усе готово
#  Решта виводу — журнал (pacman/pacstrap: «(3/80) installing foo» → @@PCT).
# ============================================================================
set -o pipefail
export LC_ALL=C.UTF-8
umask 022

CONF=${1:?потрібен шлях до install.conf}
[ "$CONF" = --detect ] && DETECT=1
[ "${DETECT:-0}" = 1 ] || [ -r "$CONF" ] || { echo "@@FAIL Немає файлу налаштувань $CONF"; exit 1; }
if [ "${DETECT:-0}" != 1 ]; then
# shellcheck disable=SC1090
source "$CONF"
[ "${DRY_RUN:-0}" = 1 ] || [ "$(id -u)" = 0 ] || { echo "@@FAIL Потрібні права root"; exit 1; }
[ "${KEEP_CONF:-0}" = 1 ] || shred -u "$CONF" 2>/dev/null || rm -f "$CONF"
fi

: "${EDITION:=mgos}" "${MODE:=erase}" "${FS:=btrfs}" "${ENCRYPT:=0}" "${SWAP_ZRAM:=1}"
: "${LOCALE:=uk_UA.UTF-8}" "${EXTRA_LOCALES:=en_US.UTF-8}" "${TIMEZONE:=UTC}" "${NTP:=1}"
: "${KB_LAYOUTS:=us}" "${KB_TOGGLE:=grp:alt_shift_toggle}" "${AUTOLOGIN:=0}" "${DRY_RUN:=0}"
: "${MGOS_SRC:=/opt/mgos/recovery}" "${HOSTNAME:=mgos}"
MNT=/mnt/mgos
LOG=/var/log/mgos-install.log; [ "$DRY_RUN" = 1 ] && LOG=${TMPDIR:-/tmp}/mgos-install-dry.log
STEPS_DONE=()

say()  { echo "@@MSG $*"; }
pct()  { echo "@@PCT $1"; }
warn() { echo "@@WARN $*"; }
die()  { echo "@@FAIL $*"; cleanup_mounts; exit 1; }
run()  { if [ "$DRY_RUN" = 1 ]; then echo "[dry] $*"; [ -z "${DRY_DELAY:-}" ] || sleep "$DRY_DELAY"; else "$@"; fi; }
inch() { if [ "$DRY_RUN" = 1 ]; then echo "[dry chroot] $*"; else arch-chroot "$MNT" "$@"; fi; }

# step <id> <функція> — обгортка: @@STEP/@@DONE, помилка → @@FAIL
step() {
    local id=$1 fn=$2
    echo "@@STEP $id"; pct 0
    "$fn" || die "Крок «$id» не вдався — подробиці в журналі $LOG"
    pct 100; echo "@@DONE $id"; STEPS_DONE+=("$id")
}

# pacstrap/pacman з прогресом: «(12/80) installing foo» → @@PCT і @@MSG
pkg_progress() {
    local line n t
    while IFS= read -r line; do
        echo "$line"
        if [[ "$line" =~ ^\(\ *([0-9]+)/([0-9]+)\)\ (installing|upgrading|reinstalling)\ ([^ .]+) ]]; then
            n=${BASH_REMATCH[1]}; t=${BASH_REMATCH[2]}
            echo "@@PCT $(( n * 100 / t ))"; echo "@@MSG ${BASH_REMATCH[3]} ${BASH_REMATCH[4]} ($n / $t)"
        fi
    done
}
# повтор (дзеркала інколи відвалюються посеред завантаження)
retry() { local i; for i in 1 2 3; do "$@" && return 0; warn "Не вдалося (спроба $i/3) — повторюю"; sleep 3; done; return 1; }

part() { [[ $1 =~ [0-9]$ ]] && echo "${1}p$2" || echo "$1$2"; }   # nvme0n1 → nvme0n1p2, sda → sda2
uefi() { [ -d /sys/firmware/efi/efivars ]; }

cleanup_mounts() {
    [ "$DRY_RUN" = 1 ] && return
    swapoff -a 2>/dev/null
    umount -R "$MNT" 2>/dev/null
    [ -e /dev/mapper/mgosroot ] && cryptsetup close mgosroot 2>/dev/null
    true
}

# ---------------------------------------------------------------------------
#  Обладнання (так само, як gui/steps.sh у mgOS, але лише пакети з офіційних репозиторіїв)
# ---------------------------------------------------------------------------
pci_has() { local d; for d in /sys/bus/pci/devices/*; do
    [ "$(cat "$d/vendor" 2>/dev/null)" = "$1" ] && [[ "$(cat "$d/class" 2>/dev/null)" == "$2"* ]] && return 0; done; return 1; }
hw_packages() {
    local p=()
    grep -q GenuineIntel /proc/cpuinfo && p+=(intel-ucode)
    grep -q AuthenticAMD /proc/cpuinfo && p+=(amd-ucode)
    if pci_has 0x8086 0x03; then p+=(mesa vulkan-intel intel-media-driver); fi
    if pci_has 0x1002 0x03; then p+=(mesa vulkan-radeon); fi
    if pci_has 0x10de 0x03; then
        p+=(nvidia-open-dkms nvidia-utils libva-nvidia-driver)
        { pci_has 0x8086 0x03 || pci_has 0x1002 0x03; } && p+=(nvidia-prime)
    fi
    compgen -G '/sys/module/snd_sof_pci_intel_*' >/dev/null && p+=(sof-firmware alsa-ucm-conf)
    pci_has 0x14e4 0x0280 && p+=(broadcom-wl-dkms)
    compgen -G '/sys/class/bluetooth/hci*' >/dev/null && p+=(bluez bluez-utils)
    compgen -G '/sys/class/power_supply/BAT*' >/dev/null && p+=(power-profiles-daemon brightnessctl)
    case $(systemd-detect-virt 2>/dev/null) in
        kvm|qemu) p+=(qemu-guest-agent spice-vdagent) ;;
        oracle)   p+=(virtualbox-guest-utils) ;;
        vmware)   p+=(open-vm-tools) ;;
    esac
    [[ " ${p[*]} " == *-dkms* ]] && p+=(linux-headers)
    printf '%s\n' "${p[@]}" | awk '!s[$0]++'
}

# --detect: що знайдено і які пакети підуть під це залізо (сторінка «Драйвери» в GUI)
#   @@HW іконка|що|пристрій|пакети
detect() {
    local l name
    lspci_name() { lspci -mm -d "$1::$2" 2>/dev/null | head -1 | awk -F'"' '{print $6}' | sed 's/\[\(.*\)\]/\1/; s/ *(rev.*//'; }
    name=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//; s/(R)//g; s/(TM)//g; s/ CPU//; s/ @.*//')
    echo "@@HW 󰻠|cpu|$name|$(grep -q GenuineIntel /proc/cpuinfo && echo intel-ucode)$(grep -q AuthenticAMD /proc/cpuinfo && echo amd-ucode)"
    if pci_has 0x8086 0x03; then echo "@@HW 󰢮|gpu|Intel $(lspci_name 8086 0300)|mesa vulkan-intel intel-media-driver"; fi
    if pci_has 0x1002 0x03; then echo "@@HW 󰢮|gpu|AMD $(lspci_name 1002 0300)|mesa vulkan-radeon"; fi
    if pci_has 0x10de 0x03; then
        l="nvidia-open-dkms nvidia-utils libva-nvidia-driver"
        { pci_has 0x8086 0x03 || pci_has 0x1002 0x03; } && l="$l nvidia-prime"
        echo "@@HW 󰢮|dgpu|NVIDIA $(lspci_name 10de 0300)$(lspci_name 10de 0302)|$l"
    else echo "@@HW 󰢮|dgpu||"; fi
    name=$(lspci -mm 2>/dev/null | awk -F'"' '$2 ~ /Network controller/ {print $4" "$6; exit}' | sed 's/ Corporation//; s/\[\(.*\)\]/\1/')
    echo "@@HW 󰖩|wifi|$name|$(pci_has 0x14e4 0x0280 && echo broadcom-wl-dkms)"
    if compgen -G '/sys/class/bluetooth/hci*' >/dev/null; then echo "@@HW 󰂯|bt|$name|bluez bluez-utils"; else echo "@@HW 󰂯|bt||"; fi
    if compgen -G '/sys/class/power_supply/BAT*' >/dev/null; then echo "@@HW 󰁹|laptop|$(cat /sys/class/dmi/id/product_name 2>/dev/null)|power-profiles-daemon brightnessctl"
    else echo "@@HW 󰇄|laptop||"; fi
    name=$(lspci -mm 2>/dev/null | awk -F'"' '$2 ~ /Audio|Multimedia/ {print $4" "$6; exit}' | sed 's/ Corporation//; s/\[\(.*\)\]/\1/')
    echo "@@HW 󰕾|audio|$name|$(compgen -G '/sys/module/snd_sof_pci_intel_*' >/dev/null && echo sof-firmware alsa-ucm-conf)"
    l=$(systemd-detect-virt 2>/dev/null); [ "$l" = none ] && l=""
    [ -n "$l" ] && echo "@@HW 󰍹|virt|$l|$(hw_packages | grep -E 'guest|vdagent|vm-tools' | xargs)"
    echo "@@HW 󰋊|fw|$(uefi && echo UEFI || echo BIOS)|"
    return 0
}
[ "${DETECT:-0}" = 1 ] && { detect; exit 0; }

# ---------------------------------------------------------------------------
#  1. Підготовка
# ---------------------------------------------------------------------------
s_prepare() {
    say "Перевіряю систему"
    [ -b "$DISK" ] || { echo "Диск $DISK не знайдено"; return 1; }
    [ -n "$USERNAME" ] && [ -n "$USER_PASS" ] || { echo "Не задано користувача або пароль"; return 1; }
    [[ $USERNAME =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] || { echo "Недопустиме ім'я користувача: $USERNAME"; return 1; }
    if [ "$EDITION" = mgos ] && [ ! -d "$MGOS_SRC/gui" ]; then echo "Немає файлів mgOS у $MGOS_SRC"; return 1; fi
    pct 20
    say "Перевіряю інтернет"
    if ! retry curl -fsI --max-time 8 https://archlinux.org >/dev/null 2>&1; then
        echo "Немає інтернету — пакети встановлюються з дзеркал Arch"; return 1
    fi
    pct 40
    [ "$NTP" = 1 ] && run timedatectl set-ntp true
    # живе середовище могло завантажитися давно — ключі та бази мають бути свіжими
    say "Оновлюю ключі пакетів"
    run retry pacman -Sy --noconfirm --needed archlinux-keyring 2>&1 | pkg_progress
    pct 70
    # паралельне завантаження у живому середовищі (pacstrap бере налаштування звідси)
    run sed -i 's/^#\?ParallelDownloads.*/ParallelDownloads = 8/' /etc/pacman.conf
    cleanup_mounts
    return 0
}

# ---------------------------------------------------------------------------
#  2. Розмітка диска
#  erase — увесь диск: ESP 1 ГіБ (/boot, FAT32) + корінь
#  free  — найбільше вільне місце: XBOOTLDR 1 ГіБ (/boot) + корінь; наявний ESP (Windows) → /efi
#  part  — наявний розділ ROOT_PART форматується під корінь; ESP береться наявний
# ---------------------------------------------------------------------------
ESP=""; XBOOT=""; ROOTDEV=""; ROOTRAW=""
find_esp() {   # перший ESP на диску $1
    lsblk -lnpo NAME,PARTTYPE "$1" 2>/dev/null | awk 'tolower($2)=="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"{print $1; exit}'
}
s_disk() {
    local n
    uefi || [ "$MODE" = erase ] || { echo "Без UEFI підтримується лише встановлення на весь диск"; return 1; }
    say "Звільняю $DISK"
    run swapoff -a 2>/dev/null
    for n in $(lsblk -lnpo NAME "$DISK" | tac); do run umount -q "$n" 2>/dev/null; done
    case $MODE in
    erase)
        run wipefs -af "$DISK" >/dev/null
        run sgdisk -Z "$DISK" >/dev/null
        if uefi; then
            run sgdisk -n1:1MiB:+1GiB -t1:ef00 -c1:EFI -n2:0:0 -t2:8304 -c2:mgos "$DISK"
            ESP=$(part "$DISK" 1); ROOTRAW=$(part "$DISK" 2)
        else   # BIOS + GPT: крихітний розділ для GRUB
            run sgdisk -n1:1MiB:+1MiB -t1:ef02 -c1:biosboot -n2:0:0 -t2:8304 -c2:mgos "$DISK"
            ROOTRAW=$(part "$DISK" 2)
        fi ;;
    free)
        ESP=$(find_esp "$DISK")
        [ -n "$ESP" ] || { echo "На диску немає розділу EFI — оберіть «Стерти диск»"; return 1; }
        local before after
        before=$(lsblk -lnpo NAME "$DISK" | sort)
        # «0» як початок — sgdisk бере найбільший вільний блок
        run sgdisk -n0:0:+1GiB -t0:ea00 -c0:mgos-boot "$DISK" || return 1
        run partprobe "$DISK"; sleep 1
        after=$(lsblk -lnpo NAME "$DISK" | sort)
        XBOOT=$(comm -13 <(echo "$before") <(echo "$after") | head -1)
        run sgdisk -n0:0:0 -t0:8304 -c0:mgos "$DISK" || return 1
        run partprobe "$DISK"; sleep 1
        ROOTRAW=$(comm -13 <(echo "$after") <(lsblk -lnpo NAME "$DISK" | sort) | head -1)
        [ -n "$XBOOT" ] && [ -n "$ROOTRAW" ] || [ "$DRY_RUN" = 1 ] || { echo "Не вдалося створити розділи у вільному місці"; return 1; }
        ;;
    part)
        [ -b "$ROOT_PART" ] || { echo "Розділ $ROOT_PART не знайдено"; return 1; }
        ESP=$(find_esp "$DISK"); [ -n "$ESP" ] || ESP=$(lsblk -lnpo NAME,PARTTYPE | awk 'tolower($2)=="c12a7328-f81f-11d2-ba4b-00a0c93ec93b"{print $1; exit}')
        [ -n "$ESP" ] || { echo "Розділ EFI не знайдено"; return 1; }
        local esp_mb; esp_mb=$(( $(lsblk -bndo SIZE "$ESP") / 1048576 ))
        [ "$esp_mb" -ge 400 ] || { echo "Розділ EFI замалий (${esp_mb} МБ) — оберіть «Поруч з іншою ОС» або «Стерти диск»"; return 1; }
        ROOTRAW=$ROOT_PART ;;
    *) echo "Невідомий режим розмітки: $MODE"; return 1 ;;
    esac
    run partprobe "$DISK" 2>/dev/null; run udevadm settle
    pct 35

    ROOTDEV=$ROOTRAW
    if [ "$ENCRYPT" = 1 ]; then
        say "Шифрую розділ (LUKS2)"
        printf '%s' "$LUKS_PASS" | run cryptsetup luksFormat --type luks2 -q --pbkdf argon2id "$ROOTRAW" -d - || return 1
        printf '%s' "$LUKS_PASS" | run cryptsetup open "$ROOTRAW" mgosroot -d - || return 1
        ROOTDEV=/dev/mapper/mgosroot
    fi
    pct 55

    say "Форматую ($FS)"
    [ "$MODE" = erase ] && [ -n "$ESP" ] && { run mkfs.fat -F32 -n EFI "$ESP" >/dev/null || return 1; }
    [ -n "$XBOOT" ] && { run mkfs.fat -F32 -n MGOSBOOT "$XBOOT" >/dev/null || return 1; }
    local o=noatime
    if [ "$FS" = btrfs ]; then
        run mkfs.btrfs -f -L mgos "$ROOTDEV" >/dev/null || return 1
        run mount "$ROOTDEV" "$MNT" || return 1
        for n in @ @home @log @pkg @snapshots; do run btrfs subvolume create "$MNT/$n" >/dev/null; done
        run umount "$MNT"
        o=noatime,compress=zstd:1
        run mount -o "$o,subvol=@" "$ROOTDEV" "$MNT" || return 1
        run mkdir -p "$MNT"/{home,var/log,var/cache/pacman/pkg,.snapshots}
        run mount -o "$o,subvol=@home" "$ROOTDEV" "$MNT/home"
        run mount -o "$o,subvol=@log" "$ROOTDEV" "$MNT/var/log"
        run mount -o "$o,subvol=@pkg" "$ROOTDEV" "$MNT/var/cache/pacman/pkg"
        run mount -o "$o,subvol=@snapshots" "$ROOTDEV" "$MNT/.snapshots"
    else
        run mkfs.ext4 -F -L mgos "$ROOTDEV" >/dev/null || return 1
        run mount -o "$o" "$ROOTDEV" "$MNT" || return 1
    fi
    pct 85
    if [ -n "$XBOOT" ]; then
        run mkdir -p "$MNT/boot" "$MNT/efi"
        run mount "$XBOOT" "$MNT/boot" && run mount "$ESP" "$MNT/efi" || return 1
    elif [ -n "$ESP" ]; then
        run mkdir -p "$MNT/boot"; run mount "$ESP" "$MNT/boot" || return 1
    fi
    lsblk "$DISK"
    return 0
}

# ---------------------------------------------------------------------------
#  3. Базова система
# ---------------------------------------------------------------------------
BASE_PKGS=(base linux linux-firmware sudo networkmanager nano vim man-db man-pages bash-completion
           dosfstools e2fsprogs exfatprogs ntfs-3g btrfs-progs pacman-contrib reflector git wget
           pipewire pipewire-pulse pipewire-alsa wireplumber)
s_base() {
    local p=("${BASE_PKGS[@]}")
    [ "$SWAP_ZRAM" = 1 ] && p+=(zram-generator)
    [ "$ENCRYPT" = 1 ] && p+=(cryptsetup)
    uefi || p+=(grub)
    say "pacstrap: базова система"
    run retry pacstrap -K "$MNT" "${p[@]}" 2>&1 | pkg_progress || return 1
    say "Таблиця монтування (fstab)"
    [ "$DRY_RUN" = 1 ] || genfstab -U "$MNT" >> "$MNT/etc/fstab"
    # налаштування pacman — як у живому середовищі (паралельно, кольори)
    run sed -i 's/^#\?ParallelDownloads.*/ParallelDownloads = 8/; s/^#Color/Color/' "$MNT/etc/pacman.conf"
    return 0
}

# ---------------------------------------------------------------------------
#  4. Драйвери під це залізо
# ---------------------------------------------------------------------------
s_drivers() {
    local p; mapfile -t p < <(hw_packages)
    echo "Під це залізо: ${p[*]:-нічого додаткового}"
    [ ${#p[@]} -eq 0 ] && return 0
    say "Драйвери: ${p[*]}"
    run retry inch pacman -S --noconfirm --needed "${p[@]}" 2>&1 | pkg_progress || return 1
    return 0
}

# ---------------------------------------------------------------------------
#  5. Система: мова, час, клавіатура, ім'я ПК, користувач, завантажувач, служби
# ---------------------------------------------------------------------------
write() { if [ "$DRY_RUN" = 1 ]; then echo "[dry] → $1"; cat >/dev/null; else mkdir -p "$(dirname "$1")"; cat > "$1"; fi; }
s_system() {
    say "Мова і регіон"
    local l
    for l in $LOCALE $EXTRA_LOCALES; do run sed -i "s/^#\s*\($l .*\)/\1/" "$MNT/etc/locale.gen"; done
    inch locale-gen >/dev/null
    echo "LANG=$LOCALE" | write "$MNT/etc/locale.conf"
    run ln -sf "/usr/share/zoneinfo/$TIMEZONE" "$MNT/etc/localtime"
    inch hwclock --systohc 2>/dev/null
    [ "$NTP" = 1 ] && inch systemctl enable systemd-timesyncd.service >/dev/null 2>&1
    pct 10

    say "Клавіатура: $KB_LAYOUTS"
    printf 'KEYMAP=%s\n' "${KB_LAYOUTS%%,*}" | write "$MNT/etc/vconsole.conf"
    write "$MNT/etc/X11/xorg.conf.d/00-keyboard.conf" <<EOF
Section "InputClass"
        Identifier "system-keyboard"
        MatchIsKeyboard "on"
        Option "XkbLayout" "$KB_LAYOUTS"
        Option "XkbOptions" "$KB_TOGGLE"
EndSection
EOF
    pct 20

    say "Ім'я комп'ютера: $HOSTNAME"
    echo "$HOSTNAME" | write "$MNT/etc/hostname"
    printf '127.0.0.1 localhost\n::1 localhost\n127.0.1.1 %s.localdomain %s\n' "$HOSTNAME" "$HOSTNAME" | write "$MNT/etc/hosts"

    say "Користувач $USERNAME"
    inch useradd -m -G wheel,video,audio,input,storage -s /bin/bash -c "${FULLNAME:-$USERNAME}" "$USERNAME" 2>/dev/null \
        || inch usermod -aG wheel,video,audio,input,storage "$USERNAME"
    [ "$DRY_RUN" = 1 ] || printf '%s:%s\n' "$USERNAME" "$USER_PASS" | arch-chroot "$MNT" chpasswd || return 1
    if [ -n "${ROOT_PASS:-}" ]; then
        [ "$DRY_RUN" = 1 ] || printf 'root:%s\n' "$ROOT_PASS" | arch-chroot "$MNT" chpasswd
    else
        inch passwd -l root >/dev/null
    fi
    echo '%wheel ALL=(ALL:ALL) ALL' | write "$MNT/etc/sudoers.d/10-wheel"
    run chmod 440 "$MNT/etc/sudoers.d/10-wheel"
    pct 35

    if [ "$SWAP_ZRAM" = 1 ]; then
        printf '[zram0]\nzram-size = min(ram / 2, 8192)\ncompression-algorithm = zstd\n' | write "$MNT/etc/systemd/zram-generator.conf"
    fi

    say "initramfs"
    local hooks="base systemd autodetect microcode modconf kms keyboard sd-vconsole block filesystems fsck"
    [ "$ENCRYPT" = 1 ] && hooks="base systemd autodetect microcode modconf kms keyboard sd-vconsole block sd-encrypt filesystems fsck"
    run sed -i "s/^HOOKS=.*/HOOKS=($hooks)/" "$MNT/etc/mkinitcpio.conf"
    # без fallback-образу: на маленькому розділі EFI (Windows — 100 МБ) він не вміщається
    run sed -i "s/^PRESETS=.*/PRESETS=('default')/" "$MNT/etc/mkinitcpio.d/linux.preset"
    run rm -f "$MNT/boot/initramfs-linux-fallback.img"
    inch mkinitcpio -P 2>&1 | tail -3
    pct 60

    say "Завантажувач"
    local luuid="" cmd="rw quiet loglevel=3 rd.udev.log_level=3"
    local ruuid; ruuid=$([ "$DRY_RUN" = 1 ] && echo DRYUUID || blkid -s UUID -o value "$ROOTDEV")
    if [ "$ENCRYPT" = 1 ]; then
        luuid=$([ "$DRY_RUN" = 1 ] && echo DRYLUKS || blkid -s UUID -o value "$ROOTRAW")
        cmd="rd.luks.name=$luuid=mgosroot root=/dev/mapper/mgosroot $cmd"
    else
        cmd="root=UUID=$ruuid $cmd"
    fi
    [ "$FS" = btrfs ] && cmd="$cmd rootflags=subvol=@"
    if uefi; then
        if [ -n "$XBOOT" ]; then inch bootctl --esp-path=/efi --boot-path=/boot install || return 1
        else inch bootctl install || return 1; fi
        # мікрокод CPU — усередині initramfs (hook microcode), окремі initrd не потрібні
        local name="Megumin OS"; [ "$EDITION" = mgos ] || name="Arch Linux"
        write "$MNT/boot/loader/entries/mgos.conf" <<EOF
title   $name
linux   /vmlinuz-linux
initrd  /initramfs-linux.img
options $cmd
EOF
        local lroot=/boot; [ -n "$XBOOT" ] && lroot=/efi
        # інша ОС на тому ж диску — показуємо меню 3 с, інакше одразу вантажимось
        local tmo=0; [ "$MODE" != erase ] && tmo=3
        printf 'default mgos.conf\ntimeout %s\nconsole-mode max\neditor no\n' "$tmo" | write "$MNT$lroot/loader/loader.conf"
        inch systemctl enable systemd-boot-update.service >/dev/null 2>&1
    else
        inch grub-install --target=i386-pc "$DISK" || return 1
        # root= і subvol GRUB додає сам; для LUKS — лише rd.luks.name
        run sed -i 's|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT="quiet loglevel=3 rd.udev.log_level=3"|' "$MNT/etc/default/grub"
        [ "$ENCRYPT" = 1 ] && run sed -i "s|^GRUB_CMDLINE_LINUX=.*|GRUB_CMDLINE_LINUX=\"rd.luks.name=$luuid=mgosroot\"|" "$MNT/etc/default/grub"
        inch grub-mkconfig -o /boot/grub/grub.cfg 2>&1 | tail -2
    fi
    pct 85

    say "Служби"
    local s
    for s in NetworkManager fstrim.timer bluetooth power-profiles-daemon qemu-guest-agent vboxservice; do
        [ -e "$MNT/usr/lib/systemd/system/$s.service" ] || [ -e "$MNT/usr/lib/systemd/system/$s" ] || continue
        inch systemctl enable "$s" >/dev/null 2>&1
    done
    # Wi-Fi, до якого підключились у живому середовищі — щоб інтернет був одразу після входу
    if [ -d /etc/NetworkManager/system-connections ]; then
        run mkdir -p "$MNT/etc/NetworkManager/system-connections"
        run cp -a /etc/NetworkManager/system-connections/. "$MNT/etc/NetworkManager/system-connections/" 2>/dev/null
    fi
    return 0
}

# ---------------------------------------------------------------------------
#  6. Hyprland (варіанти «Arch + Hyprland» і mgOS)
# ---------------------------------------------------------------------------
HYPR_PKGS=(hyprland xdg-desktop-portal-hyprland xdg-desktop-portal-gtk hyprpolkitagent qt5-wayland qt6-wayland
           kitty dunst nautilus firefox sddm grim slurp wl-clipboard pavucontrol
           noto-fonts noto-fonts-emoji ttf-jetbrains-mono-nerd)
# лише «Arch + Hyprland»: у mgOS панелі, меню й мережа — власні (SYSui)
HYPR_ONLY_PKGS=(waybar wofi network-manager-applet)
s_desktop() {
    local p=("${HYPR_PKGS[@]}") x artcraft=0
    [ "$EDITION" = hyprland ] && p+=("${HYPR_ONLY_PKGS[@]}")
    for x in $EXTRA_PKGS; do
        case $x in
            artcraft) artcraft=1; p+=(flatpak) ;;   # Flatpak, не пакет pacman
            *) p+=("$x") ;;
        esac
    done
    # Steam і Wine — лише з multilib
    if [[ " $EXTRA_PKGS " == *" steam "* ]]; then
        say "Вмикаю multilib (для Steam)"
        run sed -i '/^#\[multilib\]/,/^#Include/ s/^#//' "$MNT/etc/pacman.conf"
        inch pacman -Sy --noconfirm >/dev/null
    fi
    say "Hyprland і програми"
    run retry inch pacman -S --noconfirm --needed "${p[@]}" 2>&1 | pkg_progress || return 1
    inch systemctl enable sddm >/dev/null 2>&1
    if [ "$artcraft" = 1 ]; then
        # ArtCraft (getartcraft.com): офіційне Flatpak-сховище, лаунчер ставить і оновлює решту Craft-програм
        say "ArtCraft (Flatpak)"
        inch flatpak remote-add --if-not-exists artcraft-oci https://getartcraft.com/artcraft-oci.flatpakrepo \
            && inch flatpak install -y --noninteractive artcraft-oci ai.storyteller.artcraft-launcher \
            || warn "ArtCraft не встановився — можна поставити пізніше: flatpak install artcraft-oci ai.storyteller.artcraft-launcher"
    fi
    local H="/home/$USERNAME"
    if [ "$EDITION" = hyprland ]; then
        # стартовий конфіг Hyprland + наша розкладка клавіатури
        run install -d "$MNT$H/.config/hypr"
        if [ "$DRY_RUN" != 1 ]; then
            cp "$MNT/usr/share/hypr/hyprland.conf" "$MNT$H/.config/hypr/hyprland.conf" 2>/dev/null
            sed -i "s/^\(\s*kb_layout\s*=\).*/\1 $KB_LAYOUTS/; s/^\(\s*kb_options\s*=\).*/\1 $KB_TOGGLE/" "$MNT$H/.config/hypr/hyprland.conf"
            printf '\nexec-once = waybar\nexec-once = dunst\nexec-once = systemctl --user start hyprpolkitagent\nexec-once = nm-applet\n' >> "$MNT$H/.config/hypr/hyprland.conf"
            arch-chroot "$MNT" chown -R "$USERNAME:$USERNAME" "$H/.config"
        fi
    fi
    if [ "$AUTOLOGIN" = 1 ]; then
        printf '[Autologin]\nUser=%s\nSession=hyprland\n' "$USERNAME" | write "$MNT/etc/sddm.conf.d/autologin.conf"
    fi
    return 0
}

# ---------------------------------------------------------------------------
#  7. Megumin OS — ті самі ідемпотентні кроки, що й у mgOS-інсталятора (gui/steps.sh),
#  але всередині нової системи від імені нового користувача (тимчасово sudo без пароля)
# ---------------------------------------------------------------------------
MGOS_STEPS=(yay repos core stable aur utils drivers files configs home build theme boot scripts services look shell gpu hw)
s_mgos() {
    local H="/home/$USERNAME" D=".local/share/mgos/recovery"
    say "Копіюю файли mgOS"
    run install -d "$MNT$H/.local/share/mgos"
    run cp -a "$MGOS_SRC" "$MNT$H/$D" || return 1
    inch chown -R "$USERNAME:$USERNAME" "$H/.local"
    echo "$USERNAME ALL=(ALL:ALL) NOPASSWD: ALL" | write "$MNT/etc/sudoers.d/00-mgos-install"
    run chmod 440 "$MNT/etc/sudoers.d/00-mgos-install"
    local i=0 n=${#MGOS_STEPS[@]} s rc failed=()
    for s in "${MGOS_STEPS[@]}"; do
        say "mgOS: $s"
        pct $(( i * 100 / n ))
        # dbus-run-session: gsettings/dconf пишуть у профіль нового користувача, а не в живу систему
        inch runuser -u "$USERNAME" -- env HOME="$H" USER="$USERNAME" MGOS_DIR="$H/$D" OPT_DRIVERS=1 \
            dbus-run-session -- bash -c 'cd "$MGOS_DIR" && source gui/steps.sh && step_'"$s" 2>&1 \
            | sed -u 's/^@@MSG /@@MSG mgOS: /; s/^@@SUB .*//'
        rc=${PIPESTATUS[0]}
        [ "$rc" = 0 ] || { failed+=("$s"); warn "mgOS: крок «$s» завершився з помилкою ($rc)"; }
        i=$((i + 1))
    done
    run rm -f "$MNT/etc/sudoers.d/00-mgos-install"
    # Welcome на першому вході
    run install -d "$MNT$H/.local/state/mgos-installer"
    [ "$DRY_RUN" = 1 ] || : > "$MNT$H/.local/state/mgos-installer/welcome-pending"
    inch chown -R "$USERNAME:$USERNAME" "$H/.local/state"
    # критичні кроки (без них сесія не запуститься) — фатальні
    for s in core configs; do [[ " ${failed[*]} " == *" $s "* ]] && { echo "Критичний крок mgOS «$s» не вдався"; return 1; }; done
    return 0
}

# ---------------------------------------------------------------------------
#  8. Завершення
# ---------------------------------------------------------------------------
s_finish() {
    say "Прибираю"
    inch pacman -Scc --noconfirm >/dev/null 2>&1
    [ "$DRY_RUN" = 1 ] || cp "$LOG" "$MNT/var/log/mgos-install.log" 2>/dev/null
    sync
    return 0
}

# ---------------------------------------------------------------------------
exec > >(tee -a "$LOG") 2>&1
echo "=== mgos-arch-install $(date -Is) — $EDITION на $DISK ($MODE, $FS, LUKS=$ENCRYPT, DRY=$DRY_RUN)"
run mkdir -p "$MNT"
trap 'echo "@@FAIL Встановлення перервано"; cleanup_mounts; exit 1' INT TERM

step prepare s_prepare
step disk    s_disk
step base    s_base
step drivers s_drivers
step system  s_system
[ "$EDITION" = arch ] || step desktop s_desktop
[ "$EDITION" = mgos ] && step mgos s_mgos
step finish  s_finish
cleanup_mounts
echo "@@END ok"
