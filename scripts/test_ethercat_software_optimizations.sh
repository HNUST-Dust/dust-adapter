#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I "$root/src")
"${CXX:-c++}" "${flags[@]}" "$root/tests/digest_contracts.cpp" -o "$build/digest"
"$build/digest"
for mode in reference optimized; do
  options=()
  if [[ $mode == reference ]]; then options=(-DCONFIG_DUST_ECAT_REFERENCE_DIGEST=1)
  else options=(-DCONFIG_DUST_ECAT_PUBLISH_SKIP_OVERWRITE_CLEAR=1); fi
  "${CXX:-c++}" "${flags[@]}" "${options[@]}" "$root/src/ethercat/core.cpp" "$root/src/ethercat/layout.cpp" "$root/tests/publish_equivalence.cpp" -o "$build/publish"
  "$build/publish" > "$build/$mode.bin"
  "${CXX:-c++}" "${flags[@]}" "${options[@]}" "$root/src/ethercat/core.cpp" "$root/src/ethercat/layout.cpp" "$root/tests/ethercat_core_contracts.cpp" -o "$build/core"
  "$build/core"
done
cmp "$build/reference.bin" "$build/optimized.bin"
echo 'Reference/optimized complete PDO byte equivalence PASS'
