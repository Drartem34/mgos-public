#!/usr/bin/env bash
# Перевірка флешки у віртуальній машині: UEFI (OVMF), 4 ГБ RAM, віртуальний диск 40 ГБ.
#   ./iso/test-vm.sh [шлях.iso]     — завантажити з ISO (встановлення на віртуальний диск)
#   ./iso/test-vm.sh --disk         — завантажити вже встановлену систему з віртуального диска
#   BIOS=1 ./iso/test-vm.sh         — без UEFI (перевірка GRUB)
# Потрібно: sudo pacman -S --needed qemu-desktop edk2-ovmf
set -euo pipefail
ISO_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
OUT=$(dirname "$(dirname "$ISO_DIR")")/mgos-iso-out
VM=${VM:-/var/tmp/mgos-vm}; mkdir -p "$VM"
DISK=$VM/disk.qcow2
[ -f "$DISK" ] || qemu-img create -f qcow2 "$DISK" 40G
FW=()
if [ "${BIOS:-0}" != 1 ]; then
    [ -f "$VM/OVMF_VARS.4m.fd" ] || cp /usr/share/edk2/x64/OVMF_VARS.4m.fd "$VM/"
    FW=(-drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd -drive "if=pflash,format=raw,file=$VM/OVMF_VARS.4m.fd")
fi
BOOT=()
if [ "${1:-}" != --disk ]; then
    ISO=${1:-$(ls -t "$OUT"/*.iso | head -1)}
    BOOT=(-cdrom "$ISO" -boot order=d)
fi
exec qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 "${FW[@]}" \
    -drive "file=$DISK,if=virtio,format=qcow2" "${BOOT[@]}" \
    -nic user,model=virtio-net-pci -device virtio-vga-gl -display gtk,gl=on \
    -device qemu-xhci -device usb-tablet -audiodev pipewire,id=a -device intel-hda -device hda-duplex,audiodev=a
