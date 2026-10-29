#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DCONFIG_DUST_ECAT_DC_SYNC=1 -DCONFIG_DUST_ECAT_DC_PROFILE=1 -I "$root/tests/pdi_native" \
  -I "$root/src/ethercat/slave" "$root/src/ethercat/slave/sync0_deferred.c" \
  "$root/tests/sync0_deferred_contracts.c" -o "$build/contracts"
"$build/contracts"

for profile in 0 1 wait; do
  extra=()
  if [[ $profile == 1 ]]; then extra=(-DCONFIG_DUST_ECAT_DC_PROFILE=1); fi
  if [[ $profile == wait ]]; then extra=(-DCONFIG_DUST_ECAT_DC_WAIT_PROFILE=1); fi
  "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DCONFIG_DUST_ECAT_DC_SYNC=1 "${extra[@]+"${extra[@]}"}" -I "$root/tests/pdi_native" \
    -I "$root/src/ethercat/slave" "$root/src/ethercat/slave/sync0_deferred.c" \
    "$root/src/ethercat/slave/wait_profile.c" "$root/tests/sync0_budget_contracts.c" -o "$build/budget"
  "$build/budget"
done

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DCONFIG_DUST_ECAT_DC_WAIT_PROFILE=1 -I "$root/tests/pdi_native" \
  -I "$root/src/ethercat/slave" "$root/src/ethercat/slave/wait_profile.c" \
  "$root/tests/wait_profile_contracts.c" -o "$build/wait"
"$build/wait"
