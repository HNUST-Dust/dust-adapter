#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
transport=ethercat
if [[ $# -gt 0 && "$1" != -* ]]; then
  transport="$1"
  shift
fi
case "$transport" in
  ethercat) source_dir="$root/apps/ethercat_slave"; board=dust-hpm5e3y; output=ethercat_slave ;;
  usb) source_dir="$root"; board=dust-hpm6750; output=usb_gateway ;;
  *) echo "Usage: $0 [ethercat|usb] [west build arguments]" >&2; exit 2 ;;
esac
# Only build; no flashing. Keep independent CMake caches for both applications.
exec "${WEST:-west}" build -s "$source_dir" -b "$board" \
  -d "${BUILD_DIR:-$root/build/$output}" "$@"
