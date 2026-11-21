#!/bin/bash

# Функція для отримання часу
get_time() {
    date "+⏰ %H:%M:%S"
}

# Функція для отримання температури CPU (використовуючи sensors)
get_cpu_temp() {
    TEMP=$(sensors | grep 'Tctl' | awk '{print $2}' | tr -d '+')
    echo "🔥 CPU Temp: ${TEMP}"
}

# --- Вивід, який буде показувати Wofi ---

# 1. Загальна інформація
get_time
get_cpu_temp
echo "---"

# 2. Швидкі дії
echo "⚙️ Налаштування"
echo "🖼️ Змінити шпалери"
echo "🎵 YouTube Music"
echo "🔄 Перезавантажити Hyprland"
echo "👋 Вимкнути ПК"
