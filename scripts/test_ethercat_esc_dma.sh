#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
python3 "$root/tests/esc_dma_native/extract.py" "$build/esc_dma_functions.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I "$build" \
 "$root/tests/esc_dma_native/contracts.c" -o "$build/contracts"
for scenario in 0 1 2 3 4 6 7; do "$build/contracts" "$scenario"; done
