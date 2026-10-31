#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/tests/buzzer_native" "$root/tests/buzzer_native/contracts.c" -o "$build/contracts"
"$build/contracts"
echo 'Buzzer bounded pulse / retrigger / stop / failure contracts PASS'
