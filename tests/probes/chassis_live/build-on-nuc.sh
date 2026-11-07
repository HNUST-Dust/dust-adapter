#!/usr/bin/env bash
set -euo pipefail
probe_root="${1:-/tmp/dust-chassis-live}"
control_root="${2:-/home/wbr-infantry/dust-control}"
lib_root="$control_root/build/production/src"
link_flags=()
for component in chassis hardware/ethercat hardware/imu hardware/remote hardware/motors; do
  link_flags+=("-L$lib_root/$component" "-Wl,-rpath,$lib_root/$component")
done
g++ -std=c++20 -O2 -pthread -I"$probe_root/src" -I"$control_root/src" -I/usr/include/eigen3 \
  "$probe_root/tests/probes/chassis_live/runner.cpp" "${link_flags[@]}" \
  -ldust_chassis_runtime -ldust_chassis -ldust_ethercat -ldust_ethercat_pdo \
  -ldust_imu -ldust_remote -ldust_motors -lyaml-cpp -o "$probe_root/controller-runner"
