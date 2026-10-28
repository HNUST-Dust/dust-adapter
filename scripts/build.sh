#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
transport=ethercat
if [[ $# -gt 0 && "$1" != -* ]]; then
  transport="$1"
  shift
fi
case "$transport" in
  ethercat) source_dir="$root"; board=dust-hpm5e31; output=ethercat_slave ;;
  *) echo "Usage: $0 [ethercat] [west build arguments]" >&2; exit 2 ;;
esac
# Only build; no flashing. The root application owns all firmware sources.
exec "${WEST:-west}" build -s "$source_dir" -b "$board" \
  -d "${BUILD_DIR:-$root/build/$output}" "$@"
