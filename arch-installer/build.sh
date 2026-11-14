#!/bin/bash
# Збирає графічний інсталятор mgos-setup. sysui_common.h і pixel_waves.h — спільні з панелями SYSui.
cd "$(dirname "$(readlink -f "$0")")"
g++ -std=c++20 -O2 -s mgos_setup.cpp -o mgos-setup.new -I../configs/SYSui \
    $(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0) \
    && mv -f mgos-setup.new mgos-setup && echo "✓ mgos-setup"
