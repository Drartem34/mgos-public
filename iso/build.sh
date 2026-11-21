#!/usr/bin/env bash
# ============================================================================
#  Збирає живу флешку Megumin OS (archiso, профіль releng + mgOS):
#    Hyprland → mgos-setup (графічний інсталятор) на весь екран, Firefox/термінал над ним.
#  Потрібно: sudo pacman -S --needed archiso
#    ./iso/build.sh                 → ../mgos-iso-out/megumin-os-РРРР.ММ.ДД-x86_64.iso
#    WORK=/шлях OUT=/шлях ./iso/build.sh
#  Тестування: ./iso/test-vm.sh (qemu, UEFI, віртуальний диск 40 ГБ)
# ============================================================================
set -euo pipefail
ISO_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REPO="$(dirname "$ISO_DIR")"
WORK=${WORK:-/var/tmp/mgos-iso-work}
OUT=${OUT:-$(dirname "$REPO")/mgos-iso-out}
command -v mkarchiso >/dev/null || { echo "Немає archiso: sudo pacman -S --needed archiso"; exit 1; }

PROFILE=$(mktemp -d /var/tmp/mgos-iso-profile.XXXXXX)/profile
trap 'rm -rf "$(dirname "$PROFILE")"' EXIT
cp -r /usr/share/archiso/configs/releng "$PROFILE"
echo "→ профіль: $PROFILE"

# --- пакети
grep -v '^#' "$ISO_DIR/packages.extra" | grep -v '^$' >> "$PROFILE/packages.x86_64"
sort -u -o "$PROFILE/packages.x86_64" "$PROFILE/packages.x86_64"

# --- мережа: NetworkManager замість iwd + systemd-networkd (nmcli у GUI)
A="$PROFILE/airootfs"
rm -f "$A/etc/systemd/system/multi-user.target.wants/iwd.service" \
      "$A/etc/systemd/system/multi-user.target.wants/systemd-networkd.service" \
      "$A/etc/systemd/system/sockets.target.wants/systemd-networkd.socket" \
      "$A/etc/systemd/system/systemd-networkd-wait-online.service.d" 2>/dev/null || true
rm -rf "$A/etc/systemd/system/network-online.target.wants/systemd-networkd-wait-online.service" 2>/dev/null || true
mkdir -p "$A/etc/systemd/system/multi-user.target.wants"
ln -sf /usr/lib/systemd/system/NetworkManager.service "$A/etc/systemd/system/multi-user.target.wants/NetworkManager.service"

# --- наш оверлей
cp -a "$ISO_DIR/airootfs/." "$A/"

# --- живий користувач mgos (uid 1000, wheel, без пароля; root — як у releng)
echo 'mgos:x:1000:1000:Megumin OS Live:/home/mgos:/bin/bash' >> "$A/etc/passwd"
echo 'mgos::14871::::::' >> "$A/etc/shadow"
grep -q '^wheel:' "$A/etc/group" 2>/dev/null && sed -i 's/^wheel:x:998:.*/&mgos/' "$A/etc/group" || echo 'wheel:x:998:mgos' >> "$A/etc/group"
echo 'mgos:x:1000:' >> "$A/etc/group"
[ -f "$A/etc/gshadow" ] && { echo 'mgos:!*::' >> "$A/etc/gshadow"; grep -q '^wheel:' "$A/etc/gshadow" || echo 'wheel:!*::mgos' >> "$A/etc/gshadow"; }

# --- інсталятор: GUI + бекенд
bash "$REPO/arch-installer/build.sh"
install -Dm755 "$REPO/arch-installer/mgos-setup" "$A/usr/local/bin/mgos-setup"
install -Dm755 "$REPO/arch-installer/mgos-arch-install.sh" "$A/usr/local/bin/mgos-arch-install"
install -Dm644 "$REPO/arch-installer/edition-mgos.png" "$A/usr/share/mgos/edition-mgos.png"

# --- файли mgOS для варіанта «Arch + Hyprland + mgOS» (без шпалер-відео й kiosk — флешка має бути легкою)
mkdir -p "$A/opt/mgos/recovery"
rsync -a --delete --exclude .git --exclude wallpapers --exclude kiosk --exclude 'arch-installer/mgos-setup' \
      --exclude 'gui/mgos-installer' "$REPO/" "$A/opt/mgos/recovery/"

# --- назва та права
sed -i 's/^iso_name=.*/iso_name="megumin-os"/; s/^iso_label=.*/iso_label="MGOS_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"/;
        s/^iso_publisher=.*/iso_publisher="Megumin OS"/; s/^iso_application=.*/iso_application="Megumin OS Live\/Installer"/' "$PROFILE/profiledef.sh"
sed -i 's|^file_permissions=(|file_permissions=(\n  ["/etc/sudoers.d/00-live"]="0:0:440"\n  ["/usr/local/bin/mgos-setup"]="0:0:755"\n  ["/usr/local/bin/mgos-arch-install"]="0:0:755"|' "$PROFILE/profiledef.sh"

mkdir -p "$OUT"
sudo mkarchiso -v -r -w "$WORK" -o "$OUT" "$PROFILE"
ls -lh "$OUT"/*.iso | tail -1
