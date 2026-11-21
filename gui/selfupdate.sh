#!/usr/bin/env bash
# ============================================================================
#  Оновлення самого інсталятора з git (репозиторій mgos на GitHub).
#    gui/selfupdate.sh check  → @@UPD <скільки комітів> <тема останнього>   (є оновлення)
#                              @@UPD 0                                     (актуальний)
#                              @@UPDERR <причина>                          (не вдалося перевірити)
#    gui/selfupdate.sh apply  → git pull --ff-only; GUI перезбирається при наступному запуску
#  Приватний репозиторій: доступ через «gh auth login» (gh налаштовує git сам).
#  Локальні зміни (sync_from_system.sh, власні правки) не губляться: --autostash.
# ============================================================================
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
export GIT_TERMINAL_PROMPT=0 LC_ALL=C

[ -d .git ] || { echo "@@UPDERR інсталятор не з git (завантаж: git clone)"; exit 2; }
command -v git >/dev/null || { echo "@@UPDERR немає git"; exit 2; }
git rev-parse --abbrev-ref '@{u}' >/dev/null 2>&1 || { echo "@@UPDERR гілка без origin"; exit 2; }

case "${1:-check}" in
    check)
        if ! out=$(timeout 25 git fetch --quiet origin 2>&1); then
            grep -qiE 'auth|could not read|403|404|denied' <<< "$out" \
                && echo "@@UPDERR немає доступу до GitHub — виконай: gh auth login" \
                || echo "@@UPDERR немає з'єднання з GitHub"
            exit 1
        fi
        n=$(git rev-list --count 'HEAD..@{u}' 2>/dev/null || echo 0)
        if [ "$n" -gt 0 ]; then echo "@@UPD $n $(git log -1 --format=%s '@{u}')"; else echo "@@UPD 0"; fi
        ;;
    apply)
        old=$(git rev-parse HEAD)
        if ! git pull --ff-only --autostash --quiet 2>&1; then
            echo "@@UPDERR не вдалося оновити (git pull) — подивись: git status"; exit 1
        fi
        echo "Оновлено: $(git log --oneline "$old..HEAD" | wc -l) комітів"
        git log --format='  • %s' "$old..HEAD" | head -15
        chmod +x install.sh install-gui.sh gui/*.sh scripts/* 2>/dev/null
        ;;
    *) echo "використання: $0 check|apply"; exit 2 ;;
esac
