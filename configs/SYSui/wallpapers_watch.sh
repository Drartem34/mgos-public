#!/bin/bash
# Автоматичний перезапуск wallpapers при краші
# setsid - нова сесія, ізольована від ignis reload та інших сигналів
DIR="$(dirname "$(readlink -f "$0")")"
BINARY="$DIR/wallpapers"
SOCKET="$DIR/wallpapers.sock"
LOG="/tmp/wallpapers_restart.log"

while true; do
    rm -f "$SOCKET"
    echo "[$(date)] Запускаємо wallpapers..." >> "$LOG"
    setsid "$BINARY" >> "$LOG" 2>&1
    EXIT=$?
    echo "[$(date)] wallpapers завершився з кодом $EXIT — перезапуск через 1с..." >> "$LOG"
    sleep 1
done
