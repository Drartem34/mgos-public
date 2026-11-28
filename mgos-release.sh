#!/usr/bin/env bash
# ============================================================================
#  Випуск mgOS: просуває гілку `stable` (канал оновлень) до поточного коміту.
#    1. дерево має бути чистим (git status без змін у відстежуваних файлах)
#    2. tests/selftest.sh — має пройти повністю
#    3. лише при успіху: stable ← HEAD строго fast-forward + тег mgos-<дата>
#  Нічого не пушить; прапорці:  --push  (після успіху git push origin stable + тег)
#  Запускай з гілки mgos/* (або main) — `stable` рухається лише вперед.
# ============================================================================
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
CHAN=stable
PUSH=0; [ "${1:-}" = --push ] && PUSH=1

die() { printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
cur=$(git rev-parse --abbrev-ref HEAD)
[ "$cur" != "$CHAN" ] || die "ти на «$CHAN» — перейди на гілку з роботою (mgos/*), звідти просуваємо"
[ -z "$(git status --porcelain --untracked-files=no)" ] || { git status --short --untracked-files=no | head -20; die "брудне дерево — спершу закоміть або прибери зміни"; }

echo "▸ selftest"
bash tests/selftest.sh || die "selftest не пройшов — stable НЕ змінено"

head=$(git rev-parse HEAD)
if git rev-parse -q --verify "refs/heads/$CHAN" >/dev/null; then
    git merge-base --is-ancestor "$CHAN" "$head" || die "stable не є предком HEAD (не fast-forward) — спершу злий stable в цю гілку"
    [ "$(git rev-parse "$CHAN")" != "$head" ] || { echo "stable вже на цьому коміті"; exit 0; }
fi
git branch -f "$CHAN" "$head"
tag="mgos-$(date +%Y%m%d-%H%M)"
git tag -a "$tag" -m "mgOS stable $tag" "$head"
echo "✓ stable → $(git rev-parse --short "$head"), тег $tag"
if [ "$PUSH" = 1 ]; then git push origin "$CHAN" "$tag"; else echo "  Опублікувати: git push origin $CHAN $tag"; fi
