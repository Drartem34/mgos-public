#!/bin/bash

# Жорсткий шлях до твого юзера, щоб sudo не лізло в /root/
source /home/doki/.cache/wal/colors.sh
ACCENT=$color4
CURRENT_WALLPAPER=$wallpaper 

THEME_DIR="/usr/share/sddm/themes/sddm-astronaut-theme"
CONF="$THEME_DIR/Themes/megumin.conf"

echo "Оновлюємо SDDM..."

# Копіюємо шпалери
if [ -f "$CURRENT_WALLPAPER" ]; then
    cp "$CURRENT_WALLPAPER" "$THEME_DIR/Backgrounds/megumin_bg.jpg"
    sed -i "s|^Background=.*|Background=\"Backgrounds/megumin_bg.jpg\"|" "$CONF"
    echo "Шпалери оновлено: $CURRENT_WALLPAPER"
fi

# Прописуємо нові акцентні кольори
sed -i "s|^LoginButtonBackgroundColor=.*|LoginButtonBackgroundColor=\"$ACCENT\"|" "$CONF"
sed -i "s|^DropdownSelectedBackgroundColor=.*|DropdownSelectedBackgroundColor=\"$ACCENT\"|" "$CONF"
sed -i "s|^HighlightBackgroundColor=.*|HighlightBackgroundColor=\"$ACCENT\"|" "$CONF"
sed -i "s|^HighlightBorderColor=.*|HighlightBorderColor=\"$ACCENT\"|" "$CONF"
sed -i "s|^HoverUserIconColor=.*|HoverUserIconColor=\"$ACCENT\"|" "$CONF"
sed -i "s|^HoverPasswordIconColor=.*|HoverPasswordIconColor=\"$ACCENT\"|" "$CONF"
sed -i "s|^HoverSystemButtonsIconsColor=.*|HoverSystemButtonsIconsColor=\"$ACCENT\"|" "$CONF"
sed -i "s|^HoverSessionButtonTextColor=.*|HoverSessionButtonTextColor=\"$ACCENT\"|" "$CONF"
sed -i "s|^HoverVirtualKeyboardButtonTextColor=.*|HoverVirtualKeyboardButtonTextColor=\"$ACCENT\"|" "$CONF"

echo "Готово!"