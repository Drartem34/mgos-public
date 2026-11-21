# жива флешка Megumin OS: на tty1 одразу Hyprland з інсталятором
[ -z "$WAYLAND_DISPLAY" ] && [ "$(tty)" = /dev/tty1 ] && exec Hyprland > ~/.hyprland.log 2>&1
