#!/bin/bash

LOG=~/frozen_procs.log
declare -A last_active_time
declare -A frozen_windows

TIMEOUT=60  # секунд для заморозки

get_active_win() {
  hyprctl activewindow | grep 'address:' | awk '{print $2}'
}

get_pid_of_window() {
  local win_id=$1
  hyprctl clients | grep -A5 "handle: $win_id" | grep "pid:" | awk '{print $2}'
}

freeze_pid() {
  local pid=$1
  if kill -0 "$pid" 2>/dev/null; then
    kill -STOP "$pid" && echo "🥶 Заморожено PID $pid"
    echo "$pid" >> "$LOG"
  fi
}

unfreeze_pid() {
  local pid=$1
  if kill -0 "$pid" 2>/dev/null; then
    kill -CONT "$pid" && echo "🟢 Розморожено PID $pid"
    sed -i "/$pid/d" "$LOG"
  fi
}

# --- [1] Розмороження попередніх ---
if [ -f "$LOG" ]; then
  echo -e "\n🔓 Знайдено попередні заморожені процеси:"
  while read -r pid; do
    proc=$(ps -p "$pid" -o comm=)
    echo "PID $pid: $proc"
  done < "$LOG"

  read -p "❔ Хочеш розморозити їх? (y/N): " unfreeze
  if [[ "$unfreeze" =~ ^[Yy]$ ]]; then
    while read -r pid; do
      kill -CONT "$pid" 2>/dev/null && echo "🟢 Розморожено PID $pid"
    done < "$LOG"
    rm "$LOG"
  fi
fi

# --- [2] Очищення кешу ---
echo "🧠 Очищення кешу..."
sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"

rm -rf ~/.cache/{yay,thumbnails,electron,BraveSoftware,modrinth-app,torbrowser,google-chrome,spotify,vesktop,mozilla,zen}
rm -rf ~/.config/google-chrome/Default/{Code\ Cache,GPUCache}/*
rm -rf ~/.config/Code/{Cache,CachedData,Code\ Cache,GPUCache}/*

# --- [3] Топ-10 ---
echo -e "\n📊 Топ 10 процесів по RAM:\n"
ps --no-headers -eo pid,user,%mem,comm --sort=-%mem | head -n 10

declare -A top_actions

echo -e "\nВведи дію:\n  s = заморозити ❄️\n  k = вбити ☠️\n  Enter = нічого\n"

for row in $(ps --no-headers -eo pid,user,%mem,comm --sort=-%mem | head -n 10 | awk '{print $1}'); do
    read -p "PID $row: (s/k/Enter)? " action
    top_actions[$row]=$action
done

for pid in "${!top_actions[@]}"; do
  act="${top_actions[$pid]}"
  if [ "$act" = "s" ]; then
    freeze_pid "$pid"
  elif [ "$act" = "k" ]; then
    kill -9 "$pid" && echo "☠️ Вбито PID $pid"
  fi
done

# --- [4] Фонові процеси ---
read -p $'\n❔ Заморозити всі фонові процеси? (y/N): ' do_freeze
if [[ "$do_freeze" =~ ^[Yy]$ ]]; then
  echo -e "\n📥 Замороження фону..."

  white_list="(systemd|login|sudo|bash|zsh|fish|ps|grep|wofi|dbus|pipewire|wireplumber|Hyprland|kitty|wezterm|xdg-desktop-portal|dunst|nwg-dock|swaylock|waybar)"

  ps -e -o pid=,user=,comm= | while read -r pid user proc; do
    if [[ "$user" == "root" || "$proc" =~ $white_list || ${top_actions[$pid]+_} ]]; then
      continue
    fi
    freeze_pid "$pid"
  done
else
  echo "ℹ️ Фонові процеси не заморожено."
fi

# --- [5] Авто-замороження ---
read -p $'\n❔ Хочеш увімкнути авто-замороження вікон? (y/N): ' auto_freeze_choice
if [[ "$auto_freeze_choice" =~ ^[Yy]$ ]]; then
  echo "🚀 Запущено авто-замороження вікон... (Ctrl+C для виходу)"

  while true; do
    active_pid=$(hyprctl activewindow -j | jq '.pid' 2>/dev/null)

    if [[ -z "$active_pid" || "$active_pid" == "null" ]]; then
      echo "[!] Не вдалося отримати активне вікно. Пропуск..."
      sleep 5
      continue
    fi

    echo "[✔] Активне вікно: PID $active_pid"
    now=$(date +%s)
    last_active_time[$active_pid]=$now

    # Замороження неактивних
    ps -u "$USER" -o pid= | while read pid; do
      [[ "$pid" == "$active_pid" ]] && continue
      [[ "${frozen_windows[$pid]}" == "1" ]] && continue

      seen=${last_active_time[$pid]}
      [[ -z "$seen" ]] && last_active_time[$pid]=$now && continue

      elapsed=$((now - seen))
      if (( elapsed >= TIMEOUT )); then
        freeze_pid "$pid"
        frozen_windows[$pid]=1
      fi
    done

    # Розмороження активного
    for pid in "${!frozen_windows[@]}"; do
      if [[ "$pid" == "$active_pid" ]]; then
        unfreeze_pid "$pid"
        unset frozen_windows[$pid]
      fi
    done

    sleep 5
  done
else
  echo "ℹ️ Автозамороження не увімкнено."
fi

echo -e "\n✅ Все готово. Лог: $LOG"
notify-send "🧹 RAM очищено" "Очищення завершено."
