#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
python3 "$root/tests/test_can_profile_generator.py"
python3 "$root/tests/phy_link_contracts.py"
bash "$root/scripts/test_ethercat_software_optimizations.sh"
python3 "$root/tools/generate_can_profile.py" --input "$root/config/can_devices.yaml" --output "$build/can_profile_generated.hpp"
"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" "$root/tests/protocol_contracts.cpp" -o "$build/contracts"
"$build/contracts"

"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" -I "$build" "$root/tests/can_profile_contracts.cpp" -o "$build/profile_contracts"
"$build/profile_contracts"

"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" "$root/tests/heater_contracts.cpp" -o "$build/heater_contracts"
"$build/heater_contracts"

"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" "$root/tests/usb_message_contracts.cpp" -o "$build/usb_message_contracts"
"$build/usb_message_contracts"

"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" "$root/tests/usb_control_message_contracts.cpp" -o "$build/usb_control_message_contracts"
"$build/usb_control_message_contracts"

"$root/scripts/test_ethercat_protocol.sh"

"$root/scripts/test_ethercat_ssc.sh"

"$root/scripts/test_ethercat_io.sh"

bash "$root/scripts/test_ethercat_rtt_trace.sh"

bash "$root/scripts/test_pdi_deferred.sh"
bash "$root/scripts/test_sync0_deferred.sh"

# MCAN 批次状态模块属于锁定的板级支持，参与工作区回归。
support="${DUST_HPM_SUPPORT_DIR:-$root/../hpm_support}"
if [[ -x "$support/tests/native/test_mcan_tx.sh" ]]; then
  "$support/tests/native/test_mcan_tx.sh"
else
  echo 'MCAN contracts not run: hpm_support native test script unavailable.'
fi

if [[ -x "$support/tests/native/test_uart_idle.sh" ]]; then
  "$support/tests/native/test_uart_idle.sh"
else
  echo 'UART idle contracts not run: hpm_support native test script unavailable.'
fi

if [[ -x "$support/tests/native/test_hpm5e_clock.sh" ]]; then
  "$support/tests/native/test_hpm5e_clock.sh"
else
  echo 'HPM5E clock contracts not run: hpm_support native test script unavailable.'
fi

if [[ -f "$support/tests/native/test_esc_access.sh" ]]; then
  bash "$support/tests/native/test_esc_access.sh"
else
  echo "ESC transfer contracts not run: hpm_support native test script unavailable."
fi

bash "$root/scripts/test_ethercat_esc_dma.sh"

if [[ -f "$support/tests/native/test_mcan_timing.sh" ]]; then
  bash "$support/tests/native/test_mcan_timing.sh"
else
  echo 'MCAN RX timing contracts not run: hpm_support script unavailable.'
fi

if [[ -f "$support/tests/native/test_mcan_rx.sh" ]]; then
  bash "$support/tests/native/test_mcan_rx.sh"
else
  echo 'MCAN RX FIFO contracts not run: hpm_support script unavailable.'
fi
