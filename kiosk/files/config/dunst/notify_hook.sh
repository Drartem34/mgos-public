#!/bin/bash
# Хук dunst → плашка сповіщень mgOS (SYSui notify_osd).
# Пише /tmp/notify_osd_data: SENDER, BODY, ICON (аватарка/картинка або назва іконки),
# APP і DESKTOP (за ними плашка знаходить іконку програми), SITE і SITEICON (сповіщення сайтів у браузері).
APPNAME="$1"
SUMMARY="$2"
BODY="$3"
ICON="$4"
DESKTOP="${DUNST_DESKTOP_ENTRY:-}"

# ---------- сайт (лише для браузерів: адреси з інших програм не відкриваємо) ----------
SITE=""; SITEICON=""
app_l="${APPNAME,,} ${DESKTOP,,}"
if [[ "$app_l" =~ (firefox|chrom|brave|vivaldi|edge|opera|zen|librewolf|yandex|floorp|thorium) ]]; then
    url=$(printf '%s\n%s\n' "${DUNST_URLS:-}" "$BODY" | grep -oE 'https?://[A-Za-z0-9.-]+' | head -1)
    [ -n "$url" ] && SITE="${url#*://}"
    if [ -z "$SITE" ]; then   # Chromium без гіперпосилань: перший рядок тіла — домен
        first=$(printf '%s' "$BODY" | head -1 | sed -e 's/<[^>]*>//g' | tr -d '[:space:]')
        if [[ "$first" =~ ^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$ ]]; then
            SITE="$first"; BODY=$(printf '%s' "$BODY" | tail -n +2)
        fi
    fi
    BODY=$(printf '%s' "$BODY" | sed -E 's#<a href="https?://[^"]*">[^<]*</a>##')   # прибираємо посилання-домен з тексту
    SITE="${SITE,,}"; SITE="${SITE#www.}"
    if [[ "$SITE" =~ ^[a-z0-9-]+(\.[a-z0-9-]+)+$ ]]; then
        cache="$HOME/.cache/megumin-os/favicons"; mkdir -p "$cache"
        f="$cache/$SITE.png"
        if [ -s "$f" ]; then
            SITEICON="$f"
        elif [ ! -e "$f.none" ] || [ -n "$(find "$f.none" -mmin +60 2>/dev/null)" ]; then
            FETCH_SITE="$SITE"; FETCH_FILE="$f"   # качаємо у фоні після показу плашки
        fi
    else
        SITE=""
    fi
fi

# ---------- без іконки від програми — плашка сама знайде іконку за APP/DESKTOP ----------
[ "$ICON" == " " ] && ICON=""

CLEAN_BODY=$(printf '%s' "$BODY" | sed -e 's/<[^>]*>//g' | tr '\n' ' ' | sed -e 's/^ *//')
{
    echo "ID=${DUNST_ID:-}"
    echo "SENDER=$(printf '%s' "$SUMMARY" | tr '\n' ' ')"
    echo "ICON=$ICON"
    echo "APP=$APPNAME"
    echo "DESKTOP=$DESKTOP"
    echo "SITE=$SITE"
    echo "SITEICON=$SITEICON"
    echo "BODY=$CLEAN_BODY"
} > /tmp/notify_osd_data.tmp && mv -f /tmp/notify_osd_data.tmp /tmp/notify_osd_data

# звук сповіщення (вимикається в Налаштування → Системні звуки)
# повідомлення про помилку (іконка dialog-error або слова в тексті) — звук попередження, навіть якщо терміновість звичайна
is_err=0
if [[ "${ICON,,}" == *error* ]] || printf '%s %s' "$SUMMARY" "$BODY" | grep -qiE 'помилк|збій|не вдал|error|failed|failure|не вдалося'; then is_err=1; fi
case "${5^^}" in   # $5 — терміновість від dunst
    CRITICAL) "$HOME/.local/bin/mgos-sound" dialog-warning & ;;
    LOW) ;;
    *) if [ "$is_err" = 1 ]; then "$HOME/.local/bin/mgos-sound" dialog-warning & else "$HOME/.local/bin/mgos-sound" message-new-instant & fi ;;
esac

echo "show" | socat - UNIX-CLIENT:"$HOME/.config/SYSui/notify.sock"
dunstctl close

# іконка нового сайту: качаємо у фоні; якщо плашка ще про це ж сповіщення — підставляємо одразу
if [ -n "${FETCH_SITE:-}" ]; then
    (
        tmp="$FETCH_FILE.part"; rm -f "$FETCH_FILE.none"
        timeout 5 python3 -I "$HOME/.local/bin/mgos-favicon" "$FETCH_SITE" "$tmp" 2>/dev/null && [ -s "$tmp" ] && mv -f "$tmp" "$FETCH_FILE"
        rm -f "$tmp"
        if [ -s "$FETCH_FILE" ]; then
            grep -qx "ID=${DUNST_ID:-}" /tmp/notify_osd_data 2>/dev/null &&
                sed -i "s|^SITEICON=.*|SITEICON=$FETCH_FILE|" /tmp/notify_osd_data &&
                echo "show" | socat - UNIX-CLIENT:"$HOME/.config/SYSui/notify.sock"
        else
            : > "$FETCH_FILE.none"
        fi
    ) >/dev/null 2>&1 &
fi

echo "notify|$2|$3" | socat - UNIX-CONNECT:"$HOME/.config/SYSui/cc.sock" &
