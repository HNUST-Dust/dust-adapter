#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 统一测试入口：先构建/运行本仓库 CTest 原生测试，再运行 hpm_support 外部原生测试。
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
support="${DUST_HPM_SUPPORT_DIR:-$root/../hpm_support}"
build="${DUST_NATIVE_TEST_BUILD_DIR:-$root/build/native_tests}"

echo "== driver boundary =="
python3 "$root/tools/check_driver_boundary.py"

# ---------------------------------------------------------------------------
# 本仓库原生测试（tests/CMakeLists.txt + CTest）
# ---------------------------------------------------------------------------
cmake_args=()
if [[ ! -f "$build/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
  cmake_args+=(-G Ninja)
fi
# 可选接线随环境变量透传；未设置时使用 CMake 默认值。
if [[ -n "${DUST_HPM_SUPPORT_DIR:-}" ]]; then
  cmake_args+=(-DDUST_HPM_SUPPORT_DIR="$support")
fi
if [[ -n "${DUST_HOST_ROOT:-}" ]]; then
  cmake_args+=(-DDUST_HOST_ROOT="$DUST_HOST_ROOT")
fi
echo "== native tests: configure ($build) =="
cmake -S "$root/tests" -B "$build" ${cmake_args[@]+"${cmake_args[@]}"}
echo "== native tests: build =="
cmake --build "$build" -j
echo "== native tests: ctest =="
ctest --test-dir "$build" --output-on-failure

# ---------------------------------------------------------------------------
# 外部 hpm_support 原生测试（锁定的板级支持，属于工作区回归）
# 脚本不存在时明确 SKIP；真实失败仍会因 set -e 让总入口返回非 0。
# ---------------------------------------------------------------------------
run_optional() {
  local script="$1" label="$2" rel
  rel="${script#"$root/../"}"
  if [[ ! -f "$script" ]]; then
    echo "SKIP: $label (script unavailable: $rel)"
    return 0
  fi
  echo "== $label ($rel) =="
  if [[ -x "$script" ]]; then
    "$script"
  else
    bash "$script"
  fi
}

if [[ "$(uname -s)" == Darwin ]]; then
  # 第二阶段用 __attribute__((section(".isr"))) 模拟中断向量，Mach-O 不支持。
  echo "SKIP (macOS/Mach-O unsupported): $support/tests/native/test_uart_idle.sh (section(\".isr\") 仅支持 ELF)"
elif [[ -f "$support/tests/native/test_uart_idle.sh" ]]; then
  run_optional "$support/tests/native/test_uart_idle.sh" "UART idle contracts"
else
  echo 'SKIP: UART idle contracts (hpm_support native test script unavailable).'
fi

run_optional "$support/tests/native/test_mcan_tx.sh" "MCAN contracts"
run_optional "$support/tests/native/test_hpm5e_clock.sh" "HPM5E clock contracts"
run_optional "$support/tests/native/test_esc_access.sh" "ESC transfer contracts"
run_optional "$support/tests/native/test_mcan_timing.sh" "MCAN RX timing contracts"
run_optional "$support/tests/native/test_mcan_rx.sh" "MCAN RX FIFO contracts"

echo 'All native and external tests PASS (see SKIP lines for skipped suites).'
