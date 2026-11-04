#!/usr/bin/env bash
# Запуск кіоск-дока з оптимізацією ОЗУ
unset GSK_RENDERER
export MALLOC_ARENA_MAX=1
exec "$(dirname "$0")/dock" "$@"
