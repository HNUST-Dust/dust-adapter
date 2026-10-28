#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -DCONFIG_DUST_ECAT_RTT_TRACE=1 -DCONFIG_HPM_MCAN_RX_TIMING=1 -DCONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=24000000 \
 -I "$root/tests/rtt_trace_native" -I "$root/../hpm_support/include" "$root/tests/ethercat_rtt_trace_contracts.cpp" -o "$build/contracts"
"$build/contracts"
echo 'RTT trace contracts passed'
