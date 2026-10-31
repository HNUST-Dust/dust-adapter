#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
python3 "$root/tools/generate_gateway_esi.py" --output "$build"
cp "${DUST_TEST_SSC_DIR:-$root/external/ssc}/"*.h "${DUST_TEST_SSC_DIR:-$root/external/ssc}/"*.c "$build/"
# 仓库拥有的应用头和生成的 EEPROM 取代计数器示例，和目标构建一致。
cp "$root/src/ethercat/slave/ssc/sampleappl.h" "$build/sampleappl.h"
python3 "$root/tools/generate_gateway_esi.py" --output "$build"
flags=(${DUST_TEST_EXTRA_FLAGS:-} -DCONFIG_DUST_ECAT_ESC_DMA=1 -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-omit-frame-pointer
  -I "$build" -I "$root/tests/ssc_native" -I "$root/src/ethercat/slave" -I "$root/src"
  -include "$root/tests/ssc_native/platform.h" -include "$build/gateway_config.h")
gc_flags=(-Wl,--gc-sections)
if [[ "$(uname -s)" == Darwin ]]; then
  gc_flags=(-Wl,-dead_strip)
fi
for source in objdef coeappl; do
  # 保留真实 SSC 的 SDO 分派、长度和状态检查；只替换 ESC/时钟边界。
  "${CC:-cc}" -std=c11 "${flags[@]}" -c "$build/$source.c" -o "$build/$source.o"
done
for mode in baseline coalesce late_refresh; do
  mode_flags=()
  if [[ $mode == coalesce ]]; then
    mode_flags=(-DCONFIG_DUST_ECAT_INPUT_WRITE_COALESCE=1)
  elif [[ $mode == late_refresh ]]; then
    mode_flags=(-DCONFIG_DUST_ECAT_INPUT_LATE_REFRESH=1)
  fi
  "${CXX:-c++}" -std=c++17 "${mode_flags[@]+"${mode_flags[@]}"}" -Wall -Wextra -Werror -fno-exceptions "${flags[@]}" \
    "$root/src/ethercat/slave/gateway.cpp" "$root/src/ethercat/layout.cpp" \
    "$root/src/ethercat/core.cpp" "$root/src/ethercat/pdo_dictionary.cpp" \
    "$root/tests/ssc_native/io_stub.cpp" "$root/tests/ethercat_ssc_contracts.cpp" "$build/objdef.o" "$build/coeappl.o" \
    "${gc_flags[@]}" -o "$build/ssc_contracts"
  "$build/ssc_contracts"
done
python3 "$root/tests/ethercat_esi_contracts.py"
