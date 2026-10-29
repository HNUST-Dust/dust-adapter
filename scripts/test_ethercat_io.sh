#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
python3 "$root/tools/generate_can_profile.py" --input "$root/config/can_devices.yaml" --output "$build/can_profile_generated.hpp"
for mode in baseline window; do
  window_flags=()
  if [[ $mode == window ]]; then
    window_flags=(-DCONFIG_DUST_ECAT_CAN_FEEDBACK_WINDOW=1)
  fi
"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Wno-missing-field-initializers -Werror -fsanitize=address,undefined -DCONFIG_UART_HPMICRO=1 "${window_flags[@]+"${window_flags[@]}"}" \
 -I "$root/tests/io_native" -I "$root/../hpm_support/include" -I "$root/src" -I "$root/src/ethercat/slave" -I "$build" \
 "$root/tests/ethercat_io_contracts.cpp" "$root/src/ethercat/slave/io_backend.cpp" "$root/src/drivers/uart_receive.cpp" "$root/src/ethercat/core.cpp" "$root/src/ethercat/layout.cpp" -o "$build/io_contracts"
  "$build/io_contracts"
done
# Exercise the same backend with a real native receive thread and synchronized
# mock IRQ queues; barriers verify snapshot ownership and Stop/Arm transitions.
for notice in baseline fast; do
  notice_flags=()
  if [[ $notice == fast ]]; then
    notice_flags=(-DCONFIG_DUST_ECAT_CAN_RX_NOTIFY_FAST=1 -DCONFIG_DUST_ECAT_INPUT_LATE_REFRESH=1)
  fi
  "${CXX:-c++}" -std=c++17 -pthread "${notice_flags[@]+"${notice_flags[@]}"}" -Wall -Wextra -Wno-missing-field-initializers -Werror -fsanitize=address,undefined -DCONFIG_UART_HPMICRO=1 -DCONFIG_DUST_ECAT_CAN_RX_THREAD=1 -DCONFIG_DUST_ECAT_CAN_RX_POLL_US=0 -DCONFIG_DUST_ECAT_SCHED_STATS=1 -DCONFIG_MAIN_THREAD_PRIORITY=1 \
   -I "$root/tests/io_native" -I "$root/../hpm_support/include" -I "$root/src" -I "$root/src/ethercat/slave" -I "$build" \
   "$root/tests/ethercat_io_contracts.cpp" "$root/src/ethercat/slave/io_backend.cpp" "$root/src/drivers/uart_receive.cpp" "$root/src/ethercat/core.cpp" "$root/src/ethercat/layout.cpp" -o "$build/io_thread_contracts"
  "$build/io_thread_contracts"
done
"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I "$root/tests/io_native" -I "$root/src" \
 "$root/tests/ethercat_boot_contracts.cpp" "$root/src/ethercat/slave/boot_identity.cpp" "$root/src/ethercat/layout.cpp" -o "$build/boot_contracts"
for scenario in success zero_uid mount_error read_error corrupt write_short write_error verify_error; do
 "$build/boot_contracts" "$scenario"
done
