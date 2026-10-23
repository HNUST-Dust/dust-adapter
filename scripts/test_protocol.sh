#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
python3 "$root/tests/test_can_profile_generator.py"
python3 "$root/tools/generate_can_profile.py" --input "$root/config/can_devices.yaml" --output "$build/can_profile_generated.hpp"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" "$root/tests/protocol_contracts.cpp" -o "$build/contracts"
"$build/contracts"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$root/src" -I "$build" "$root/tests/can_profile_contracts.cpp" -o "$build/profile_contracts"
"$build/profile_contracts"
