#!/usr/bin/env bash
# ============================================================================
#  Самоперевірка репозиторію mgOS — запускається вручну і з scripts/mgos-release.sh.
#    1. bash -n усіх shell-скриптів репозиторію (kiosk/ теж лише читаємо)
#    2. збірка SYSui: g++ -fsyntax-only усіх модулів (бінарники не чіпаємо)
#    3. постер кожного піксельного стилю × 2 теми (збирається тимчасовий pixel_wallpaper)
#    4. Hyprland --verify-config на configs/hypr/hyprland.conf
#  Нічого в репозиторії не змінює. Код виходу 0 — усе гаразд.
#  Пропуск етапів: SELFTEST_SKIP="bash,build,poster,hypr" (через кому).
# ============================================================================
set -uo pipefail
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
cd "$ROOT" || exit 1
SKIP=",${SELFTEST_SKIP:-},"
FAIL=0; WARN=0
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
bad()  { printf '  \033[31m✗\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; WARN=$((WARN + 1)); }
skipped() { [[ "$SKIP" == *",$1,"* ]]; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# ── 1. bash -n ──
echo "== 1. bash -n"
if skipped bash; then warn "пропущено"; else
    n=0; e=0
    while IFS= read -r f; do
        n=$((n + 1))
        if ! bash -n "$f" 2>"$TMP/err"; then bad "$f: $(head -1 "$TMP/err")"; e=$((e + 1)); fi
    done < <({ git ls-files -- '*.sh'
               git ls-files -- scripts | while read -r s; do head -c 64 "$s" 2>/dev/null | head -1 | grep -qE '^#!.*(ba)?sh' && echo "$s"; done
               echo mgos-release.sh; } 2>/dev/null | sort -u | while read -r f; do [ -f "$f" ] && echo "$f"; done)
    [ "$e" = 0 ] && ok "bash -n: $n файлів"
fi

# ── 2. SYSui ──
echo "== 2. SYSui (g++ -fsyntax-only)"
if skipped build; then warn "пропущено"; else
    FL=$(pkg-config --cflags gtk4 gtk4-layer-shell-0 gio-unix-2.0 libinput libudev 2>/dev/null)
    if [ -z "$FL" ]; then warn "немає pkg-config gtk4/gtk4-layer-shell — збірку пропущено"; else
        pids=(); names=()
        for src in dock topbar runner wallpapers cc clipboard_osd osd_hub powermenu_osd controller_addon about_osd pixel_wallpaper welcome genie_osd cards_osd dashboard; do
            f=configs/SYSui/$src.cpp; [ -f "$f" ] || continue
            ( g++ -std=c++17 -fsyntax-only -w -I configs/SYSui $FL "$f" >"$TMP/b_$src.log" 2>&1 ) & pids+=($!); names+=("$src")
        done
        for i in "${!pids[@]}"; do
            if wait "${pids[$i]}"; then ok "${names[$i]}"; else bad "${names[$i]}: $(grep -m1 -E 'error' "$TMP/b_${names[$i]}.log")"; fi
        done
    fi
fi

# ── 3. постери ──
echo "== 3. піксельні стилі × 2 теми"
if skipped poster; then warn "пропущено"; else
    PW=$TMP/pixel_wallpaper
    FL=$(pkg-config --cflags --libs gtk4 gtk4-layer-shell-0 gio-unix-2.0 2>/dev/null)
    if [ -z "$FL" ] || [ ! -f configs/SYSui/pixel_wallpaper.cpp ]; then warn "немає залежностей для pixel_wallpaper — пропущено"
    elif ! g++ -std=c++17 -O1 -w -I configs/SYSui configs/SYSui/pixel_wallpaper.cpp -o "$PW" $FL 2>"$TMP/pw.log"; then
        bad "pixel_wallpaper не збирається: $(grep -m1 error "$TMP/pw.log")"
    else
        mapfile -t STYLES < <(python3 -I - configs/ignis/settings.py <<'PY'
import re, sys
t = open(sys.argv[1], encoding="utf-8").read()
a = t.index("PIXEL_STYLES = [")
b = t.index("PIXEL_STYLES_WP = PIXEL_STYLES + [")
b = t.index("]", b)
ids = re.findall(r'\(\s*"([a-z0-9_]+)"\s*,', t[a:b])
print("\n".join(dict.fromkeys(ids)))
PY
)
        [ "${#STYLES[@]}" -gt 0 ] || bad "не вдалося прочитати список стилів з configs/ignis/settings.py"
        for s in "${STYLES[@]}"; do
            for th in dark light; do
                out=$TMP/p_${s}_$th.png; rm -f "$out"
                if timeout 60 "$PW" --poster "$out" 400x224 --style "$s" --color '#c13cff' --theme "$th" >/dev/null 2>&1 \
                   && [ -s "$out" ]; then :; else bad "постер $s/$th"; continue; fi
            done
        done
        [ "$FAIL" = 0 ] && ok "постери: ${#STYLES[@]} стилів × 2 теми"
    fi
fi

# ── 4. Hyprland ──
echo "== 4. Hyprland --verify-config"
if skipped hypr; then warn "пропущено"; elif ! command -v Hyprland >/dev/null; then warn "Hyprland не встановлено — пропущено"
elif [ ! -f configs/hypr/hyprland.conf ]; then warn "немає configs/hypr/hyprland.conf"
else
    # знімок у тимчасовому HOME: source = ~/.config/hypr/… має вказувати на файли з репозиторію
    mkdir -p "$TMP/home/.config"
    for d in configs/*/; do ln -s "$ROOT/$d" "$TMP/home/.config/$(basename "$d")"; done
    mkdir -p "$TMP/home/.cache"; ln -s "$ROOT/cache_wal" "$TMP/home/.cache/wal"
    out=$(HOME=$TMP/home XDG_CONFIG_HOME=$TMP/home/.config timeout 60 Hyprland --verify-config -c "$TMP/home/.config/hypr/hyprland.conf" 2>&1); rc=$?
    # відсутні source-файли (генеруються на машині: mgos-auto.conf, touchpad.conf…) — не помилка знімка
    errs=$(grep -iE 'error|failed' <<< "$out" | grep -viE 'source=|globbing|no such file' || true)
    if [ -n "$errs" ]; then bad "verify-config: $(head -1 <<< "$errs")"
    elif grep -qiE 'globbing|no such file' <<< "$out"; then warn "verify-config: відсутні згенеровані source-файли ($(grep -ciE 'globbing|no such file' <<< "$out"))"
    elif [ "$rc" != 0 ]; then bad "verify-config впав (код $rc): $(tail -1 <<< "$out")"
    else ok "hyprland.conf коректний"; fi
fi

# ── 5. step_configs у fake HOME: чиста система та без jq ──
if ! skipped clean; then
    echo "== 5. step_configs: чиста система / без jq"
    if bash "$ROOT/tests/step_configs_clean.sh" > "$TMP/clean.out" 2>&1; then ok "step_configs_clean.sh"
    else bad "step_configs_clean.sh: $(grep '✗' "$TMP/clean.out" | head -3 | tr '\n' ' ')"; fi
fi

echo
if [ "$FAIL" -gt 0 ]; then printf '\033[31mselftest: ПОМИЛОК %d, попереджень %d\033[0m\n' "$FAIL" "$WARN"; exit 1; fi
printf '\033[32mselftest: OK\033[0m (попереджень: %d)\n' "$WARN"
