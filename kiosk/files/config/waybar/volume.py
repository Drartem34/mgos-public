#!/usr/bin/env python3
import sys
import os
import socket
import select
import time
import subprocess
import json

SOCKET_PATH = "/dev/shm/waybar_vol.sock"
BAR_TOTAL_LENGTH = 8
FRAME_DELAY = 0.022  # 22ms per frame for smooth 45fps slide
HOLD_DELAY = 1.3     # seconds to hold before sliding back

def get_volume_info():
    try:
        res = subprocess.run(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"], capture_output=True, text=True, timeout=1)
        output = res.stdout.strip()
        parts = output.split()
        if len(parts) >= 2:
            vol = int(round(float(parts[1]) * 100))
            muted = "[MUTED]" in output
            return vol, muted
    except Exception:
        pass
    return 0, False

def get_icon(vol, muted):
    if muted:
        return "󰖁"
    elif vol < 33:
        return "󰕿"
    elif vol < 66:
        return "󰖀"
    else:
        return "󰕾"

def render_bar(vol, length, max_len=BAR_TOTAL_LENGTH):
    if length <= 0:
        return ""
    filled_ratio = vol / 100.0
    filled = int(round(length * filled_ratio))
    empty = length - filled
    return "━" * filled + "─" * empty

def emit(text, tooltip, css_class):
    payload = json.dumps({"text": text, "tooltip": tooltip, "class": css_class})
    sys.stdout.write(payload + "\n")
    sys.stdout.flush()

def client_mode(cmd):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.5)
        s.connect(SOCKET_PATH)
        s.sendall((cmd + "\n").encode())
        s.close()
        return True
    except Exception:
        # Fallback if daemon is not running
        if cmd == "up":
            subprocess.run(["wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+"])
        elif cmd == "down":
            subprocess.run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"])
        elif cmd == "mute":
            subprocess.run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"])
        return False

def daemon_mode():
    if os.path.exists(SOCKET_PATH):
        try:
            os.unlink(SOCKET_PATH)
        except OSError:
            pass

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    server.listen(10)
    server.setblocking(False)

    vol, muted = get_volume_info()
    icon = get_icon(vol, muted)
    emit(f"{icon} {vol}%", f"Гучність: {vol}%", "normal")

    # Animation state
    # state: "NORMAL", "SLIDE_OUT", "BAR", "SLIDE_IN"
    state = "NORMAL"
    anim_len = 0
    last_action_time = 0

    while True:
        # Determine select timeout based on current animation state
        timeout = None
        if state in ("SLIDE_OUT", "SLIDE_IN"):
            timeout = FRAME_DELAY
        elif state == "BAR":
            remaining = HOLD_DELAY - (time.time() - last_action_time)
            timeout = max(0.01, remaining)

        readable, _, _ = select.select([server], [], [], timeout)

        command_received = None
        if readable:
            try:
                conn, _ = server.accept()
                data = conn.recv(128).decode().strip()
                conn.close()
                if data:
                    command_received = data
            except Exception:
                pass

        if command_received:
            if command_received == "up":
                subprocess.run(["wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+"])
            elif command_received == "down":
                subprocess.run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"])
            elif command_received == "mute":
                subprocess.run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"])

            vol, muted = get_volume_info()
            icon = get_icon(vol, muted)
            last_action_time = time.time()

            if command_received == "mute":
                if muted:
                    state = "NORMAL"
                    emit(f"{icon} Muted", f"Гучність: {vol}% (Вимкнено)", "muted")
                    continue

            if state == "NORMAL":
                state = "SLIDE_OUT"
                anim_len = 1
                bar_str = render_bar(vol, anim_len)
                emit(f"{icon} {bar_str}", f"Гучність: {vol}%", "bar")
            elif state in ("BAR", "SLIDE_OUT"):
                state = "BAR"
                anim_len = BAR_TOTAL_LENGTH
                bar_str = render_bar(vol, anim_len)
                emit(f"{icon} {bar_str}", f"Гучність: {vol}%", "bar")
            elif state == "SLIDE_IN":
                # User pressed key while sliding in: reverse back out!
                state = "SLIDE_OUT"
                bar_str = render_bar(vol, anim_len)
                emit(f"{icon} {bar_str}", f"Гучність: {vol}%", "bar")
            continue

        # Handle animation timer ticks
        now = time.time()
        vol, muted = get_volume_info()
        icon = get_icon(vol, muted)

        if state == "SLIDE_OUT":
            anim_len += 1
            if anim_len >= BAR_TOTAL_LENGTH:
                anim_len = BAR_TOTAL_LENGTH
                state = "BAR"
            bar_str = render_bar(vol, anim_len)
            emit(f"{icon} {bar_str}", f"Гучність: {vol}%", "bar")

        elif state == "BAR":
            if now - last_action_time >= HOLD_DELAY:
                state = "SLIDE_IN"
                anim_len = BAR_TOTAL_LENGTH - 1
                bar_str = render_bar(vol, anim_len)
                emit(f"{icon} {bar_str}", f"Гучність: {vol}%", "bar")

        elif state == "SLIDE_IN":
            anim_len -= 1
            if anim_len <= 0:
                anim_len = 0
                state = "NORMAL"
                if muted:
                    emit(f"{icon} Muted", f"Гучність: {vol}% (Вимкнено)", "muted")
                else:
                    emit(f"{icon} {vol}%", f"Гучність: {vol}%", "normal")
            else:
                bar_str = render_bar(vol, anim_len)
                emit(f"{icon} {bar_str}", f"Гучність: {vol}%", "bar")

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("up", "down", "mute", "get"):
        client_mode(sys.argv[1])
    else:
        daemon_mode()
