#!/bin/bash
# ============================================================================
#  mgOS: синхронізація екрана входу (SDDM) з системою. Запускається від root через
#  sudo -n (правило /etc/sudoers.d/mgos-sddm-sync) — із «Шпалер» і «Налаштувань».
#
#   • шпалери → фон обох тем (для відео — постер, перший кадр: вхід без «стрибка»)
#   • акцент системи → кольори тем
#   • тема входу mgOS (~/.config/megumin-os/sddm-pixel) → /usr/share/sddm/themes/mgos-pixel
#   • прибирає старий зламаний drop-in plymouth → SDDM (вішав X)
#   • «Налаштування → Екран входу» (settings.json, ключі sddm_*) → theme.conf.user і вибір теми
#   • аватарка
#  Файли користувача тут лише ЧИТАЮТЬСЯ (жодного source/eval), значення перевіряються.
# ============================================================================
set -u
U_NAME="${SUDO_USER:-$USER}"
U_HOME=$(getent passwd "$U_NAME" | cut -d: -f6)
[ -n "$U_HOME" ] && [ -d "$U_HOME" ] || { echo "Немає домашньої теки для $U_NAME"; exit 1; }

ASTRO="/usr/share/sddm/themes/sddm-astronaut-theme"
ASTRO_CONF="$ASTRO/Themes/megumin.conf"
PIXEL_SRC="$U_HOME/.config/megumin-os/sddm-pixel"
PIXEL="/usr/share/sddm/themes/mgos-pixel"
SETTINGS="$U_HOME/.config/megumin-os/settings.json"
hex_ok() { [[ "$1" =~ ^#[0-9A-Fa-f]{6}$ ]]; }

echo "Оновлюємо SDDM..."

# ---------- налаштування (settings.json → перевірені змінні) ----------
eval_settings() {
    python3 -I - "$SETTINGS" <<'PY'
import json, sys, re
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    d = {}
def pick(k, allowed, default):
    v = d.get(k, default)
    return v if v in allowed else default
def num(k, lo, hi, default):
    try: v = int(d.get(k, default))
    except Exception: v = default
    return max(lo, min(hi, v))
def b(k, default):
    v = d.get(k, default)
    return "true" if v is True or v == "true" else "false"
acc = d.get("accent_color", "")
out = {
    "THEME": pick("sddm_theme", ("pixel", "classic"), "pixel"),
    "BGMODE": pick("sddm_bg_mode", ("wallpaper", "pixel", "waves"), "wallpaper"),
    "PXSIZE": str(num("sddm_pixel_size", 3, 16, 6)),
    "DIM": str(num("sddm_dim", 0, 80, 25)),
    "BLUR": b("sddm_blur", False),
    "CLOCKSTYLE": pick("sddm_clock_style", ("wave", "solid"), "wave"),
    "CLOCK24": b("sddm_clock24", True),
    "SHOWSEC": b("sddm_seconds", True),
    "PXAVATAR": b("sddm_pixel_avatar", False),
    "ACCMODE": pick("accent_mode", ("wal", "custom"), "wal"),
    "ACCCUSTOM": acc if re.fullmatch(r"#[0-9A-Fa-f]{6}", str(acc)) else "",
}
for k, v in out.items(): print(f"{k}={v}")
PY
}
THEME=pixel BGMODE=wallpaper PXSIZE=6 DIM=25 BLUR=false CLOCKSTYLE=wave CLOCK24=true SHOWSEC=true PXAVATAR=false ACCMODE=wal ACCCUSTOM=""
while IFS='=' read -r k v; do
    case "$k" in THEME|BGMODE|PXSIZE|DIM|BLUR|CLOCKSTYLE|CLOCK24|SHOWSEC|PXAVATAR|ACCMODE|ACCCUSTOM) printf -v "$k" '%s' "$v" ;; esac
done < <(eval_settings 2>/dev/null)

# ---------- акцент і шпалери (з кешу pywal, лише читання) ----------
WAL="$U_HOME/.cache/wal/colors.sh"
walget() { sed -n "s/^$1='\(.*\)'$/\1/p" "$WAL" 2>/dev/null | head -1; }
# Розумний акцент, як у SYSui (sysui::smart_wal_accent / ~/.local/bin/mgos-accent): колір домінантної
# групи відтінків color1..6, а не сирий color5. Код вбудовано сюди — скрипт іде від root, і запускати
# файли з домашньої теки користувача від root не можна.
ACCENT=$(python3 -I - "$U_HOME/.cache/wal/colors.css" <<'PYEOF' 2>/dev/null
import colorsys, re, sys
try: css = open(sys.argv[1]).read()
except OSError: sys.exit(1)
cols = {int(k): v for k, v in re.findall(r"--color(\d+):\s*(#[0-9A-Fa-f]{6})", css)}
cs = []
for i in range(1, 7):
    hx = cols.get(i)
    if not hx: continue
    h, l, s = colorsys.rgb_to_hls(*(int(hx[j:j + 2], 16) / 255 for j in (1, 3, 5)))
    if s < 0.15 or l < 0.15 or l > 0.9: continue
    cs.append((hx, h * 360, s, l))
if not cs: print(cols.get(5, "")); sys.exit()
d = lambda a, b: min(abs(a - b), 360 - abs(a - b))
hx, h, s, l = max(cs, key=lambda c: sum(o[2] for o in cs if o is not c and d(c[1], o[1]) <= 35) + c[2] - abs(c[3] - 0.55) * 1.2)
l2, s2 = min(max(l, 0.45), 0.66), max(s, 0.45)
if (l2, s2) == (l, s): print(hx)
else:
    r, g, b = colorsys.hls_to_rgb(h / 360, l2, s2); print("#%02x%02x%02x" % (round(r * 255), round(g * 255), round(b * 255)))
PYEOF
)
hex_ok "$ACCENT" || ACCENT=$(walget color5); hex_ok "$ACCENT" || ACCENT="#9929CF"
[ "$ACCMODE" = custom ] && hex_ok "$ACCCUSTOM" && ACCENT=$ACCCUSTOM
ASTRO_ACCENT=$(walget color4); hex_ok "$ASTRO_ACCENT" || ASTRO_ACCENT=$ACCENT

# постер: готовий кадр для відео (mgos-wp-poster) або сама картинка
BG=""
if [ -f "$U_HOME/.cache/megumin-os/wp_poster.src" ]; then
    LASTWP=$(head -1 "$U_HOME/.cache/ignis/last_wallpaper.txt" 2>/dev/null)
    case "${LASTWP##*.}" in
        mp4|webm|gif|mkv|mov|MP4|WEBM) [ -f "$U_HOME/.cache/megumin-os/wp_poster.jpg" ] && BG="$U_HOME/.cache/megumin-os/wp_poster.jpg" ;;
    esac
fi
# піксельні живі шпалери (pixel:<стиль>): фон входу — їхній постер (малює pixel_wallpaper в акценті системи)
LASTWP=$(head -1 "$U_HOME/.cache/ignis/last_wallpaper.txt" 2>/dev/null)
case "$LASTWP" in
    pixel:*) [ -f "$U_HOME/.cache/megumin-os/wp_poster_pixel.png" ] && BG="$U_HOME/.cache/megumin-os/wp_poster_pixel.png" ;;
