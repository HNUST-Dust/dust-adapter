#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/src" "$root/tests/latest_can_contracts.cpp" -o "$build/contracts"
"$build/contracts"
echo 'CAN latest slots isolation / expiry / wrap contracts PASS'
