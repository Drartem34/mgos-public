#!/bin/bash
# Збирає GUI-інсталятор. pixel_waves.h — спільний із панелями SYSui (configs/SYSui).
cd "$(dirname "$(readlink -f "$0")")"
g++ -std=c++17 -O2 -s mgos_installer.cpp -o mgos-installer.new \
    -I../configs/SYSui -I"$HOME/.config/SYSui" $(pkg-config --cflags --libs gtk4) \
    && mv -f mgos-installer.new mgos-installer && echo "✓ mgos-installer"