esac
[ -z "$BG" ] && BG=$(walget wallpaper)
# лише справжні файли з дому користувача (root не має копіювати чужі файли через посилання)
[ -n "$BG" ] && BG=$(realpath -e -- "$BG" 2>/dev/null)
case "$BG" in "$U_HOME"/*) ;; *) BG="" ;; esac
safe_file() { local r; r=$(realpath -e -- "$1" 2>/dev/null) && [ -f "$r" ] && case "$r" in "$U_HOME"/*) return 0 ;; esac; return 1; }

# ---------- класична тема (astronaut) ----------
if [ -d "$ASTRO" ] && [ -f "$ASTRO_CONF" ]; then
    if [ -n "$BG" ] && [ -f "$BG" ]; then
        install -m 644 "$BG" "$ASTRO/Backgrounds/megumin_bg.jpg"
        sed -i "s|^Background=.*|Background=\"Backgrounds/megumin_bg.jpg\"|" "$ASTRO_CONF"
    fi
    for key in LoginButtonBackgroundColor DropdownSelectedBackgroundColor HighlightBackgroundColor HighlightBorderColor \
               HoverUserIconColor HoverPasswordIconColor HoverSystemButtonsIconsColor HoverSessionButtonTextColor \
               HoverVirtualKeyboardButtonTextColor; do
        sed -i "s|^$key=.*|$key=\"$ASTRO_ACCENT\"|" "$ASTRO_CONF"
    done
    safe_file "$U_HOME/.cache/megumin-os/avatar_round.png" && install -m 644 "$U_HOME/.cache/megumin-os/avatar_round.png" "$ASTRO/Assets/avatar.png"
fi

# ---------- тема mgOS Pixel ----------
if [ -f "$PIXEL_SRC/Main.qml" ] && [ -f "$PIXEL_SRC/metadata.desktop" ]; then
    install -d -m 755 "$PIXEL" "$PIXEL/components" "$PIXEL/Backgrounds"
    for f in Main.qml metadata.desktop theme.conf; do safe_file "$PIXEL_SRC/$f" && install -m 644 "$PIXEL_SRC/$f" "$PIXEL/$f"; done
    for f in "$PIXEL_SRC"/components/*.qml "$PIXEL_SRC"/components/*.js; do
        safe_file "$f" && install -m 644 "$f" "$PIXEL/components/$(basename "$f")"
    done
    install -d -m 755 "$PIXEL/assets"
    for f in "$PIXEL_SRC"/assets/*.png; do
        safe_file "$f" && install -m 644 "$f" "$PIXEL/assets/$(basename "$f")"
    done
    # лого переходу — той самий файл, що малює plymouth (кадр збігається піксель у піксель)
    [ -f /usr/share/plymouth/themes/mgos/watermark.png ] && install -m 644 /usr/share/plymouth/themes/mgos/watermark.png "$PIXEL/assets/logo.png"
fi
if [ -d "$PIXEL" ]; then
    if [ -n "$BG" ] && [ -f "$BG" ]; then
        install -m 644 "$BG" "$PIXEL/Backgrounds/wallpaper.jpg"
    elif [ ! -f "$PIXEL/Backgrounds/wallpaper.jpg" ] && safe_file "$PIXEL_SRC/Backgrounds/wallpaper.jpg"; then
        install -m 644 "$PIXEL_SRC/Backgrounds/wallpaper.jpg" "$PIXEL/Backgrounds/wallpaper.jpg"   # нова система: типовий фон
    fi
    cat > "$PIXEL/theme.conf.user" <<EOF
[General]
; Згенеровано mgOS (Налаштування → Екран входу). Не редагуй вручну — перезапишеться.
Background=Backgrounds/wallpaper.jpg
BackgroundMode=$BGMODE
PixelSize=$PXSIZE
Dim=$DIM
Blur=$BLUR
Accent=$ACCENT
ClockStyle=$CLOCKSTYLE
Clock24=$CLOCK24
ShowSeconds=$SHOWSEC
PixelAvatar=$PXAVATAR
EOF
    chmod 644 "$PIXEL/theme.conf.user"
fi

# ---------- яка тема активна ----------
install -d /etc/sddm.conf.d
if [ "$THEME" = pixel ] && [ -f "$PIXEL/Main.qml" ]; then CUR=mgos-pixel; else CUR=sddm-astronaut-theme; fi
printf '[Theme]\nCurrent=%s\n' "$CUR" > /etc/sddm.conf.d/theme.conf
# /etc/sddm.conf читається ПІСЛЯ sddm.conf.d і перекриває його. Старий sddm_theme/setup.sh писав туди
# Current=sddm-astronaut-theme — через це нова тема ніколи не з'являлась. Вибір теми — лише в theme.conf.
if [ -f /etc/sddm.conf ] && grep -qE '^[[:space:]]*Current[[:space:]]*=' /etc/sddm.conf; then
    sed -i -E '/^[[:space:]]*Current[[:space:]]*=/d' /etc/sddm.conf
fi
echo "Тема входу: $CUR"

# ---------- перехід plymouth → SDDM ----------
# НЕ тримати plymouth живим під час старту SDDM: drop-in з Conflicts=plymouth-quit +
# "plymouth deactivate" (12.11.2026) вішав X-сервер SDDM на відкритті DRM (modesetting) —
# система не доходила до екрана входу. Видаляємо його, якщо він лишився.
PLY_DROPIN=/etc/systemd/system/sddm.service.d/10-mgos-plymouth.conf
if [ -e "$PLY_DROPIN" ]; then
    rm -f "$PLY_DROPIN"
    rmdir "${PLY_DROPIN%/*}" 2>/dev/null || true
    systemctl daemon-reload 2>/dev/null || true
    echo "Прибрано зламаний drop-in plymouth → SDDM"
fi

# ---------- аватарка (стандартне місце SDDM) ----------
if safe_file "$U_HOME/.face.icon"; then
    install -d /usr/share/sddm/faces
    install -m 644 "$U_HOME/.face.icon" "/usr/share/sddm/faces/$U_NAME.face.icon"
fi

echo "Готово!"
