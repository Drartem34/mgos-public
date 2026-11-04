#!/usr/bin/env bash
# Запуск Spotlight Runner для kioskOS із мінімальним споживанням RAM
unset GSK_RENDERER
export MALLOC_ARENA_MAX=1
exec "$(dirname "$0")/runner" "$@"
