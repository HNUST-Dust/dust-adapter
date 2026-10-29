#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DCONFIG_DUST_ECAT_SM_SYNC=1 -I "$root/tests/pdi_native" \
  -I "$root/src/ethercat/slave" "$root/src/ethercat/slave/pdi_deferred.c" \
  "$root/tests/pdi_deferred_contracts.c" -o "$build/contracts"
"$build/contracts"
