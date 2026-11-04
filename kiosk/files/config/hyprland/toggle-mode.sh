#!/usr/bin/env bash

CURRENT=$(powerprofilesctl get 2>/dev/null)

case "$CURRENT" in
    "power-saver")
        powerprofilesctl set balanced
        notify-send -u low -t 1500 -a "Режим живлення" "Режим живлення" "⚖️ Збалансований (білий)"
        ;;
    "balanced")
        powerprofilesctl set performance
        notify-send -u low -t 1500 -a "Режим живлення" "Режим живлення" "⚡ Продуктивний (червоний)"
        ;;
    "performance"|*)
        powerprofilesctl set power-saver
        notify-send -u low -t 1500 -a "Режим живлення" "Режим живлення" "🔋 Економ (сірий)"
        ;;
esac
