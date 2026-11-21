#!/bin/bash

# ASCII-арт для кнопок
OFF="  (╯°□°)╯︵ ┻━┻  [ ВИМКНУТИ ]"
EXIT="  (•_•)  [ ВИЙТИ З КІОСКУ ]"

# Виводимо красу
echo -e "\e[31m$OFF\e[0m (Натисни 1)"
echo -e "\e[32m$EXIT\e[0m (Натисни 2)"

read -n 1 -p "Вибір: " choice

case $choice in
    1) shutdown now ;;
    2) hyprctl dispatch exit ;; # або твоя команда виходу
    *) exit ;;
esac