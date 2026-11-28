#!/usr/bin/env bash
# ============================================================================
#  Оновлення самого інсталятора з git (репозиторій mgos на GitHub) — канал «stable».
#    gui/selfupdate.sh check  → @@UPD <скільки комітів> <тема останнього>   (є оновлення)
#                              @@UPD 0                                     (актуальний)
#                              @@UPDERR <причина>                          (не вдалося перевірити)
#                              @@UPDOFF <причина>                          (оновлення вимкнено — dev-гілка)
#    gui/selfupdate.sh apply  → git merge --ff-only origin/stable
#  Оновлення йдуть лише на гілці `stable` (її просуває scripts/mgos-release.sh після selftest).
#  На будь-якій іншій гілці (mgos/*, main…) оновлення вимкнено — це не помилка.
#  Брудне дерево → відмова зі status (без --autostash). MGOS_CHANNEL_BRANCH — перевизначити гілку.
#  Приватний репозиторій: доступ через «gh auth login».
# ============================================================================
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
export GIT_TERMINAL_PROMPT=0 LC_ALL=C
CHAN=${MGOS_CHANNEL_BRANCH:-stable}
UP="origin/$CHAN"

[ -d .git ] || { echo "@@UPDERR інсталятор не з git (завантаж: git clone)"; exit 2; }
command -v git >/dev/null || { echo "@@UPDERR немає git"; exit 2; }

cur=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)
if [ "$cur" != "$CHAN" ]; then
    echo "@@UPDOFF оновлення вимкнено: гілка «$cur» (канал оновлень — «$CHAN»)"
    exit 0
fi
git remote get-url origin >/dev/null 2>&1 || { echo "@@UPDERR немає origin"; exit 2; }

case "${1:-check}" in
    check)
        if ! out=$(timeout 25 git fetch --quiet origin "$CHAN" 2>&1); then
            grep -qiE 'auth|could not read|403|404|denied' <<< "$out" \
                && echo "@@UPDERR немає доступу до GitHub — виконай: gh auth login" \
                || echo "@@UPDERR немає з'єднання з GitHub"
            exit 1
        fi
        n=$(git rev-list --count "HEAD..$UP" 2>/dev/null || echo 0)
        if [ "$n" -gt 0 ]; then echo "@@UPD $n $(git log -1 --format=%s "$UP")"; else echo "@@UPD 0"; fi
        ;;
    apply)
        if [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
            echo "@@UPDERR є локальні зміни — оновлення скасовано (нічого не втрачено):"
            git status --short --untracked-files=no | head -15
            exit 1
        fi
        timeout 60 git fetch --quiet origin "$CHAN" 2>&1 || { echo "@@UPDERR немає з'єднання з GitHub"; exit 1; }
        old=$(git rev-parse HEAD)
        if ! git merge --ff-only --quiet "$UP" 2>&1; then
            echo "@@UPDERR не вдалося оновити (не fast-forward) — подивись: git status"; exit 1
        fi
        echo "Оновлено: $(git log --oneline "$old..HEAD" | wc -l) комітів"
        git log --format='  • %s' "$old..HEAD" | head -15
        chmod +x install.sh install-gui.sh gui/*.sh scripts/* 2>/dev/null
        ;;
    *) echo "використання: $0 check|apply"; exit 2 ;;
esac
