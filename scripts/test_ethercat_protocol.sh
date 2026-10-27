#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
flags=(-std=c++17 -fno-exceptions -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I "$root/src")
"${CXX:-c++}" "${flags[@]}" "$root/src/ethercat/layout.cpp" "$root/src/ethercat/core.cpp" \
  "$root/tests/ethercat_core_contracts.cpp" -o "$build/core_contracts"
"$build/core_contracts"
"${CXX:-c++}" "${flags[@]}" "$root/src/ethercat/pdo_dictionary.cpp" \
  "$root/tests/ethercat_mapping_contracts.cpp" -o "$build/mapping_contracts"
"$build/mapping_contracts"
if [[ -n "${DUST_HOST_ROOT:-}" ]]; then
  # 直接调用主机现有生成器，不能用从站实现自己生成“期望值”。
  "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I "$root/src" -I "$DUST_HOST_ROOT/src" \
    "$root/src/ethercat/layout.cpp" "$root/src/ethercat/pdo_dictionary.cpp" \
    "$DUST_HOST_ROOT/src/hardware/ethercat/pdo_config.cpp" \
    "$root/tests/ethercat_layout_crosscheck.cpp" -lyaml-cpp -o "$build/crosscheck"
  "$build/crosscheck"
else
  echo 'Host crosscheck not run: set DUST_HOST_ROOT to the dust-control checkout.'
fi
