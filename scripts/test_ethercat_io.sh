#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
python3 "$root/tools/generate_can_profile.py" --input "$root/config/can_devices.yaml" --output "$build/can_profile_generated.hpp"
"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Wno-missing-field-initializers -Werror -fsanitize=address,undefined -DCONFIG_UART_HPMICRO=1 \
 -I "$root/tests/io_native" -I "$root/../hpm_support/include" -I "$root/src" -I "$root/apps/ethercat_slave/src" -I "$build" \
 "$root/tests/ethercat_io_contracts.cpp" "$root/apps/ethercat_slave/src/io_backend.cpp" "$root/src/drivers/uart_receive.cpp" "$root/src/ethercat/core.cpp" "$root/src/ethercat/layout.cpp" -o "$build/io_contracts"
"$build/io_contracts"
"${CXX:-c++}" -std=c++17 -fno-exceptions -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I "$root/tests/io_native" -I "$root/src" \
 "$root/tests/ethercat_boot_contracts.cpp" "$root/apps/ethercat_slave/src/boot_identity.cpp" "$root/src/ethercat/layout.cpp" -o "$build/boot_contracts"
for scenario in success zero_uid mount_error read_error corrupt write_short write_error verify_error; do
 "$build/boot_contracts" "$scenario"
done
